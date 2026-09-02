#include "babylonExport.h"

#include <fileformatutils/common.h>
#include <fileformatutils/featureFlags.h>
#include <fileformatutils/geometry.h>
#include <fileformatutils/materials.h>
#include <fileformatutils/naming.h>
#include <meshOptimization.h>

#include <nlohmann/json.hpp>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdSkel/utils.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace PXR_NS;
using namespace adobe::usd;

namespace usd_web::babylon {
namespace {

using Json = nlohmann::json;
using MeshGroupKey = std::pair<int, int>;
using MeshGroups = std::map<MeshGroupKey, std::vector<int>>;

constexpr const char* kDefaultMaterialId = "material_default";

void
writeString(std::ostream& output, const std::string& value)
{
    output << Json(value).dump();
}

template<typename T>
void
writeInteger(std::ostream& output, T value)
{
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    output.write(buffer, result.ptr - buffer);
}

template<typename T>
void
writeFloatingPoint(std::ostream& output, T value)
{
    if (!std::isfinite(value)) {
        output.put('0');
        return;
    }
    char buffer[64];
    const auto result =
      std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::general);
    if (result.ec == std::errc()) {
        output.write(buffer, result.ptr - buffer);
    } else {
        output.put('0');
    }
}

void
writeFloat(std::ostream& output, float value)
{
    writeFloatingPoint(output, value);
}

void
writeFloat(std::ostream& output, double value)
{
    writeFloatingPoint(output, value);
}

template<typename Array, typename WriteElement>
void
writeArray(std::ostream& output, const Array& values, WriteElement&& writeElement)
{
    output.put('[');
    bool first = true;
    for (const auto& value : values) {
        if (!first) {
            output.put(',');
        }
        first = false;
        writeElement(value);
    }
    output.put(']');
}

template<typename T>
void
writeScalarArray(std::ostream& output, const T& values)
{
    writeArray(output, values, [&](const auto& value) { writeFloat(output, value); });
}

void
writeIntArray(std::ostream& output, const VtIntArray& values)
{
    writeArray(output, values, [&](int value) { writeInteger(output, value); });
}

void
writeVec2Array(std::ostream& output, const VtVec2fArray& values, bool flipV)
{
    output.put('[');
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            output.put(',');
        }
        writeFloat(output, values[i][0]);
        output.put(',');
        writeFloat(output, flipV ? 1.0f - values[i][1] : values[i][1]);
    }
    output.put(']');
}

template<typename T>
void
writeVec3Array(std::ostream& output, const VtArray<T>& values)
{
    output.put('[');
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            output.put(',');
        }
        writeFloat(output, values[i][0]);
        output.put(',');
        writeFloat(output, values[i][1]);
        output.put(',');
        writeFloat(output, values[i][2]);
    }
    output.put(']');
}

void
writeVec4Array(std::ostream& output, const VtVec4fArray& values)
{
    output.put('[');
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            output.put(',');
        }
        for (int component = 0; component < 4; ++component) {
            if (component > 0) {
                output.put(',');
            }
            writeFloat(output, values[i][component]);
        }
    }
    output.put(']');
}

void
writeMatrix(std::ostream& output, const GfMatrix4d& matrix)
{
    output.put('[');
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            if (row != 0 || column != 0) {
                output.put(',');
            }
            writeFloat(output, matrix[row][column]);
        }
    }
    output.put(']');
}

std::string
nodeId(size_t nodeIndex)
{
    return "node_" + std::to_string(nodeIndex);
}

std::string
geometryId(int meshIndex)
{
    return "geometry_" + std::to_string(meshIndex);
}

std::string
meshId(const MeshGroupKey& key)
{
    return "mesh_" + std::to_string(key.first) + "_skin_" + std::to_string(key.second);
}

std::string
materialId(int materialIndex)
{
    return materialIndex >= 0 ? "material_" + std::to_string(materialIndex)
                              : kDefaultMaterialId;
}

std::string
multiMaterialId(int meshIndex)
{
    return "multimaterial_" + std::to_string(meshIndex);
}

std::string
skeletonId(int skeletonIndex)
{
    return "skeleton_" + std::to_string(skeletonIndex);
}

