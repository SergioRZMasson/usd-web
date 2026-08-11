# -----------------------------------------------------------------------------
# usd-web overlay port
#
# This shadows vcpkg's stock `usd` port to produce the one configuration the
# WebAssembly build needs and the stock port refuses to make: a STATIC,
# MONOLITHIC OpenUSD (libusd_m.a) for the wasm32-emscripten triplet.
#
# Deltas from the stock port:
#   * The `vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)` guard is removed. Emscripten
#     cannot link shared libraries, so a static build is mandatory, not optional.
#     Under EMSCRIPTEN, OpenUSD's own `pxr_plugin` macro already routes plugins to
#     `pxr_library(TYPE STATIC)`, so the file-format plugins register through static
#     initialisers rather than dlopen.
#   * PXR_BUILD_MONOLITHIC=ON collapses every module into a single archive, which is
#     what the wasm link step expects (and whole-archives).
#   * USD validation and usdview are disabled; neither contributes to reading a
#     stage or writing glTF, and each costs binary size.
#
# Everything else — the source revision, the patch set and the feature options —
# is inherited unchanged from the stock port, so it stays in lockstep with the
# vcpkg baseline this repository pins.
# -----------------------------------------------------------------------------

# zero-pad version components to two digits
string(REPLACE "." ";" version_components ${VERSION})
foreach(component IN LISTS version_components)
    string(LENGTH ${component} component_length)
    if(component_length LESS 2)
        list(APPEND USD_VERSION "0${component}")
    else()
        list(APPEND USD_VERSION "${component}")
    endif()
endforeach()
string(JOIN "." USD_VERSION ${USD_VERSION})

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO PixarAnimationStudios/OpenUSD
    REF "v${USD_VERSION}"
    SHA512 d10222a457d71470a26ad6dc812685f257bf5c90a64a11d90e543ef7eaba803aa4e2593c358ebd430ba55856e987f7a6f50597b1ad6d2da737c239ad4f18ad6a
    HEAD_REF release
    PATCHES
        003-fix-dep.patch
        004-fix_cmake_package.patch
        007-fix_cmake_hgi_interop.patch
        008-fix_clang8_compiler_error.patch
        009-vcpkg_install_folder_conventions.patch
        010-cmake_export_plugin_as_modules.patch
        011-fix-tbb2023-task-api.patch
)

# Changes accompanying 003-fix-dep.patch
file(REMOVE
    "${SOURCE_PATH}/cmake/modules/FindOpenColorIO.cmake"
    "${SOURCE_PATH}/pxr/imaging/hgiVulkan/vk_mem_alloc.cpp"
    "${SOURCE_PATH}/pxr/imaging/hgiVulkan/vk_mem_alloc.h"
)

