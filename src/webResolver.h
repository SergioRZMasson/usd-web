/*
 * WebResolver — asset resolution for USD running in a browser.
 *
 * See webResolver.cpp for the rationale. In short: it behaves exactly like
 * ArDefaultResolver, but when a reference cannot be resolved it falls back to matching
 * the file name against the assets the caller supplied. That recovers the very common
 * case of a USD file carrying absolute paths from the machine it was authored on.
 */

#pragma once

#include <pxr/pxr.h>
#include <pxr/usd/ar/defaultResolver.h>
#include <pxr/usd/ar/resolvedPath.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class WebResolver : public ArDefaultResolver
{
public:
    /// Indexes every file under \p directory (recursively) for name-based fallback.
    static void RegisterAssetDirectory(const std::string& directory);

    /// Drops the index and the record of unresolved names.
    static void ClearAssetIndex();

    /// Enables or disables name-based fallback. Enabled by default.
    static void SetFallbackEnabled(bool enabled);

    /// File names that could not be resolved, for reporting back to the caller.
    static std::vector<std::string> GetUnresolvedAssetNames();

protected:
    ArResolvedPath _Resolve(const std::string& assetPath) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE
