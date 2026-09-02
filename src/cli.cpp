#include "webResolver.h"

#include <exportTiming.h>
#include <pxr/base/plug/registry.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <json.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

struct CliOptions
{
    std::filesystem::path input;
    std::filesystem::path output;
    std::vector<std::filesystem::path> assetDirectories;
    int iterations = 1;
    int warmupIterations = 0;
    bool json = false;
    bool optimizeMeshes = true;
    bool meshoptCompression = false;
};

struct GlbStats
{
    size_t nodes = 0;
    size_t meshes = 0;
    size_t meshInstances = 0;
    size_t primitives = 0;
    uint64_t vertices = 0;
    uint64_t drawVertices = 0;
    uint64_t triangles = 0;
    uint64_t meshoptBufferViews = 0;
    uint64_t meshoptDecodedBytes = 0;
};

struct ConversionTiming
{
    double totalMs = 0.0;
    double stageOpenMs = 0.0;
    double stageFlattenMs = 0.0;
    double exportDispatchMs = 0.0;
    double pluginReadMs = 0.0;
    double transcodeMs = 0.0;
    double serializeMs = 0.0;
};

void
printUsage(const char* executable)
{
    std::cerr
      << "Usage: " << executable << " [options] <input.usd> <output.glb>\n"
      << "Options:\n"
      << "  --asset-dir <path>          Register assets for fallback resolution (repeatable)\n"
      << "  --iterations <count>        Timed conversions to run (default: 1)\n"
      << "  --warmup <count>            Untimed conversions before measurement\n"
      << "  --no-mesh-optimization      Preserve the exporter's original vertex/index order\n"
      << "  --meshopt-compression       Emit EXT_meshopt_compression buffer views\n"
      << "  --json                      Print machine-readable benchmark results\n";
}

bool
parsePositiveInt(const std::string& text, int minimum, int maximum, int& result)
{
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    result = static_cast<int>(parsed);
    return true;
}

bool
parseArguments(int argc, char** argv, CliOptions& options)
{
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto readValue = [&](const char* optionName) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << optionName << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (argument == "--asset-dir") {
            const char* value = readValue("--asset-dir");
            if (!value) {
                return false;
            }
            options.assetDirectories.emplace_back(value);
        } else if (argument == "--iterations") {
            const char* value = readValue("--iterations");
            if (!value || !parsePositiveInt(value, 1, 1000, options.iterations)) {
                std::cerr << "--iterations must be between 1 and 1000\n";
                return false;
            }
        } else if (argument == "--warmup") {
            const char* value = readValue("--warmup");
            if (!value || !parsePositiveInt(value, 0, 1000, options.warmupIterations)) {
                std::cerr << "--warmup must be between 0 and 1000\n";
                return false;
            }
        } else if (argument == "--no-mesh-optimization") {
            options.optimizeMeshes = false;
        } else if (argument == "--meshopt-compression") {
            options.meshoptCompression = true;
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else if (!argument.empty() && argument.front() == '-') {
            std::cerr << "Unknown option: " << argument << '\n';
            return false;
        } else {
            positional.push_back(argument);
        }
    }

    if (positional.size() != 2) {
        return false;
    }
    options.input = positional[0];
    options.output = positional[1];
    return true;
}

uint32_t
readUint32(const std::vector<uint8_t>& data, size_t offset)
{
    return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) |
           (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
}

bool
readAccessorCount(const nlohmann::json& accessors, int index, uint64_t& count)
{
    if (index < 0 || static_cast<size_t>(index) >= accessors.size()) {
        return false;
    }
    const nlohmann::json& accessor = accessors[static_cast<size_t>(index)];
    if (!accessor.contains("count") || !accessor["count"].is_number_unsigned()) {
        return false;
    }
    count = accessor["count"].get<uint64_t>();
    return true;
}

bool
readUnsigned(const nlohmann::json& object, const char* property, size_t& value)
{
    if (!object.contains(property) || !object[property].is_number_unsigned()) {
        return false;
    }
    value = object[property].get<size_t>();
    return true;
}

