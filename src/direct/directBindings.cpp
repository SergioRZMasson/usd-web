#include "directScene.h"

#include "../webResolver.h"

#include <fileformatutils/layerRead.h>

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/usd/stage.h>

#include <emscripten/bind.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

emscripten::val g_logCallback = emscripten::val::undefined();

void
emitLog(int level, const std::string& message)
{
    if (!g_logCallback.isUndefined() && !g_logCallback.isNull()) {
        g_logCallback(level, message);
    }
}

class DiagnosticDelegate final : public TfDiagnosticMgr::Delegate
{
public:
    void IssueError(const TfError& error) override { emitLog(2, error.GetCommentary()); }
    void IssueFatalError(const TfCallContext&, const std::string& message) override
    {
        emitLog(2, message);
    }
    void IssueStatus(const TfStatus& status) override { emitLog(0, status.GetCommentary()); }
    void IssueWarning(const TfWarning& warning) override { emitLog(1, warning.GetCommentary()); }
};

DiagnosticDelegate&
diagnosticDelegate()
{
    static DiagnosticDelegate delegate;
    static const bool registered = [] {
        TfDiagnosticMgr::GetInstance().AddDelegate(&delegate);
        return true;
    }();
    (void)registered;
    return delegate;
}

void
ensureResolver()
{
    static const bool initialized = [] {
        ArSetPreferredResolver("WebResolver");
        return true;
    }();
    (void)initialized;
}

class DirectResult
{
public:
    bool ok() const { return m_ok; }
    const std::string& error() const { return m_error; }
    uintptr_t commandPtr() const { return reinterpret_cast<uintptr_t>(m_commands.data()); }
    size_t commandSize() const { return m_commands.size(); }
    uintptr_t dataPtr() const { return reinterpret_cast<uintptr_t>(m_data.data()); }
    size_t dataSize() const { return m_data.size(); }
    double totalMs() const { return m_totalMs; }
    double stageOpenMs() const { return m_stageOpenMs; }
    double stageReadMs() const { return m_stageReadMs; }
    double preparationMs() const { return m_preparationMs; }
    double packingMs() const { return m_packingMs; }
    uint32_t nodeCount() const { return m_nodeCount; }
    uint32_t meshCount() const { return m_meshCount; }
    uint32_t instanceCount() const { return m_instanceCount; }
    uint32_t materialCount() const { return m_materialCount; }
    double vertexCount() const { return static_cast<double>(m_vertexCount); }
    double triangleCount() const { return static_cast<double>(m_triangleCount); }

    void fail(std::string message) { m_error = std::move(message); }

    void complete(usd_web::direct::SceneBuffers&& buffers,
                  double totalMs,
                  double stageOpenMs,
                  double stageReadMs)
    {
        m_ok = true;
        m_commands = std::move(buffers.commands);
        m_data = std::move(buffers.data);
        m_totalMs = totalMs;
        m_stageOpenMs = stageOpenMs;
        m_stageReadMs = stageReadMs;
        m_preparationMs = buffers.preparationMs;
        m_packingMs = buffers.packingMs;
        m_nodeCount = buffers.nodeCount;
        m_meshCount = buffers.meshCount;
        m_instanceCount = buffers.instanceCount;
        m_materialCount = buffers.materialCount;
        m_vertexCount = buffers.vertexCount;
        m_triangleCount = buffers.triangleCount;
    }

private:
    bool m_ok = false;
    std::string m_error;
    std::vector<uint8_t> m_commands;
    std::vector<uint8_t> m_data;
    double m_totalMs = 0.0;
    double m_stageOpenMs = 0.0;
    double m_stageReadMs = 0.0;
    double m_preparationMs = 0.0;
    double m_packingMs = 0.0;
    uint32_t m_nodeCount = 0;
    uint32_t m_meshCount = 0;
    uint32_t m_instanceCount = 0;
    uint32_t m_materialCount = 0;
    uint64_t m_vertexCount = 0;
    uint64_t m_triangleCount = 0;
};