# Keep the wasm build single-threaded. On Emscripten, find_package(Threads) always reports
# pthreads available (they live in libc), so USD's gccclangshareddefaults.cmake unconditionally
# adds `-pthread`, which switches on the shared-memory ABI and would force every consumer — and
# the final module — to be cross-origin isolated. oneTBB is already built with
# EMSCRIPTEN_WITHOUT_PTHREAD by the overlay tbb port, so guarding this keeps the whole stack free
# of the pthread ABI and lets the module run from any static host with no special headers.
vcpkg_replace_string(
    "${SOURCE_PATH}/cmake/defaults/gccclangshareddefaults.cmake"
    "if(CMAKE_USE_PTHREADS_INIT)"
    "if(CMAKE_USE_PTHREADS_INIT AND NOT EMSCRIPTEN)"
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        imaging        PXR_BUILD_IMAGING
        imaging        PXR_BUILD_USD_IMAGING
        imaging        PXR_ENABLE_GL_SUPPORT
        materialx      PXR_ENABLE_MATERIALX_SUPPORT
        openimageio    PXR_BUILD_OPENIMAGEIO_PLUGIN
        vulkan         PXR_ENABLE_VULKAN_SUPPORT
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS ${FEATURE_OPTIONS}
        # One archive (libusd_m.a) instead of one shared library per module: the
        # only shape the Emscripten link step can consume.
        -DPXR_BUILD_MONOLITHIC:BOOL=ON

        -DPXR_BUILD_DOCUMENTATION:BOOL=OFF
        -DPXR_BUILD_EXAMPLES:BOOL=OFF
        -DPXR_BUILD_TESTS:BOOL=OFF
        -DPXR_BUILD_TUTORIALS:BOOL=OFF
        -DPXR_BUILD_USD_TOOLS:BOOL=OFF
        -DPXR_BUILD_USD_VALIDATION:BOOL=OFF
        -DPXR_BUILD_USDVIEW:BOOL=OFF

        -DPXR_BUILD_ALEMBIC_PLUGIN:BOOL=OFF
        -DPXR_BUILD_DRACO_PLUGIN:BOOL=OFF
        -DPXR_BUILD_EMBREE_PLUGIN:BOOL=OFF
        -DPXR_BUILD_PRMAN_PLUGIN:BOOL=OFF

        -DPXR_ENABLE_OPENVDB_SUPPORT:BOOL=OFF
        -DPXR_ENABLE_PTEX_SUPPORT:BOOL=OFF

        -DPXR_PREFER_SAFETY_OVER_SPEED:BOOL=ON

        -DPXR_ENABLE_PYTHON_SUPPORT:BOOL=OFF
        -DPXR_USE_DEBUG_PYTHON:BOOL=OFF
    MAYBE_UNUSED_VARIABLES
        PXR_ENABLE_PTEX_SUPPORT
        PXR_USE_PYTHON_3
        PYTHON_EXECUTABLE
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

# Handle debug path for USD plugins
if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
    file(GLOB_RECURSE debug_targets
        "${CURRENT_PACKAGES_DIR}/debug/share/pxr/*-debug.cmake"
        )
    foreach(debug_target IN LISTS debug_targets)
        file(READ "${debug_target}" contents)
        string(REPLACE "\${_IMPORT_PREFIX}/usd" "\${_IMPORT_PREFIX}/debug/usd" contents "${contents}")
        string(REPLACE "\${_IMPORT_PREFIX}/plugin" "\${_IMPORT_PREFIX}/debug/plugin" contents "${contents}")
        file(WRITE "${debug_target}" "${contents}")
    endforeach()
endif()

vcpkg_cmake_config_fixup(PACKAGE_NAME "pxr")

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

if(VCPKG_TARGET_IS_WINDOWS)
    # Move all dlls to bin
    file(GLOB RELEASE_DLL ${CURRENT_PACKAGES_DIR}/lib/*.dll)
    file(MAKE_DIRECTORY ${CURRENT_PACKAGES_DIR}/bin)
    if(NOT VCPKG_BUILD_TYPE)
      file(GLOB DEBUG_DLL ${CURRENT_PACKAGES_DIR}/debug/lib/*.dll)
      file(MAKE_DIRECTORY ${CURRENT_PACKAGES_DIR}/debug/bin)
    endif()
    foreach(CURRENT_FROM ${RELEASE_DLL} ${DEBUG_DLL})
        string(REPLACE "/lib/" "/bin/" CURRENT_TO ${CURRENT_FROM})
        file(RENAME ${CURRENT_FROM} ${CURRENT_TO})
    endforeach()

    function(file_replace_regex filename match_string replace_string)
        file(READ ${filename} _contents)
        string(REGEX REPLACE "${match_string}" "${replace_string}" _contents "${_contents}")
        file(WRITE ${filename} "${_contents}")
    endfunction()

    # fix dll path for cmake
    if(NOT VCPKG_BUILD_TYPE)
      file_replace_regex(${CURRENT_PACKAGES_DIR}/share/pxr/pxrTargets-debug.cmake "debug/lib/([a-zA-Z0-9_]+)\\.dll" "debug/bin/\\1.dll")
    endif()
    file_replace_regex(${CURRENT_PACKAGES_DIR}/share/pxr/pxrTargets-release.cmake "lib/([a-zA-Z0-9_]+)\\.dll" "bin/\\1.dll")

    # fix plugInfo.json for runtime
    file(GLOB_RECURSE PLUGINFO_FILES ${CURRENT_PACKAGES_DIR}/lib/usd/*/resources/plugInfo.json)
    file(GLOB_RECURSE PLUGINFO_FILES_DEBUG ${CURRENT_PACKAGES_DIR}/debug/lib/usd/*/resources/plugInfo.json)
    foreach(PLUGINFO ${PLUGINFO_FILES} ${PLUGINFO_FILES_DEBUG})
        file_replace_regex(${PLUGINFO} [=["LibraryPath": "../../([a-zA-Z0-9_]+).dll"]=] [=["LibraryPath": "../../../bin/\1.dll"]=])
    endforeach()
endif()

# Handle copyright
vcpkg_install_copyright(FILE_LIST ${SOURCE_PATH}/LICENSE.txt)
