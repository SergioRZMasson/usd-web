#include "fileFormat.h"

#include "babylonExport.h"

#include <fileformatutils/common.h>
#include <fileformatutils/layerRead.h>
#include <fileformatutils/usdData.h>

#include <pxr/usd/sdf/layer.h>

#include <fstream>
#include <sstream>

PXR_NAMESPACE_OPEN_SCOPE

using namespace adobe::usd;

TF_DEFINE_PUBLIC_TOKENS(UsdBabylonFileFormatTokens, USDBABYLON_FILE_FORMAT_TOKENS);

TF_REGISTRY_FUNCTION(TfType)
{
    SDF_DEFINE_FILE_FORMAT(UsdBabylonFileFormat, SdfFileFormat);
}

UsdBabylonFileFormat::UsdBabylonFileFormat()
  : SdfFileFormat(UsdBabylonFileFormatTokens->Id,
                  UsdBabylonFileFormatTokens->Version,
                  UsdBabylonFileFormatTokens->Target,
                  UsdBabylonFileFormatTokens->Id)
{
}

UsdBabylonFileFormat::~UsdBabylonFileFormat() = default;

SdfAbstractDataRefPtr
UsdBabylonFileFormat::InitData(const FileFormatArguments& args) const
{
    BabylonDataRefPtr data(new BabylonData());
    data->parseFromFileFormatArgs(args, "usdBabylon");
    return data;
}

bool
UsdBabylonFileFormat::CanRead(const std::string&) const
{
    return false;
}

bool
UsdBabylonFileFormat::Read(SdfLayer*, const std::string&, bool) const
{
    TF_WARN("The Babylon file format plugin only supports export");
    return false;
}

bool
UsdBabylonFileFormat::ReadFromString(SdfLayer*, const std::string&) const
{
    TF_WARN("The Babylon file format plugin only supports export");
    return false;
}

namespace {

bool
writeBabylon(const SdfLayer& layer,
             std::ostream& output,
             const SdfFileFormat::FileFormatArguments& args)
{
    bool optimizeMeshes = true;
    bool embedTextures = true;
    argReadBool(args, "optimizeMeshes", optimizeMeshes, "usdBabylon");
    argReadBool(args, "embedTextures", embedTextures, "usdBabylon");

    ReadLayerOptions readOptions;
    readOptions.triangulate = true;
    readOptions.maxMeshInfluenceCount = 8;
    readOptions.ignoreInvisible = true;

    UsdData data;
    if (!readLayer(readOptions, layer, data, "usdBabylon")) {
        return false;
    }

    usd_web::babylon::ExportOptions exportOptions;
    exportOptions.optimizeMeshes = optimizeMeshes;
    exportOptions.embedTextures = embedTextures;
    return usd_web::babylon::exportScene(exportOptions, data, output);
}

} // namespace

bool
UsdBabylonFileFormat::WriteToFile(const SdfLayer& layer,
                                  const std::string& filePath,
                                  const std::string&,
                                  const FileFormatArguments& args) const
{
    std::ofstream output(filePath, std::ios::binary);
    return output && writeBabylon(layer, output, args) && output.good();
}

bool
UsdBabylonFileFormat::WriteToString(const SdfLayer& layer,
                                    std::string* str,
                                    const std::string&) const
{
    if (str == nullptr) {
        return false;
    }
    std::ostringstream output;
    if (!writeBabylon(layer, output, FileFormatArguments())) {
        return false;
    }
    *str = output.str();
    return true;
}

bool
UsdBabylonFileFormat::WriteToStream(const SdfSpecHandle&, std::ostream&, size_t) const
{
    return false;
}

PXR_NAMESPACE_CLOSE_SCOPE
