cmake_minimum_required(VERSION 3.24)

# usd-web-gltf
#
# Compiles Adobe's USD-Fileformat-plugins (https://github.com/adobe/USD-Fileformat-plugins)
# to WebAssembly and exposes a single JavaScript entry point that turns a USD asset into a
# GLB buffer.
#
# Dependencies are handled entirely by the build, so nothing has to be configured on the
# machine beyond an Emscripten install and a vcpkg checkout (VCPKG_ROOT):
#
#   * OpenUSD is compiled by vcpkg as a STATIC, MONOLITHIC library for the
#     wasm32-emscripten triplet. The overlay port under ports/usd and the overlay triplet
#     under triplets/ carry the wasm-specific settings the stock vcpkg port refuses to make.
#   * Adobe's USD-Fileformat-plugins and syoyo/tinygltf are git submodules under
#     dependencies/, compiled in place. Nothing is forked or vendored, so tracking upstream
#     is a matter of moving a submodule forward.
#
# Only two things are supplied locally on top of the Adobe sources:
#
#   1. Build rules that produce STATIC libraries. Upstream builds `fileformatUtils` and
#      `usdGltf` as SHARED, which Emscripten cannot link.
#   2. A replacement for `utils/src/images.cpp` (see src/imagesWasm.cpp) that drops the
#      OpenImageIO and pxr/imaging/hio dependencies in favour of stb.
#
# Everything else — the whole USD -> glTF translation — is upstream code, unmodified.

# The CMake presets already point CMAKE_TOOLCHAIN_FILE at vcpkg (which in turn chainloads
# the Emscripten toolchain for the wasm32-emscripten triplet). This fallback keeps a bare
# `cmake -B build` working when VCPKG_ROOT is exported, but configuring through a preset is
# the supported path — see CMakePresets.json.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    else()
        message(FATAL_ERROR
            "VCPKG_ROOT is not set and no CMAKE_TOOLCHAIN_FILE was provided.\n"
            "Configure with the `wasm` preset (see CMakePresets.json), or export VCPKG_ROOT.")
    endif()
endif()