MeshGroups
collectMeshGroups(const UsdData& data)
{
    MeshGroups groups;
    for (size_t nodeIndex = 0; nodeIndex < data.nodes.size(); ++nodeIndex) {
        const Node& node = data.nodes[nodeIndex];
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

void
completeMaterialSubsets(Mesh& mesh)
{
    if (mesh.subsets.empty() || mesh.faces.empty() || mesh.indices.size() != mesh.faces.size() * 3) {
        return;
    }

    std::vector<int> assignedSubset(mesh.faces.size(), -1);
    for (size_t subsetIndex = 0; subsetIndex < mesh.subsets.size(); ++subsetIndex) {
        for (const int faceIndex : mesh.subsets[subsetIndex].faces) {
            if (faceIndex < 0 || static_cast<size_t>(faceIndex) >= assignedSubset.size()) {
                continue;
            }
            if (assignedSubset[faceIndex] >= 0) {
                TF_WARN("Babylon export: face %d in mesh '%s' belongs to overlapping material "
                        "subsets; the first binding wins",
                        faceIndex,
                        mesh.name.c_str());
                continue;
            }
            assignedSubset[faceIndex] = static_cast<int>(subsetIndex);
        }
    }

    std::vector<Subset> partition;
    Subset base;
    base.material = mesh.material;
    for (size_t faceIndex = 0; faceIndex < assignedSubset.size(); ++faceIndex) {
        if (assignedSubset[faceIndex] < 0) {
            base.faces.push_back(static_cast<int>(faceIndex));
            base.indices.push_back(mesh.indices[faceIndex * 3]);
            base.indices.push_back(mesh.indices[faceIndex * 3 + 1]);
            base.indices.push_back(mesh.indices[faceIndex * 3 + 2]);
        }
    }
    if (!base.faces.empty()) {
        partition.push_back(std::move(base));
    }

    for (size_t subsetIndex = 0; subsetIndex < mesh.subsets.size(); ++subsetIndex) {
        Subset subset;
        subset.material = mesh.subsets[subsetIndex].material;
        for (size_t faceIndex = 0; faceIndex < assignedSubset.size(); ++faceIndex) {
            if (assignedSubset[faceIndex] != static_cast<int>(subsetIndex)) {
                continue;
            }
            subset.faces.push_back(static_cast<int>(faceIndex));
            subset.indices.push_back(mesh.indices[faceIndex * 3]);
            subset.indices.push_back(mesh.indices[faceIndex * 3 + 1]);
            subset.indices.push_back(mesh.indices[faceIndex * 3 + 2]);
        }
        if (!subset.faces.empty()) {
            partition.push_back(std::move(subset));
        }
    }
    mesh.subsets = std::move(partition);
}

void
decomposeNode(const Node& node,
              GfVec3f& translation,
              GfQuatf& rotation,
              GfVec3h& scale)
{
    if (node.hasTransform) {
        UsdSkelDecomposeTransform(node.transform, &translation, &rotation, &scale);
    } else {
        translation = GfVec3f(node.translation);
        rotation = node.rotation;
        scale = GfVec3h(node.scale);
    }
    rotation.Normalize();
}

template<typename T>
bool
getInputValue(const Input& input, T& value)
{
    if (input.value.IsHolding<T>()) {
        value = input.value.UncheckedGet<T>();
        return true;
    }
    return false;
}

float
getFloat(const Input& input, float fallback)
{
    if (input.image >= 0) {
        const int channel = token2Channel(input.channel);
        return input.scale[channel >= 0 ? channel : 0];
    }
    float value = fallback;
    if (getInputValue(input, value)) {
        return value;
    }
    if (input.value.IsHolding<double>()) {
        return static_cast<float>(input.value.UncheckedGet<double>());
    }
    return fallback;
}

GfVec3f
getColor(const Input& input, const GfVec3f& fallback)
{
    if (input.image >= 0) {
        return GfVec3f(input.scale[0], input.scale[1], input.scale[2]);
    }
    GfVec3f color;
    if (getInputValue(input, color)) {
        return color;
    }
    if (input.value.IsHolding<GfVec4f>()) {
        const GfVec4f value = input.value.UncheckedGet<GfVec4f>();
        return GfVec3f(value[0], value[1], value[2]);
    }
    return fallback;
}

int
wrapMode(const TfToken& wrap)
{
    if (wrap == AdobeTokens->clamp) {
        return 0;
    }
    if (wrap == AdobeTokens->mirror) {
        return 2;
    }
    return 1;
}

std::string
base64Encode(const std::vector<uint8_t>& data)
{
    static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t first = data[i];
        const uint32_t second = i + 1 < data.size() ? data[i + 1] : 0;
        const uint32_t third = i + 2 < data.size() ? data[i + 2] : 0;
        const uint32_t value = (first << 16) | (second << 8) | third;
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(i + 1 < data.size() ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(i + 2 < data.size() ? alphabet[value & 63] : '=');
    }
    return result;
}

std::string
mimeType(ImageFormat format)
{
    switch (format) {
        case ImageFormatJpg:
            return "image/jpeg";
        case ImageFormatBmp:
            return "image/bmp";
        case ImageFormatWebp:
            return "image/webp";
        default:
            return "image/png";
    }
}

Json
textureJson(const InputTranslator& translator, const Input& input, float level = 1.0f)
{
    if (input.image < 0) {
        return nullptr;
    }
    const ImageAsset& image = translator.getImage(input.image);
    if (image.image.empty()) {
        return nullptr;
    }

    Json texture;
    texture["name"] = "data:" + mimeType(image.format) + ";base64," + base64Encode(image.image);
    texture["level"] = level;
    texture["hasAlpha"] = true;
    texture["coordinatesIndex"] = input.uvIndex;
    texture["uOffset"] = input.uvTranslation[0];
    texture["vOffset"] = input.uvTranslation[1];
    texture["uScale"] = input.uvScale[0];
    texture["vScale"] = input.uvScale[1];
    texture["wAng"] = input.uvRotation;
    texture["wrapU"] = wrapMode(input.wrapS);
    texture["wrapV"] = wrapMode(input.wrapT);
    texture["coordinatesMode"] = 0;
    texture["samplingMode"] = 3;
    texture["invertY"] = false;
    return texture;
}

std::vector<bool>
findDoubleSidedMaterials(const UsdData& data, size_t materialCount)
{
    std::vector<bool> result(materialCount, false);
    for (const Mesh& mesh : data.meshes) {
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

void
writeMaterials(std::ostream& output, const ExportOptions& options, UsdData& data)
{
    const bool useOpenPbr = isNativeOpenPbrProcessingEnabled();
    const size_t count = useOpenPbr ? data.openPbrMaterials.size() : data.materials.size();
    const std::vector<bool> doubleSided = findDoubleSidedMaterials(data, count);
    InputTranslator translator(options.embedTextures, data.images, "usdBabylon");

    output << "\"materials\":[";
    for (size_t index = 0; index <= count; ++index) {
        if (index > 0) {
            output.put(',');
        }
        if (index == count) {
            Json material = {
                { "name", "Default USD material" },
                { "id", kDefaultMaterialId },
                { "customType", "BABYLON.PBRMetallicRoughnessMaterial" },
                { "baseColor", { 1.0f, 1.0f, 1.0f } },
                { "metallic", 0.0f },
                { "roughness", 0.75f },
                { "backFaceCulling", false },
                { "doubleSided", true },
            };
            output << material.dump();
            continue;
        }

        const Material* material = useOpenPbr ? nullptr : &data.materials[index];
        const OpenPbrMaterial* openPbr = useOpenPbr ? &data.openPbrMaterials[index] : nullptr;
        const Input& baseColorSource = useOpenPbr ? openPbr->base_color : material->diffuseColor;
        const Input& opacitySource =
          useOpenPbr ? openPbr->geometry_opacity : material->opacity;
        const Input& metallicSource =
          useOpenPbr ? openPbr->base_metalness : material->metallic;
        const Input& roughnessSource =
          useOpenPbr ? openPbr->specular_roughness : material->roughness;
        const Input& emissiveSource =
          useOpenPbr ? openPbr->emission_color : material->emissiveColor;
        const Input& normalSource =
          useOpenPbr ? openPbr->geometry_normal : material->normal;
        const Input& occlusionSource =
          useOpenPbr ? openPbr->occlusion : material->occlusion;
        const bool isUnlit = useOpenPbr ? openPbr->isUnlit : material->isUnlit;

        Input baseColor;
        Input opacity = opacitySource;
        if (opacity.image >= 0 || !opacity.value.IsEmpty()) {
            translator.translateMix("babylonBaseColor",
                                    AdobeTokens->sRGB,
                                    translator.split3f(baseColorSource, 0),
                                    translator.split3f(baseColorSource, 1),
                                    translator.split3f(baseColorSource, 2),
                                    opacity,
                                    baseColor);
        } else {
            translator.translateDirect(baseColorSource, baseColor);
        }

        Input metallicRoughness;
        if (metallicSource.image >= 0 || roughnessSource.image >= 0) {
            Input one;
            one.value = 1.0f;
            translator.translateMix("babylonMetallicRoughness",
                                    AdobeTokens->raw,
                                    occlusionSource,
                                    roughnessSource,
                                    metallicSource,
                                    one,
                                    metallicRoughness);
        }

        Input normal;
        Input emissive;
        translator.translateDirect(normalSource, normal);
        translator.translateDirect(emissiveSource, emissive);

        const GfVec3f color = getColor(baseColor, GfVec3f(1.0f));
        const GfVec3f emissiveColor = getColor(emissive, GfVec3f(0.0f));
        const float alpha = getFloat(opacity, 1.0f);
        const float metallic = getFloat(metallicSource, 0.0f);
        const float roughness = getFloat(roughnessSource, 0.5f);
        const float normalScale =
          useOpenPbr ? openPbr->normalScale : getFloat(material->normalScale, 1.0f);
        const float alphaCutoff =
          useOpenPbr ? openPbr->opacityThreshold : getFloat(material->opacityThreshold, 0.0f);

        Json exported = {
            { "name", useOpenPbr ? getNodeName(*openPbr) : getNodeName(*material) },
            { "id", materialId(static_cast<int>(index)) },
            { "customType", "BABYLON.PBRMetallicRoughnessMaterial" },
            { "baseColor", { color[0], color[1], color[2] } },
            { "metallic", metallic },
            { "roughness", roughness },
            { "emissive", { emissiveColor[0], emissiveColor[1], emissiveColor[2] } },
            { "alpha", alpha },
            { "backFaceCulling", !doubleSided[index] },
            { "doubleSided", doubleSided[index] },
            { "disableLighting", isUnlit },
        };
        if (alpha < 0.999f || opacity.image >= 0) {
            exported["transparencyMode"] = 2;
        }
        if (alphaCutoff > 0.0f) {
            exported["transparencyMode"] = 1;
            exported["alphaCutOff"] = alphaCutoff;
        }
        if (options.embedTextures) {
            const Json baseTexture = textureJson(translator, baseColor);
            if (!baseTexture.is_null()) {
                exported["baseTexture"] = baseTexture;
            }
            const Json metallicTexture = textureJson(translator, metallicRoughness);
            if (!metallicTexture.is_null()) {
                exported["metallicRoughnessTexture"] = metallicTexture;
            }
            const Json normalTexture = textureJson(translator, normal, normalScale);
            if (!normalTexture.is_null()) {
                exported["normalTexture"] = normalTexture;
            }
            const Json emissiveTexture = textureJson(translator, emissive);
            if (!emissiveTexture.is_null()) {
                exported["emissiveTexture"] = emissiveTexture;
            }
        }
        output << exported.dump();
    }
    output << "],";
}

void
writeMultiMaterials(std::ostream& output, const UsdData& data, const MeshGroups& groups)
{
    output << "\"multiMaterials\":[";
    bool first = true;
    std::vector<bool> written(data.meshes.size(), false);
    for (const auto& [key, nodes] : groups) {
        const int meshIndex = key.first;
        if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= data.meshes.size() ||
            written[meshIndex] || data.meshes[meshIndex].subsets.empty()) {
            continue;
        }
        written[meshIndex] = true;
        if (!first) {
            output.put(',');
        }
        first = false;
        const Mesh& mesh = data.meshes[meshIndex];
        Json materials = Json::array();
        for (const Subset& subset : mesh.subsets) {
            materials.push_back(materialId(subset.material));
        }
        Json multi = {
            { "name", multiMaterialId(meshIndex) },
            { "id", multiMaterialId(meshIndex) },
            { "materials", std::move(materials) },
        };
        output << multi.dump();
    }
    output << "],";
}

VtVec4fArray
buildTangents(const Mesh& mesh)
{
    if (mesh.tangents.values.empty()) {
        return {};
    }
    if (mesh.bitangents.values.size() != mesh.tangents.values.size() ||
        mesh.normals.values.size() != mesh.tangents.values.size()) {
        return mesh.tangents.values;
    }

    VtVec4fArray tangents(mesh.tangents.values.size());
    for (size_t i = 0; i < tangents.size(); ++i) {
        const GfVec4f& tangent = mesh.tangents.values[i];
        const GfVec3f tangent3(tangent[0], tangent[1], tangent[2]);
        const GfVec3f expected = GfCross(mesh.normals.values[i], tangent3);
        const float handedness = GfDot(expected, mesh.bitangents.values[i]) >= 0.0f ? 1.0f : -1.0f;
        tangents[i] = GfVec4f(tangent3[0], tangent3[1], tangent3[2], handedness);
    }
    return tangents;
}

void
writeColors(std::ostream& output, const Mesh& mesh)
{
    const size_t vertexCount = mesh.points.size();
    const bool hasColors = !mesh.colors.empty() && mesh.colors[0].values.size() == vertexCount;
    const bool hasOpacity =
      !mesh.opacities.empty() && mesh.opacities[0].values.size() == vertexCount;
    output.put('[');
    for (size_t i = 0; i < vertexCount; ++i) {
        if (i > 0) {
            output.put(',');
        }
        const GfVec3f color = hasColors ? mesh.colors[0].values[i] : GfVec3f(1.0f);
        writeFloat(output, color[0]);
        output.put(',');
        writeFloat(output, color[1]);
        output.put(',');
        writeFloat(output, color[2]);
        output.put(',');
        writeFloat(output, hasOpacity ? mesh.opacities[0].values[i] : 1.0f);
    }
    output.put(']');
}

void
writeSkinData(std::ostream& output, const Mesh& mesh, size_t setIndex)
{
    const size_t vertexCount = mesh.points.size();
    const size_t influenceCount = static_cast<size_t>(std::max(mesh.influenceCount, 0));
    const size_t offset = setIndex * 4;
    output.put('[');
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        if (vertex > 0) {
            output.put(',');
        }
        for (size_t component = 0; component < 4; ++component) {
            if (component > 0) {
                output.put(',');
            }
            const size_t influence = offset + component;
            const size_t sourceIndex = vertex * influenceCount + influence;
            if (influence < influenceCount && sourceIndex < mesh.joints.size()) {
                writeInteger(output, mesh.joints[sourceIndex]);
            } else {
                output.put('0');
            }
        }
    }
    output.put(']');
}

void
writeWeightData(std::ostream& output, const Mesh& mesh, size_t setIndex)
{
    const size_t vertexCount = mesh.points.size();
    const size_t influenceCount = static_cast<size_t>(std::max(mesh.influenceCount, 0));
    const size_t offset = setIndex * 4;
    output.put('[');
    for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
        if (vertex > 0) {
            output.put(',');
        }
        for (size_t component = 0; component < 4; ++component) {
            if (component > 0) {
                output.put(',');
            }
            const size_t influence = offset + component;
            const size_t sourceIndex = vertex * influenceCount + influence;
            if (influence < influenceCount && sourceIndex < mesh.weights.size()) {
                writeFloat(output, mesh.weights[sourceIndex]);
            } else {
                output.put('0');
            }
        }
    }
    output.put(']');
}

void
writeGeometryIndices(std::ostream& output, const Mesh& mesh)
{
    if (mesh.subsets.empty()) {
        writeIntArray(output, mesh.indices);
        return;
    }
    output.put('[');
    bool first = true;
    for (const Subset& subset : mesh.subsets) {
        for (const int index : subset.indices) {
            if (!first) {
                output.put(',');
            }
            first = false;
            writeInteger(output, index);
        }
    }
    output.put(']');
}

void
writeGeometries(std::ostream& output, const UsdData& data, const MeshGroups& groups)
{
    output << "\"geometries\":{\"vertexData\":[";
    bool first = true;
    std::vector<bool> written(data.meshes.size(), false);
    for (const auto& [key, nodes] : groups) {
        const int meshIndex = key.first;
        if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= data.meshes.size() ||
            written[meshIndex]) {
            continue;
        }
        written[meshIndex] = true;
        const Mesh& mesh = data.meshes[meshIndex];
        if (mesh.points.empty()) {
            continue;
        }
        if (!first) {
            output.put(',');
        }
        first = false;

        output << "{\"id\":";
        writeString(output, geometryId(meshIndex));
        output << ",\"updatable\":false,\"positions\":";
        writeVec3Array(output, mesh.points);
        if (mesh.normals.values.size() == mesh.points.size()) {
            output << ",\"normals\":";
            writeVec3Array(output, mesh.normals.values);
        }
        const VtVec4fArray tangents = buildTangents(mesh);
        if (tangents.size() == mesh.points.size()) {
            output << ",\"tangents\":";
            writeVec4Array(output, tangents);
        }
        if (mesh.uvs.values.size() == mesh.points.size()) {
            output << ",\"uvs\":";
            writeVec2Array(output, mesh.uvs.values, true);
        }
        for (size_t uvIndex = 0; uvIndex < std::min<size_t>(mesh.extraUVSets.size(), 5);
             ++uvIndex) {
            if (mesh.extraUVSets[uvIndex].values.size() != mesh.points.size()) {
                continue;
            }
            output << ",\"uvs" << (uvIndex + 2) << "\":";
            writeVec2Array(output, mesh.extraUVSets[uvIndex].values, true);
        }
        if ((!mesh.colors.empty() && mesh.colors[0].values.size() == mesh.points.size()) ||
            (!mesh.opacities.empty() &&
             mesh.opacities[0].values.size() == mesh.points.size())) {
            output << ",\"colors\":";
            writeColors(output, mesh);
            output << ",\"hasVertexAlpha\":true";
        }
        if (mesh.influenceCount > 0 && !mesh.joints.empty() && !mesh.weights.empty()) {
            output << ",\"matricesIndices\":";
            writeSkinData(output, mesh, 0);
            output << ",\"matricesIndicesExpanded\":true,\"matricesWeights\":";
            writeWeightData(output, mesh, 0);
            if (mesh.influenceCount > 4) {
                output << ",\"matricesIndicesExtra\":";
                writeSkinData(output, mesh, 1);
                output << ",\"matricesIndicesExtraExpanded\":true,\"matricesWeightsExtra\":";
                writeWeightData(output, mesh, 1);
            }
        }
        output << ",\"indices\":";
        writeGeometryIndices(output, mesh);
        output.put('}');
    }
    output << "]},";
}

