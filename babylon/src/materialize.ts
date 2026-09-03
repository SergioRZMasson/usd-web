import { Animation } from "@babylonjs/core/Animations/animation.js";
import { AnimationGroup } from "@babylonjs/core/Animations/animationGroup.js";
import { AssetContainer } from "@babylonjs/core/assetContainer.js";
import { Bone } from "@babylonjs/core/Bones/bone.js";
import { Skeleton } from "@babylonjs/core/Bones/skeleton.js";
import { ShaderStore } from "@babylonjs/core/Engines/shaderStore.js";
import { Color3 } from "@babylonjs/core/Maths/math.color.js";
import { Matrix, Quaternion, Vector3 } from "@babylonjs/core/Maths/math.vector.js";
import { Material } from "@babylonjs/core/Materials/material.js";
import { MultiMaterial } from "@babylonjs/core/Materials/multiMaterial.js";
import { PBRMaterial } from "@babylonjs/core/Materials/PBR/pbrMaterial.js";
import { Texture } from "@babylonjs/core/Materials/Textures/texture.js";
import { Mesh } from "@babylonjs/core/Meshes/mesh.js";
import { SubMesh } from "@babylonjs/core/Meshes/subMesh.js";
import { TransformNode } from "@babylonjs/core/Meshes/transformNode.js";
import { VertexData } from "@babylonjs/core/Meshes/mesh.vertexData.js";
import type { Scene } from "@babylonjs/core/scene.js";
import { pbrPixelShader } from "@babylonjs/core/Shaders/pbr.fragment.js";
import { pbrVertexShader } from "@babylonjs/core/Shaders/pbr.vertex.js";

import {
    AnimationProperty,
    AnimationTarget,
    Command,
    GeometryFlags,
    MaterialFlags,
    MeshFlags,
    MISSING_OFFSET,
    PayloadReader,
    readCommands,
} from "./protocol.js";

interface GeometryDescriptor {
    vertexCount: number;
    indexCount: number;
    flags: number;
    positions: number;
    normals: number;
    tangents: number;
    uv0: number;
    colors: number;
    joints0: number;
    weights0: number;
    joints1: number;
    weights1: number;
    indices: number;
    influences: number;
}

export interface MaterializationResult {
    container: AssetContainer;
    materializeMs: number;
}

const decoder = new TextDecoder();

function assertRange(
    data: ArrayBuffer,
    offset: number,
    count: number,
    bytesPerElement: number,
    label: string,
): void {
    if (
        !Number.isInteger(offset) ||
        !Number.isInteger(count) ||
        offset < 0 ||
        count < 0 ||
        offset % Math.min(bytesPerElement, 4) !== 0 ||
        count > Math.floor((data.byteLength - offset) / bytesPerElement)
    ) {
        throw new Error(`Invalid ${label} range in OpenUSD Babylon data buffer.`);
    }
}

function stringAt(data: ArrayBuffer, offset: number, length: number): string {
    assertRange(data, offset, length, 1, "string");
    return decoder.decode(new Uint8Array(data, offset, length));
}

function matrixAt(data: ArrayBuffer, offset: number): Matrix {
    assertRange(data, offset, 16, 4, "matrix");
    const values = new Float32Array(data, offset, 16);
    return Matrix.FromValues(
        values[0],
        values[1],
        values[2],
        values[3],
        values[4],
        values[5],
        values[6],
        values[7],
        values[8],
        values[9],
        values[10],
        values[11],
        values[12],
        values[13],
        values[14],
        values[15],
    );
}

function applyMatrix(node: TransformNode, matrix: Matrix): void {
    const scale = new Vector3();
    const rotation = new Quaternion();
    const translation = new Vector3();
    matrix.decompose(scale, rotation, translation);
    node.position.copyFrom(translation);
    node.rotationQuaternion = rotation;
    node.scaling.copyFrom(scale);
}

function mimeType(value: number): string {
    switch (value) {
        case 2:
            return "image/jpeg";
        case 3:
            return "image/bmp";
        case 4:
            return "image/webp";
        default:
            return "image/png";
    }
}

function wrapMode(value: number): number {
    switch (value) {
        case 0:
            return Texture.CLAMP_ADDRESSMODE;
        case 1:
            return Texture.WRAP_ADDRESSMODE;
        case 2:
            return Texture.MIRROR_ADDRESSMODE;
        default:
            throw new Error(`Invalid texture wrap mode ${value}.`);
    }
}

