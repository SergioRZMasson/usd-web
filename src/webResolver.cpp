/*
 * Asset resolution for the browser.
 *
 * USD assets routinely reference other files, and in a browser those references have to
 * be satisfied from the Emscripten virtual filesystem rather than a real disk. Two cases
 * come up in practice:
 *
 *   1. Relative references — `@mycobot_280_jn.usda@` anchored to the referencing layer.
 *      These resolve correctly as long as the sibling files have been written into the
 *      same virtual directory as the root layer. ArDefaultResolver handles it.
 *
 *   2. Absolute references baked in on another machine — for example
 *      `@/Users/someone/Desktop/TravelDemo.usd@`. These can never resolve as authored:
 *      the path describes a directory that only existed on the authoring workstation.
 *      ArDefaultResolver correctly reports them as missing, and the payload is dropped.
 *
 * Case 2 is common enough in real-world USD (Omniverse and DCC exports frequently embed
 * absolute paths) that failing outright is unhelpful. WebResolver therefore falls back to
 * matching an unresolvable reference by *file name* against the files the caller supplied,
 * which is the same relocation strategy DCC tools use when a project moves.
 *
 * The fallback is deliberately conservative:
 *   - it only runs after normal resolution has failed, so correct references are never
 *     redirected;
 *   - it matches on the full file name, not a fuzzy match;
 *   - an ambiguous name (two supplied files with the same base name) is reported and left
 *     unresolved, rather than silently picking one.
 */

#include "webResolver.h"

#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/ar/defineResolver.h>

#include <map>
#include <mutex>
#include <set>

PXR_NAMESPACE_OPEN_SCOPE

AR_DEFINE_RESOLVER(WebResolver, ArDefaultResolver);

namespace {

/// Index of supplied files, keyed by lowercase file name.
///
/// Shared process-wide because ArResolver is a singleton created by Ar, so the index
/// cannot be handed to the instance through a constructor.
struct FallbackIndex
{
    std::mutex mutex;
    /// file name (lowercased) -> resolved virtual paths carrying that name
    std::map<std::string, std::set<std::string>> byName;
    /// File names that could not be resolved, for reporting back to the caller.
    std::set<std::string> unresolved;
    /// Names already warned about, so each is reported once per conversion.
    std::set<std::string> warned;
    bool enabled = true;
};

FallbackIndex&
index()
{
    static FallbackIndex instance;
    return instance;
}

} // namespace

void
WebResolver::RegisterAssetDirectory(const std::string& directory)
{
    if (!TfIsDir(directory)) {
        return;
    }

    std::vector<std::string> dirNames;
    std::vector<std::string> fileNames;
    std::vector<std::string> symlinkNames;
    if (!TfReadDir(directory, &dirNames, &fileNames, &symlinkNames)) {
        return;
    }

    {
        FallbackIndex& idx = index();
        std::lock_guard<std::mutex> lock(idx.mutex);
        for (const std::string& fileName : fileNames) {
            const std::string full = TfStringCatPaths(directory, fileName);
            idx.byName[TfStringToLower(fileName)].insert(full);
        }
    }

    // Nested directories are indexed too, so a reference can be satisfied by a file the
    // caller supplied under a subdirectory.
    for (const std::string& subDir : dirNames) {
        RegisterAssetDirectory(TfStringCatPaths(directory, subDir));
    }
}

void
WebResolver::ClearAssetIndex()
{
    FallbackIndex& idx = index();
    std::lock_guard<std::mutex> lock(idx.mutex);
    idx.byName.clear();
    idx.unresolved.clear();
    idx.warned.clear();
}

void
WebResolver::SetFallbackEnabled(bool enabled)
{
    FallbackIndex& idx = index();
    std::lock_guard<std::mutex> lock(idx.mutex);
    idx.enabled = enabled;
}

std::vector<std::string>
WebResolver::GetUnresolvedAssetNames()
{
    FallbackIndex& idx = index();
    std::lock_guard<std::mutex> lock(idx.mutex);
    return std::vector<std::string>(idx.unresolved.begin(), idx.unresolved.end());
}

ArResolvedPath
WebResolver::_Resolve(const std::string& assetPath) const
{
    // Normal resolution first: anything that resolves conventionally is never redirected.
    if (ArResolvedPath resolved = ArDefaultResolver::_Resolve(assetPath)) {
        return resolved;
    }

    if (assetPath.empty()) {
        return ArResolvedPath();
    }

    const std::string baseName = TfGetBaseName(assetPath);
    if (baseName.empty()) {
        return ArResolvedPath();
    }

    FallbackIndex& idx = index();
    std::lock_guard<std::mutex> lock(idx.mutex);

    // Recorded before the fallback is attempted, so the caller learns about the
    // reference whether or not it ends up being recovered. Removed again on success.
    idx.unresolved.insert(baseName);

    /// Warns once per file name, keeping the log readable when a layer is referenced
    /// many times.
    const auto warnOnce = [&idx, &baseName](const char* fmt, auto&&... args) {
        if (idx.warned.insert(baseName).second) {
            TF_WARN(fmt, args...);
        }
    };

    if (!idx.enabled) {
        warnOnce("Could not resolve '%s'. Supply it alongside the root layer, or enable "
                 "file-name fallback resolution.",
                 assetPath.c_str());
        return ArResolvedPath();
    }

    const auto it = idx.byName.find(TfStringToLower(baseName));
    if (it == idx.byName.end() || it->second.empty()) {
        warnOnce("Could not resolve '%s', and no supplied file is named '%s'. "
                 "Provide it alongside the root layer to satisfy this reference.",
                 assetPath.c_str(),
                 baseName.c_str());
        return ArResolvedPath();
    }

    if (it->second.size() > 1) {
        // Picking arbitrarily here would produce a silently wrong scene.
        warnOnce("Could not resolve '%s': %zu supplied files are named '%s', so the "
                 "reference is ambiguous. Preserve the original directory structure "
                 "to disambiguate.",
                 assetPath.c_str(),
                 it->second.size(),
                 baseName.c_str());
        return ArResolvedPath();
    }

    const std::string& candidate = *it->second.begin();
    warnOnce("Resolved '%s' to '%s' by file name. The reference uses an absolute path "
             "from another machine.",
             assetPath.c_str(),
             candidate.c_str());
    idx.unresolved.erase(baseName);
    return ArResolvedPath(candidate);
}

PXR_NAMESPACE_CLOSE_SCOPE
