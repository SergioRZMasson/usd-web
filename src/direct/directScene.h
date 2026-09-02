#pragma once

#include <fileformatutils/usdData.h>

#include <cstdint>
#include <vector>

namespace usd_web::direct {

struct SceneBuffers
{
    std::vector<uint8_t> commands;
    std::vector<uint8_t> data;
    double preparationMs = 0.0;
    double packingMs = 0.0;
    uint32_t nodeCount = 0;
    uint32_t meshCount = 0;
    uint32_t instanceCount = 0;
    uint32_t materialCount = 0;
    uint64_t vertexCount = 0;
    uint64_t triangleCount = 0;
};

bool
buildSceneBuffers(adobe::usd::UsdData& usd, SceneBuffers& result);

} // namespace usd_web::direct
