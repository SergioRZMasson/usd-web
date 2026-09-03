#include "directScene.h"

#include "commandProtocol.h"

#include <meshoptimizer.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdSkel/animQuery.h>
#include <pxr/usd/usdSkel/binding.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/skinningQuery.h>
#include <pxr/usd/usdSkel/topology.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usd_web::direct {
namespace {

constexpr size_t kMaxInfluences = 8;

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
        if (byteCount == 0) {
            return offset;
        }
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

struct TextureData
{
    SdfAssetPath asset;
    std::string name;
    TfToken channel;
    TfToken wrapS = TfToken("repeat");
    TfToken wrapT = TfToken("repeat");
    GfVec2f scale = GfVec2f(1.0f);
    GfVec2f translation = GfVec2f(0.0f);
    float rotation = 0.0f;
    int uvIndex = 0;
    TfToken uvName = TfToken("st");

    explicit operator bool() const { return !asset.GetAssetPath().empty(); }
};

struct MaterialData
{
    std::string name = "Default USD material";
    GfVec3f baseColor = GfVec3f(1.0f);
    GfVec3f emissive = GfVec3f(0.0f);
    float metallic = 0.0f;
    float roughness = 0.5f;
    float opacity = 1.0f;
    float normalScale = 1.0f;
    float alphaCutoff = 0.0f;
    bool unlit = false;
    bool doubleSided = false;
    TextureData baseTexture;
    TextureData opacityTexture;
    TextureData normalTexture;
    TextureData ormTexture;
    TextureData emissiveTexture;
    uint32_t opacityChannel = kMissingOffset;
    uint32_t roughnessChannel = kMissingOffset;
    uint32_t metallicChannel = kMissingOffset;
    uint32_t occlusionChannel = kMissingOffset;
};

struct NodeAnimation
{
    std::vector<float> times;
    std::vector<GfVec3f> translations;
    std::vector<GfVec4f> rotations;
    std::vector<GfVec3f> scales;
};

struct NodeData
{
    SdfPath path;
    std::string name;
    uint32_t parentId = kMissingOffset;
    GfMatrix4d localTransform = GfMatrix4d(1.0);
    NodeAnimation animation;
};

struct SubmeshData
{
    uint32_t materialId = 0;
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
};

using JointSet = std::array<uint16_t, kMaxInfluences>;
using WeightSet = std::array<float, kMaxInfluences>;

struct MeshData
{
    std::string name;
    bool doubleSided = false;
    uint32_t skeletonId = kMissingOffset;
    uint32_t influenceCount = 0;
    std::vector<GfVec3f> positions;
    std::vector<GfVec3f> normals;
    std::vector<GfVec2f> uvs;
    std::vector<GfVec4f> colors;
    std::vector<JointSet> joints;
    std::vector<WeightSet> weights;
    std::vector<uint32_t> indices;
    std::vector<SubmeshData> submeshes;
    std::vector<uint32_t> nodeIds;
};

struct SkeletonAnimation
{
    std::vector<float> times;
    std::vector<VtMatrix4dArray> localTransforms;
};

struct SkeletonData
{
    std::string name;
    VtTokenArray joints;
    VtIntArray parents;
    VtMatrix4dArray restTransforms;
    SkeletonAnimation animation;
};

struct SkinBinding
{
    uint32_t skeletonId = kMissingOffset;
    UsdSkelSkinningQuery query;
    std::vector<uint16_t> jointMap;
};

struct SceneData
{
    TfToken upAxis = UsdGeomTokens->y;
    double metersPerUnit = 1.0;
    double timeCodesPerSecond = 24.0;
    std::vector<MaterialData> materials{ MaterialData{} };
    std::unordered_map<std::string, uint32_t> materialIds;
    std::vector<NodeData> nodes;
    std::unordered_map<std::string, uint32_t> nodeIds;
    std::vector<MeshData> meshes;
    std::unordered_map<std::string, size_t> meshIds;
    std::vector<SkeletonData> skeletons;
    std::unordered_map<std::string, uint32_t> skeletonIds;
    std::unordered_map<std::string, SkinBinding> skinBindings;
    UsdShadeMaterialBindingAPI::BindingsCache bindingsCache;
    UsdShadeMaterialBindingAPI::CollectionQueryCache collectionQueryCache;
    UsdSkelCache skelCache;
};

template<typename T>
struct PrimvarData
{
    VtArray<T> values;
    TfToken interpolation;

    explicit operator bool() const { return !values.empty(); }

