/*
 * usd-web-gltf — JavaScript bindings.
 *
 * Exposes a single operation: hand in a USD asset, get back a GLB.
 *
 * The conversion itself is done entirely by USD and Adobe's usdGltf plugin. Opening the
 * stage runs USD's own composition engine; exporting goes through SdfFileFormat dispatch,
 * which resolves to `UsdGltfFileFormat::WriteToFile` in the statically-linked plugin. No
 * translation logic lives in this file — it only marshals bytes across the JS boundary.
 */

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/usd/stage.h>

#include "webResolver.h"

#include <emscripten/bind.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// --------------------------------------------------------------------------
// Diagnostics
// --------------------------------------------------------------------------

enum LogLevel : int
{
    LogInfo = 0,
    LogWarning = 1,
    LogError = 2,
};

emscripten::val g_logCallback = emscripten::val::undefined();

void
emitLog(LogLevel level, const std::string& message)
{
    if (!g_logCallback.isUndefined() && !g_logCallback.isNull()) {
        g_logCallback(static_cast<int>(level), message);
    }
}

/// Forwards USD's diagnostics to the JS callback instead of stderr.
class JsDiagnosticDelegate final : public TfDiagnosticMgr::Delegate
{
public:
    void IssueError(const TfError& err) override
    {
        emitLog(LogError, err.GetCommentary());
    }

    void IssueFatalError(const TfCallContext& context, const std::string& msg) override
    {
        emitLog(LogError, msg);
    }

    void IssueStatus(const TfStatus& status) override
    {
        emitLog(LogInfo, status.GetCommentary());
    }

    void IssueWarning(const TfWarning& warning) override
    {
        emitLog(LogWarning, warning.GetCommentary());
    }
};

JsDiagnosticDelegate&
diagnosticDelegate()
{
    static JsDiagnosticDelegate delegate;
    static bool registered = [] {
        TfDiagnosticMgr::GetInstance().AddDelegate(&delegate);
        return true;
    }();
    (void)registered;
    return delegate;
}

// --------------------------------------------------------------------------
// Result
// --------------------------------------------------------------------------

/// Owns the encoded output so JS can copy it out of the heap before freeing.
class ConvertResult
{
public:
    bool ok() const { return _ok; }
    std::string error() const { return _error; }
    double durationMs() const { return _durationMs; }

    /// Offset of the payload within HEAPU8.
    uintptr_t dataPtr() const { return reinterpret_cast<uintptr_t>(_data.data()); }
    size_t dataSize() const { return _data.size(); }

    void setError(std::string message)
    {
        _ok = false;
        _error = std::move(message);
    }

    void setData(std::vector<uint8_t> data)
    {
        _ok = true;
        _data = std::move(data);
    }

    void setDuration(double ms) { _durationMs = ms; }

private:
    bool _ok = false;
    std::string _error;
    double _durationMs = 0.0;
    std::vector<uint8_t> _data;
};

/// Reads a file back out of the Emscripten filesystem.
bool
readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    if (size < 0) {
        std::fclose(file);
        return false;
    }
    std::fseek(file, 0, SEEK_SET);
    out.resize(static_cast<size_t>(size));
    const size_t read = size > 0 ? std::fread(out.data(), 1, out.size(), file) : 0;
    std::fclose(file);
    return read == out.size();
}

// --------------------------------------------------------------------------
// API
// --------------------------------------------------------------------------

/// Ensures WebResolver is the resolver Ar instantiates.
///
/// Ar creates its resolver lazily on the first ArGetResolver(), and the preference has to
/// be registered before that happens. Every entry point below routes through here so the
/// ordering holds regardless of which binding the caller touches first.
void
ensureResolver()
{
    static const bool once = [] {
        ArSetPreferredResolver("WebResolver");
        return true;
    }();
    (void)once;
}

/// Converts a USD file already written into the virtual filesystem.
///
/// @param inputPath  Path of the source asset, e.g. "/work/scene.usdz".
/// @param outputPath Path to write, e.g. "/work/scene.glb". The extension selects the
///                   output format, exactly as it would on the command line.
std::unique_ptr<ConvertResult>
convertWithOptions(const std::string& inputPath,
                   const std::string& outputPath,
                   bool optimizeMeshes,
                   bool meshoptCompression,
                   bool embedTextures)
{
    ensureResolver();
    diagnosticDelegate();

    auto result = std::make_unique<ConvertResult>();
    const auto started = std::chrono::steady_clock::now();

    UsdStageRefPtr stage = UsdStage::Open(inputPath);
    if (!stage) {
        result->setError("Could not open '" + inputPath +
                         "' as a USD stage. The file may be corrupt or not a USD asset.");
        return result;
    }
    // Dispatches to UsdGltfFileFormat::WriteToFile through SdfFileFormat, which is only
    // reachable because the plugin registered itself from its static initialisers.
    SdfLayer::FileFormatArguments arguments;
    arguments["embedImages"] = "true";
    arguments["useMaterialExtensions"] = "true";
    arguments["optimizeMeshes"] = optimizeMeshes ? "true" : "false";
    arguments["meshoptCompression"] = meshoptCompression ? "true" : "false";
    arguments["embedTextures"] = embedTextures ? "true" : "false";
    if (!stage->Export(outputPath, false, arguments)) {
        result->setError("USD could not export '" + inputPath + "' to '" + outputPath +
                         "'. Is the requested file format plugin registered?");
        return result;
    }

    std::vector<uint8_t> bytes;
    if (!readFile(outputPath, bytes)) {
        result->setError("The exporter reported success but '" + outputPath +
                         "' could not be read back.");
        return result;
    }
    if (bytes.empty()) {
        result->setError("The exporter produced an empty file.");
        return result;
    }

    const auto finished = std::chrono::steady_clock::now();
    result->setDuration(
      std::chrono::duration<double, std::milli>(finished - started).count());
    result->setData(std::move(bytes));
    return result;
}