void
writeTransform(std::ostream& output, const Node& node)
{
    GfVec3f translation;
    GfQuatf rotation;
    GfVec3h scale;
    decomposeNode(node, translation, rotation, scale);
    const GfVec3f imaginary = rotation.GetImaginary();

    output << "\"position\":[";
    writeFloat(output, translation[0]);
    output.put(',');
    writeFloat(output, translation[1]);
    output.put(',');
    writeFloat(output, translation[2]);
    output << "],\"rotationQuaternion\":[";
    writeFloat(output, imaginary[0]);
    output.put(',');
    writeFloat(output, imaginary[1]);
    output.put(',');
    writeFloat(output, imaginary[2]);
    output.put(',');
    writeFloat(output, rotation.GetReal());
    output << "],\"scaling\":[";
    writeFloat(output, scale[0]);
    output.put(',');
    writeFloat(output, scale[1]);
    output.put(',');
    writeFloat(output, scale[2]);
    output.put(']');
}

bool
needsCorrectionNode(const UsdData&)
{
    return true;
}

void
writeTransformNodes(std::ostream& output, const UsdData& data)
{
    output << "\"transformNodes\":[";
    bool first = true;
    if (needsCorrectionNode(data)) {
        const float scale =
          data.metersPerUnit > 0.0 ? static_cast<float>(data.metersPerUnit) : 1.0f;
        const bool zUp = data.upAxis == UsdGeomTokens->z;
        const Json correction = {
            { "name", "USD correction" },
            { "id", "usd_correction" },
            { "position", { 0.0f, 0.0f, 0.0f } },
            { "rotationQuaternion",
              zUp
                ? Json{ -0.7071068f, 0.0f, 0.0f, 0.7071068f }
                : Json{ 0.0f, 0.0f, 0.0f, 1.0f } },
            { "scaling",
              zUp ? Json{ scale, -scale, scale } : Json{ scale, scale, -scale } },
            { "isEnabled", true },
        };
        output << correction.dump();
        first = false;
    }

    for (size_t index = 0; index < data.nodes.size(); ++index) {
        if (!first) {
            output.put(',');
        }
        first = false;
        const Node& node = data.nodes[index];
        output << "{\"name\":";
        writeString(output, getNodeName(node));
        output << ",\"id\":";
        writeString(output, nodeId(index));
        output.put(',');
        writeTransform(output, node);
        output << ",\"isEnabled\":true";
        if (node.parent >= 0) {
            output << ",\"parentId\":";
            writeString(output, nodeId(static_cast<size_t>(node.parent)));
        } else if (needsCorrectionNode(data)) {
            output << ",\"parentId\":\"usd_correction\"";
        }
        output.put('}');
    }
    output << "],";
}