    const T* value(size_t faceIndex, size_t cornerIndex, size_t pointIndex) const
    {
        size_t index = 0;
        if (interpolation == UsdGeomTokens->constant) {
            index = 0;
        } else if (interpolation == UsdGeomTokens->uniform) {
            index = faceIndex;
        } else if (interpolation == UsdGeomTokens->faceVarying) {
            index = cornerIndex;
        } else {
            index = pointIndex;
        }
        return index < values.size() ? &values[index] : nullptr;
    }
};

std::string
displayName(const UsdPrim& prim, const char* fallback)
{
    const std::string authored = prim.GetDisplayName();
    if (!authored.empty()) {
        return authored;
    }
    const std::string name = prim.GetName().GetString();
    return name.empty() ? std::string(fallback) : name;
}

uint32_t
appendString(BufferWriter& data, const std::string& value, uint32_t& length)
{
    length = static_cast<uint32_t>(value.size());
    return data.appendString(value);
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

template<typename T>
bool
getInputValue(const UsdShadeShader& shader, const char* name, T& value)
{
    const UsdShadeInput input = shader.GetInput(TfToken(name));
    return input && input.Get(&value);
}

bool
getTokenInput(const UsdShadeShader& shader, const char* name, TfToken& value)
{
    const UsdShadeInput input = shader.GetInput(TfToken(name));
    if (!input) {
        return false;
    }
    if (input.Get(&value)) {
        return true;
    }
    std::string text;
    if (input.Get(&text)) {
        value = TfToken(text);
        return true;
    }
    return false;
}

UsdShadeShader
connectedShader(const UsdShadeInput& input, TfToken* outputName = nullptr)
{
    if (!input) {
        return UsdShadeShader();
    }
    UsdShadeConnectableAPI source;
    TfToken sourceName;
    UsdShadeAttributeType sourceType = UsdShadeAttributeType::Invalid;
    if (!input.GetConnectedSource(&source, &sourceName, &sourceType)) {
        return UsdShadeShader();
    }
    if (outputName) {
        *outputName = sourceName;
    }
    return UsdShadeShader(source);
}

void
readUvConnection(const UsdShadeShader& textureShader, TextureData& texture)
{
    UsdShadeShader source = connectedShader(textureShader.GetInput(TfToken("st")));
    if (!source) {
        return;
    }
    TfToken sourceId;
    source.GetShaderId(&sourceId);
    if (sourceId == TfToken("UsdTransform2d")) {
        getInputValue(source, "scale", texture.scale);
        getInputValue(source, "translation", texture.translation);
        float rotationDegrees = 0.0f;
        if (getInputValue(source, "rotation", rotationDegrees)) {
            texture.rotation =
              rotationDegrees * static_cast<float>(3.14159265358979323846 / 180.0);
        }
        source = connectedShader(source.GetInput(TfToken("in")));
        if (!source) {
            return;
        }
        source.GetShaderId(&sourceId);
    }
    if (sourceId == TfToken("UsdPrimvarReader_float2")) {
        TfToken varname;
        if (!getTokenInput(source, "varname", varname)) {
            return;
        }
        texture.uvName = varname;
        texture.uvIndex = 0;
    }
}

TextureData
readTexture(const UsdShadeInput& materialInput)
{
    TextureData result;
    TfToken outputName;
    const UsdShadeShader shader = connectedShader(materialInput, &outputName);
    if (!shader) {
        return result;
    }
    TfToken shaderId;
    shader.GetShaderId(&shaderId);
    if (shaderId != TfToken("UsdUVTexture")) {
        return result;
    }
    if (!getInputValue(shader, "file", result.asset) || result.asset.GetAssetPath().empty()) {
        return {};
    }
    result.name = TfGetBaseName(result.asset.GetAssetPath());
    result.channel = outputName;
    getTokenInput(shader, "wrapS", result.wrapS);
    getTokenInput(shader, "wrapT", result.wrapT);
    readUvConnection(shader, result);
    return result;
}

bool
sameTexture(const TextureData& left, const TextureData& right)
{
    if (!left || !right) {
        return false;
    }
    const std::string leftPath =
      left.asset.GetResolvedPath().empty() ? left.asset.GetAssetPath()
                                           : left.asset.GetResolvedPath();
    const std::string rightPath =
      right.asset.GetResolvedPath().empty() ? right.asset.GetAssetPath()
                                            : right.asset.GetResolvedPath();
    return leftPath == rightPath;
}

uint32_t
textureChannel(const TfToken& channel)
{
    if (channel == TfToken("r")) {
        return 0;
    }
    if (channel == TfToken("g")) {
        return 1;
    }
    if (channel == TfToken("b")) {
        return 2;
    }
    if (channel == TfToken("a")) {
        return 3;
    }
    return 4;
}

std::optional<MaterialData>
readMaterial(const UsdShadeMaterial& material)
{
    MaterialData result;
    result.name = displayName(material.GetPrim(), "Material");
    const UsdShadeShader shader = material.ComputeSurfaceSource();
    if (!shader) {
        return std::nullopt;
    }
    TfToken shaderId;
    shader.GetShaderId(&shaderId);
    if (shaderId != TfToken("UsdPreviewSurface")) {
        TF_WARN("Using the default material for unsupported surface shader '%s' at <%s>.",
                shaderId.GetText(),
                material.GetPath().GetText());
        return std::nullopt;
    }

    getInputValue(shader, "diffuseColor", result.baseColor);
    getInputValue(shader, "emissiveColor", result.emissive);
    getInputValue(shader, "metallic", result.metallic);
    getInputValue(shader, "roughness", result.roughness);
    getInputValue(shader, "opacity", result.opacity);
    getInputValue(shader, "opacityThreshold", result.alphaCutoff);
    const float authoredOpacity = result.opacity;

    const UsdShadeInput baseInput = shader.GetInput(TfToken("diffuseColor"));
    const UsdShadeInput opacityInput = shader.GetInput(TfToken("opacity"));
    const UsdShadeInput metallicInput = shader.GetInput(TfToken("metallic"));
    const UsdShadeInput roughnessInput = shader.GetInput(TfToken("roughness"));
    const UsdShadeInput occlusionInput = shader.GetInput(TfToken("occlusion"));
    result.baseTexture = readTexture(baseInput);
    result.normalTexture = readTexture(shader.GetInput(TfToken("normal")));
    result.emissiveTexture = readTexture(shader.GetInput(TfToken("emissiveColor")));

    result.opacityTexture = readTexture(opacityInput);
    if (result.opacityTexture) {
        result.opacityChannel = textureChannel(result.opacityTexture.channel);
        if (result.opacityChannel == 3) {
            result.opacity = std::min(result.opacity, 0.999f);
        } else {
            TF_WARN("Ignoring opacity texture output '%s' on material <%s>; Babylon requires "
                    "this baseline path to provide opacity in the alpha channel.",
                    result.opacityTexture.channel.GetText(),
                    material.GetPath().GetText());
            result.opacityTexture = {};
            result.opacityChannel = kMissingOffset;
        }
    }

    const TextureData metallicTexture = readTexture(metallicInput);
    const TextureData roughnessTexture = readTexture(roughnessInput);
    const TextureData occlusionTexture = readTexture(occlusionInput);
    if (sameTexture(metallicTexture, roughnessTexture)) {
        const uint32_t metallicChannel = textureChannel(metallicTexture.channel);
        const uint32_t roughnessChannel = textureChannel(roughnessTexture.channel);
        const bool supportedMetallic = metallicChannel == 0 || metallicChannel == 2;
        const bool supportedRoughness = roughnessChannel == 1 || roughnessChannel == 3;
        if (supportedMetallic && supportedRoughness) {
            result.ormTexture = metallicTexture;
            result.metallicChannel = metallicChannel;
            result.roughnessChannel = roughnessChannel;
            if (sameTexture(result.ormTexture, occlusionTexture) &&
                textureChannel(occlusionTexture.channel) == 0) {
                result.occlusionChannel = 0;
            }
        } else {
            TF_WARN("Ignoring unsupported metallic/roughness channel layout on material <%s>.",
                    material.GetPath().GetText());
        }
    } else if (metallicTexture || roughnessTexture) {
        TF_WARN("Ignoring separately authored metallic and roughness textures on material <%s>; "
                "the native baseline currently requires a shared packed texture.",
                material.GetPath().GetText());
    }
    if (occlusionTexture && result.occlusionChannel == kMissingOffset) {
        TF_WARN("Ignoring separate occlusion texture on material <%s> in the native direct "
                "baseline.",
                material.GetPath().GetText());
    }

    TfToken materialUv;
    auto validateUv = [&](TextureData& texture, const char* slot) {
        if (!texture) {
            return true;
        }
        if (materialUv.IsEmpty()) {
            materialUv = texture.uvName;
            return true;
        }
        if (materialUv == texture.uvName) {
            return true;
        }
        TF_WARN("Ignoring %s texture on material <%s>: it uses UV primvar '%s', while the "
                "material's first texture uses '%s'.",
                slot,
                material.GetPath().GetText(),
                texture.uvName.GetText(),
                materialUv.GetText());
        texture = {};
        return false;
    };
    validateUv(result.baseTexture, "base-color");
    if (!validateUv(result.opacityTexture, "opacity")) {
        result.opacityChannel = kMissingOffset;
        result.opacity = authoredOpacity;
    }
    validateUv(result.normalTexture, "normal");
    if (!validateUv(result.ormTexture, "metallic-roughness")) {
        result.roughnessChannel = kMissingOffset;
        result.metallicChannel = kMissingOffset;
        result.occlusionChannel = kMissingOffset;
    }
    validateUv(result.emissiveTexture, "emissive");

    result.unlit = TfStringToLower(shaderId.GetString()).find("unlit") != std::string::npos;
    return result;
}

uint32_t
materialId(SceneData& scene, const UsdShadeMaterial& material)
{
    if (!material) {
        return 0;
    }
    const std::string key = material.GetPath().GetString();
    const auto found = scene.materialIds.find(key);
    if (found != scene.materialIds.end()) {
        return found->second;
    }
    const std::optional<MaterialData> parsed = readMaterial(material);
    if (!parsed) {
        scene.materialIds.emplace(key, 0);
        return 0;
    }
    const uint32_t id = static_cast<uint32_t>(scene.materials.size());
    scene.materialIds.emplace(key, id);
    scene.materials.push_back(*parsed);
    return id;
}

uint32_t
boundMaterialId(SceneData& scene, const UsdPrim& prim)
{
    const UsdShadeMaterial material =
      UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(&scene.bindingsCache,
                                                            &scene.collectionQueryCache);
    return materialId(scene, material);
}

template<typename T>
PrimvarData<T>
readPrimvar(const UsdGeomPrimvar& primvar)
{
    PrimvarData<T> result;
    if (!primvar || !primvar.ComputeFlattened(&result.values, UsdTimeCode::Default())) {
        return {};
    }
    result.interpolation = primvar.GetInterpolation();
    if (result.interpolation.IsEmpty()) {
        result.interpolation = UsdGeomTokens->constant;
    }
    return result;
}

template<typename T>
PrimvarData<T>
readAuthoredPrimvar(const UsdGeomPrimvar& primvar)
{
    return primvar && primvar.HasAuthoredValue() ? readPrimvar<T>(primvar)
                                                 : PrimvarData<T>{};
}

PrimvarData<GfVec3f>
readNormals(const UsdGeomMesh& mesh)
{
    const UsdGeomPrimvar authored =
      UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(TfToken("normals"));
    PrimvarData<GfVec3f> result = readPrimvar<GfVec3f>(authored);
    if (result) {
        return result;
    }
    mesh.GetNormalsAttr().Get(&result.values, UsdTimeCode::Default());
    result.interpolation = mesh.GetNormalsInterpolation();
    return result;
}

PrimvarData<GfVec2f>
readUvs(const UsdGeomMesh& mesh, const TfToken& requestedName)
{
    const UsdGeomPrimvarsAPI primvars(mesh.GetPrim());
    if (!requestedName.IsEmpty()) {
        PrimvarData<GfVec2f> requested =
          readPrimvar<GfVec2f>(primvars.FindPrimvarWithInheritance(requestedName));
        if (requested) {
            return requested;
        }
        TF_WARN("Mesh <%s> does not provide requested UV primvar '%s'.",
                mesh.GetPath().GetText(),
                requestedName.GetText());
    }
    static const std::array<TfToken, 3> preferred = {
        TfToken("st"), TfToken("uv"), TfToken("UVMap")
    };
    for (const TfToken& name : preferred) {
        PrimvarData<GfVec2f> result = readPrimvar<GfVec2f>(primvars.GetPrimvar(name));
        if (result) {
            return result;
        }
    }
    for (const UsdGeomPrimvar& primvar : primvars.GetPrimvarsWithValues()) {
        PrimvarData<GfVec2f> result = readPrimvar<GfVec2f>(primvar);
        if (result) {
            return result;
        }
    }
    return {};
}

bool
materialUvName(const SceneData& scene,
               const std::vector<uint32_t>& materialIds,
               TfToken& selected)
{
    std::vector<bool> visited(scene.materials.size(), false);
    for (const uint32_t materialId : materialIds) {
        if (materialId >= scene.materials.size() || visited[materialId]) {
            continue;
        }
        visited[materialId] = true;
        const MaterialData& material = scene.materials[materialId];
        const std::array<const TextureData*, 5> textures = {
            &material.baseTexture,
            &material.opacityTexture,
            &material.normalTexture,
            &material.ormTexture,
            &material.emissiveTexture,
        };
        for (const TextureData* texture : textures) {
            if (!*texture) {
                continue;
            }
            if (selected.IsEmpty()) {
                selected = texture->uvName;
            } else if (selected != texture->uvName) {
                TF_WARN("Cannot emit mesh using material '%s': its UV primvar '%s' conflicts "
                        "with '%s' required by another bound material.",
                        material.name.c_str(),
                        texture->uvName.GetText(),
                        selected.GetText());
                return false;
            }
        }
    }
    return true;
}

std::vector<GfVec3f>
computeSmoothNormals(const VtVec3fArray& points,
                     const VtIntArray& faceCounts,
                     const VtIntArray& faceIndices)
{
    std::vector<GfVec3f> normals(points.size(), GfVec3f(0.0f));
    size_t cornerOffset = 0;
    for (const int count : faceCounts) {
        if (count < 3 || cornerOffset + static_cast<size_t>(count) > faceIndices.size()) {
            cornerOffset += std::max(count, 0);
            continue;
        }
        const int first = faceIndices[cornerOffset];
        for (int corner = 1; corner + 1 < count; ++corner) {
            const int second = faceIndices[cornerOffset + corner];
            const int third = faceIndices[cornerOffset + corner + 1];
            if (first < 0 || second < 0 || third < 0 ||
                static_cast<size_t>(first) >= points.size() ||
                static_cast<size_t>(second) >= points.size() ||
                static_cast<size_t>(third) >= points.size()) {
                continue;
            }
            const GfVec3f faceNormal =
              GfCross(points[second] - points[first], points[third] - points[first]);
            normals[first] += faceNormal;
            normals[second] += faceNormal;
            normals[third] += faceNormal;
        }
        cornerOffset += static_cast<size_t>(count);
    }
    for (GfVec3f& normal : normals) {
        if (normal.Normalize() <= std::numeric_limits<float>::epsilon()) {
            normal = GfVec3f(0.0f, 1.0f, 0.0f);
        }
    }
    return normals;
}

template<typename T>
void
remapVertices(std::vector<T>& values,
              size_t sourceCount,
              size_t destinationCount,
              const std::vector<unsigned int>& remap)
{
    if (values.size() != sourceCount) {
        return;
    }
    std::vector<T> destination(destinationCount);
    meshopt_remapVertexBuffer(
      destination.data(), values.data(), sourceCount, sizeof(T), remap.data());
    values = std::move(destination);
}

bool
optimizeMesh(MeshData& mesh)
{
    const size_t sourceVertexCount = mesh.positions.size();
    if (sourceVertexCount == 0 || mesh.indices.empty()) {
        return false;
    }
    std::vector<meshopt_Stream> streams;
    streams.push_back({ mesh.positions.data(), sizeof(GfVec3f), sizeof(GfVec3f) });
    if (mesh.normals.size() == sourceVertexCount) {
        streams.push_back({ mesh.normals.data(), sizeof(GfVec3f), sizeof(GfVec3f) });
    }
    if (mesh.uvs.size() == sourceVertexCount) {
        streams.push_back({ mesh.uvs.data(), sizeof(GfVec2f), sizeof(GfVec2f) });
    }
    if (mesh.colors.size() == sourceVertexCount) {
        streams.push_back({ mesh.colors.data(), sizeof(GfVec4f), sizeof(GfVec4f) });
    }
    if (mesh.joints.size() == sourceVertexCount) {
        streams.push_back({ mesh.joints.data(), sizeof(JointSet), sizeof(JointSet) });
    }
    if (mesh.weights.size() == sourceVertexCount) {
        streams.push_back({ mesh.weights.data(), sizeof(WeightSet), sizeof(WeightSet) });
    }

    std::vector<unsigned int> remap(sourceVertexCount);
    const size_t uniqueCount =
      meshopt_generateVertexRemapMulti(remap.data(),
                                       mesh.indices.data(),
                                       mesh.indices.size(),
                                       sourceVertexCount,
                                       streams.data(),
                                       streams.size());
    if (uniqueCount == 0) {
        return false;
    }
    std::vector<uint32_t> remappedIndices(mesh.indices.size());
    meshopt_remapIndexBuffer(remappedIndices.data(),
                             mesh.indices.data(),
                             mesh.indices.size(),
                             remap.data());
    mesh.indices = std::move(remappedIndices);
    remapVertices(mesh.positions, sourceVertexCount, uniqueCount, remap);
    remapVertices(mesh.normals, sourceVertexCount, uniqueCount, remap);
    remapVertices(mesh.uvs, sourceVertexCount, uniqueCount, remap);
    remapVertices(mesh.colors, sourceVertexCount, uniqueCount, remap);
    remapVertices(mesh.joints, sourceVertexCount, uniqueCount, remap);
    remapVertices(mesh.weights, sourceVertexCount, uniqueCount, remap);

    for (const SubmeshData& submesh : mesh.submeshes) {
        if (submesh.indexCount == 0 || submesh.indexCount % 3 != 0) {
            continue;
        }
        uint32_t* indices = mesh.indices.data() + submesh.indexStart;
        std::vector<uint32_t> cacheOptimized(submesh.indexCount);
        std::vector<uint32_t> overdrawOptimized(submesh.indexCount);
        meshopt_optimizeVertexCache(
          cacheOptimized.data(), indices, submesh.indexCount, mesh.positions.size());
        meshopt_optimizeOverdraw(overdrawOptimized.data(),
                                 cacheOptimized.data(),
                                 submesh.indexCount,
                                 reinterpret_cast<const float*>(mesh.positions.data()),
                                 mesh.positions.size(),
                                 sizeof(GfVec3f),
                                 1.05f);
        std::copy(overdrawOptimized.begin(), overdrawOptimized.end(), indices);
    }

    const size_t weldedCount = mesh.positions.size();
    std::vector<unsigned int> fetchRemap(weldedCount);
    const size_t fetchedCount = meshopt_optimizeVertexFetchRemap(
      fetchRemap.data(), mesh.indices.data(), mesh.indices.size(), weldedCount);
    if (fetchedCount == 0) {
        return false;
    }
    std::vector<uint32_t> fetchIndices(mesh.indices.size());
    meshopt_remapIndexBuffer(
      fetchIndices.data(), mesh.indices.data(), mesh.indices.size(), fetchRemap.data());
    mesh.indices = std::move(fetchIndices);
    remapVertices(mesh.positions, weldedCount, fetchedCount, fetchRemap);
    remapVertices(mesh.normals, weldedCount, fetchedCount, fetchRemap);
    remapVertices(mesh.uvs, weldedCount, fetchedCount, fetchRemap);
    remapVertices(mesh.colors, weldedCount, fetchedCount, fetchRemap);
    remapVertices(mesh.joints, weldedCount, fetchedCount, fetchRemap);
    remapVertices(mesh.weights, weldedCount, fetchedCount, fetchRemap);
    return true;
}

uint32_t
findParentNode(const SceneData& scene, SdfPath path)
{
    for (path = path.GetParentPath(); !path.IsEmpty() && path != SdfPath::AbsoluteRootPath();
         path = path.GetParentPath()) {
        const auto found = scene.nodeIds.find(path.GetString());
        if (found != scene.nodeIds.end()) {
            return found->second;
        }
    }
    return kMissingOffset;
}

NodeData
readNode(const UsdPrim& prim, const SceneData& scene)
{
    NodeData node;
    node.path = prim.GetPath();
    node.name = displayName(prim, "Node");
    node.parentId = findParentNode(scene, prim.GetPath());
    const UsdGeomXformable xformable(prim);
    bool resets = false;
    xformable.GetLocalTransformation(
      &node.localTransform, &resets, UsdTimeCode::Default());
    if (resets) {
        node.parentId = kMissingOffset;
    }

    if (!xformable.TransformMightBeTimeVarying()) {
        return node;
    }
    std::vector<double> times;
    xformable.GetTimeSamples(&times);
    node.animation.times.reserve(times.size());
    node.animation.translations.reserve(times.size());
    node.animation.rotations.reserve(times.size());
    node.animation.scales.reserve(times.size());
    for (const double time : times) {
        GfMatrix4d matrix(1.0);
        bool sampleResets = false;
        if (!xformable.GetLocalTransformation(&matrix, &sampleResets, UsdTimeCode(time))) {
            continue;
        }
        const GfTransform transform(matrix);
        const GfVec3d translation = transform.GetTranslation();
        const GfVec3d scale = transform.GetScale();
        const GfQuatd rotation = transform.GetRotation().GetQuat();
        const GfVec3d imaginary = rotation.GetImaginary();
        node.animation.times.push_back(static_cast<float>(time));
        node.animation.translations.emplace_back(translation);
        GfVec4f quaternion(static_cast<float>(imaginary[0]),
                           static_cast<float>(imaginary[1]),
                           static_cast<float>(imaginary[2]),
                           static_cast<float>(rotation.GetReal()));
        if (!node.animation.rotations.empty() &&
            GfDot(node.animation.rotations.back(), quaternion) < 0.0f) {
            quaternion = -quaternion;
        }
        node.animation.rotations.push_back(quaternion);
        node.animation.scales.emplace_back(scale);
    }
    return node;
}

std::vector<uint16_t>
buildJointMap(const UsdSkelSkinningQuery& skinningQuery, const VtTokenArray& skeletonJoints)
{
    VtTokenArray localJoints;
    if (!skinningQuery.GetJointOrder(&localJoints) || localJoints.empty()) {
        std::vector<uint16_t> identity(skeletonJoints.size());
        for (size_t index = 0; index < identity.size(); ++index) {
            identity[index] = static_cast<uint16_t>(index);
        }
        return identity;
    }
    std::vector<uint16_t> result(localJoints.size(), 0);
    for (size_t localIndex = 0; localIndex < localJoints.size(); ++localIndex) {
        const auto found =
          std::find(skeletonJoints.begin(), skeletonJoints.end(), localJoints[localIndex]);
        if (found != skeletonJoints.end()) {
            result[localIndex] =
              static_cast<uint16_t>(std::distance(skeletonJoints.begin(), found));
        }
    }
    return result;
}

uint32_t
registerSkeleton(SceneData& scene, const UsdSkelSkeletonQuery& query)
{
    if (!query) {
        return kMissingOffset;
    }
    const UsdSkelAnimQuery& animQuery = query.GetAnimQuery();
    const std::string key = query.GetPrim().GetPath().GetString() + "|" +
                            (animQuery ? animQuery.GetPrim().GetPath().GetString() : "");
    const auto found = scene.skeletonIds.find(key);
    if (found != scene.skeletonIds.end()) {
        return found->second;
    }

    SkeletonData skeleton;
    skeleton.name = displayName(query.GetPrim(), "Skeleton");
    skeleton.joints = query.GetJointOrder();
    skeleton.parents.resize(skeleton.joints.size());
    const UsdSkelTopology& topology = query.GetTopology();
    for (size_t index = 0; index < skeleton.joints.size(); ++index) {
        skeleton.parents[index] = topology.GetParent(index);
    }
    query.ComputeJointLocalTransforms(
      &skeleton.restTransforms, UsdTimeCode::Default(), true);
    if (skeleton.restTransforms.size() != skeleton.joints.size()) {
        skeleton.restTransforms.assign(skeleton.joints.size(), GfMatrix4d(1.0));
    }

    if (animQuery) {
        std::vector<double> times;
        animQuery.GetJointTransformTimeSamples(&times);
        for (const double time : times) {
            VtMatrix4dArray transforms;
            if (!query.ComputeJointLocalTransforms(
                  &transforms, UsdTimeCode(time), false) ||
                transforms.size() != skeleton.joints.size()) {
                continue;
            }
            skeleton.animation.times.push_back(static_cast<float>(time));
            skeleton.animation.localTransforms.push_back(std::move(transforms));
        }
    }

    scene.skeletons.push_back(std::move(skeleton));
    const uint32_t id = static_cast<uint32_t>(scene.skeletons.size());
    scene.skeletonIds.emplace(key, id);
    return id;
}

void
collectSkeletonBindings(const UsdStageRefPtr& stage, SceneData& scene)
{
    const Usd_PrimFlagsPredicate predicate = UsdTraverseInstanceProxies();
    for (const UsdPrim& prim : stage->Traverse(predicate)) {
        if (!prim.IsA<UsdSkelRoot>()) {
            continue;
        }
        const UsdSkelRoot root(prim);
        if (!scene.skelCache.Populate(root, predicate)) {
            continue;
        }
        std::vector<UsdSkelBinding> bindings;
        if (!scene.skelCache.ComputeSkelBindings(root, &bindings, predicate)) {
            continue;
        }
        for (const UsdSkelBinding& binding : bindings) {
            const UsdSkelSkeletonQuery skeletonQuery =
              scene.skelCache.GetSkelQuery(binding.GetSkeleton());
            const uint32_t skeletonId = registerSkeleton(scene, skeletonQuery);
            if (skeletonId == kMissingOffset) {
                continue;
            }
            const SkeletonData& skeleton = scene.skeletons[skeletonId - 1];
            for (const UsdSkelSkinningQuery& skinningQuery :
                 binding.GetSkinningTargets()) {
                SkinBinding skin;
                skin.skeletonId = skeletonId;
                skin.query = skinningQuery;
                skin.jointMap = buildJointMap(skinningQuery, skeleton.joints);
                scene.skinBindings[skinningQuery.GetPrim().GetPath().GetString()] =
                  std::move(skin);
            }
        }
    }
}

bool
readSkinning(const SkinBinding* skin,
             size_t pointCount,
             GfMatrix4d& geomBind,
             uint32_t& influenceCount,
             std::vector<JointSet>& joints,
             std::vector<WeightSet>& weights)
{
    if (!skin) {
        return false;
    }
    VtIntArray sourceJoints;
    VtFloatArray sourceWeights;
    if (!skin->query.ComputeVaryingJointInfluences(
          pointCount, &sourceJoints, &sourceWeights, UsdTimeCode::Default())) {
        return false;
    }
    const int sourceInfluences = skin->query.GetNumInfluencesPerComponent();
    if (sourceInfluences <= 0 ||
        sourceJoints.size() != pointCount * static_cast<size_t>(sourceInfluences) ||
        sourceWeights.size() != sourceJoints.size()) {
        return false;
    }

    joints.resize(pointCount);
    weights.resize(pointCount);
    influenceCount = std::min<uint32_t>(sourceInfluences, kMaxInfluences);
    for (size_t point = 0; point < pointCount; ++point) {
        std::vector<std::pair<float, uint16_t>> influences;
        influences.reserve(sourceInfluences);
        for (int influence = 0; influence < sourceInfluences; ++influence) {
            const size_t sourceIndex =
              point * static_cast<size_t>(sourceInfluences) + influence;
            const int localJoint = sourceJoints[sourceIndex];
            const uint16_t skeletonJoint =
              localJoint >= 0 && static_cast<size_t>(localJoint) < skin->jointMap.size()
                ? skin->jointMap[localJoint]
                : 0;
            influences.emplace_back(sourceWeights[sourceIndex], skeletonJoint);
        }
        std::partial_sort(influences.begin(),
                          influences.begin() + influenceCount,
                          influences.end(),
                          [](const auto& left, const auto& right) {
                              return left.first > right.first;
                          });
        float weightSum = 0.0f;
        for (size_t influence = 0; influence < influenceCount; ++influence) {
            joints[point][influence] = influences[influence].second;
            weights[point][influence] = std::max(influences[influence].first, 0.0f);
            weightSum += weights[point][influence];
        }
        if (weightSum > 0.0f) {
            for (size_t influence = 0; influence < influenceCount; ++influence) {
                weights[point][influence] /= weightSum;
            }
        } else {
            weights[point][0] = 1.0f;
        }
    }
    geomBind = skin->query.GetGeomBindTransform(UsdTimeCode::Default());
    return true;
}

std::string
meshCacheKey(const UsdPrim& prim,
             const std::vector<uint32_t>& faceMaterials,
             uint32_t skeletonId,
             bool doubleSided)
{
    const UsdPrim source = prim.IsInstanceProxy() ? prim.GetPrimInPrototype() : prim;
    std::string key = source ? source.GetPath().GetString() : prim.GetPath().GetString();
    key += "|s" + std::to_string(skeletonId);
    key += doubleSided ? "|d1" : "|d0";
    uint64_t materialHash = 1469598103934665603ull;
    for (const uint32_t material : faceMaterials) {
        materialHash ^= material;
        materialHash *= 1099511628211ull;
    }
    key += "|m" + std::to_string(materialHash);
    return key;
}

bool
extractMesh(const UsdGeomMesh& usdMesh,
            uint32_t nodeId,
            SceneData& scene,
            const SkinBinding* skin)
{
    VtVec3fArray points;
    VtIntArray faceCounts;
    VtIntArray faceIndices;
    if (!usdMesh.GetPointsAttr().Get(&points, UsdTimeCode::Default()) ||
        !usdMesh.GetFaceVertexCountsAttr().Get(&faceCounts, UsdTimeCode::Default()) ||
        !usdMesh.GetFaceVertexIndicesAttr().Get(&faceIndices, UsdTimeCode::Default()) ||
        points.empty() || faceCounts.empty()) {
        return false;
    }
    size_t expectedCorners = 0;
    for (const int count : faceCounts) {
        if (count < 3) {
            TF_WARN("Skipping mesh <%s> with a face containing fewer than three vertices.",
                    usdMesh.GetPath().GetText());
            return false;
        }
        expectedCorners += static_cast<size_t>(count);
    }
    if (expectedCorners != faceIndices.size()) {
        TF_WARN("Skipping mesh <%s> with inconsistent face topology.",
                usdMesh.GetPath().GetText());
        return false;
    }

    const uint32_t fallbackMaterial = boundMaterialId(scene, usdMesh.GetPrim());
    std::vector<uint32_t> faceMaterials(faceCounts.size(), fallbackMaterial);
    for (const UsdGeomSubset& subset :
         UsdShadeMaterialBindingAPI(usdMesh.GetPrim()).GetMaterialBindSubsets()) {
        VtIntArray subsetFaces;
        if (!subset.GetIndicesAttr().Get(&subsetFaces, UsdTimeCode::Default())) {
            continue;
        }
        const uint32_t subsetMaterial = boundMaterialId(scene, subset.GetPrim());
        for (const int face : subsetFaces) {
            if (face >= 0 && static_cast<size_t>(face) < faceMaterials.size()) {
                faceMaterials[face] = subsetMaterial;
            }
        }
    }

    bool doubleSided = false;
    usdMesh.GetDoubleSidedAttr().Get(&doubleSided, UsdTimeCode::Default());
    const uint32_t skeletonId = skin ? skin->skeletonId : kMissingOffset;
    const std::string key =
      meshCacheKey(usdMesh.GetPrim(), faceMaterials, skeletonId, doubleSided);
    const auto cached = scene.meshIds.find(key);
    if (cached != scene.meshIds.end()) {
        scene.meshes[cached->second].nodeIds.push_back(nodeId);
        return true;
    }

    MeshData mesh;
    mesh.name = displayName(usdMesh.GetPrim(), "Mesh");
    mesh.doubleSided = doubleSided;
    mesh.skeletonId = skeletonId;

    PrimvarData<GfVec3f> normalData = readNormals(usdMesh);
    std::vector<GfVec3f> generatedNormals;
    if (!normalData) {
        generatedNormals = computeSmoothNormals(points, faceCounts, faceIndices);
        normalData.values.assign(generatedNormals.begin(), generatedNormals.end());
        normalData.interpolation = UsdGeomTokens->vertex;
    }
    TfToken uvName;
    if (!materialUvName(scene, faceMaterials, uvName)) {
        return false;
    }
    const PrimvarData<GfVec2f> uvData = readUvs(usdMesh, uvName);
    const UsdGeomGprim gprim(usdMesh.GetPrim());
    const PrimvarData<GfVec3f> colorData =
      readAuthoredPrimvar<GfVec3f>(gprim.GetDisplayColorPrimvar());
    const PrimvarData<float> opacityData =
      readAuthoredPrimvar<float>(gprim.GetDisplayOpacityPrimvar());

    GfMatrix4d geomBind(1.0);
    std::vector<JointSet> pointJoints;
    std::vector<WeightSet> pointWeights;
    if (skin &&
        !readSkinning(
          skin, points.size(), geomBind, mesh.influenceCount, pointJoints, pointWeights)) {
        TF_WARN("Could not read skinning influences for mesh <%s>; emitting it as a static mesh.",
                usdMesh.GetPath().GetText());
        mesh.skeletonId = kMissingOffset;
        mesh.influenceCount = 0;
    }
    const GfMatrix4d normalTransform = geomBind.GetInverse().GetTranspose();

    std::map<uint32_t, std::vector<uint32_t>> materialIndices;
    auto appendVertex = [&](size_t faceIndex, size_t cornerIndex, int pointIndex) {
        const size_t point = static_cast<size_t>(pointIndex);
        const GfVec3d transformedPoint = geomBind.Transform(GfVec3d(points[point]));
        mesh.positions.emplace_back(transformedPoint);

        GfVec3f normal(0.0f, 1.0f, 0.0f);
        if (const GfVec3f* value = normalData.value(faceIndex, cornerIndex, point)) {
            const GfVec3d transformedNormal =
              normalTransform.TransformDir(GfVec3d(*value));
            normal = GfVec3f(transformedNormal);
            normal.Normalize();
        }
        mesh.normals.push_back(normal);

        if (uvData) {
            const GfVec2f* uv = uvData.value(faceIndex, cornerIndex, point);
            mesh.uvs.push_back(uv ? *uv : GfVec2f(0.0f));
        }
        if (colorData || opacityData) {
            const GfVec3f* color = colorData.value(faceIndex, cornerIndex, point);
            const float* opacity = opacityData.value(faceIndex, cornerIndex, point);
            const GfVec3f rgb = color ? *color : GfVec3f(1.0f);
            mesh.colors.emplace_back(rgb[0], rgb[1], rgb[2], opacity ? *opacity : 1.0f);
        }
        if (!pointJoints.empty()) {
            mesh.joints.push_back(pointJoints[point]);
            mesh.weights.push_back(pointWeights[point]);
        }
        return static_cast<uint32_t>(mesh.positions.size() - 1);
    };

    size_t cornerOffset = 0;
    for (size_t face = 0; face < faceCounts.size(); ++face) {
        const int count = faceCounts[face];
        for (int triangle = 0; triangle < count - 2; ++triangle) {
            const std::array<size_t, 3> corners = {
                cornerOffset,
                cornerOffset + static_cast<size_t>(triangle + 1),
                cornerOffset + static_cast<size_t>(triangle + 2),
            };
            auto& output = materialIndices[faceMaterials[face]];
            for (const size_t corner : corners) {
                const int pointIndex = faceIndices[corner];
                if (pointIndex < 0 || static_cast<size_t>(pointIndex) >= points.size()) {
                    TF_WARN("Skipping mesh <%s> with an invalid point index.",
                            usdMesh.GetPath().GetText());
                    return false;
                }
                output.push_back(appendVertex(face, corner, pointIndex));
            }
        }
        cornerOffset += static_cast<size_t>(count);
    }

    for (auto& [material, indices] : materialIndices) {
        SubmeshData submesh;
        submesh.materialId = material;
        submesh.indexStart = static_cast<uint32_t>(mesh.indices.size());
        submesh.indexCount = static_cast<uint32_t>(indices.size());
        mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
        mesh.submeshes.push_back(submesh);
    }
    mesh.nodeIds.push_back(nodeId);
    const size_t meshIndex = scene.meshes.size();
    scene.meshIds.emplace(key, meshIndex);
    scene.meshes.push_back(std::move(mesh));
    return true;
}

bool
isVisible(const UsdPrim& prim)
{
    const UsdGeomImageable imageable(prim);
    return !imageable ||
           imageable.ComputeVisibility(UsdTimeCode::Default()) != UsdGeomTokens->invisible;
}

bool
extractStage(const UsdStageRefPtr& stage, SceneData& scene)
{
    scene.upAxis = UsdGeomGetStageUpAxis(stage);
    scene.metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
    scene.timeCodesPerSecond = stage->GetTimeCodesPerSecond();
    collectSkeletonBindings(stage, scene);

    for (const UsdPrim& prim : stage->Traverse(UsdTraverseInstanceProxies())) {
        if (!isVisible(prim)) {
            continue;
        }
        const UsdGeomXformable xformable(prim);
        uint32_t nodeId = kMissingOffset;
        if (xformable) {
            scene.nodes.push_back(readNode(prim, scene));
            nodeId = static_cast<uint32_t>(scene.nodes.size());
            scene.nodeIds[prim.GetPath().GetString()] = nodeId;
        }
        if (!prim.IsA<UsdGeomMesh>() || nodeId == kMissingOffset) {
            continue;
        }
        const auto skin = scene.skinBindings.find(prim.GetPath().GetString());
        if (!extractMesh(UsdGeomMesh(prim),
                         nodeId,
                         scene,
                         skin == scene.skinBindings.end() ? nullptr : &skin->second)) {
            return false;
        }
    }
    return true;
}

std::string
resolvedTexturePath(const TextureData& texture)
{
    if (!texture.asset.GetResolvedPath().empty()) {
        return texture.asset.GetResolvedPath();
    }
    return ArGetResolver().Resolve(texture.asset.GetAssetPath()).GetPathString();
}

uint32_t
mimeType(const std::string& path)
{
    const std::string extension =
      TfStringToLower(ArGetResolver().GetExtension(path));
    if (extension == "jpg" || extension == "jpeg") {
        return 2;
    }
    if (extension == "bmp") {
        return 3;
    }
    if (extension == "webp") {
        return 4;
    }
    if (extension == "png") {
        return 1;
    }
    return 0;
}

uint32_t
wrapMode(const TfToken& mode)
{
    if (mode == TfToken("mirror")) {
        return 2;
    }
    return mode == TfToken("repeat") ? 1 : 0;
}

int32_t
emitTexture(CommandWriter& commands,
            BufferWriter& data,
            const TextureData& texture,
            uint32_t textureId,
            std::unordered_map<std::string, std::pair<uint32_t, uint32_t>>& imageCache)
{
    if (!texture) {
        return -1;
    }
    const std::string resolved = resolvedTexturePath(texture);
    if (resolved.empty()) {
        return -1;
    }
    const uint32_t mime = mimeType(resolved);
    if (mime == 0) {
        TF_WARN("Skipping browser-unsupported texture format '%s'.", resolved.c_str());
        return -1;
    }

    auto cached = imageCache.find(resolved);
    if (cached == imageCache.end()) {
        const std::shared_ptr<ArAsset> asset =
          ArGetResolver().OpenAsset(ArResolvedPath(resolved));
        if (!asset || asset->GetSize() == 0) {
            return -1;
        }
        const std::shared_ptr<const char> buffer = asset->GetBuffer();
        if (!buffer) {
            return -1;
        }
        const uint32_t offset =
          data.appendBytes(buffer.get(), asset->GetSize(), 4);
        cached =
          imageCache.emplace(resolved,
                             std::make_pair(offset,
                                            static_cast<uint32_t>(asset->GetSize())))
            .first;
    }

    uint32_t nameLength = 0;
    const uint32_t nameOffset =
      appendString(data, texture.name.empty() ? TfGetBaseName(resolved) : texture.name, nameLength);
    data.align();
    const uint32_t transformOffset = data.size();
    data.f32(texture.scale[0]);
    data.f32(texture.scale[1]);
    data.f32(texture.translation[0]);
    data.f32(texture.translation[1]);
    data.f32(texture.rotation);

    const uint32_t record = commands.begin(Command::Texture);
    commands.buffer.u32(textureId);
    commands.buffer.u32(nameOffset);
    commands.buffer.u32(nameLength);
    commands.buffer.u32(mime);
    commands.buffer.u32(cached->second.first);
    commands.buffer.u32(cached->second.second);
    commands.buffer.u32(static_cast<uint32_t>(std::max(texture.uvIndex, 0)));
    commands.buffer.u32(transformOffset);
    commands.buffer.u32(wrapMode(texture.wrapS));
    commands.buffer.u32(wrapMode(texture.wrapT));
    commands.end(record);
    return static_cast<int32_t>(textureId);
}

void
emitMaterials(CommandWriter& commands, BufferWriter& data, const SceneData& scene)
{
    uint32_t nextTextureId = 1;
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> imageCache;
    for (size_t materialIndex = 0; materialIndex < scene.materials.size();
         ++materialIndex) {
        const MaterialData& material = scene.materials[materialIndex];
        const int32_t baseTexture =
          emitTexture(commands,
                      data,
                      material.baseTexture,
                      nextTextureId,
                      imageCache);
        nextTextureId += baseTexture >= 0 ? 1 : 0;
        const int32_t opacityTexture =
          emitTexture(commands,
                      data,
                      material.opacityTexture,
                      nextTextureId,
                      imageCache);
        nextTextureId += opacityTexture >= 0 ? 1 : 0;
        const int32_t normalTexture =
          emitTexture(commands,
                      data,
                      material.normalTexture,
                      nextTextureId,
                      imageCache);
        nextTextureId += normalTexture >= 0 ? 1 : 0;
        const int32_t ormTexture =
          emitTexture(commands,
                      data,
                      material.ormTexture,
                      nextTextureId,
                      imageCache);
        nextTextureId += ormTexture >= 0 ? 1 : 0;
        const int32_t emissiveTexture =
          emitTexture(commands,
                      data,
                      material.emissiveTexture,
                      nextTextureId,
                      imageCache);
        nextTextureId += emissiveTexture >= 0 ? 1 : 0;

        data.align();
        const uint32_t baseOffset = data.size();
        data.f32(material.baseColor[0]);
        data.f32(material.baseColor[1]);
        data.f32(material.baseColor[2]);
        data.f32(material.opacity);
        const uint32_t emissiveOffset = data.size();
        data.f32(material.emissive[0]);
        data.f32(material.emissive[1]);
        data.f32(material.emissive[2]);
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, material.name, nameLength);

        uint32_t flags = material.unlit ? MaterialUnlit : 0;
        flags |= material.doubleSided ? MaterialDoubleSided : 0;
        flags |= material.opacity < 0.999f || opacityTexture >= 0 ? MaterialAlphaBlend : 0;
        const uint32_t record = commands.begin(Command::Material);
        commands.buffer.u32(static_cast<uint32_t>(materialIndex));
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(baseOffset);
        commands.buffer.u32(emissiveOffset);
        commands.buffer.f32(material.metallic);
        commands.buffer.f32(material.roughness);
        commands.buffer.f32(material.normalScale);
        commands.buffer.f32(material.alphaCutoff);
        commands.buffer.u32(flags);
        commands.buffer.u32(static_cast<uint32_t>(baseTexture));
        commands.buffer.u32(static_cast<uint32_t>(opacityTexture));
        commands.buffer.u32(static_cast<uint32_t>(normalTexture));
        commands.buffer.u32(static_cast<uint32_t>(ormTexture));
        commands.buffer.u32(static_cast<uint32_t>(emissiveTexture));
        commands.buffer.u32(material.opacityChannel);
        commands.buffer.u32(material.roughnessChannel);
        commands.buffer.u32(material.metallicChannel);
        commands.buffer.u32(material.occlusionChannel);
        commands.end(record);
    }
}

template<typename T>
uint32_t
appendArray(BufferWriter& data, const std::vector<T>& values)
{
    return values.empty()
             ? kMissingOffset
             : data.appendBytes(values.data(), values.size() * sizeof(T), alignof(T));
}

uint32_t
appendUvs(BufferWriter& data, const std::vector<GfVec2f>& values)
{
    if (values.empty()) {
        return kMissingOffset;
    }
    data.align();
    const uint32_t offset = data.size();
    for (const GfVec2f& uv : values) {
        data.f32(uv[0]);
        data.f32(1.0f - uv[1]);
    }
    return offset;
}

void
emitAnimation(CommandWriter& commands,
              BufferWriter& data,
              AnimationTarget target,
              uint32_t targetId,
              AnimationProperty property,
              const std::vector<float>& times,
              const void* values,
              size_t valueBytes,
              uint32_t stride)
{
    if (times.empty() || values == nullptr || valueBytes == 0) {
        return;
    }
    const uint32_t timesOffset =
      data.appendBytes(times.data(), times.size() * sizeof(float), 4);
    const uint32_t valuesOffset = data.appendBytes(values, valueBytes, 4);
    const uint32_t record = commands.begin(Command::Animation);
    commands.buffer.u32(static_cast<uint32_t>(target));
    commands.buffer.u32(targetId);
    commands.buffer.u32(static_cast<uint32_t>(property));
    commands.buffer.u32(0);
    commands.buffer.u32(static_cast<uint32_t>(times.size()));
    commands.buffer.u32(timesOffset);
    commands.buffer.u32(valuesOffset);
    commands.buffer.u32(stride);
    commands.end(record);
}

bool
packScene(const SceneData& scene, SceneBuffers& result)
{
    CommandWriter commands;
    BufferWriter data;

    const uint32_t sceneRecord = commands.begin(Command::Scene);
    commands.buffer.u32(scene.upAxis == UsdGeomTokens->z ? 1 : 0);
    commands.buffer.f32(
      scene.metersPerUnit > 0.0 ? static_cast<float>(scene.metersPerUnit) : 1.0f);
    commands.buffer.f32(static_cast<float>(scene.timeCodesPerSecond));
    commands.end(sceneRecord);
    emitMaterials(commands, data, scene);

    for (size_t nodeIndex = 0; nodeIndex < scene.nodes.size(); ++nodeIndex) {
        const NodeData& node = scene.nodes[nodeIndex];
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, node.name, nameLength);
        const uint32_t matrixOffset = appendMatrix(data, node.localTransform);
        const uint32_t record = commands.begin(Command::TransformNode);
        commands.buffer.u32(static_cast<uint32_t>(nodeIndex + 1));
        commands.buffer.u32(node.parentId);
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(matrixOffset);
        commands.end(record);
    }

