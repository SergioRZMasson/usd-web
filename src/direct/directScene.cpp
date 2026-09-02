#include "directScene.h"

#include "commandProtocol.h"

#include <fileformatutils/common.h>
#include <fileformatutils/featureFlags.h>
#include <fileformatutils/geometry.h>
#include <fileformatutils/materials.h>
#include <fileformatutils/naming.h>
#include <meshOptimization.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdSkel/utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace PXR_NS;
using namespace adobe::usd;

namespace usd_web::direct {
namespace {

class BufferWriter
{
public:
    uint32_t size() const { return static_cast<uint32_t>(bytes.size()); }

    void align(uint32_t alignment = 4)
    {
        while (bytes.size() % alignment != 0) {
            bytes.push_back(0);
        }
    }

    void u16(uint16_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }

    void u32(uint32_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        bytes.push_back(static_cast<uint8_t>(value >> 24));
    }

    void f32(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void patchU16(uint32_t offset, uint16_t value)
    {
        bytes[offset] = static_cast<uint8_t>(value);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
    }

    void patchU32(uint32_t offset, uint32_t value)
    {
        bytes[offset] = static_cast<uint8_t>(value);
        bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<uint8_t>(value >> 16);
        bytes[offset + 3] = static_cast<uint8_t>(value >> 24);
    }

    uint32_t appendBytes(const void* source, size_t byteCount, uint32_t alignment = 4)
    {
        align(alignment);
        const uint32_t offset = size();
        const auto* first = static_cast<const uint8_t*>(source);
        bytes.insert(bytes.end(), first, first + byteCount);
        return offset;
    }

    uint32_t appendString(const std::string& value)
    {
        return appendBytes(value.data(), value.size(), 1);
    }

    std::vector<uint8_t> bytes;
};

class CommandWriter
{
public:
    CommandWriter()
    {
        buffer.u32(kCommandMagic);
        buffer.u16(kProtocolVersion);
        buffer.u16(0);
        buffer.u32(0);
        buffer.u32(0);
    }

    uint32_t begin(Command command, uint16_t flags = 0)
    {
        buffer.u16(static_cast<uint16_t>(command));
        buffer.u16(flags);
        const uint32_t lengthOffset = buffer.size();
        buffer.u32(0);
        return lengthOffset;
    }

    void end(uint32_t lengthOffset)
    {
        const uint32_t payloadStart = lengthOffset + 4;
        buffer.patchU32(lengthOffset, buffer.size() - payloadStart);
        ++commandCount;
    }

    std::vector<uint8_t> finish()
    {
        buffer.patchU32(8, commandCount);
        return std::move(buffer.bytes);
    }