void
writeSubMeshes(std::ostream& output, const Mesh& mesh)
{
    output << "\"subMeshes\":[";
    if (mesh.subsets.empty()) {
        output << "{\"materialIndex\":0,\"verticesStart\":0,\"verticesCount\":";
        writeInteger(output, mesh.points.size());
        output << ",\"indexStart\":0,\"indexCount\":";
        writeInteger(output, mesh.indices.size());
        output.put('}');
    } else {
        size_t indexStart = 0;
        for (size_t subsetIndex = 0; subsetIndex < mesh.subsets.size(); ++subsetIndex) {
            if (subsetIndex > 0) {
                output.put(',');
            }
            const Subset& subset = mesh.subsets[subsetIndex];
            output << "{\"materialIndex\":";
            writeInteger(output, subsetIndex);
            output << ",\"verticesStart\":0,\"verticesCount\":";
            writeInteger(output, mesh.points.size());
            output << ",\"indexStart\":";
            writeInteger(output, indexStart);
            output << ",\"indexCount\":";
            writeInteger(output, subset.indices.size());
            output.put('}');
            indexStart += subset.indices.size();
        }
    }
    output.put(']');
}

void
writeMeshes(std::ostream& output, const UsdData& data, const MeshGroups& groups)
{
    output << "\"meshes\":[";
    bool first = true;
    for (const auto& [key, nodeIndices] : groups) {
        if (key.first < 0 || static_cast<size_t>(key.first) >= data.meshes.size() ||
            nodeIndices.empty()) {
            continue;
        }
        const Mesh& mesh = data.meshes[key.first];
        if (mesh.points.empty()) {
            continue;
        }
        if (!first) {
            output.put(',');
        }
        first = false;

        const std::string id = meshId(key);
        output << "{\"name\":";
        writeString(output, getNodeName(mesh));
        output << ",\"id\":";
        writeString(output, id);
        output << ",\"position\":[0,0,0],\"rotationQuaternion\":[0,0,0,1],"
                  "\"scaling\":[1,1,1],\"isEnabled\":true,\"isVisible\":true,"
                  "\"pickable\":true,\"receiveShadows\":false,\"checkCollisions\":false,"
                  "\"geometryId\":";
        writeString(output, geometryId(key.first));
        output << ",\"parentId\":";
        writeString(output, nodeId(static_cast<size_t>(nodeIndices.front())));
        output << ",\"materialId\":";
        writeString(output,
                    mesh.subsets.empty() ? materialId(mesh.material)
                                         : multiMaterialId(key.first));
        if (key.second >= 0) {
            output << ",\"skeletonId\":";
            writeString(output, skeletonId(key.second));
            output << ",\"numBoneInfluencers\":";
            writeInteger(output, std::min(mesh.influenceCount, 8));
        }
        output.put(',');
        writeSubMeshes(output, mesh);
        output << ",\"instances\":[";
        for (size_t occurrence = 1; occurrence < nodeIndices.size(); ++occurrence) {
            if (occurrence > 1) {
                output.put(',');
            }
            output << "{\"name\":";
            writeString(output, id + "_instance_" + std::to_string(occurrence));
            output << ",\"id\":";
            writeString(output, id + "_instance_" + std::to_string(occurrence));
            output << ",\"position\":[0,0,0],\"rotationQuaternion\":[0,0,0,1],"
                      "\"scaling\":[1,1,1],\"isEnabled\":true,\"isVisible\":true,"
                      "\"isPickable\":true,\"checkCollisions\":false,\"parentId\":";
            writeString(output, nodeId(static_cast<size_t>(nodeIndices[occurrence])));
            output.put('}');
        }
        output << "]}";
    }
    output << "],";
}