bool
validateMeshoptBufferViews(const nlohmann::json& document,
                           const std::vector<uint8_t>& bytes,
                           uint32_t jsonLength,
                           GlbStats& stats,
                           std::string& error)
{
    if (!document.contains("bufferViews") || !document["bufferViews"].is_array()) {
        return true;
    }
    const size_t binaryChunkHeader = 20 + jsonLength;
    constexpr uint32_t binChunkType = 0x004E4942;
    if (binaryChunkHeader + 8 > bytes.size() ||
        readUint32(bytes, binaryChunkHeader + 4) != binChunkType) {
        error = "Missing GLB binary chunk";
        return false;
    }
    const size_t binaryLength = readUint32(bytes, binaryChunkHeader);
    const size_t binaryOffset = binaryChunkHeader + 8;
    if (binaryLength > bytes.size() - binaryOffset) {
        error = "Invalid GLB binary chunk length";
        return false;
    }

    for (const nlohmann::json& bufferView : document["bufferViews"]) {
        if (!bufferView.contains("extensions") || !bufferView["extensions"].is_object()) {
            continue;
        }
        const nlohmann::json& extensions = bufferView["extensions"];
        if (!extensions.contains("EXT_meshopt_compression") ||
            !extensions["EXT_meshopt_compression"].is_object()) {
            continue;
        }
        const nlohmann::json& extension = extensions["EXT_meshopt_compression"];
        size_t sourceOffset = 0;
        size_t sourceLength = 0;
        size_t stride = 0;
        size_t count = 0;
        if (!readUnsigned(extension, "byteLength", sourceLength) ||
            !readUnsigned(extension, "byteStride", stride) ||
            !readUnsigned(extension, "count", count)) {
            error = "Incomplete EXT_meshopt_compression metadata";
            return false;
        }
        if (extension.contains("byteOffset")) {
            if (!readUnsigned(extension, "byteOffset", sourceOffset)) {
                error = "Invalid EXT_meshopt_compression byteOffset";
                return false;
            }
        }
        if (sourceOffset > binaryLength || sourceLength > binaryLength - sourceOffset ||
            stride == 0 || count > std::numeric_limits<size_t>::max() / stride) {
            error = "EXT_meshopt_compression data is out of range";
            return false;
        }

        std::vector<uint8_t> decoded(count * stride);
        const uint8_t* source = bytes.data() + binaryOffset + sourceOffset;
        const std::string mode = extension.value("mode", "");
        int decodeResult = -1;
        if (mode == "ATTRIBUTES") {
            decodeResult = meshopt_decodeVertexBuffer(
              decoded.data(), count, stride, source, sourceLength);
        } else if (mode == "TRIANGLES") {
            decodeResult = meshopt_decodeIndexBuffer(
              decoded.data(), count, stride, source, sourceLength);
        } else if (mode == "INDICES") {
            decodeResult = meshopt_decodeIndexSequence(
              decoded.data(), count, stride, source, sourceLength);
        }
        if (decodeResult != 0) {
            error = "Failed to decode an EXT_meshopt_compression buffer view";
            return false;
        }
        ++stats.meshoptBufferViews;
        stats.meshoptDecodedBytes += decoded.size();
    }
    return true;
}

