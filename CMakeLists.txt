cmake_minimum_required(VERSION 3.24)

# usd-web-gltf
#
# Compiles Adobe's USD-Fileformat-plugins (https://github.com/adobe/USD-Fileformat-plugins)
# to WebAssembly and exposes a single JavaScript entry point that turns a USD asset into a
# GLB buffer.
#
# The Adobe sources are compiled *in place* from their own checkout: nothing is forked or
# vendored, so tracking upstream is a matter of moving the checkout forward. Only two things
# are supplied locally:
#
#   1. Build rules that produce STATIC libraries. Upstream builds `fileformatUtils` and
#      `usdGltf` as SHARED, which Emscripten cannot link.
#   2. A replacement for `utils/src/images.cpp` (see src/imagesWasm.cpp) that drops the
#      OpenImageIO and pxr/imaging/hio dependencies in favour of stb.
#
# Everything else — the whole USD -> glTF translation — is upstream code, unmodified.

project(usd_web_gltf LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "This project only targets WebAssembly. Configure with `emcmake cmake ...`.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

set(ADOBE_PLUGINS_DIR "" CACHE PATH
    "Checkout of adobe/USD-Fileformat-plugins")
set(TINYGLTF_DIR "" CACHE PATH
    "Checkout of syoyo/tinygltf (header-only)")

if(NOT ADOBE_PLUGINS_DIR OR NOT EXISTS "${ADOBE_PLUGINS_DIR}/utils/src/layerRead.cpp")
    message(FATAL_ERROR
        "ADOBE_PLUGINS_DIR must point at a USD-Fileformat-plugins checkout. Got: '${ADOBE_PLUGINS_DIR}'")
endif()
if(NOT TINYGLTF_DIR OR NOT EXISTS "${TINYGLTF_DIR}/tiny_gltf.h")
    message(FATAL_ERROR
        "TINYGLTF_DIR must point at a tinygltf checkout containing tiny_gltf.h. Got: '${TINYGLTF_DIR}'")
endif()

find_package(pxr REQUIRED)

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
    "SHELL:-Wl,--whole-archive ${PXR_CMAKE_DIR}/lib/libusd_m.a -Wl,--no-whole-archive"
)

set(_plugin_resources "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdGltf")

set(_resource_stage "${CMAKE_BINARY_DIR}/usd-resources")

# USD discovers plugins by scanning for plugInfo.json under the tree rooted at /usd in the
# virtual filesystem. Staging the core resources alongside the Adobe plugin's manifest and
# our resolver's reproduces the layout USD expects from a normal install.
file(REMOVE_RECURSE "${_resource_stage}")
file(COPY "${PXR_CMAKE_DIR}/lib/usd/" DESTINATION "${_resource_stage}")
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