    std::vector<std::vector<uint32_t>> boneIds(scene.skeletons.size());
    uint32_t nextBoneId = 1;
    for (size_t skeletonIndex = 0; skeletonIndex < scene.skeletons.size();
         ++skeletonIndex) {
        const SkeletonData& skeleton = scene.skeletons[skeletonIndex];
        boneIds[skeletonIndex].resize(skeleton.joints.size());
        data.align();
        const uint32_t jointsOffset = data.size();
        struct PendingJoint
        {
            uint32_t parent;
            uint32_t boneId;
            uint32_t nameOffset;
            uint32_t nameLength;
            uint32_t matrixOffset;
        };
        std::vector<PendingJoint> pending;
        pending.reserve(skeleton.joints.size());
        for (size_t jointIndex = 0; jointIndex < skeleton.joints.size(); ++jointIndex) {
            const uint32_t boneId = nextBoneId++;
            boneIds[skeletonIndex][jointIndex] = boneId;
            uint32_t nameLength = 0;
            const uint32_t nameOffset =
              appendString(data, skeleton.joints[jointIndex].GetString(), nameLength);
            const uint32_t matrixOffset =
              appendMatrix(data, skeleton.restTransforms[jointIndex]);
            pending.push_back({
                skeleton.parents[jointIndex] >= 0
                  ? static_cast<uint32_t>(skeleton.parents[jointIndex])
                  : kMissingOffset,
                boneId,
                nameOffset,
                nameLength,
                matrixOffset,
            });
        }
        data.align();
        const uint32_t actualJointsOffset = data.size();
        for (const PendingJoint& joint : pending) {
            data.u32(joint.parent);
            data.u32(joint.boneId);
            data.u32(joint.nameOffset);
            data.u32(joint.nameLength);
            data.u32(joint.matrixOffset);
        }
        (void)jointsOffset;
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, skeleton.name, nameLength);
        const uint32_t record = commands.begin(Command::Skeleton);
        commands.buffer.u32(static_cast<uint32_t>(skeletonIndex + 1));
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(static_cast<uint32_t>(skeleton.joints.size()));
        commands.buffer.u32(actualJointsOffset);
        commands.end(record);
    }

    for (size_t meshIndex = 0; meshIndex < scene.meshes.size(); ++meshIndex) {
        const MeshData& mesh = scene.meshes[meshIndex];
        const uint32_t positionsOffset = appendArray(data, mesh.positions);
        const uint32_t normalsOffset = appendArray(data, mesh.normals);
        const uint32_t uvOffset = appendUvs(data, mesh.uvs);
        const uint32_t colorsOffset = appendArray(data, mesh.colors);
        uint32_t joints0Offset = kMissingOffset;
        uint32_t joints1Offset = kMissingOffset;
        uint32_t weights0Offset = kMissingOffset;
        uint32_t weights1Offset = kMissingOffset;
        if (!mesh.joints.empty()) {
            data.align();
            joints0Offset = data.size();
            for (const JointSet& joints : mesh.joints) {
                for (size_t influence = 0; influence < 4; ++influence) {
                    data.u16(joints[influence]);
                }
            }
            if (mesh.influenceCount > 4) {
                data.align();
                joints1Offset = data.size();
                for (const JointSet& joints : mesh.joints) {
                    for (size_t influence = 4; influence < 8; ++influence) {
                        data.u16(joints[influence]);
                    }
                }
            }
        }
        if (!mesh.weights.empty()) {
            data.align();
            weights0Offset = data.size();
            for (const WeightSet& weights : mesh.weights) {
                for (size_t influence = 0; influence < 4; ++influence) {
                    data.f32(weights[influence]);
                }
            }
            if (mesh.influenceCount > 4) {
                data.align();
                weights1Offset = data.size();
                for (const WeightSet& weights : mesh.weights) {
                    for (size_t influence = 4; influence < 8; ++influence) {
                        data.f32(weights[influence]);
                    }
                }
            }
        }
        const uint32_t indicesOffset = appendArray(data, mesh.indices);
        uint32_t geometryFlags = GeometryHasNormals;
        geometryFlags |= !mesh.uvs.empty() ? GeometryHasUv0 : 0;
        geometryFlags |= !mesh.colors.empty() ? GeometryHasColors : 0;
        geometryFlags |= joints0Offset != kMissingOffset ? GeometryHasSkin0 : 0;
        geometryFlags |= joints1Offset != kMissingOffset ? GeometryHasSkin1 : 0;
        const uint32_t geometryRecord = commands.begin(Command::Geometry);
        commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
        commands.buffer.u32(static_cast<uint32_t>(mesh.positions.size()));
        commands.buffer.u32(static_cast<uint32_t>(mesh.indices.size()));
        commands.buffer.u32(geometryFlags);
        commands.buffer.u32(positionsOffset);
        commands.buffer.u32(normalsOffset);
        commands.buffer.u32(kMissingOffset);
        commands.buffer.u32(uvOffset);
        commands.buffer.u32(colorsOffset);
        commands.buffer.u32(joints0Offset);
        commands.buffer.u32(weights0Offset);
        commands.buffer.u32(joints1Offset);
        commands.buffer.u32(weights1Offset);
        commands.buffer.u32(indicesOffset);
        commands.buffer.u32(mesh.influenceCount);
        commands.end(geometryRecord);

        data.align();
        const uint32_t submeshesOffset = data.size();
        for (const SubmeshData& submesh : mesh.submeshes) {
            data.u32(submesh.materialId);
            data.u32(submesh.indexStart);
            data.u32(submesh.indexCount);
            data.u32(0);
            data.u32(static_cast<uint32_t>(mesh.positions.size()));
        }
        uint32_t nameLength = 0;
        const uint32_t nameOffset = appendString(data, mesh.name, nameLength);
        const uint32_t meshRecord = commands.begin(Command::Mesh);
        commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
        commands.buffer.u32(mesh.nodeIds.front());
        commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
        commands.buffer.u32(mesh.submeshes.size() == 1
                              ? mesh.submeshes.front().materialId
                              : kMissingOffset);
        commands.buffer.u32(nameOffset);
        commands.buffer.u32(nameLength);
        commands.buffer.u32(mesh.doubleSided ? 1 : 0);
        commands.buffer.u32(mesh.skeletonId);
        commands.buffer.u32(submeshesOffset);
        commands.buffer.u32(static_cast<uint32_t>(mesh.submeshes.size()));
        commands.end(meshRecord);
        ++result.meshCount;

        for (size_t placement = 1; placement < mesh.nodeIds.size(); ++placement) {
            const std::string name = mesh.name + " instance";
            uint32_t instanceNameLength = 0;
            const uint32_t instanceNameOffset =
              appendString(data, name, instanceNameLength);
            const uint32_t instanceRecord = commands.begin(Command::Instance);
            commands.buffer.u32(static_cast<uint32_t>(meshIndex + 1));
            commands.buffer.u32(mesh.nodeIds[placement]);
            commands.buffer.u32(instanceNameOffset);
            commands.buffer.u32(instanceNameLength);
            commands.end(instanceRecord);
            ++result.instanceCount;
        }

        result.vertexCount += mesh.positions.size();
        result.triangleCount += mesh.indices.size() / 3;
    }

    for (size_t nodeIndex = 0; nodeIndex < scene.nodes.size(); ++nodeIndex) {
        const NodeAnimation& animation = scene.nodes[nodeIndex].animation;
        emitAnimation(commands,
                      data,
                      AnimationTarget::Node,
                      static_cast<uint32_t>(nodeIndex + 1),
                      AnimationProperty::Position,
                      animation.times,
                      animation.translations.data(),
                      animation.translations.size() * sizeof(GfVec3f),
                      3);
        emitAnimation(commands,
                      data,
                      AnimationTarget::Node,
                      static_cast<uint32_t>(nodeIndex + 1),
                      AnimationProperty::RotationQuaternion,
                      animation.times,
                      animation.rotations.data(),
                      animation.rotations.size() * sizeof(GfVec4f),
                      4);
        emitAnimation(commands,
                      data,
                      AnimationTarget::Node,
                      static_cast<uint32_t>(nodeIndex + 1),
                      AnimationProperty::Scaling,
                      animation.times,
                      animation.scales.data(),
                      animation.scales.size() * sizeof(GfVec3f),
                      3);
    }

    for (size_t skeletonIndex = 0; skeletonIndex < scene.skeletons.size();
         ++skeletonIndex) {
        const SkeletonData& skeleton = scene.skeletons[skeletonIndex];
        for (size_t jointIndex = 0; jointIndex < skeleton.joints.size(); ++jointIndex) {
            std::vector<GfMatrix4f> matrices;
            matrices.reserve(skeleton.animation.localTransforms.size());
            for (const VtMatrix4dArray& sample : skeleton.animation.localTransforms) {
                matrices.emplace_back(sample[jointIndex]);
            }
            emitAnimation(commands,
                          data,
                          AnimationTarget::Bone,
                          boneIds[skeletonIndex][jointIndex],
                          AnimationProperty::Matrix,
                          skeleton.animation.times,
                          matrices.data(),
                          matrices.size() * sizeof(GfMatrix4f),
                          16);
        }
    }

    result.commands = commands.finish();
    result.data = std::move(data.bytes);
    result.nodeCount = static_cast<uint32_t>(scene.nodes.size());
    result.materialCount =
      static_cast<uint32_t>(scene.materials.empty() ? 0 : scene.materials.size() - 1);
    return true;
}

} // namespace

bool
buildSceneBuffers(const UsdStageRefPtr& stage, SceneBuffers& result)
{
    if (!stage) {
        return false;
    }

    const auto readStarted = std::chrono::steady_clock::now();
    SceneData scene;
    if (!extractStage(stage, scene)) {
        return false;
    }
    const auto readFinished = std::chrono::steady_clock::now();

    for (MeshData& mesh : scene.meshes) {
        optimizeMesh(mesh);
    }
    const auto preparationFinished = std::chrono::steady_clock::now();

    if (!packScene(scene, result)) {
        return false;
    }
    const auto packingFinished = std::chrono::steady_clock::now();

    result.stageReadMs =
      std::chrono::duration<double, std::milli>(readFinished - readStarted).count();
    result.preparationMs =
      std::chrono::duration<double, std::milli>(preparationFinished - readFinished)
        .count();
    result.packingMs =
      std::chrono::duration<double, std::milli>(packingFinished - preparationFinished)
        .count();
    return true;
}

} // namespace usd_web::direct
