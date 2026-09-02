#pragma once

#include <fileformatutils/usdData.h>

#include <iosfwd>

namespace usd_web::babylon {

struct ExportOptions
{
    bool optimizeMeshes = true;
    bool embedTextures = true;
};

bool
exportScene(const ExportOptions& options, adobe::usd::UsdData& data, std::ostream& output);

} // namespace usd_web::babylon