bool
readGlbStats(const std::filesystem::path& path, GlbStats& stats, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open the file";
        return false;
    }
    std::vector<uint8_t> bytes(
      (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    constexpr uint32_t glbMagic = 0x46546C67;
    constexpr uint32_t jsonChunkType = 0x4E4F534A;
    if (bytes.size() < 20 || readUint32(bytes, 0) != glbMagic ||
        readUint32(bytes, 16) != jsonChunkType) {
        error = "Invalid GLB header";
        return false;
    }
    const uint32_t jsonLength = readUint32(bytes, 12);
    if (jsonLength > bytes.size() - 20) {
        error = "Invalid GLB JSON chunk length";
        return false;
    }
    const std::string jsonText(
      reinterpret_cast<const char*>(bytes.data() + 20), jsonLength);
    const nlohmann::json document = nlohmann::json::parse(jsonText, nullptr, false);
    if (document.is_discarded()) {
        error = "Invalid GLB JSON";
        return false;
    }
    if (!validateMeshoptBufferViews(document, bytes, jsonLength, stats, error)) {
        return false;
    }

    const nlohmann::json emptyArray = nlohmann::json::array();
    const nlohmann::json& nodes =
      document.contains("nodes") && document["nodes"].is_array() ? document["nodes"]
                                                                 : emptyArray;
    const nlohmann::json& meshes =
      document.contains("meshes") && document["meshes"].is_array() ? document["meshes"]
                                                                   : emptyArray;
    const nlohmann::json& accessors =
      document.contains("accessors") && document["accessors"].is_array()
        ? document["accessors"]
        : emptyArray;

    stats.nodes = nodes.size();
    stats.meshes = meshes.size();
    for (const nlohmann::json& node : nodes) {
        if (node.contains("mesh") && node["mesh"].is_number_integer()) {
            ++stats.meshInstances;
        }
    }

    std::set<int> positionAccessors;
    for (const nlohmann::json& mesh : meshes) {
        if (!mesh.contains("primitives") || !mesh["primitives"].is_array()) {
            continue;
        }
        for (const nlohmann::json& primitive : mesh["primitives"]) {
            ++stats.primitives;
            int positionAccessor = -1;
            if (primitive.contains("attributes") && primitive["attributes"].is_object()) {
                const nlohmann::json& attributes = primitive["attributes"];
                if (attributes.contains("POSITION") &&
                    attributes["POSITION"].is_number_integer()) {
                    positionAccessor = attributes["POSITION"].get<int>();
                }
            }
            uint64_t positionCount = 0;
            if (readAccessorCount(accessors, positionAccessor, positionCount)) {
                const uint64_t count = positionCount;
                stats.drawVertices += count;
                positionAccessors.insert(positionAccessor);
            }

            uint64_t indexCount = 0;
            if (primitive.contains("indices") && primitive["indices"].is_number_integer() &&
                readAccessorCount(accessors, primitive["indices"].get<int>(), indexCount)) {
                stats.triangles += indexCount / 3;
            } else {
                stats.triangles += positionCount / 3;
            }
        }
    }
    for (const int accessor : positionAccessors) {
        uint64_t count = 0;
        if (readAccessorCount(accessors, accessor, count)) {
            stats.vertices += count;
        }
    }
    return true;
}

bool
convert(const CliOptions& options, ConversionTiming& timing, std::string& error)
{
    const auto started = std::chrono::steady_clock::now();
    const auto openStarted = std::chrono::steady_clock::now();
    const UsdStageRefPtr stage = UsdStage::Open(options.input.string());
    const auto openFinished = std::chrono::steady_clock::now();
    timing.stageOpenMs =
      std::chrono::duration<double, std::milli>(openFinished - openStarted).count();
    if (!stage) {
        error = "Could not open input as a USD stage";
        return false;
    }
    const auto flattenStarted = std::chrono::steady_clock::now();
    const SdfLayerRefPtr flattened = stage->Flatten(false);
    const auto flattenFinished = std::chrono::steady_clock::now();
    timing.stageFlattenMs =
      std::chrono::duration<double, std::milli>(flattenFinished - flattenStarted).count();
    if (!flattened) {
        error = "Could not flatten the USD stage";
        return false;
    }

    SdfLayer::FileFormatArguments arguments;
    arguments["embedImages"] = "true";
    arguments["useMaterialExtensions"] = "true";
    arguments["optimizeMeshes"] = options.optimizeMeshes ? "true" : "false";
    arguments["meshoptCompression"] = options.meshoptCompression ? "true" : "false";

    std::error_code filesystemError;
    if (!options.output.parent_path().empty()) {
        std::filesystem::create_directories(options.output.parent_path(), filesystemError);
        if (filesystemError) {
            error = "Could not create output directory: " + filesystemError.message();
            return false;
        }
    }

    const auto exportStarted = std::chrono::steady_clock::now();
    const bool exported = flattened->Export(options.output.string(), std::string(), arguments);
    const auto exportFinished = std::chrono::steady_clock::now();
    timing.exportDispatchMs =
      std::chrono::duration<double, std::milli>(exportFinished - exportStarted).count();
    if (!exported) {
        error = "USD failed to export the stage as GLB";
        return false;
    }
    const adobe::usd::ExportTiming pluginTiming = adobe::usd::getLastExportTiming();
    timing.pluginReadMs = pluginTiming.readLayerMs;
    timing.transcodeMs = pluginTiming.transcodeMs;
    timing.serializeMs = pluginTiming.writeMs;

    const auto finished = std::chrono::steady_clock::now();
    timing.totalMs = std::chrono::duration<double, std::milli>(finished - started).count();
    return true;
}

} // namespace