    BufferWriter buffer;
    uint32_t commandCount = 0;
};

using MeshGroupKey = std::pair<int, int>;
using MeshGroups = std::map<MeshGroupKey, std::vector<int>>;

std::string
nameOr(const std::string& displayName, const std::string& name, const std::string& fallback)
{
    if (!displayName.empty()) {
        return displayName;
    }
    if (!name.empty()) {
        return name;
    }
    return fallback;
}

void
completeMaterialSubsets(Mesh& mesh)
{
    if (mesh.subsets.empty() || mesh.faces.empty() || mesh.indices.size() != mesh.faces.size() * 3) {
        return;
    }
    std::vector<int> assignment(mesh.faces.size(), -1);
    for (size_t subsetIndex = 0; subsetIndex < mesh.subsets.size(); ++subsetIndex) {
        for (const int face : mesh.subsets[subsetIndex].faces) {
            if (face >= 0 && static_cast<size_t>(face) < assignment.size() &&
                assignment[face] < 0) {
                assignment[face] = static_cast<int>(subsetIndex);
            }
        }
    }

    std::vector<Subset> partition;
    auto appendSubset = [&](int assigned, int material) {
        Subset subset;
        subset.material = material;
        for (size_t face = 0; face < assignment.size(); ++face) {
            if (assignment[face] != assigned) {
                continue;
            }
            subset.faces.push_back(static_cast<int>(face));
            subset.indices.push_back(mesh.indices[face * 3]);
            subset.indices.push_back(mesh.indices[face * 3 + 1]);
            subset.indices.push_back(mesh.indices[face * 3 + 2]);
        }
        if (!subset.faces.empty()) {
            partition.push_back(std::move(subset));
        }
    };
    appendSubset(-1, mesh.material);
    for (size_t subsetIndex = 0; subsetIndex < mesh.subsets.size(); ++subsetIndex) {
        appendSubset(static_cast<int>(subsetIndex), mesh.subsets[subsetIndex].material);
    }
    mesh.subsets = std::move(partition);
}

MeshGroups
collectMeshGroups(const UsdData& usd)
{
    MeshGroups groups;
    for (size_t nodeIndex = 0; nodeIndex < usd.nodes.size(); ++nodeIndex) {
        const Node& node = usd.nodes[nodeIndex];
        for (const int meshIndex : node.staticMeshes) {
            groups[{ meshIndex, -1 }].push_back(static_cast<int>(nodeIndex));
        }
        for (const auto& [skeletonIndex, meshIndices] : node.skinnedMeshes) {
            for (const int meshIndex : meshIndices) {
                groups[{ meshIndex, skeletonIndex }].push_back(static_cast<int>(nodeIndex));
            }
        }
    }
    return groups;
}

std::vector<bool>
findDoubleSidedMaterials(const UsdData& usd, size_t materialCount)
{
    std::vector<bool> result(materialCount, false);
    for (const Mesh& mesh : usd.meshes) {
        if (!mesh.doubleSided) {
            continue;
        }
        if (mesh.material >= 0 && static_cast<size_t>(mesh.material) < result.size()) {
            result[mesh.material] = true;
        }
        for (const Subset& subset : mesh.subsets) {
            if (subset.material >= 0 && static_cast<size_t>(subset.material) < result.size()) {
                result[subset.material] = true;
            }
        }
    }
    return result;
}

uint32_t
appendString(BufferWriter& data, const std::string& value, uint32_t& length)
{
    length = static_cast<uint32_t>(value.size());
    return data.appendString(value);
}

uint32_t
appendVec3(BufferWriter& data, const VtVec3fArray& values)
{
    data.align();
    const uint32_t offset = data.size();
    for (const GfVec3f& value : values) {
        data.f32(value[0]);
        data.f32(value[1]);
        data.f32(value[2]);
    }
    return offset;
}

uint32_t
appendVec4(BufferWriter& data, const VtVec4fArray& values)
{
    data.align();
    const uint32_t offset = data.size();
    for (const GfVec4f& value : values) {
        for (int component = 0; component < 4; ++component) {
            data.f32(value[component]);
        }
    }
    return offset;
}

uint32_t
appendUv(BufferWriter& data, const VtVec2fArray& values)
{
    data.align();
    const uint32_t offset = data.size();
    for (const GfVec2f& value : values) {
        data.f32(value[0]);
        data.f32(1.0f - value[1]);
    }
    return offset;
}

uint32_t
appendMatrix(BufferWriter& data, const GfMatrix4d& matrix)
{
    data.align();
    const uint32_t offset = data.size();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            data.f32(static_cast<float>(matrix[row][column]));
        }
    }
    return offset;
}

GfMatrix4d
nodeMatrix(const Node& node)
{
    if (node.hasTransform) {
        return node.transform;
    }
    GfMatrix4d matrix;
    UsdSkelMakeTransform(
      GfVec3f(node.translation), node.rotation, GfVec3h(node.scale), &matrix);
    return matrix;
}

uint32_t
appendIndices(BufferWriter& data, const Mesh& mesh)
{
    data.align();
    const uint32_t offset = data.size();
    const auto append = [&](const VtIntArray& indices) {
        for (const int index : indices) {
            data.u32(static_cast<uint32_t>(index));
        }
    };
    if (mesh.subsets.empty()) {
        append(mesh.indices);
    } else {
        for (const Subset& subset : mesh.subsets) {
            append(subset.indices);
        }
    }
    return offset;
}

uint32_t
indexCount(const Mesh& mesh)
{
    if (mesh.subsets.empty()) {
        return static_cast<uint32_t>(mesh.indices.size());
    }
    size_t count = 0;
    for (const Subset& subset : mesh.subsets) {
        count += subset.indices.size();
    }
    return static_cast<uint32_t>(count);
}

uint32_t
appendColors(BufferWriter& data, const Mesh& mesh)
{
    const size_t count = mesh.points.size();
    const bool hasColor = !mesh.colors.empty() && mesh.colors[0].values.size() == count;
    const bool hasOpacity = !mesh.opacities.empty() && mesh.opacities[0].values.size() == count;
    if (!hasColor && !hasOpacity) {
        return kMissingOffset;
    }
    data.align();
    const uint32_t offset = data.size();
    for (size_t index = 0; index < count; ++index) {
        const GfVec3f color = hasColor ? mesh.colors[0].values[index] : GfVec3f(1.0f);
        data.f32(color[0]);
        data.f32(color[1]);
        data.f32(color[2]);
        data.f32(hasOpacity ? mesh.opacities[0].values[index] : 1.0f);
    }
    return offset;
}

uint32_t
appendJointSet(BufferWriter& data, const Mesh& mesh, size_t set)
{
    if (mesh.influenceCount <= static_cast<int>(set * 4) || mesh.joints.empty()) {
        return kMissingOffset;
    }
    data.align();
    const uint32_t offset = data.size();
    for (size_t vertex = 0; vertex < mesh.points.size(); ++vertex) {
        for (size_t component = 0; component < 4; ++component) {
            const size_t influence = set * 4 + component;
            const size_t source = vertex * static_cast<size_t>(mesh.influenceCount) + influence;
            const uint16_t value =
              influence < static_cast<size_t>(mesh.influenceCount) && source < mesh.joints.size()
                ? static_cast<uint16_t>(mesh.joints[source])
                : 0;
            data.u16(value);
        }
    }
    return offset;
}

uint32_t
appendWeightSet(BufferWriter& data, const Mesh& mesh, size_t set)
{
    if (mesh.influenceCount <= static_cast<int>(set * 4) || mesh.weights.empty()) {
        return kMissingOffset;
    }
    data.align();
    const uint32_t offset = data.size();
    for (size_t vertex = 0; vertex < mesh.points.size(); ++vertex) {
        for (size_t component = 0; component < 4; ++component) {
            const size_t influence = set * 4 + component;
            const size_t source = vertex * static_cast<size_t>(mesh.influenceCount) + influence;
            data.f32(
              influence < static_cast<size_t>(mesh.influenceCount) && source < mesh.weights.size()
                ? mesh.weights[source]
                : 0.0f);
        }
    }
    return offset;
}

VtVec4fArray
buildTangents(const Mesh& mesh)
{
    if (mesh.tangents.values.empty()) {
        return {};
    }
    if (mesh.tangents.values.size() != mesh.normals.values.size() ||
        mesh.tangents.values.size() != mesh.bitangents.values.size()) {
        return mesh.tangents.values;
    }
    VtVec4fArray tangents(mesh.tangents.values.size());
    for (size_t index = 0; index < tangents.size(); ++index) {
        const GfVec4f tangent = mesh.tangents.values[index];
        const GfVec3f direction(tangent[0], tangent[1], tangent[2]);
        const float sign =
          GfDot(GfCross(mesh.normals.values[index], direction), mesh.bitangents.values[index]) >=
              0.0f
            ? 1.0f
            : -1.0f;
        tangents[index] = GfVec4f(direction[0], direction[1], direction[2], sign);
    }
    return tangents;
}

template<typename T>
bool
getValue(const Input& input, T& value)
{
    if (!input.value.IsHolding<T>()) {
        return false;
    }
    value = input.value.UncheckedGet<T>();
    return true;
}

float
floatValue(const Input& input, float fallback)
{
    float value = fallback;
    return getValue(input, value) ? value : fallback;
}

GfVec3f
colorValue(const Input& input, const GfVec3f& fallback)
{
    GfVec3f value;
    if (getValue(input, value)) {
        return value;
    }
    if (input.value.IsHolding<GfVec4f>()) {
        const GfVec4f rgba = input.value.UncheckedGet<GfVec4f>();
        return GfVec3f(rgba[0], rgba[1], rgba[2]);
    }
    return fallback;
}

uint32_t
imageMime(ImageFormat format)
{
    switch (format) {
        case ImageFormatJpg:
            return 2;
        case ImageFormatBmp:
            return 3;
        case ImageFormatWebp:
            return 4;
        default:
            return 1;
    }
}

int32_t
emitTexture(CommandWriter& commands,
            BufferWriter& data,
            const std::vector<ImageAsset>& images,
            const Input& input,
            uint32_t textureId,
            std::vector<uint32_t>& imageOffsets,
            std::vector<uint32_t>& imageLengths)
{
    if (input.image < 0 || static_cast<size_t>(input.image) >= images.size()) {
        return -1;
    }
    const ImageAsset& image = images[input.image];
    if (image.image.empty()) {
        return -1;
    }
    if (imageOffsets[input.image] == kMissingOffset) {
        imageOffsets[input.image] =
          data.appendBytes(image.image.data(), image.image.size(), 4);
        imageLengths[input.image] = static_cast<uint32_t>(image.image.size());
    }
    uint32_t nameLength = 0;
    const uint32_t nameOffset =
      appendString(data, nameOr("", image.name, image.uri), nameLength);
    data.align();
    const uint32_t transformOffset = data.size();
    data.f32(input.uvScale[0]);
    data.f32(input.uvScale[1]);
    data.f32(input.uvTranslation[0]);
    data.f32(input.uvTranslation[1]);
    data.f32(input.uvRotation);

    const uint32_t record = commands.begin(Command::Texture);
    commands.buffer.u32(textureId);
    commands.buffer.u32(nameOffset);
    commands.buffer.u32(nameLength);
    commands.buffer.u32(imageMime(image.format));
    commands.buffer.u32(imageOffsets[input.image]);
    commands.buffer.u32(imageLengths[input.image]);
    commands.buffer.u32(static_cast<uint32_t>(std::max(input.uvIndex, 0)));
    commands.buffer.u32(transformOffset);
    const auto wrapMode = [](const TfToken& mode) {
        if (mode == AdobeTokens->mirror) {
            return 2u;
        }
        return mode == AdobeTokens->repeat ? 1u : 0u;
    };
    commands.buffer.u32(wrapMode(input.wrapS));
    commands.buffer.u32(wrapMode(input.wrapT));
    commands.end(record);
    return static_cast<int32_t>(textureId);
}

void
emitMaterials(CommandWriter& commands, BufferWriter& data, UsdData& usd)
{
    const bool useOpenPbr = isNativeOpenPbrProcessingEnabled();
    const size_t materialCount =
      useOpenPbr ? usd.openPbrMaterials.size() : usd.materials.size();
    const std::vector<bool> doubleSided =
      findDoubleSidedMaterials(usd, materialCount);
    InputTranslator translator(true, usd.images, "directBabylon");
    std::vector<uint32_t> imageOffsets;
    std::vector<uint32_t> imageLengths;
    uint32_t nextTextureId = 1;

    auto emitMaterial = [&](uint32_t materialId,
                            const std::string& name,
                            const Input& baseColor,
                            const Input& emissive,
                            const Input& metallic,
                            const Input& roughness,
                            const Input& opacity,
                            const Input& normal,
                            const Input& occlusion,
                            float normalScale,
                            float alphaCutoff,
                            bool unlit,
                            bool isDoubleSided) {
        Input packedBaseColor;
        if (opacity.image >= 0 || !opacity.value.IsEmpty()) {
            translator.translateMix("directBaseColor",
                                    AdobeTokens->sRGB,
                                    translator.split3f(baseColor, 0),
                                    translator.split3f(baseColor, 1),
                                    translator.split3f(baseColor, 2),
                                    opacity,
                                    packedBaseColor);
        } else {
            translator.translateDirect(baseColor, packedBaseColor);
        }

        Input packedOrm;
        if (metallic.image >= 0 || roughness.image >= 0) {
            Input one;
            one.value = 1.0f;
            translator.translateMix("directOrm",
                                    AdobeTokens->raw,
                                    occlusion,
                                    roughness,
                                    metallic,
                                    one,
                                    packedOrm);
        }

        Input packedNormal;
        Input packedEmissive;
        translator.translateDirect(normal, packedNormal);
        translator.translateDirect(emissive, packedEmissive);

        const std::vector<ImageAsset>& images = translator.getImages();
        imageOffsets.resize(images.size(), kMissingOffset);
        imageLengths.resize(images.size(), 0);
        const int32_t baseTexture = emitTexture(
          commands,
          data,
          images,
          packedBaseColor,
          nextTextureId,
          imageOffsets,
          imageLengths);
        if (baseTexture >= 0) {
            ++nextTextureId;
        }
        const int32_t normalTexture = emitTexture(
          commands,
          data,
          images,
          packedNormal,
          nextTextureId,
          imageOffsets,
          imageLengths);
        if (normalTexture >= 0) {
            ++nextTextureId;
        }
        const int32_t ormTexture = emitTexture(
          commands, data, images, packedOrm, nextTextureId, imageOffsets, imageLengths);
        if (ormTexture >= 0) {
            ++nextTextureId;
        }
        const int32_t emissiveTexture = emitTexture(
          commands,
          data,
          images,
          packedEmissive,
          nextTextureId,
          imageOffsets,
          imageLengths);
        if (emissiveTexture >= 0) {
            ++nextTextureId;
        }

        const GfVec3f base = colorValue(baseColor, GfVec3f(1.0f));
        const GfVec3f emission = colorValue(emissive, GfVec3f(0.0f));
        data.align();
        const uint32_t baseOffset = data.size();
        data.f32(base[0]);
        data.f32(base[1]);
        data.f32(base[2]);
        data.f32(floatValue(opacity, 1.0f));
        const uint32_t emissiveOffset = data.size();
        data.f32(emission[0]);
        data.f32(emission[1]);
        data.f32(emission[2]);
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, name, nameLength);

        uint32_t flags = unlit ? MaterialUnlit : 0;
        flags |= isDoubleSided ? MaterialDoubleSided : 0;
        if (floatValue(opacity, 1.0f) < 0.999f || opacity.image >= 0) {
            flags |= MaterialAlphaBlend;
        }
        const uint32_t record = commands.begin(Command::Material);
        commands.buffer.u32(materialId);
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(baseOffset);
        commands.buffer.u32(emissiveOffset);
        commands.buffer.f32(floatValue(metallic, 0.0f));
        commands.buffer.f32(floatValue(roughness, 0.5f));
        commands.buffer.f32(normalScale);
        commands.buffer.f32(alphaCutoff);
        commands.buffer.u32(flags);
        commands.buffer.u32(static_cast<uint32_t>(baseTexture));
        commands.buffer.u32(static_cast<uint32_t>(normalTexture));
        commands.buffer.u32(static_cast<uint32_t>(ormTexture));
        commands.buffer.u32(static_cast<uint32_t>(emissiveTexture));
        commands.end(record);
    };

