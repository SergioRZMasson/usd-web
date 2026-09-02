#pragma once

#include "api.h"

#include <fileformatutils/usdData.h>

#include <iosfwd>

namespace usd_web::babylon {

struct ExportOptions
{
    bool optimizeMeshes = true;
    bool embedTextures = true;
};

struct ExportPhaseTiming
{
    double meshPreparationMs = 0.0;
    double serializationMs = 0.0;
};

struct FileFormatTiming
{
    double readLayerMs = 0.0;
    double meshPreparationMs = 0.0;
    double serializationMs = 0.0;
    double totalMs = 0.0;
};

bool
exportScene(const ExportOptions& options,
            adobe::usd::UsdData& data,
            std::ostream& output,
            ExportPhaseTiming* timing = nullptr);

USDBABYLON_API FileFormatTiming
getLastFileFormatTiming();

} // namespace usd_web::babylon
