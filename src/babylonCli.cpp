#include "webResolver.h"

#include <pxr/base/plug/registry.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/usd/stage.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

struct Options
{
    std::filesystem::path input;
    std::filesystem::path output;
    std::vector<std::filesystem::path> assetDirectories;
    int iterations = 1;
    int warmupIterations = 0;
    bool optimizeMeshes = true;
    bool embedTextures = true;
    bool json = false;
};

struct SceneStats
{
    uint64_t meshes = 0;
    uint64_t instances = 0;
    uint64_t transformNodes = 0;
    uint64_t materials = 0;
    uint64_t skeletons = 0;
    uint64_t animationGroups = 0;
    uint64_t vertices = 0;
    uint64_t triangles = 0;
};

void
printUsage(const char* executable)
{
    std::cerr
      << "Usage: " << executable << " [options] <input.usd> <output.babylon>\n"
      << "  --asset-dir <path>          Register fallback assets (repeatable)\n"
      << "  --iterations <count>        Timed conversions (default: 1)\n"
      << "  --warmup <count>            Untimed conversions before measurement\n"
      << "  --no-mesh-optimization      Preserve expanded USD vertex/index order\n"
      << "  --no-embed-textures         Omit embedded texture data\n"
      << "  --json                      Print machine-readable statistics\n";
}

bool
parseInt(const char* text, int minimum, int maximum, int& value)
{
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool
parseArguments(int argc, char** argv, Options& options)
{
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next = [&]() -> const char* {
            return i + 1 < argc ? argv[++i] : nullptr;
        };
        if (argument == "--asset-dir") {
            const char* value = next();
            if (!value) {
                return false;
            }
            options.assetDirectories.emplace_back(value);
        } else if (argument == "--iterations") {
            const char* value = next();
            if (!value || !parseInt(value, 1, 1000, options.iterations)) {
                return false;
            }
        } else if (argument == "--warmup") {
            const char* value = next();
            if (!value || !parseInt(value, 0, 1000, options.warmupIterations)) {
                return false;
            }
        } else if (argument == "--no-mesh-optimization") {
            options.optimizeMeshes = false;
        } else if (argument == "--no-embed-textures") {
            options.embedTextures = false;
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else if (!argument.empty() && argument.front() == '-') {
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

bool
convert(const Options& options, double& durationMs, std::string& error)
{
    const auto started = std::chrono::steady_clock::now();
    const UsdStageRefPtr stage = UsdStage::Open(options.input.string());
    if (!stage) {
        error = "Could not open input as a USD stage";
        return false;
    }

    SdfLayer::FileFormatArguments arguments;
    arguments["optimizeMeshes"] = options.optimizeMeshes ? "true" : "false";
    arguments["embedTextures"] = options.embedTextures ? "true" : "false";
    std::error_code filesystemError;
    if (!options.output.parent_path().empty()) {
        std::filesystem::create_directories(options.output.parent_path(), filesystemError);
        if (filesystemError) {
            error = filesystemError.message();
            return false;
        }
    }
    if (!stage->Export(options.output.string(), false, arguments)) {
        error = "USD failed to export the stage as .babylon";
        return false;
    }
    durationMs = std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - started)
                   .count();
    return true;
}

bool
readStats(const std::filesystem::path& path, SceneStats& stats, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open generated .babylon file";
        return false;
    }
    const nlohmann::json scene = nlohmann::json::parse(input, nullptr, false);
    if (scene.is_discarded()) {
        error = "Generated .babylon file is invalid JSON";
        return false;
    }

    const auto countArray = [&](const char* name) -> uint64_t {
        return scene.contains(name) && scene[name].is_array() ? scene[name].size() : 0;
    };
    stats.meshes = countArray("meshes");
    stats.transformNodes = countArray("transformNodes");
    stats.materials = countArray("materials");
    stats.skeletons = countArray("skeletons");
    stats.animationGroups = countArray("animationGroups");

    if (scene.contains("meshes") && scene["meshes"].is_array()) {
        for (const nlohmann::json& mesh : scene["meshes"]) {
            if (mesh.contains("instances") && mesh["instances"].is_array()) {
                stats.instances += mesh["instances"].size();
            }
        }
    }
    if (scene.contains("geometries") && scene["geometries"].is_object()) {
        const nlohmann::json& geometries = scene["geometries"];
        if (geometries.contains("vertexData") && geometries["vertexData"].is_array()) {
            for (const nlohmann::json& geometry : geometries["vertexData"]) {
                if (geometry.contains("positions") && geometry["positions"].is_array()) {
                    stats.vertices += geometry["positions"].size() / 3;
                }
                if (geometry.contains("indices") && geometry["indices"].is_array()) {
                    stats.triangles += geometry["indices"].size() / 3;
                }
            }
        }
    }
    return true;
}

} // namespace

int
main(int argc, char** argv)
{
    Options options;
    if (!parseArguments(argc, argv, options)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }
    if (!std::filesystem::is_regular_file(options.input)) {
        std::cerr << "Input does not exist: " << options.input << '\n';
        return EXIT_FAILURE;
    }

    PlugRegistry::GetInstance().RegisterPlugins(
      std::vector<std::string>{ USD_WEB_BABYLON_PLUGIN_RESOURCES,
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
        double ignored = 0.0;
        if (!convert(options, ignored, error)) {
            std::cerr << error << '\n';
            return EXIT_FAILURE;
        }
    }

    std::vector<double> durations;
    durations.reserve(options.iterations);
    for (int i = 0; i < options.iterations; ++i) {
        double duration = 0.0;
        if (!convert(options, duration, error)) {
            std::cerr << error << '\n';
            return EXIT_FAILURE;
        }
        durations.push_back(duration);
    }

    SceneStats stats;
    if (!readStats(options.output, stats, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    const double average =
      std::accumulate(durations.begin(), durations.end(), 0.0) / durations.size();
    const auto [minimum, maximum] = std::minmax_element(durations.begin(), durations.end());
    const uintmax_t outputBytes = std::filesystem::file_size(options.output);

    if (options.json) {
        const nlohmann::json result = {
            { "input", options.input.string() },
            { "output", options.output.string() },
            { "iterations", options.iterations },
            { "averageMs", average },
            { "minimumMs", *minimum },
            { "maximumMs", *maximum },
            { "outputBytes", outputBytes },
            { "meshes", stats.meshes },
            { "instances", stats.instances },
            { "transformNodes", stats.transformNodes },
            { "materials", stats.materials },
            { "skeletons", stats.skeletons },
            { "animationGroups", stats.animationGroups },
            { "vertices", stats.vertices },
            { "triangles", stats.triangles },
            { "optimized", options.optimizeMeshes },
            { "embeddedTextures", options.embedTextures },
        };
        std::cout << result.dump() << '\n';
    } else {
        std::cout << "Converted " << options.input << " to " << options.output << " in "
                  << average << " ms (" << outputBytes << " bytes, " << stats.vertices
                  << " vertices, " << stats.triangles << " triangles, " << stats.instances
                  << " instances)\n";
    }
    return EXIT_SUCCESS;
}