    Input empty;
    emitMaterial(0,
                 "Default USD material",
                 empty,
                 empty,
                 empty,
                 empty,
                 empty,
                 empty,
                 empty,
                 1.0f,
                 0.0f,
                 false,
                 true);
    if (useOpenPbr) {
        for (size_t index = 0; index < usd.openPbrMaterials.size(); ++index) {
            const OpenPbrMaterial& material = usd.openPbrMaterials[index];
            emitMaterial(static_cast<uint32_t>(index + 1),
                         nameOr(material.displayName, material.name, "Material"),
                         material.base_color,
                         material.emission_color,
                         material.base_metalness,
                         material.specular_roughness,
                         material.geometry_opacity,
                         material.geometry_normal,
                         material.occlusion,
                         material.normalScale,
                         material.opacityThreshold,
                         material.isUnlit,
                         doubleSided[index]);
        }
    } else {
        for (size_t index = 0; index < usd.materials.size(); ++index) {
            const Material& material = usd.materials[index];
            emitMaterial(static_cast<uint32_t>(index + 1),
                         nameOr(material.displayName, material.name, "Material"),
                         material.diffuseColor,
                         material.emissiveColor,
                         material.metallic,
                         material.roughness,
                         material.opacity,
                         material.normal,
                         material.occlusion,
                         floatValue(material.normalScale, 1.0f),
                         floatValue(material.opacityThreshold, 0.0f),
                         material.isUnlit,
                         doubleSided[index]);
        }
    }
}