void
writeSkeletons(std::ostream& output, const UsdData& data)
{
    output << "\"skeletons\":[";
    for (size_t skeletonIndex = 0; skeletonIndex < data.skeletons.size(); ++skeletonIndex) {
        if (skeletonIndex > 0) {
            output.put(',');
        }
        const Skeleton& skeleton = data.skeletons[skeletonIndex];
        output << "{\"name\":";
        writeString(output, getNodeName(skeleton));
        output << ",\"id\":";
        writeString(output, skeletonId(static_cast<int>(skeletonIndex)));
        output << ",\"needInitialSkinMatrix\":false,\"bones\":[";
        for (size_t jointIndex = 0; jointIndex < skeleton.joints.size(); ++jointIndex) {
            if (jointIndex > 0) {
                output.put(',');
            }
            output << "{\"parentBoneIndex\":";
            const int parent = jointIndex < skeleton.jointParents.size()
                                 ? skeleton.jointParents[jointIndex]
                                 : -1;
            writeInteger(output, parent);
            output << ",\"index\":";
            writeInteger(output, jointIndex);
            output << ",\"name\":";
            const std::string name =
              jointIndex < skeleton.jointNames.size()
                ? skeleton.jointNames[jointIndex].GetString()
                : skeleton.joints[jointIndex].GetString();
            writeString(output, name);
            output << ",\"id\":";
            writeString(output,
                        "bone_" + std::to_string(skeletonIndex) + "_" +
                          std::to_string(jointIndex));
            output << ",\"matrix\":";
            if (jointIndex < skeleton.restTransforms.size()) {
                writeMatrix(output, skeleton.restTransforms[jointIndex]);
            } else {
                writeMatrix(output, GfMatrix4d(1.0));
            }
            output << ",\"rest\":";
            if (jointIndex < skeleton.restTransforms.size()) {
                writeMatrix(output, skeleton.restTransforms[jointIndex]);
            } else {
                writeMatrix(output, GfMatrix4d(1.0));
            }
            output.put('}');
        }
        output << "],\"ranges\":[]}";
    }
    output << "],";
}