project(usd_web_gltf LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "This project only targets WebAssembly. Configure with the `wasm` preset, which "
        "selects the wasm32-emscripten triplet so vcpkg chainloads the Emscripten toolchain.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# When ON, the npm package under js/ is built as part of the CMake build: after the wasm
# module links, its artifacts are bundled into the publishable library with webpack. This
# lets a single `cmake --build` produce everything. Enabled by the `wasm-js` preset; OFF by
# default so iterating on the wasm alone does not pay the npm cost.
option(BUILD_JS_LIBRARY "Build the usd-web-gltf npm library (js/) after the wasm module" OFF)

# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

# The Adobe plugins and tinygltf are git submodules under dependencies/. The paths stay
# overridable for out-of-tree checkouts, but default to the submodules so a fresh clone
# (with `--recurse-submodules`) needs no configuration.
set(ADOBE_PLUGINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/USD-Fileformat-plugins" CACHE PATH
    "Checkout of adobe/USD-Fileformat-plugins")
set(TINYGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/tinygltf" CACHE PATH
    "Checkout of syoyo/tinygltf (header-only)")

if(NOT EXISTS "${ADOBE_PLUGINS_DIR}/utils/src/layerRead.cpp")
    message(FATAL_ERROR
        "USD-Fileformat-plugins not found under '${ADOBE_PLUGINS_DIR}'.\n"
        "Initialise the submodules: git submodule update --init --recursive")
endif()
if(NOT EXISTS "${TINYGLTF_DIR}/tiny_gltf.h")
    message(FATAL_ERROR
        "tinygltf not found under '${TINYGLTF_DIR}'.\n"
        "Initialise the submodules: git submodule update --init --recursive")
endif()

# OpenUSD is provided by the vcpkg overlay port as a static monolithic build (libusd_m.a).
find_package(pxr CONFIG REQUIRED)

# zlib is a vcpkg dependency of USD, and Adobe's fileformatUtils (neuralAssetsHelper.cpp)
# also calls inflate/deflate directly. In a from-source USD install the monolithic archive
# does not re-export zlib, so the final module must link it explicitly. (The previous
# hand-rolled build pulled Emscripten's own `--use-port=zlib` instead; vcpkg builds it.)
find_package(ZLIB REQUIRED)

# find_package(pxr) sets PXR_CMAKE_DIR to the directory that holds pxrConfig.cmake, which
# vcpkg installs at <prefix>/share/pxr. The monolithic archive and the plugin resources
# (plugInfo.json manifests + schemas) live under <prefix>/lib, so derive that root here
# rather than assuming a layout relative to PXR_CMAKE_DIR.
get_filename_component(PXR_INSTALL_PREFIX "${PXR_CMAKE_DIR}/../.." ABSOLUTE)
set(PXR_USD_PLUGIN_RESOURCES "${PXR_INSTALL_PREFIX}/lib/usd")
if(NOT EXISTS "${PXR_USD_PLUGIN_RESOURCES}")
    message(FATAL_ERROR
        "Expected USD plugin resources at '${PXR_USD_PLUGIN_RESOURCES}' but none were found. "
        "Was OpenUSD built with PXR_BUILD_MONOLITHIC by the overlay port?")
endif()

# USD's exported targets attach one --embed-file link option per resource file. With the
# full schema set that produces a link command well past Windows' 32 KiB command line
# limit. The options are cleared here and every resource is instead staged into a single
# directory that is mounted with one --preload-file below. This is also faster to link and
# keeps the resources in a separate .data file rather than inflating the .wasm.
set(_pxr_targets
    arch tf gf pegtl js trace work plug vt ts ar kind sdf sdr pcp usd usdGeom usdVol
    usdMedia usdShade usdLod usdLux usdProc usdProfiles usdRender usdHydra usdRi
    usdSemantics usdSkel usdUI usdUtils usdPhysics usd_m
)
foreach(_target IN LISTS _pxr_targets)
    if(TARGET ${_target})
        set_property(TARGET ${_target} PROPERTY INTERFACE_LINK_OPTIONS "")
    endif()
endforeach()

message(STATUS "Adobe plugins : ${ADOBE_PLUGINS_DIR}")
message(STATUS "tinygltf      : ${TINYGLTF_DIR}")
message(STATUS "USD           : ${PXR_CMAKE_DIR}")

# ---------------------------------------------------------------------------
# version.h — normally produced by the Adobe project's own configure step
# ---------------------------------------------------------------------------

file(READ "${ADOBE_PLUGINS_DIR}/version.json" _version_json)
string(JSON VERSION GET "${_version_json}" version)
configure_file("${ADOBE_PLUGINS_DIR}/version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/version.h")
message(STATUS "Adobe plugin version: ${VERSION}")

# ---------------------------------------------------------------------------
# fileformatUtils — Adobe's shared translation layer, minus OpenImageIO/hio
# ---------------------------------------------------------------------------

set(_utils_sources
    assetresolver.cpp
    common.cpp
    dictencoder.cpp
    featureFlags.cpp
    geometry.cpp
    transforms.cpp
    layerRead.cpp
    layerReadMaterial.cpp
    layerReadMaterialUtils.cpp
    layerWriteShared.cpp
    layerWriteMaterial.cpp
    layerWriteOpenPBR.cpp
    layerWriteSdfData.cpp
    materials.cpp
    naming.cpp
    neuralAssetsHelper.cpp
    resolver.cpp
    sdfMaterialUtils.cpp
    sdfUtils.cpp
    usdData.cpp
)
list(TRANSFORM _utils_sources PREPEND "${ADOBE_PLUGINS_DIR}/utils/src/")

# images.cpp is deliberately excluded above and replaced here.
add_library(fileformatUtils STATIC
    ${_utils_sources}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/imagesWasm.cpp"
)

target_include_directories(fileformatUtils
    PUBLIC
        "${ADOBE_PLUGINS_DIR}/utils/include"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
)

# USDFFUTILS_EXPORTS drives the dllexport macro; harmless and correct for a static build.
target_compile_definitions(fileformatUtils PRIVATE USDFFUTILS_EXPORTS)

target_link_libraries(fileformatUtils PUBLIC
    # usd_m is the monolithic static archive; the per-module names (tf, sdf, ...) are
    # INTERFACE targets in this build and carry only include dirs and the --embed-file
    # options for their plugInfo/schema resources, so both are required.
    usd_m
    tf sdf usd usdGeom usdLux usdRender usdSkel usdShade usdUtils usdVol arch
    # neuralAssetsHelper.cpp uses zlib directly; PUBLIC so the symbols reach the final link.
    ZLIB::ZLIB
)

# ---------------------------------------------------------------------------
# usdGltf — Adobe's glTF file format plugin, built static so USD can register it
# ---------------------------------------------------------------------------

set(_gltf_sources
    debugCodes.cpp
    fileFormat.cpp
    gltf.cpp
    gltfAnisotropy.cpp
    gltfAnisotropyASM.cpp
    gltfAnisotropyOpenPBR.cpp
    gltfExport.cpp
    gltfImport.cpp
    gltfResolver.cpp
    gltfSpecGloss.cpp
    tinygltf.cpp
)
list(TRANSFORM _gltf_sources PREPEND "${ADOBE_PLUGINS_DIR}/gltf/src/")

add_library(usdGltf STATIC ${_gltf_sources})

target_include_directories(usdGltf PRIVATE
    "${ADOBE_PLUGINS_DIR}/gltf/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${TINYGLTF_DIR}"
)

target_compile_definitions(usdGltf PRIVATE
    USDGLTF_EXPORTS
    # tinygltf's own image handling is unused: Adobe's code owns decoding, and the stb
    # implementation already lives in fileformatUtils. Defining these avoids pulling a
    # second copy of stb into the binary and the duplicate symbols that would follow.
    TINYGLTF_NO_STB_IMAGE
    TINYGLTF_NO_STB_IMAGE_WRITE
    TINYGLTF_NO_EXTERNAL_IMAGE
)

target_link_libraries(usdGltf PUBLIC fileformatUtils usd_m usd usdGeom usdSkel usdShade)

# ---------------------------------------------------------------------------
# The wasm module
# ---------------------------------------------------------------------------

add_executable(usd-web-gltf
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bindings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/webResolver.cpp"
)

target_include_directories(usd-web-gltf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(usd-web-gltf PRIVATE usdGltf fileformatUtils)

# Static initialisers are what register the plugin's TfTypes and the SdfFileFormat
# subclass. Without whole-archive the linker discards those objects (nothing references
# them directly) and USD reports the format as unknown at runtime.
#
# usd_m needs the same treatment: its plug/initConfig.cpp translation unit is what
# installs USD's default plugin search paths, and nothing references it symbolically.
# Dropping it yields "No plugin search paths" and a stage that cannot even be opened.
# This mirrors what USD's own pxr_library macro does for Emscripten targets.
target_link_options(usd-web-gltf PRIVATE
    "SHELL:-Wl,--whole-archive $<TARGET_FILE:usdGltf> -Wl,--no-whole-archive"
    "SHELL:-Wl,--whole-archive $<TARGET_FILE:usd_m> -Wl,--no-whole-archive"
)

set(_plugin_resources "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdGltf")

set(_resource_stage "${CMAKE_BINARY_DIR}/usd-resources")

# USD discovers plugins by scanning for plugInfo.json under the tree rooted at /usd in the
# virtual filesystem. Staging the core resources alongside the Adobe plugin's manifest and
# our resolver's reproduces the layout USD expects from a normal install.
file(REMOVE_RECURSE "${_resource_stage}")
file(COPY "${PXR_USD_PLUGIN_RESOURCES}/" DESTINATION "${_resource_stage}")
file(COPY "${_plugin_resources}" DESTINATION "${_resource_stage}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdWebResolver" DESTINATION "${_resource_stage}")

target_link_options(usd-web-gltf PRIVATE
    --bind
    -sMODULARIZE=1
    -sEXPORT_NAME=createUsdGltfModule
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=2GB
    -sFORCE_FILESYSTEM=1
    -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8
    -sSTACK_SIZE=1MB
    # Emit an ES module so the npm package can be "type": "module" and so bundlers can
    # statically analyse the import. Without this Emscripten emits CommonJS, which Node
    # refuses to load from an ESM package.
    -sEXPORT_ES6=1
    "SHELL:--preload-file ${_resource_stage}@/usd"
)

set_target_properties(usd-web-gltf PROPERTIES
    OUTPUT_NAME "usd-web-gltf"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# JavaScript library (optional) — the npm package under js/
# ---------------------------------------------------------------------------
#
# With -DBUILD_JS_LIBRARY=ON (the `wasm-js` preset), the npm package is built as part of the
# CMake build so one command produces both the wasm module and the publishable library.
#
# The webpack build (npm run build) copies the Emscripten artifacts out of this target's
# output directory into js/dist/wasm and bundles the TypeScript. The directory is passed
# through USD_WEB_GLTF_BIN — which js/webpack.config.mjs already honours — so the js build
# tracks the wasm output wherever it lands rather than assuming a fixed layout.
if(BUILD_JS_LIBRARY)
    find_program(NPM_EXECUTABLE NAMES npm.cmd npm REQUIRED)

    set(_js_dir "${CMAKE_CURRENT_SOURCE_DIR}/js")
    set(_js_install_stamp "${CMAKE_CURRENT_BINARY_DIR}/js-npm-install.stamp")

    # Install dependencies only when the manifest changes; the stamp lives in the build tree
    # so a fresh build directory always installs first.
    add_custom_command(
        OUTPUT "${_js_install_stamp}"
        COMMAND "${NPM_EXECUTABLE}" install
        COMMAND "${CMAKE_COMMAND}" -E touch "${_js_install_stamp}"
        WORKING_DIRECTORY "${_js_dir}"
        DEPENDS "${_js_dir}/package.json" "${_js_dir}/package-lock.json"
        COMMENT "usd-web-gltf (js): installing npm dependencies"
        USES_TERMINAL
        VERBATIM
    )

    # `npm run build` runs tsc (types) then webpack (bundle + wasm copy). Kept as an ALL
    # target so it always re-bundles against the current wasm, and made to depend on the
    # wasm target so the artifacts exist first.
    add_custom_target(js-library ALL
        COMMAND "${CMAKE_COMMAND}" -E env
                "USD_WEB_GLTF_BIN=$<TARGET_FILE_DIR:usd-web-gltf>"
                "${NPM_EXECUTABLE}" run build
        WORKING_DIRECTORY "${_js_dir}"
        DEPENDS "${_js_install_stamp}"
        COMMENT "usd-web-gltf (js): building the npm library with webpack"
        USES_TERMINAL
        VERBATIM
    )
    add_dependencies(js-library usd-web-gltf)

    message(STATUS "BUILD_JS_LIBRARY=ON — the npm library will be built after the wasm module")
endif()
