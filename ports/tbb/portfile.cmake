
set(VCPKG_POLICY_MISMATCHED_NUMBER_OF_BINARIES enabled)

# -----------------------------------------------------------------------------
# usd-web overlay port
#
# Shadows vcpkg's stock `tbb` port so oneTBB builds SINGLE-THREADED under
# Emscripten (EMSCRIPTEN_WITHOUT_PTHREAD builds its serial task scheduler instead
# of a pthread-backed one). This is half of keeping the final module free of the
# pthread ABI — the other half is the overlay usd port, which stops OpenUSD adding
# `-pthread` on Emscripten. Both are needed: a single-threaded module runs from any
# static host with no SharedArrayBuffer and no cross-origin isolation, which is the
# whole point of the shipped build. Keep the two overlays in agreement.
# -----------------------------------------------------------------------------
if(VCPKG_TARGET_ARCHITECTURE STREQUAL "wasm32" OR VCPKG_CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(USDWEB_EMSCRIPTEN ON)
else()
    set(USDWEB_EMSCRIPTEN OFF)
endif()

set(TBB_EMSCRIPTEN_OPTIONS "")
if(USDWEB_EMSCRIPTEN)
    set(TBB_EMSCRIPTEN_OPTIONS -DEMSCRIPTEN_WITHOUT_PTHREAD=ON)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO oneapi-src/oneTBB
    REF "v${VERSION}"
    SHA512 c8e9b9100873d6f8514da18ca700165466a9c042d24a6ce9e8901c8996c348f2e58f0251b1eca47d33cc5f382783bfc7693bc6c451716540d4caa57339b3b535
    HEAD_REF master
    PATCHES
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    INVERTED_FEATURES
        hwloc TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        ${TBB_EMSCRIPTEN_OPTIONS}
        -DTBB_TEST=OFF
        -DTBB_STRICT=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/TBB")
vcpkg_copy_pdbs()

if(NOT VCPKG_BUILD_TYPE)
    if(VCPKG_TARGET_ARCHITECTURE MATCHES "^(x86|arm|wasm32)$")
        set(arch_suffix "32")
    endif()
    if(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/tbb${arch_suffix}.pc" "-ltbb12" "-ltbb12_debug")
    else()
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/tbb${arch_suffix}.pc" "-ltbb" "-ltbb_debug")
    endif()
    unset(arch_suffix)
endif()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/share/doc"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    # These are duplicate libraries provided on Windows -- users should use the tbb12 libraries instead
    "${CURRENT_PACKAGES_DIR}/lib/tbb.lib"
    "${CURRENT_PACKAGES_DIR}/debug/lib/tbb_debug.lib"
)

# A single-threaded Emscripten build links no pthread, so it must not advertise a
# Threads dependency: pulling in Threads::Threads here would re-introduce -pthread
# and break the no-cross-origin-isolation guarantee. Everywhere else, keep the
# stock behaviour so downstream find_package(TBB) resolves Threads.
if(NOT USDWEB_EMSCRIPTEN)
    file(READ "${CURRENT_PACKAGES_DIR}/share/tbb/TBBConfig.cmake" _contents)
    file(WRITE "${CURRENT_PACKAGES_DIR}/share/tbb/TBBConfig.cmake" "
include(CMakeFindDependencyMacro)
find_dependency(Threads)
${_contents}")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