template<typename Times>
Json
makeAnimation(const std::string& name,
              const std::string& property,
              int dataType,
              double framePerSecond,
              const Times& times,
              const Json& values)
{
    Json keys = Json::array();
    for (size_t index = 0; index < times.size() && index < values.size(); ++index) {
        keys.push_back({ { "frame", times[index] }, { "values", values[index] } });
    }
    return {
        { "name", name },
        { "property", property },
        { "framePerSecond", framePerSecond },
        { "dataType", dataType },
        { "loopBehavior", 1 },
        { "keys", std::move(keys) },
    };
}

Json
matrixValue(const GfMatrix4d& matrix)
{
    Json values = Json::array();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            values.push_back(matrix[row][column]);
        }
    }
    return values;
}

template<typename T>
Json
vec3Values(const VtArray<T>& values)
{
    Json result = Json::array();
    for (const T& value : values) {
        result.push_back({ value[0], value[1], value[2] });
    }
    return result;
}

Json
quatValues(const VtQuatfArray& values)
{
    Json result = Json::array();
    for (const GfQuatf& value : values) {
        const GfVec3f imaginary = value.GetImaginary();
        result.push_back({ imaginary[0], imaginary[1], imaginary[2], value.GetReal() });
    }
    return result;
}

void
writeAnimationGroups(std::ostream& output, const UsdData& data)
{
    output << "\"animationGroups\":[";
    bool firstGroup = true;
    size_t trackCount = data.animationTracks.size();
    for (const Skeleton& skeleton : data.skeletons) {
        trackCount = std::max(trackCount, skeleton.skeletonAnimations.size());
    }
    for (size_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
        Json targeted = Json::array();
        for (size_t nodeIndex = 0; nodeIndex < data.nodes.size(); ++nodeIndex) {
            const Node& node = data.nodes[nodeIndex];
            if (trackIndex >= node.animations.size()) {
                continue;
            }
            const NodeAnimation& animation = node.animations[trackIndex];
            auto addTarget = [&](const Json& animationJson) {
                targeted.push_back(
                  { { "targetId", nodeId(nodeIndex) }, { "animation", animationJson } });
            };
            if (!animation.translations.times.empty()) {
                addTarget(makeAnimation("translation",
                                        "position",
                                        1,
                                        data.timeCodesPerSecond,
                                        animation.translations.times,
                                        vec3Values(animation.translations.values)));
            }
            if (!animation.rotations.times.empty()) {
                addTarget(makeAnimation("rotation",
                                        "rotationQuaternion",
                                        2,
                                        data.timeCodesPerSecond,
                                        animation.rotations.times,
                                        quatValues(animation.rotations.values)));
            }
            if (!animation.scales.times.empty()) {
                addTarget(makeAnimation("scale",
                                        "scaling",
                                        1,
                                        data.timeCodesPerSecond,
                                        animation.scales.times,
                                        vec3Values(animation.scales.values)));
            }
        }
        for (size_t skeletonIndex = 0; skeletonIndex < data.skeletons.size();
             ++skeletonIndex) {
            const Skeleton& skeleton = data.skeletons[skeletonIndex];
            if (trackIndex >= skeleton.skeletonAnimations.size()) {
                continue;
            }
            const SkeletonAnimation& animation = skeleton.skeletonAnimations[trackIndex];
            for (size_t animatedJointIndex = 0;
                 animatedJointIndex < skeleton.animatedJoints.size();
                 ++animatedJointIndex) {
                const TfToken joint = skeleton.animatedJoints[animatedJointIndex];
                const auto jointIt =
                  std::find(skeleton.joints.begin(), skeleton.joints.end(), joint);
                if (jointIt == skeleton.joints.end()) {
                    continue;
                }
                const size_t jointIndex = std::distance(skeleton.joints.begin(), jointIt);
                std::vector<float> times;
                Json matrices = Json::array();
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
                    UsdSkelMakeTransform(animation.translations[sampleIndex][animatedJointIndex],
                                         animation.rotations[sampleIndex][animatedJointIndex],
                                         animation.scales[sampleIndex][animatedJointIndex],
                                         &matrix);
                    times.push_back(animation.times[sampleIndex]);
                    matrices.push_back(matrixValue(matrix));
                }
                if (times.empty()) {
                    continue;
                }
                targeted.push_back({
                    { "targetId",
                      "bone_" + std::to_string(skeletonIndex) + "_" +
                        std::to_string(jointIndex) },
                    { "animation",
                      makeAnimation("boneMatrix",
                                    "_matrix",
                                    3,
                                    data.timeCodesPerSecond,
                                    times,
                                    matrices) },
                });
            }
        }
        if (targeted.empty()) {
            continue;
        }
        if (!firstGroup) {
            output.put(',');
        }
        firstGroup = false;
        const AnimationTrack* track =
          trackIndex < data.animationTracks.size() ? &data.animationTracks[trackIndex] : nullptr;
        float from = track ? track->minTime : 0.0f;
        float to = track ? track->maxTime : 0.0f;
        if (!track) {
            for (const Skeleton& skeleton : data.skeletons) {
                if (trackIndex < skeleton.skeletonAnimations.size() &&
                    !skeleton.skeletonAnimations[trackIndex].times.empty()) {
                    from = skeleton.skeletonAnimations[trackIndex].times.front();
                    to = skeleton.skeletonAnimations[trackIndex].times.back();
                    break;
                }
            }
        }
        const Json group = {
            { "name", track ? getNodeName(*track) : "Skeleton animation" },
            { "from", from },
            { "to", to },
            { "speedRatio", 1.0f },
            { "loopAnimation", true },
            { "isAdditive", false },
            { "weight", 1.0f },
            { "targetedAnimations", std::move(targeted) },
        };
        output << group.dump();
    }
    output << ']';
}

} // namespace