std::unique_ptr<DirectResult>
extract(const std::string& inputPath)
{
    ensureResolver();
    diagnosticDelegate();

    auto result = std::make_unique<DirectResult>();
    const auto totalStarted = std::chrono::steady_clock::now();
    const auto openStarted = totalStarted;
    const UsdStageRefPtr stage = UsdStage::Open(inputPath);
    const auto openFinished = std::chrono::steady_clock::now();
    if (!stage) {
        result->fail("Could not open '" + inputPath + "' as a USD stage.");
        return result;
    }

    adobe::usd::ReadLayerOptions options;
    options.triangulate = true;
    options.ignoreInvisible = true;
    options.maxMeshInfluenceCount = 8;
    adobe::usd::UsdData usd;
    const auto readStarted = std::chrono::steady_clock::now();
    if (!adobe::usd::readStage(options, stage, usd, "directBabylon")) {
        result->fail("Could not extract the composed USD stage.");
        return result;
    }
    const auto readFinished = std::chrono::steady_clock::now();

    usd_web::direct::SceneBuffers buffers;
    if (!usd_web::direct::buildSceneBuffers(usd, buffers)) {
        result->fail("Could not build the Babylon command buffers.");
        return result;
    }
    const auto finished = std::chrono::steady_clock::now();
    result->complete(
      std::move(buffers),
      std::chrono::duration<double, std::milli>(finished - totalStarted).count(),
      std::chrono::duration<double, std::milli>(openFinished - openStarted).count(),
      std::chrono::duration<double, std::milli>(readFinished - readStarted).count());
    return result;
}

void
setLogCallback(emscripten::val callback)
{
    g_logCallback = callback;
    diagnosticDelegate();
}

void
registerAssetDirectory(const std::string& directory)
{
    ensureResolver();
    WebResolver::RegisterAssetDirectory(directory);
}

void
clearAssetIndex()
{
    WebResolver::ClearAssetIndex();
}

void
setAssetFallbackEnabled(bool enabled)
{
    WebResolver::SetFallbackEnabled(enabled);
}

std::string
getUnresolvedAssets()
{
    return TfStringJoin(WebResolver::GetUnresolvedAssetNames(), ",");
}

std::string
getResolverName()
{
    ensureResolver();
    const TfType type = TfType::Find(ArGetUnderlyingResolver());
    return type.IsUnknown() ? std::string("<unknown>") : type.GetTypeName();
}

std::string
getUsdVersion()
{
    return TfStringPrintf("%d.%d.%d", PXR_MAJOR_VERSION, PXR_MINOR_VERSION, PXR_PATCH_VERSION);
}

} // namespace

EMSCRIPTEN_BINDINGS(openusd_babylon_direct)
{
    emscripten::class_<DirectResult>("DirectResult")
      .function("ok", &DirectResult::ok)
      .function("error", &DirectResult::error)
      .function("commandPtr", &DirectResult::commandPtr)
      .function("commandSize", &DirectResult::commandSize)
      .function("dataPtr", &DirectResult::dataPtr)
      .function("dataSize", &DirectResult::dataSize)
      .function("totalMs", &DirectResult::totalMs)
      .function("stageOpenMs", &DirectResult::stageOpenMs)
      .function("stageReadMs", &DirectResult::stageReadMs)
      .function("preparationMs", &DirectResult::preparationMs)
      .function("packingMs", &DirectResult::packingMs)
      .function("nodeCount", &DirectResult::nodeCount)
      .function("meshCount", &DirectResult::meshCount)
      .function("instanceCount", &DirectResult::instanceCount)
      .function("materialCount", &DirectResult::materialCount)
      .function("vertexCount", &DirectResult::vertexCount)
      .function("triangleCount", &DirectResult::triangleCount);

    emscripten::function("extract", &extract);
    emscripten::function("setLogCallback", &setLogCallback);
    emscripten::function("registerAssetDirectory", &registerAssetDirectory);
    emscripten::function("clearAssetIndex", &clearAssetIndex);
    emscripten::function("setAssetFallbackEnabled", &setAssetFallbackEnabled);
    emscripten::function("getUnresolvedAssets", &getUnresolvedAssets);
    emscripten::function("getResolverName", &getResolverName);
    emscripten::function("getUsdVersion", &getUsdVersion);
}