uint32_t
materialId(int index)
{
    return index >= 0 ? static_cast<uint32_t>(index + 1) : 0;
}

} // namespace

bool
buildSceneBuffers(UsdData& usd, SceneBuffers& result)
{
    const auto preparationStarted = std::chrono::steady_clock::now();
    for (Mesh& mesh : usd.meshes) {
        if (mesh.points.empty()) {
            continue;
        }
        completeMaterialSubsets(mesh);
        transformMesh(mesh, mesh.geomBindTransform);
        optimizeMeshForGltf(mesh);
    }
    const MeshGroups groups = collectMeshGroups(usd);
    const auto preparationFinished = std::chrono::steady_clock::now();

    CommandWriter commands;
    BufferWriter data;

    uint32_t sceneRecord = commands.begin(Command::Scene);
    commands.buffer.u32(usd.upAxis == UsdGeomTokens->z ? 1 : 0);
    commands.buffer.f32(
      usd.metersPerUnit > 0.0 ? static_cast<float>(usd.metersPerUnit) : 1.0f);
    commands.buffer.f32(static_cast<float>(usd.timeCodesPerSecond));
    commands.end(sceneRecord);

    emitMaterials(commands, data, usd);

    for (size_t nodeIndex = 0; nodeIndex < usd.nodes.size(); ++nodeIndex) {
        const Node& node = usd.nodes[nodeIndex];
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(
          data, nameOr(node.displayName, node.name, "Node"), nameLength);
        const uint32_t matrixOffset = appendMatrix(data, nodeMatrix(node));
        const uint32_t record = commands.begin(Command::TransformNode);
        commands.buffer.u32(static_cast<uint32_t>(nodeIndex + 1));
        commands.buffer.u32(
          node.parent >= 0 ? static_cast<uint32_t>(node.parent + 1) : kMissingOffset);
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(matrixOffset);
        commands.end(record);
    }

    std::vector<std::vector<uint32_t>> boneIds(usd.skeletons.size());
    uint32_t nextBoneId = 1;
    for (size_t skeletonIndex = 0; skeletonIndex < usd.skeletons.size(); ++skeletonIndex) {
        const Skeleton& skeleton = usd.skeletons[skeletonIndex];
        boneIds[skeletonIndex].resize(skeleton.joints.size());
        std::vector<std::array<uint32_t, 5>> jointRecords;
        jointRecords.reserve(skeleton.joints.size());
        for (size_t jointIndex = 0; jointIndex < skeleton.joints.size(); ++jointIndex) {
            const uint32_t boneId = nextBoneId++;
            boneIds[skeletonIndex][jointIndex] = boneId;
            const std::string jointName =
              jointIndex < skeleton.jointNames.size()
                ? skeleton.jointNames[jointIndex].GetString()
                : skeleton.joints[jointIndex].GetString();
            uint32_t jointNameLength = 0;
            const uint32_t jointNameOffset =
              appendString(data, jointName, jointNameLength);
            const uint32_t matrixOffset =
              appendMatrix(data,
                           jointIndex < skeleton.restTransforms.size()
                             ? skeleton.restTransforms[jointIndex]
                             : GfMatrix4d(1.0));
            jointRecords.push_back({
                jointIndex < skeleton.jointParents.size() &&
                    skeleton.jointParents[jointIndex] >= 0
                  ? static_cast<uint32_t>(skeleton.jointParents[jointIndex])
                  : kMissingOffset,
                boneId,
                jointNameOffset,
                jointNameLength,
                matrixOffset,
            });
        }
        data.align();
        const uint32_t jointsOffset = data.size();
        for (const auto& joint : jointRecords) {
            for (const uint32_t value : joint) {
                data.u32(value);
            }
        }
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(
          data, nameOr(skeleton.displayName, skeleton.name, "Skeleton"), nameLength);
        const uint32_t record = commands.begin(Command::Skeleton);
        commands.buffer.u32(static_cast<uint32_t>(skeletonIndex + 1));
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(static_cast<uint32_t>(skeleton.joints.size()));
        commands.buffer.u32(jointsOffset);
        commands.end(record);
    }

    std::vector<bool> geometryWritten(usd.meshes.size(), false);
    for (const auto& [key, placements] : groups) {
        const int meshIndex = key.first;
        if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= usd.meshes.size() ||
            geometryWritten[meshIndex]) {
            continue;
        }
        geometryWritten[meshIndex] = true;
        const Mesh& mesh = usd.meshes[meshIndex];
        if (mesh.points.empty()) {
            continue;
        }

        const uint32_t positionsOffset = appendVec3(data, mesh.points);
        const uint32_t normalsOffset =
          mesh.normals.values.size() == mesh.points.size()
            ? appendVec3(data, mesh.normals.values)
            : kMissingOffset;
        const VtVec4fArray tangents = buildTangents(mesh);
        const uint32_t tangentsOffset =
          tangents.size() == mesh.points.size() ? appendVec4(data, tangents) : kMissingOffset;
        const uint32_t uvOffset =
          mesh.uvs.values.size() == mesh.points.size()
            ? appendUv(data, mesh.uvs.values)
            : kMissingOffset;
        const uint32_t colorsOffset = appendColors(data, mesh);
        const uint32_t joints0Offset = appendJointSet(data, mesh, 0);
        const uint32_t weights0Offset = appendWeightSet(data, mesh, 0);
        const uint32_t joints1Offset = appendJointSet(data, mesh, 1);
        const uint32_t weights1Offset = appendWeightSet(data, mesh, 1);
        const uint32_t indicesOffset = appendIndices(data, mesh);
        uint32_t flags = 0;
        flags |= normalsOffset != kMissingOffset ? GeometryHasNormals : 0;
        flags |= tangentsOffset != kMissingOffset ? GeometryHasTangents : 0;
        flags |= uvOffset != kMissingOffset ? GeometryHasUv0 : 0;
        flags |= colorsOffset != kMissingOffset ? GeometryHasColors : 0;
        flags |= joints0Offset != kMissingOffset ? GeometryHasSkin0 : 0;
        flags |= joints1Offset != kMissingOffset ? GeometryHasSkin1 : 0;

        const uint32_t record = commands.begin(Command::Geometry);
        commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
        commands.buffer.u32(static_cast<uint32_t>(mesh.points.size()));
        commands.buffer.u32(indexCount(mesh));
        commands.buffer.u32(flags);
        commands.buffer.u32(positionsOffset);
        commands.buffer.u32(normalsOffset);
        commands.buffer.u32(tangentsOffset);
        commands.buffer.u32(uvOffset);
        commands.buffer.u32(colorsOffset);
        commands.buffer.u32(joints0Offset);
        commands.buffer.u32(weights0Offset);
        commands.buffer.u32(joints1Offset);
        commands.buffer.u32(weights1Offset);
        commands.buffer.u32(indicesOffset);
        commands.buffer.u32(static_cast<uint32_t>(std::max(mesh.influenceCount, 0)));
        commands.end(record);
        result.vertexCount += mesh.points.size();
        result.triangleCount += indexCount(mesh) / 3;
    }

    uint32_t nextMeshId = 1;
    for (const auto& [key, placements] : groups) {
        if (key.first < 0 || static_cast<size_t>(key.first) >= usd.meshes.size() ||
            placements.empty()) {
            continue;
        }
        const Mesh& mesh = usd.meshes[key.first];
        if (mesh.points.empty()) {
            continue;
        }
        data.align();
        const uint32_t submeshesOffset = data.size();
        uint32_t indexStart = 0;
        if (mesh.subsets.empty()) {
            data.u32(materialId(mesh.material));
            data.u32(0);
            data.u32(static_cast<uint32_t>(mesh.indices.size()));
            data.u32(0);
            data.u32(static_cast<uint32_t>(mesh.points.size()));
        } else {
            for (const Subset& subset : mesh.subsets) {
                data.u32(materialId(subset.material));
                data.u32(indexStart);
                data.u32(static_cast<uint32_t>(subset.indices.size()));
                data.u32(0);
                data.u32(static_cast<uint32_t>(mesh.points.size()));
                indexStart += static_cast<uint32_t>(subset.indices.size());
            }
        }
        uint32_t nameLength = 0;
        const uint32_t nameOffset =
          appendString(data, nameOr(mesh.displayName, mesh.name, "Mesh"), nameLength);
        const uint32_t meshId = nextMeshId++;
        const uint32_t record = commands.begin(Command::Mesh);
        commands.buffer.u32(meshId);
        commands.buffer.u32(static_cast<uint32_t>(placements.front() + 1));
        commands.buffer.u32(static_cast<uint32_t>(key.first + 1));
        commands.buffer.u32(
          mesh.subsets.empty() ? materialId(mesh.material) : kMissingOffset);
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(mesh.doubleSided ? 1 : 0);
        commands.buffer.u32(key.second >= 0 ? static_cast<uint32_t>(key.second + 1)
                                            : kMissingOffset);
        commands.buffer.u32(submeshesOffset);
        commands.buffer.u32(
          static_cast<uint32_t>(mesh.subsets.empty() ? 1 : mesh.subsets.size()));
        commands.end(record);
        ++result.meshCount;

        for (size_t placement = 1; placement < placements.size(); ++placement) {
            const std::string instanceName =
              nameOr(mesh.displayName, mesh.name, "Mesh") + " instance";
            uint32_t instanceNameLength = 0;
            const uint32_t instanceNameOffset =
              appendString(data, instanceName, instanceNameLength);
            const uint32_t instanceRecord = commands.begin(Command::Instance);
            commands.buffer.u32(meshId);
            commands.buffer.u32(static_cast<uint32_t>(placements[placement] + 1));
            commands.buffer.u32(instanceNameOffset);
            commands.buffer.u32(instanceNameLength);
            commands.end(instanceRecord);
            ++result.instanceCount;
        }
    }

    for (size_t trackIndex = 0; trackIndex < usd.animationTracks.size(); ++trackIndex) {
        for (size_t nodeIndex = 0; nodeIndex < usd.nodes.size(); ++nodeIndex) {
            if (trackIndex >= usd.nodes[nodeIndex].animations.size()) {
                continue;
            }
            const NodeAnimation& animation = usd.nodes[nodeIndex].animations[trackIndex];
            auto emit = [&](AnimationProperty property,
                            const VtFloatArray& times,
                            const auto& values,
                            uint32_t stride) {
                if (times.empty() || values.empty()) {
                    return;
                }
                data.align();
                const uint32_t timesOffset = data.size();
                for (const float time : times) {
                    data.f32(time);
                }
                data.align();
                const uint32_t valuesOffset = data.size();
                for (const auto& value : values) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, GfQuatf>) {
                        const GfVec3f imaginary = value.GetImaginary();
                        data.f32(imaginary[0]);
                        data.f32(imaginary[1]);
                        data.f32(imaginary[2]);
                        data.f32(value.GetReal());
                    } else {
                        data.f32(value[0]);
                        data.f32(value[1]);
                        data.f32(value[2]);
                    }
                }
                const uint32_t record = commands.begin(Command::Animation);
                commands.buffer.u32(static_cast<uint32_t>(AnimationTarget::Node));
                commands.buffer.u32(static_cast<uint32_t>(nodeIndex + 1));
                commands.buffer.u32(static_cast<uint32_t>(property));
                commands.buffer.u32(static_cast<uint32_t>(trackIndex));
                commands.buffer.u32(static_cast<uint32_t>(times.size()));
                commands.buffer.u32(timesOffset);
                commands.buffer.u32(valuesOffset);
                commands.buffer.u32(stride);
                commands.end(record);
            };
            emit(AnimationProperty::Position,
                 animation.translations.times,
                 animation.translations.values,
                 3);
            emit(AnimationProperty::RotationQuaternion,
                 animation.rotations.times,
                 animation.rotations.values,
                 4);
            emit(AnimationProperty::Scaling,
                 animation.scales.times,
                 animation.scales.values,
                 3);
        }
    }

    for (size_t skeletonIndex = 0; skeletonIndex < usd.skeletons.size(); ++skeletonIndex) {
        const Skeleton& skeleton = usd.skeletons[skeletonIndex];
        for (size_t trackIndex = 0; trackIndex < skeleton.skeletonAnimations.size();
             ++trackIndex) {
            const SkeletonAnimation& animation = skeleton.skeletonAnimations[trackIndex];
            for (size_t animatedJointIndex = 0;
                 animatedJointIndex < skeleton.animatedJoints.size();
                 ++animatedJointIndex) {
                const auto jointIt = std::find(skeleton.joints.begin(),
                                               skeleton.joints.end(),
                                               skeleton.animatedJoints[animatedJointIndex]);
                if (jointIt == skeleton.joints.end()) {
                    continue;
                }
                const size_t jointIndex =
                  static_cast<size_t>(std::distance(skeleton.joints.begin(), jointIt));
                std::vector<float> times;
                std::vector<GfMatrix4d> matrices;
                times.reserve(animation.times.size());
                matrices.reserve(animation.times.size());
                for (size_t sampleIndex = 0; sampleIndex < animation.times.size();
                     ++sampleIndex) {
                    if (sampleIndex >= animation.translations.size() ||
                        sampleIndex >= animation.rotations.size() ||
                        sampleIndex >= animation.scales.size() ||
                        animatedJointIndex >= animation.translations[sampleIndex].size() ||
                        animatedJointIndex >= animation.rotations[sampleIndex].size() ||
                        animatedJointIndex >= animation.scales[sampleIndex].size()) {
                        continue;
                    }
                    GfMatrix4d matrix;
                    UsdSkelMakeTransform(
                      animation.translations[sampleIndex][animatedJointIndex],
                      animation.rotations[sampleIndex][animatedJointIndex],
                      animation.scales[sampleIndex][animatedJointIndex],
                      &matrix);
                    times.push_back(animation.times[sampleIndex]);
                    matrices.push_back(matrix);
                }
                if (times.empty()) {
                    continue;
                }
                data.align();
                const uint32_t timesOffset = data.size();
                for (const float time : times) {
                    data.f32(time);
                }
                data.align();
                const uint32_t valuesOffset = data.size();
                for (const GfMatrix4d& matrix : matrices) {
                    for (int row = 0; row < 4; ++row) {
                        for (int column = 0; column < 4; ++column) {
                            data.f32(static_cast<float>(matrix[row][column]));
                        }
                    }
                }
                const uint32_t record = commands.begin(Command::Animation);
                commands.buffer.u32(static_cast<uint32_t>(AnimationTarget::Bone));
                commands.buffer.u32(boneIds[skeletonIndex][jointIndex]);
                commands.buffer.u32(static_cast<uint32_t>(AnimationProperty::Matrix));
                commands.buffer.u32(static_cast<uint32_t>(trackIndex));
                commands.buffer.u32(static_cast<uint32_t>(times.size()));
                commands.buffer.u32(timesOffset);
                commands.buffer.u32(valuesOffset);
                commands.buffer.u32(16);
                commands.end(record);
            }
        }
    }

    result.commands = commands.finish();
    result.data = std::move(data.bytes);
    result.nodeCount = static_cast<uint32_t>(usd.nodes.size());
    result.materialCount = static_cast<uint32_t>(
      isNativeOpenPbrProcessingEnabled() ? usd.openPbrMaterials.size() : usd.materials.size());
    result.preparationMs =
      std::chrono::duration<double, std::milli>(preparationFinished - preparationStarted)
        .count();
    result.packingMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                               preparationFinished)
        .count();
    return true;
}

} // namespace usd_web::direct