bool
exportScene(const ExportOptions& options,
            UsdData& data,
            std::ostream& output,
            ExportPhaseTiming* timing)
{
    const auto preparationStarted = std::chrono::steady_clock::now();
    for (Mesh& mesh : data.meshes) {
        if (mesh.points.empty()) {
            continue;
        }
        completeMaterialSubsets(mesh);
        transformMesh(mesh, mesh.geomBindTransform);
        if (options.optimizeMeshes) {
            optimizeMeshForGltf(mesh);
        }
    }

    const MeshGroups groups = collectMeshGroups(data);
    const auto preparationFinished = std::chrono::steady_clock::now();
    const auto serializationStarted = preparationFinished;
    output << "{\"producer\":{\"name\":\"usd-web\",\"version\":\"1.0\","
              "\"exporter_version\":\"1.0\",\"file\":\"USD\"},"
              "\"useRightHandedSystem\":false,";
    writeMaterials(output, options, data);
    writeMultiMaterials(output, data, groups);
    writeGeometries(output, data, groups);
    writeSkeletons(output, data);
    writeTransformNodes(output, data);
    writeMeshes(output, data, groups);
    writeAnimationGroups(output, data);
    output.put('}');
    const auto serializationFinished = std::chrono::steady_clock::now();
    if (timing != nullptr) {
        timing->meshPreparationMs =
          std::chrono::duration<double, std::milli>(preparationFinished - preparationStarted)
            .count();
        timing->serializationMs =
          std::chrono::duration<double, std::milli>(serializationFinished - serializationStarted)
            .count();
    }
    return output.good();
}

} // namespace usd_web::babylon
