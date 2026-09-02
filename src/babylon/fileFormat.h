#pragma once

#include "api.h"

#include <fileformatutils/sdfUtils.h>

#include <pxr/base/tf/staticTokens.h>
#include <pxr/usd/sdf/fileFormat.h>

#include <iosfwd>

PXR_NAMESPACE_OPEN_SCOPE

#define USDBABYLON_FILE_FORMAT_TOKENS \
    ((Id, "babylon"))                 \
    ((Version, "1.0"))                \
    ((Target, "usd"))

TF_DECLARE_PUBLIC_TOKENS(UsdBabylonFileFormatTokens, USDBABYLON_FILE_FORMAT_TOKENS);
TF_DECLARE_WEAK_AND_REF_PTRS(BabylonData);
TF_DECLARE_WEAK_AND_REF_PTRS(UsdBabylonFileFormat);

class BabylonData : public FileFormatDataBase
{
};

class USDBABYLON_API UsdBabylonFileFormat final : public SdfFileFormat
{
public:
    SdfAbstractDataRefPtr InitData(const FileFormatArguments& args) const override;
    bool CanRead(const std::string& file) const override;
    bool Read(SdfLayer* layer, const std::string& resolvedPath, bool metadataOnly) const override;
    bool ReadFromString(SdfLayer* layer, const std::string& str) const override;
    bool WriteToString(const SdfLayer& layer,
                       std::string* str,
                       const std::string& comment = std::string()) const override;
    bool WriteToStream(const SdfSpecHandle& spec, std::ostream& out, size_t indent) const override;
    bool WriteToFile(
      const SdfLayer& layer,
      const std::string& filePath,
      const std::string& comment = std::string(),
      const FileFormatArguments& args = FileFormatArguments()) const override;

protected:
    SDF_FILE_FORMAT_FACTORY_ACCESS;

    UsdBabylonFileFormat();
    ~UsdBabylonFileFormat() override;
};

PXR_NAMESPACE_CLOSE_SCOPE