int
main(int argc, char** argv)
{
    CliOptions options;
    if (!parseArguments(argc, argv, options)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }
    if (!std::filesystem::is_regular_file(options.input)) {
        std::cerr << "Input does not exist: " << options.input << '\n';
        return EXIT_FAILURE;
    }

    PlugRegistry::GetInstance().RegisterPlugins(
      std::vector<std::string>{ USD_WEB_GLTF_PLUGIN_RESOURCES,
                                USD_WEB_RESOLVER_PLUGIN_RESOURCES });
    ArSetPreferredResolver("WebResolver");

    if (options.assetDirectories.empty()) {
        options.assetDirectories.push_back(options.input.parent_path());
    }
    for (const std::filesystem::path& directory : options.assetDirectories) {
        WebResolver::RegisterAssetDirectory(directory.string());
    }

    std::string error;
    for (int i = 0; i < options.warmupIterations; ++i) {
        ConversionTiming ignored;
        if (!convert(options, ignored, error)) {
            std::cerr << error << '\n';
            return EXIT_FAILURE;
        }
    }

    std::vector<ConversionTiming> timings;
    timings.reserve(options.iterations);
    for (int i = 0; i < options.iterations; ++i) {
        ConversionTiming timing;
        if (!convert(options, timing, error)) {
            std::cerr << error << '\n';
            return EXIT_FAILURE;
        }
        timings.push_back(timing);
    }

    GlbStats stats;
    if (!readGlbStats(options.output, stats, error)) {
        std::cerr << "Could not inspect generated GLB: " << error << '\n';
        return EXIT_FAILURE;
    }

    const auto averageTiming = [&](auto member) {
        double total = 0.0;
        for (const ConversionTiming& timing : timings) {
            total += timing.*member;
        }
        return total / static_cast<double>(timings.size());
    };
    const double average = averageTiming(&ConversionTiming::totalMs);
    const auto [minimum, maximum] = std::minmax_element(
      timings.begin(), timings.end(), [](const ConversionTiming& left, const ConversionTiming& right) {
          return left.totalMs < right.totalMs;
      });
    const uintmax_t inputBytes = std::filesystem::file_size(options.input);
    const uintmax_t outputBytes = std::filesystem::file_size(options.output);

    if (options.json) {
        const nlohmann::json result = {
            { "input", options.input.string() },
            { "output", options.output.string() },
            { "iterations", options.iterations },
            { "averageMs", average },
            { "minimumMs", minimum->totalMs },
            { "maximumMs", maximum->totalMs },
            { "stageOpenMs", averageTiming(&ConversionTiming::stageOpenMs) },
            { "stageFlattenMs", averageTiming(&ConversionTiming::stageFlattenMs) },
            { "exportDispatchMs", averageTiming(&ConversionTiming::exportDispatchMs) },
            { "pluginReadMs", averageTiming(&ConversionTiming::pluginReadMs) },
            { "transcodeMs", averageTiming(&ConversionTiming::transcodeMs) },
            { "serializeMs", averageTiming(&ConversionTiming::serializeMs) },
            { "inputBytes", inputBytes },
            { "outputBytes", outputBytes },
            { "nodes", stats.nodes },
            { "meshes", stats.meshes },
            { "meshInstances", stats.meshInstances },
            { "primitives", stats.primitives },
            { "vertices", stats.vertices },
            { "drawVertices", stats.drawVertices },
            { "triangles", stats.triangles },
            { "meshoptBufferViews", stats.meshoptBufferViews },
            { "meshoptDecodedBytes", stats.meshoptDecodedBytes },
            { "optimized", options.optimizeMeshes },
            { "meshoptCompression", options.meshoptCompression },
        };
        std::cout << result.dump() << '\n';
    } else {
        std::cout << std::fixed << std::setprecision(1)
                  << "Converted " << options.input << " -> " << options.output << '\n'
                  << "Time: " << average << " ms average (" << minimum->totalMs << '-'
                  << maximum->totalMs
                  << " ms), output: " << outputBytes << " bytes\n"
                  << "Phases: open " << averageTiming(&ConversionTiming::stageOpenMs)
                  << " ms, flatten " << averageTiming(&ConversionTiming::stageFlattenMs)
                  << " ms, extract " << averageTiming(&ConversionTiming::pluginReadMs)
                  << " ms, transcode " << averageTiming(&ConversionTiming::transcodeMs)
                  << " ms, serialize " << averageTiming(&ConversionTiming::serializeMs)
                  << " ms\n"
                  << "Geometry: " << stats.meshes << " meshes, " << stats.meshInstances
                  << " instances, " << stats.vertices << " vertices, " << stats.triangles
                  << " triangles, " << stats.primitives << " draw primitives\n";
    }
    return EXIT_SUCCESS;
}
