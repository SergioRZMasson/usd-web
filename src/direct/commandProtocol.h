#pragma once

#include <cstdint>

namespace usd_web::direct {

constexpr uint32_t kCommandMagic = 0x42445355; // "USDB"
constexpr uint16_t kProtocolVersion = 3;
constexpr uint32_t kMissingOffset = 0xffffffffu;

enum class Command : uint16_t
{
    Scene = 1,
    Texture = 2,
    Material = 3,
    TransformNode = 4,
    Skeleton = 5,
    Geometry = 6,
    Mesh = 7,
    Instance = 8,
    Animation = 9,
};

enum class AnimationTarget : uint32_t
{
    Node = 0,
    Bone = 1,
};

enum class AnimationProperty : uint32_t
{
    Position = 0,
    RotationQuaternion = 1,
    Scaling = 2,
    Matrix = 3,
};

enum MaterialFlags : uint32_t
{
    MaterialDoubleSided = 1u << 0,
    MaterialUnlit = 1u << 1,
    MaterialAlphaBlend = 1u << 2,
};

enum MeshFlags : uint32_t
{
    MeshDoubleSided = 1u << 0,
    MeshLeftHanded = 1u << 1,
};

enum GeometryFlags : uint32_t
{
    GeometryHasNormals = 1u << 0,
    GeometryHasTangents = 1u << 1,
    GeometryHasUv0 = 1u << 2,
    GeometryHasColors = 1u << 3,
    GeometryHasSkin0 = 1u << 4,
    GeometryHasSkin1 = 1u << 5,
};

} // namespace usd_web::direct