std::unique_ptr<ConvertResult>
convert(const std::string& inputPath, const std::string& outputPath)
{
    return convertWithOptions(inputPath, outputPath, true, true, true);
}

void
setLogCallback(emscripten::val callback)
{
    g_logCallback = callback;
    diagnosticDelegate();
}

/// Extensions USD can write, as registered by the linked file format plugins.
/// Useful as a runtime check that the Adobe plugin actually registered.
std::string
getSupportedOutputFormats()
{
    std::set<std::string> extensions;
    for (const std::string& ext : SdfFileFormat::FindAllFileFormatExtensions()) {
        extensions.insert(ext);
    }
    return TfStringJoin(std::vector<std::string>(extensions.begin(), extensions.end()), ",");
}

/// True when the glTF file format plugin resolved successfully.
bool
isGltfPluginAvailable()
{
    return SdfFileFormat::FindByExtension("glb") != nullptr;
}

bool
isBabylonPluginAvailable()
{
    return SdfFileFormat::FindByExtension("babylon") != nullptr;
}

std::string
getUsdVersion()
{
    return TfStringPrintf("%d.%d.%d", PXR_MAJOR_VERSION, PXR_MINOR_VERSION, PXR_PATCH_VERSION);
}

// --------------------------------------------------------------------------
// Asset resolution
// --------------------------------------------------------------------------

/// Indexes every file under `directory` so unresolvable references can fall back to a
/// name match. Call once after writing the caller's files into the virtual filesystem.
void
registerAssetDirectory(const std::string& directory)
{
    ensureResolver();
    WebResolver::RegisterAssetDirectory(directory);
}

/// Drops the fallback index. Call between conversions so one asset's files cannot
/// satisfy another's references.
void
clearAssetIndex()
{
    WebResolver::ClearAssetIndex();
}

/// Enables or disables name-based fallback resolution. Enabled by default.
void
setAssetFallbackEnabled(bool enabled)
{
    WebResolver::SetFallbackEnabled(enabled);
}

/// Comma-separated file names that could not be resolved during the last conversion.
std::string
getUnresolvedAssets()
{
    const std::vector<std::string> names = WebResolver::GetUnresolvedAssetNames();
    return TfStringJoin(names, ",");
}

/// Name of the asset resolver Ar actually instantiated. Useful as a sanity check that
/// WebResolver was picked up rather than the stock ArDefaultResolver.
std::string
getResolverName()
{
    ensureResolver();
    // ArResolver exposes no accessor for its own type, so it is looked up from the
    // instance. This is only used as a sanity check that WebResolver was selected.
    const TfType type = TfType::Find(ArGetUnderlyingResolver());
    return type.IsUnknown() ? std::string("<unknown>") : type.GetTypeName();
}

} // namespace

EMSCRIPTEN_BINDINGS(usd_web_gltf)
{
    emscripten::class_<ConvertResult>("ConvertResult")
        .function("ok", &ConvertResult::ok)
        .function("error", &ConvertResult::error)
        .function("dataPtr", &ConvertResult::dataPtr)
        .function("dataSize", &ConvertResult::dataSize)
        .function("durationMs", &ConvertResult::durationMs);

    emscripten::function("convert", &convert);
    emscripten::function("convertWithOptions", &convertWithOptions);
    emscripten::function("setLogCallback", &setLogCallback);
    emscripten::function("getSupportedOutputFormats", &getSupportedOutputFormats);
    emscripten::function("isGltfPluginAvailable", &isGltfPluginAvailable);
    emscripten::function("isBabylonPluginAvailable", &isBabylonPluginAvailable);
    emscripten::function("getUsdVersion", &getUsdVersion);

    emscripten::function("registerAssetDirectory", &registerAssetDirectory);
    emscripten::function("clearAssetIndex", &clearAssetIndex);
    emscripten::function("setAssetFallbackEnabled", &setAssetFallbackEnabled);
    emscripten::function("getUnresolvedAssets", &getUnresolvedAssets);
    emscripten::function("getResolverName", &getResolverName);
}