function ensurePbrShaders(): void {
    ShaderStore.ShadersStore[pbrVertexShader.name] ??= pbrVertexShader.shader;
    ShaderStore.ShadersStore[pbrPixelShader.name] ??= pbrPixelShader.shader;
}

export async function materializeCommandBuffers(
    scene: Scene,
    commandBuffer: ArrayBuffer,
    dataBuffer: ArrayBuffer,
    addToScene: boolean,
): Promise<MaterializationResult> {
    const started = performance.now();
    ensurePbrShaders();
    const container = new AssetContainer(scene);
    const commands = readCommands(commandBuffer);
    const nodes = new Map<number, TransformNode>();
    const pendingParents: Array<{ node: TransformNode; parentId: number }> = [];
    const textures = new Map<number, Texture>();
    const materials = new Map<number, PBRMaterial>();
    const doubleSidedMaterials = new Map<number, PBRMaterial>();
    const skeletons = new Map<number, Skeleton>();
    const bones = new Map<number, Bone>();
    const geometries = new Map<number, GeometryDescriptor>();
    const meshes = new Map<number, Mesh>();
    const animationGroups = new Map<number, AnimationGroup>();
    const textureLoads: Promise<void>[] = [];
    let root: TransformNode | undefined;
    let timeCodesPerSecond = 24;

    try {
    for (const command of commands) {
        const payload = new PayloadReader(
            commandBuffer,
            command.payloadOffset,
            command.payloadLength,
        );
        switch (command.opcode) {
            case Command.Scene: {
                const zUp = payload.u32() === 1;
                const metersPerUnit = payload.f32();
                timeCodesPerSecond = payload.f32() || 24;
                root = new TransformNode("USD Root", scene);
                root.rotationQuaternion = zUp
                    ? Quaternion.FromArray([-0.7071068, 0, 0, 0.7071068])
                    : Quaternion.Identity();
                root.scaling.copyFrom(
                    scene.useRightHandedSystem
                        ? new Vector3(metersPerUnit, metersPerUnit, metersPerUnit)
                        : zUp
                          ? new Vector3(metersPerUnit, -metersPerUnit, metersPerUnit)
                          : new Vector3(metersPerUnit, metersPerUnit, -metersPerUnit),
                );
                container.transformNodes.push(root);
                container.rootNodes.push(root);
                break;
            }
            case Command.Texture: {
                const id = payload.u32();
                const nameOffset = payload.u32();
                const nameLength = payload.u32();
                const mime = mimeType(payload.u32());
                const imageOffset = payload.u32();
                const imageLength = payload.u32();
                const coordinatesIndex = payload.u32();
                const transformOffset = payload.u32();
                const wrapU = payload.u32();
                const wrapV = payload.u32();
                assertRange(dataBuffer, transformOffset, 5, 4, "texture transform");
                assertRange(dataBuffer, imageOffset, imageLength, 1, "texture image");
                const transform = new Float32Array(dataBuffer, transformOffset, 5);
                const bytes = new Uint8Array(dataBuffer, imageOffset, imageLength);
                const url = URL.createObjectURL(new Blob([bytes], { type: mime }));
                let resolveLoad!: () => void;
                let rejectLoad!: (error: Error) => void;
                const loaded = new Promise<void>((resolve, reject) => {
                    resolveLoad = resolve;
                    rejectLoad = reject;
                });
                const texture = new Texture(
                    url,
                    scene,
                    false,
                    false,
                    Texture.TRILINEAR_SAMPLINGMODE,
                    resolveLoad,
                    (message, exception) => {
                        URL.revokeObjectURL(url);
                        rejectLoad(
                            exception instanceof Error
                                ? exception
                                : new Error(message || `Could not load texture '${url}'.`),
                        );
                    },
                );
                textureLoads.push(loaded);
                texture.name = stringAt(dataBuffer, nameOffset, nameLength);
                texture.coordinatesIndex = coordinatesIndex;
                texture.uScale = transform[0];
                texture.vScale = transform[1];
                texture.uOffset = transform[2];
                texture.vOffset = transform[3];
                texture.wAng = transform[4];
                texture.wrapU = wrapMode(wrapU);
                texture.wrapV = wrapMode(wrapV);
                texture.onDisposeObservable.addOnce(() => URL.revokeObjectURL(url));
                textures.set(id, texture);
                container.textures.push(texture);
                break;
            }
            case Command.Material: {
                const id = payload.u32();
                const nameOffset = payload.u32();
                const nameLength = payload.u32();
                const baseOffset = payload.u32();
                const emissiveOffset = payload.u32();
                const metallic = payload.f32();
                const roughness = payload.f32();
                const normalScale = payload.f32();
                const alphaCutoff = payload.f32();
                const flags = payload.u32();
                const baseTexture = payload.u32();
                const opacityTexture = payload.u32();
                const normalTexture = payload.u32();
                const ormTexture = payload.u32();
                const emissiveTexture = payload.u32();
                const opacityChannel = payload.u32();
                const roughnessChannel = payload.u32();
                const metallicChannel = payload.u32();
                const occlusionChannel = payload.u32();
                assertRange(dataBuffer, baseOffset, 4, 4, "material base color");
                assertRange(dataBuffer, emissiveOffset, 3, 4, "material emissive color");
                const base = new Float32Array(dataBuffer, baseOffset, 4);
                const emissive = new Float32Array(dataBuffer, emissiveOffset, 3);
                const material = new PBRMaterial(
                    stringAt(dataBuffer, nameOffset, nameLength),
                    scene,
                );
                material.id = `usd-material-${id}`;
                material.albedoColor = new Color3(base[0], base[1], base[2]);
                material.alpha = base[3];
                material.metallic = metallic;
                material.roughness = roughness;
                material.emissiveColor = new Color3(emissive[0], emissive[1], emissive[2]);
                const doubleSided = Boolean(flags & MaterialFlags.DoubleSided);
                material.backFaceCulling = !doubleSided;
                material.twoSidedLighting = doubleSided;
                material.unlit = Boolean(flags & MaterialFlags.Unlit);
                if (flags & MaterialFlags.AlphaBlend) {
                    material.transparencyMode = 2;
                }
                if (alphaCutoff > 0) {
                    material.transparencyMode = 1;
                    material.alphaCutOff = alphaCutoff;
                }
                if (baseTexture !== MISSING_OFFSET) {
                    material.albedoTexture = textures.get(baseTexture) ?? null;
                    if (
                        material.albedoTexture &&
                        opacityTexture === MISSING_OFFSET &&
                        flags & MaterialFlags.AlphaBlend
                    ) {
                        material.albedoTexture.hasAlpha = true;
                    }
                }
                if (opacityTexture !== MISSING_OFFSET) {
                    material.opacityTexture = textures.get(opacityTexture) ?? null;
                    if (material.opacityTexture) {
                        material.opacityTexture.gammaSpace = false;
                        material.opacityTexture.getAlphaFromRGB = opacityChannel !== 3;
                    }
                }
                if (normalTexture !== MISSING_OFFSET) {
                    material.bumpTexture = textures.get(normalTexture) ?? null;
                    if (material.bumpTexture) {
                        material.bumpTexture.gammaSpace = false;
                        material.bumpTexture.level = normalScale;
                    }
                }
                if (ormTexture !== MISSING_OFFSET) {
                    material.metallicTexture = textures.get(ormTexture) ?? null;
                    if (material.metallicTexture) {
                        material.metallicTexture.gammaSpace = false;
                        material.useRoughnessFromMetallicTextureAlpha =
                            roughnessChannel === 3;
                        material.useRoughnessFromMetallicTextureGreen =
                            roughnessChannel === 1;
                        material.useMetallnessFromMetallicTextureBlue =
                            metallicChannel === 2;
                        material.useAmbientOcclusionFromMetallicTextureRed =
                            occlusionChannel === 0;
                    }
                }
                if (emissiveTexture !== MISSING_OFFSET) {
                    material.emissiveTexture = textures.get(emissiveTexture) ?? null;
                }
                materials.set(id, material);
                container.materials.push(material);
                break;
            }
            case Command.TransformNode: {
                const id = payload.u32();
                const parentId = payload.u32();
                const nameOffset = payload.u32();
                const nameLength = payload.u32();
                const matrixOffset = payload.u32();
                const node = new TransformNode(
                    stringAt(dataBuffer, nameOffset, nameLength),
                    scene,
                );
                applyMatrix(node, matrixAt(dataBuffer, matrixOffset));
                nodes.set(id, node);
                container.transformNodes.push(node);
                if (parentId === MISSING_OFFSET) {
                    node.parent = root ?? null;
                } else {
                    pendingParents.push({ node, parentId });
                }
                break;
            }
            case Command.Skeleton: {
                const id = payload.u32();
                const nameOffset = payload.u32();
                const nameLength = payload.u32();
                const jointCount = payload.u32();
                const jointsOffset = payload.u32();
                const skeleton = new Skeleton(
                    stringAt(dataBuffer, nameOffset, nameLength),
                    `usd-skeleton-${id}`,
                    scene,
                );
                const jointView = new DataView(dataBuffer);
                assertRange(dataBuffer, jointsOffset, jointCount * 5, 4, "skeleton joints");
                const created: Bone[] = [];
                for (let index = 0; index < jointCount; ++index) {
                    const offset = jointsOffset + index * 20;
                    const parentIndex = jointView.getUint32(offset, true);
                    const boneId = jointView.getUint32(offset + 4, true);
                    const jointNameOffset = jointView.getUint32(offset + 8, true);
                    const jointNameLength = jointView.getUint32(offset + 12, true);
                    const matrixOffset = jointView.getUint32(offset + 16, true);
                    if (parentIndex !== MISSING_OFFSET && parentIndex >= index) {
                        throw new Error(`Skeleton ${id} has an invalid parent joint index.`);
                    }
                    const bone = new Bone(
                        stringAt(dataBuffer, jointNameOffset, jointNameLength),
                        skeleton,
                        parentIndex === MISSING_OFFSET ? null : created[parentIndex],
                        matrixAt(dataBuffer, matrixOffset),
                        matrixAt(dataBuffer, matrixOffset),
                        undefined,
                        index,
                    );
                    created.push(bone);
                    bones.set(boneId, bone);
                }
                skeletons.set(id, skeleton);
                container.skeletons.push(skeleton);
                break;
            }
            case Command.Geometry: {
                const id = payload.u32();
                geometries.set(id, {
                    vertexCount: payload.u32(),
                    indexCount: payload.u32(),
                    flags: payload.u32(),
                    positions: payload.u32(),
                    normals: payload.u32(),
                    tangents: payload.u32(),
                    uv0: payload.u32(),
                    colors: payload.u32(),
                    joints0: payload.u32(),
                    weights0: payload.u32(),
                    joints1: payload.u32(),
                    weights1: payload.u32(),
                    indices: payload.u32(),
                    influences: payload.u32(),
                });
                break;
            }
            case Command.Mesh: {
                const id = payload.u32();
                const nodeId = payload.u32();
                const geometryId = payload.u32();
                const materialId = payload.u32();
                const nameOffset = payload.u32();
                const nameLength = payload.u32();
                const flags = payload.u32();
                const skeletonId = payload.u32();
                const submeshesOffset = payload.u32();
                const submeshCount = payload.u32();
                const descriptor = geometries.get(geometryId);
                if (!descriptor) {
                    throw new Error(`Mesh ${id} references missing geometry ${geometryId}.`);
                }
                const mesh = new Mesh(stringAt(dataBuffer, nameOffset, nameLength), scene);
                const sourceIsRightHanded = !(flags & MeshFlags.LeftHanded);
                mesh.sideOrientation =
                    scene.useRightHandedSystem === sourceIsRightHanded
                        ? Material.CounterClockWiseSideOrientation
                        : Material.ClockWiseSideOrientation;
                mesh.parent = nodes.get(nodeId) ?? root ?? null;
                const vertexData = new VertexData();
                assertRange(
                    dataBuffer,
                    descriptor.positions,
                    descriptor.vertexCount * 3,
                    4,
                    "positions",
                );
                vertexData.positions = new Float32Array(
                    dataBuffer,
                    descriptor.positions,
                    descriptor.vertexCount * 3,
                );
                if (descriptor.flags & GeometryFlags.Normals) {
                    assertRange(
                        dataBuffer,
                        descriptor.normals,
                        descriptor.vertexCount * 3,
                        4,
                        "normals",
                    );
                    vertexData.normals = new Float32Array(
                        dataBuffer,
                        descriptor.normals,
                        descriptor.vertexCount * 3,
                    );
                }
                if (descriptor.flags & GeometryFlags.Tangents) {
                    assertRange(
                        dataBuffer,
                        descriptor.tangents,
                        descriptor.vertexCount * 4,
                        4,
                        "tangents",
                    );
                    vertexData.tangents = new Float32Array(
                        dataBuffer,
                        descriptor.tangents,
                        descriptor.vertexCount * 4,
                    );
                }
                if (descriptor.flags & GeometryFlags.Uv0) {
                    assertRange(
                        dataBuffer,
                        descriptor.uv0,
                        descriptor.vertexCount * 2,
                        4,
                        "texture coordinates",
                    );
                    vertexData.uvs = new Float32Array(
                        dataBuffer,
                        descriptor.uv0,
                        descriptor.vertexCount * 2,
                    );
                }
                if (descriptor.flags & GeometryFlags.Colors) {
                    assertRange(
                        dataBuffer,
                        descriptor.colors,
                        descriptor.vertexCount * 4,
                        4,
                        "vertex colors",
                    );
                    vertexData.colors = new Float32Array(
                        dataBuffer,
                        descriptor.colors,
                        descriptor.vertexCount * 4,
                    );
                }
                if (descriptor.flags & GeometryFlags.Skin0) {
                    assertRange(
                        dataBuffer,
                        descriptor.joints0,
                        descriptor.vertexCount * 4,
                        2,
                        "joint indices",
                    );
                    assertRange(
                        dataBuffer,
                        descriptor.weights0,
                        descriptor.vertexCount * 4,
                        4,
                        "joint weights",
                    );
                    vertexData.matricesIndices = Float32Array.from(
                        new Uint16Array(
                            dataBuffer,
                            descriptor.joints0,
                            descriptor.vertexCount * 4,
                        ),
                    );
                    vertexData.matricesWeights = new Float32Array(
                        dataBuffer,
                        descriptor.weights0,
                        descriptor.vertexCount * 4,
                    );
                }
                if (descriptor.flags & GeometryFlags.Skin1) {
                    assertRange(
                        dataBuffer,
                        descriptor.joints1,
                        descriptor.vertexCount * 4,
                        2,
                        "extra joint indices",
                    );
                    assertRange(
                        dataBuffer,
                        descriptor.weights1,
                        descriptor.vertexCount * 4,
                        4,
                        "extra joint weights",
                    );
                    vertexData.matricesIndicesExtra = Float32Array.from(
                        new Uint16Array(
                            dataBuffer,
                            descriptor.joints1,
                            descriptor.vertexCount * 4,
                        ),
                    );
                    vertexData.matricesWeightsExtra = new Float32Array(
                        dataBuffer,
                        descriptor.weights1,
                        descriptor.vertexCount * 4,
                    );
                }
                assertRange(dataBuffer, descriptor.indices, descriptor.indexCount, 4, "indices");
                vertexData.indices = new Uint32Array(
                    dataBuffer,
                    descriptor.indices,
                    descriptor.indexCount,
                );
                vertexData.applyToMesh(mesh, true);
                mesh.numBoneInfluencers = Math.min(descriptor.influences, 8);
                if (skeletonId !== MISSING_OFFSET) {
                    mesh.skeleton = skeletons.get(skeletonId) ?? null;
                }

                const submeshView = new DataView(dataBuffer);
                assertRange(dataBuffer, submeshesOffset, submeshCount * 5, 4, "submeshes");
                const doubleSided = Boolean(flags & MeshFlags.DoubleSided);
                const materialForMesh = (id: number): PBRMaterial | null => {
                    const material = materials.get(id);
                    if (!material || !doubleSided || !material.backFaceCulling) {
                        return material ?? null;
                    }
                    let variant = doubleSidedMaterials.get(id);
                    if (!variant) {
                        variant = material.clone(`${material.name} (double-sided)`);
                        variant.backFaceCulling = false;
                        variant.twoSidedLighting = true;
                        doubleSidedMaterials.set(id, variant);
                        container.materials.push(variant);
                    }
                    return variant;
                };
                if (submeshCount === 1 && materialId !== MISSING_OFFSET) {
                    mesh.material = materialForMesh(materialId);
                } else {
                    const multi = new MultiMaterial(`${mesh.name} materials`, scene);
                    for (let index = 0; index < submeshCount; ++index) {
                        const offset = submeshesOffset + index * 20;
                        multi.subMaterials.push(
                            materialForMesh(submeshView.getUint32(offset, true)),
                        );
                    }
                    mesh.material = multi;
                    container.multiMaterials.push(multi);
                }
                mesh.releaseSubMeshes();
                for (let index = 0; index < submeshCount; ++index) {
                    const offset = submeshesOffset + index * 20;
                    new SubMesh(
                        index,
                        submeshView.getUint32(offset + 12, true),
                        submeshView.getUint32(offset + 16, true),
                        submeshView.getUint32(offset + 4, true),
                        submeshView.getUint32(offset + 8, true),
                        mesh,
                    );
                }
                meshes.set(id, mesh);
                container.meshes.push(mesh);
                if (mesh.geometry) {
                    container.geometries.push(mesh.geometry);
                }
                break;
            }
            case Command.Instance: {
                const sourceId = payload.u32();
                const nodeId = payload.u32();
                const nameOffset = payload.u32();
                const nameLength = payload.u32();
                const source = meshes.get(sourceId);
                if (!source) {
                    throw new Error(`Instance references missing mesh ${sourceId}.`);
                }
                const instance = source.createInstance(
                    stringAt(dataBuffer, nameOffset, nameLength),
                );
                instance.parent = nodes.get(nodeId) ?? root ?? null;
                container.meshes.push(instance);
                break;
            }
            case Command.Animation: {
                const targetKind = payload.u32();
                const targetId = payload.u32();
                const property = payload.u32();
                const trackIndex = payload.u32();
                const keyCount = payload.u32();
                const timesOffset = payload.u32();
                const valuesOffset = payload.u32();
                const stride = payload.u32();
                if (
                    targetKind !== AnimationTarget.Node &&
                    targetKind !== AnimationTarget.Bone
                ) {
                    throw new Error(`Invalid animation target kind ${targetKind}.`);
                }
                const expectedStride =
                    property === AnimationProperty.Position ||
                    property === AnimationProperty.Scaling
                        ? 3
                        : property === AnimationProperty.RotationQuaternion
                          ? 4
                          : property === AnimationProperty.Matrix
                            ? 16
                            : 0;
                if (stride !== expectedStride) {
                    throw new Error(
                        `Invalid animation value stride ${stride} for property ${property}.`,
                    );
                }
                const target =
                    targetKind === AnimationTarget.Node
                        ? nodes.get(targetId)
                        : bones.get(targetId);
                if (!target) {
                    break;
                }
                const propertyName =
                    property === AnimationProperty.Position
                        ? "position"
                        : property === AnimationProperty.RotationQuaternion
                          ? "rotationQuaternion"
                          : property === AnimationProperty.Scaling
                            ? "scaling"
                            : "_matrix";
                const dataType =
                    property === AnimationProperty.RotationQuaternion
                        ? Animation.ANIMATIONTYPE_QUATERNION
                        : property === AnimationProperty.Matrix
                          ? Animation.ANIMATIONTYPE_MATRIX
                          : Animation.ANIMATIONTYPE_VECTOR3;
                const animation = new Animation(
                    `USD ${propertyName}`,
                    propertyName,
                    timeCodesPerSecond,
                    dataType,
                    Animation.ANIMATIONLOOPMODE_CYCLE,
                );
                assertRange(dataBuffer, timesOffset, keyCount, 4, "animation times");
                assertRange(
                    dataBuffer,
                    valuesOffset,
                    keyCount * stride,
                    4,
                    "animation values",
                );
                const times = new Float32Array(dataBuffer, timesOffset, keyCount);
                const values = new Float32Array(dataBuffer, valuesOffset, keyCount * stride);
                animation.setKeys(
                    Array.from({ length: keyCount }, (_, index) => ({
                        frame: times[index],
                        value:
                            dataType === Animation.ANIMATIONTYPE_QUATERNION
                                ? Quaternion.FromArray(values, index * stride)
                                : dataType === Animation.ANIMATIONTYPE_MATRIX
                                  ? Matrix.FromArray(values, index * stride)
                                  : Vector3.FromArray(values, index * stride),
                    })),
                );
                let group = animationGroups.get(trackIndex);
                if (!group) {
                    group = new AnimationGroup(`USD Animation ${trackIndex + 1}`, scene);
                    animationGroups.set(trackIndex, group);
                    container.animationGroups.push(group);
                }
                group.addTargetedAnimation(animation, target);
                break;
            }
        }
    }

    await Promise.all(textureLoads);
    for (const { node, parentId } of pendingParents) {
        node.parent = nodes.get(parentId) ?? root ?? null;
    }
    if (!addToScene) {
        container.removeAllFromScene();
    }
    return { container, materializeMs: performance.now() - started };
    } catch (error) {
        container.dispose();
        throw error;
    }
}
