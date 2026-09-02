cmake_minimum_required(VERSION 3.24)

# usd-web-gltf
#
# Compiles the performance-focused USD-Fileformat-plugins fork
# (https://github.com/SergioRZMasson/USD-Fileformat-plugins)
# as a native Babylon JSON command-line converter and as separate GLB/Babylon WebAssembly
# modules for isolated size and runtime comparisons.
#
# Dependencies are handled entirely by the build, so nothing has to be configured on the
# machine beyond an Emscripten install and a vcpkg checkout (VCPKG_ROOT):
#
#   * OpenUSD is compiled by vcpkg as a STATIC, MONOLITHIC library for the
#     wasm32-emscripten triplet. The overlay port under ports/usd and the overlay triplet
#     under triplets/ carry the wasm-specific settings the stock vcpkg port refuses to make.
#   * USD-Fileformat-plugins, syoyo/tinygltf, and meshoptimizer are git submodules under
#     dependencies/ and compiled in place.
#
# Only a small build/runtime layer is supplied locally on top of the Adobe sources:
#
#   1. Build rules that produce STATIC libraries. Upstream builds `fileformatUtils` and
#      `usdGltf` as SHARED, which Emscripten cannot link.
#   2. A replacement for `utils/src/images.cpp` (see src/imagesWasm.cpp) that drops the
#      OpenImageIO and pxr/imaging/hio dependencies in favour of stb.
#   3. An export-only Babylon JSON file format plugin.
#   4. Native CLI and browser bindings around the two conversion paths.
#
# USD -> glTF translation changes live in the USD-Fileformat-plugins fork, while this
# repository owns platform integration, CLI benchmarking, and browser bindings.

# The CMake presets already point CMAKE_TOOLCHAIN_FILE at vcpkg (which in turn chainloads
# the Emscripten toolchain for the wasm32-emscripten triplet). This fallback keeps a bare
# `cmake -B build` working when VCPKG_ROOT is exported, but configuring through a preset is
# the supported path — see CMakePresets.json.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT} AND NOT CMAKE_PREFIX_PATH)
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    elseif(NOT EMSCRIPTEN)
        message(STATUS
            "VCPKG_ROOT is not set; the native build will use packages from CMAKE_PREFIX_PATH")
    else()
        message(FATAL_ERROR
            "VCPKG_ROOT is not set and no CMAKE_TOOLCHAIN_FILE was provided.\n"
            "Configure with the `wasm` preset (see CMakePresets.json), or export VCPKG_ROOT.")
    endif()
endif()

project(usd_web_gltf LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# When ON, the npm package under js/ is built as part of the CMake build: after the wasm
# module links, its artifacts are bundled into the publishable library with webpack. This
# lets a single `cmake --build` produce everything. Enabled by the `wasm-js` preset; OFF by
# default so iterating on the wasm alone does not pay the npm cost.
option(BUILD_JS_LIBRARY "Build the usd-web-gltf npm library (js/) after the wasm module" OFF)

if(EMSCRIPTEN)
    set(_native_cli_default OFF)
else()
    set(_native_cli_default ON)
endif()
option(BUILD_NATIVE_CLI "Build the standard C++ USD-to-GLB command-line converter"
       ${_native_cli_default})
set(USD_WEB_BENCHMARK_INPUTS "" CACHE STRING
    "Semicolon-separated private USD files registered as native CLI benchmark tests")

# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

# The Adobe plugins and tinygltf are git submodules under dependencies/. The paths stay
# overridable for out-of-tree checkouts, but default to the submodules so a fresh clone
# (with `--recurse-submodules`) needs no configuration.
set(ADOBE_PLUGINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/USD-Fileformat-plugins" CACHE PATH
    "Checkout of SergioRZMasson/USD-Fileformat-plugins")
set(TINYGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/tinygltf" CACHE PATH
    "Checkout of syoyo/tinygltf (header-only)")
set(MESHOPTIMIZER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/meshoptimizer" CACHE PATH
    "Checkout of zeux/meshoptimizer")

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
if(NOT EXISTS "${MESHOPTIMIZER_DIR}/src/meshoptimizer.h")
    message(FATAL_ERROR
        "meshoptimizer not found under '${MESHOPTIMIZER_DIR}'.\n"
        "Initialise the submodules: git submodule update --init --recursive")
endif()

set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory("${MESHOPTIMIZER_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/meshoptimizer"
                 EXCLUDE_FROM_ALL)
if(EMSCRIPTEN)
    target_compile_options(meshoptimizer PRIVATE -O3 -msimd128 -flto)
endif()

# OpenUSD is provided by vcpkg. The wasm overlay builds a static monolithic libusd_m.a;
# native builds use the regular exported module targets.
find_package(pxr CONFIG REQUIRED)

# zlib is a vcpkg dependency of USD, and Adobe's fileformatUtils (neuralAssetsHelper.cpp)
# also calls inflate/deflate directly. In a from-source USD install the monolithic archive
# does not re-export zlib, so the final module must link it explicitly. (The previous
# hand-rolled build pulled Emscripten's own `--use-port=zlib` instead; vcpkg builds it.)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(_pxr_targets
    arch tf gf pegtl js trace work plug vt ts ar kind sdf sdr pcp usd usdGeom usdVol
    usdMedia usdShade usdLod usdLux usdProc usdProfiles usdRender usdHydra usdRi
    usdSemantics usdSkel usdUI usdUtils usdPhysics usd_m
)
if(EMSCRIPTEN)
    # find_package(pxr) sets PXR_CMAKE_DIR to <prefix>/share/pxr. The monolithic archive
    # and plugInfo/schema resources live under <prefix>/lib.
    get_filename_component(PXR_INSTALL_PREFIX "${PXR_CMAKE_DIR}/../.." ABSOLUTE)
    set(PXR_USD_PLUGIN_RESOURCES "${PXR_INSTALL_PREFIX}/lib/usd")
    if(NOT EXISTS "${PXR_USD_PLUGIN_RESOURCES}")
        message(FATAL_ERROR
            "Expected USD plugin resources at '${PXR_USD_PLUGIN_RESOURCES}' but none were "
            "found. Was OpenUSD built with PXR_BUILD_MONOLITHIC by the overlay port?")
    endif()

    # Replace hundreds of --embed-file options with one staged resource directory.
    foreach(_target IN LISTS _pxr_targets)
        if(TARGET ${_target})
            set_property(TARGET ${_target} PROPERTY INTERFACE_LINK_OPTIONS "")
        endif()
    endforeach()
endif()

message(STATUS "Adobe plugins : ${ADOBE_PLUGINS_DIR}")
message(STATUS "tinygltf      : ${TINYGLTF_DIR}")
message(STATUS "meshoptimizer : ${MESHOPTIMIZER_DIR}")
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
if(EMSCRIPTEN)
    target_compile_options(fileformatUtils PRIVATE -O3 -msimd128 -flto)
endif()

target_link_libraries(fileformatUtils PUBLIC
    tf sdf usd usdGeom usdLux usdRender usdSkel usdShade usdUtils usdVol arch
    ZLIB::ZLIB
)
if(TARGET usd_m)
    # wasm uses the monolithic archive while the module targets carry include/resource data.
    target_link_libraries(fileformatUtils PUBLIC usd_m)
endif()

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
    meshCompression.cpp
    meshOptimization.cpp
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
if(EMSCRIPTEN)
    target_compile_options(usdGltf PRIVATE -O3 -msimd128 -flto)
endif()

target_link_libraries(usdGltf PUBLIC
    fileformatUtils
    meshoptimizer
    usd usdGeom usdSkel usdShade
)
if(TARGET usd_m)
    target_link_libraries(usdGltf PUBLIC usd_m)
endif()

# ---------------------------------------------------------------------------
# usdBabylon — export-only Babylon.js JSON file format plugin
# ---------------------------------------------------------------------------

add_library(usdBabylon STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/babylon/fileFormat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/babylon/babylonExport.cpp"
)
target_include_directories(usdBabylon
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/babylon"
    PRIVATE
        "${ADOBE_PLUGINS_DIR}/gltf/src"
        "${CMAKE_CURRENT_BINARY_DIR}"
)
target_compile_definitions(usdBabylon PRIVATE USDBABYLON_EXPORTS)
if(EMSCRIPTEN)
    target_compile_options(usdBabylon PRIVATE -O3 -msimd128 -flto)
endif()
target_link_libraries(usdBabylon PUBLIC
    fileformatUtils
    nlohmann_json::nlohmann_json
    usdGltf
    usd usdGeom usdSkel usdShade
)
if(TARGET usd_m)
    target_link_libraries(usdBabylon PUBLIC usd_m)
endif()

set(_gltf_plugin_resources "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdGltf")
set(_babylon_plugin_resources "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdBabylon")
set(_resolver_resources "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdWebResolver")

if(EMSCRIPTEN)
    # -----------------------------------------------------------------------
    # Separate WebAssembly modules for an isolated size/runtime comparison.
    # -----------------------------------------------------------------------
    add_executable(usd-web-gltf
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bindings.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webResolver.cpp"
    )
    target_include_directories(usd-web-gltf PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${ADOBE_PLUGINS_DIR}/gltf/src"
    )
    target_compile_definitions(usd-web-gltf PRIVATE USD_WEB_GLTF_BACKEND)
    target_link_libraries(usd-web-gltf PRIVATE usdGltf fileformatUtils)
    target_compile_options(usd-web-gltf PRIVATE -O3 -msimd128 -flto)
    target_link_options(usd-web-gltf PRIVATE
        "SHELL:-Wl,--whole-archive $<TARGET_FILE:usdGltf> -Wl,--no-whole-archive"
        "SHELL:-Wl,--whole-archive $<TARGET_FILE:usd_m> -Wl,--no-whole-archive"
    )

    set(_gltf_resource_stage "${CMAKE_BINARY_DIR}/usd-resources-gltf")
    file(REMOVE_RECURSE "${_gltf_resource_stage}")
    file(COPY "${PXR_USD_PLUGIN_RESOURCES}/" DESTINATION "${_gltf_resource_stage}")
    file(COPY "${_gltf_plugin_resources}" DESTINATION "${_gltf_resource_stage}")
    file(COPY "${_resolver_resources}" DESTINATION "${_gltf_resource_stage}")

    target_link_options(usd-web-gltf PRIVATE
        --bind
        -sMODULARIZE=1
        -sEXPORT_NAME=createUsdGltfModule
        -sENVIRONMENT=web,worker,node
        -sALLOW_MEMORY_GROWTH=1
        -sMEMORY_GROWTH_GEOMETRIC_STEP=0.5
        -sMAXIMUM_MEMORY=2GB
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8
        -sSTACK_SIZE=1MB
        -sEXPORT_ES6=1
        -sASSERTIONS=0
        -msimd128
        -flto
        --strip-debug
        "SHELL:--preload-file ${_gltf_resource_stage}@/usd"
    )
    set_target_properties(usd-web-gltf PROPERTIES
        OUTPUT_NAME "usd-web-gltf"
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(usd-web-babylon
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bindings.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webResolver.cpp"
    )
    target_include_directories(usd-web-babylon PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/babylon"
    )
    target_compile_definitions(usd-web-babylon PRIVATE USD_WEB_BABYLON_BACKEND)
    target_link_libraries(usd-web-babylon PRIVATE usdBabylon)
    target_compile_options(usd-web-babylon PRIVATE -O3 -msimd128 -flto)
    target_link_options(usd-web-babylon PRIVATE
        "SHELL:-Wl,--whole-archive $<TARGET_FILE:usdBabylon> -Wl,--no-whole-archive"
        "SHELL:-Wl,--whole-archive $<TARGET_FILE:usd_m> -Wl,--no-whole-archive"
    )

    set(_babylon_resource_stage "${CMAKE_BINARY_DIR}/usd-resources-babylon")
    file(REMOVE_RECURSE "${_babylon_resource_stage}")
    file(COPY "${PXR_USD_PLUGIN_RESOURCES}/" DESTINATION "${_babylon_resource_stage}")
    file(COPY "${_babylon_plugin_resources}" DESTINATION "${_babylon_resource_stage}")
    file(COPY "${_resolver_resources}" DESTINATION "${_babylon_resource_stage}")

    target_link_options(usd-web-babylon PRIVATE
        --bind
        -sMODULARIZE=1
        -sEXPORT_NAME=createUsdBabylonModule
        -sENVIRONMENT=web,worker,node
        -sALLOW_MEMORY_GROWTH=1
        -sMEMORY_GROWTH_GEOMETRIC_STEP=0.5
        -sMAXIMUM_MEMORY=2GB
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8
        -sSTACK_SIZE=1MB
        -sEXPORT_ES6=1
        -sASSERTIONS=0
        -msimd128
        -flto
        --strip-debug
        "SHELL:--preload-file ${_babylon_resource_stage}@/usd"
    )
    set_target_properties(usd-web-babylon PROPERTIES
        OUTPUT_NAME "usd-web-babylon"
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
elseif(BUILD_NATIVE_CLI)
    # -----------------------------------------------------------------------
    # Standard C++ command-line converters and private-input benchmark tests
    # -----------------------------------------------------------------------
    add_executable(usd-web-gltf-cli
        "${CMAKE_CURRENT_SOURCE_DIR}/src/cli.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webResolver.cpp"
    )
    target_include_directories(usd-web-gltf-cli PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${ADOBE_PLUGINS_DIR}/gltf/src"
        "${TINYGLTF_DIR}"
    )
    target_compile_definitions(usd-web-gltf-cli PRIVATE
        USD_WEB_GLTF_PLUGIN_RESOURCES="${_gltf_plugin_resources}/resources"
        USD_WEB_RESOLVER_PLUGIN_RESOURCES="${_resolver_resources}/resources"
        TINYGLTF_NO_STB_IMAGE
        TINYGLTF_NO_STB_IMAGE_WRITE
        TINYGLTF_NO_EXTERNAL_IMAGE
    )
    target_link_libraries(usd-web-gltf-cli PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,usdGltf>"
        fileformatUtils
    )
    set_target_properties(usd-web-gltf-cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(usd-web-babylon-cli
        "${CMAKE_CURRENT_SOURCE_DIR}/src/babylonCli.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webResolver.cpp"
    )
    target_include_directories(usd-web-babylon-cli PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_definitions(usd-web-babylon-cli PRIVATE
        USD_WEB_BABYLON_PLUGIN_RESOURCES="${_babylon_plugin_resources}/resources"
        USD_WEB_RESOLVER_PLUGIN_RESOURCES="${_resolver_resources}/resources"
    )
    target_link_libraries(usd-web-babylon-cli PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,usdBabylon>"
    )
    set_target_properties(usd-web-babylon-cli PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    include(CTest)
    if(BUILD_TESTING AND USD_WEB_BENCHMARK_INPUTS)
        set(_benchmark_index 0)
        foreach(_benchmark_input IN LISTS USD_WEB_BENCHMARK_INPUTS)
            math(EXPR _benchmark_index "${_benchmark_index} + 1")
            get_filename_component(_benchmark_name "${_benchmark_input}" NAME_WE)
            string(MAKE_C_IDENTIFIER "${_benchmark_name}_${_benchmark_index}" _benchmark_id)
            add_test(
                NAME "usd-web-gltf-${_benchmark_id}"
                COMMAND usd-web-gltf-cli
                    --json
                    --iterations 1
                    --meshopt-compression
                    "${_benchmark_input}"
                    "${CMAKE_BINARY_DIR}/benchmarks/gltf/${_benchmark_name}.glb"
            )
            set_tests_properties("usd-web-gltf-${_benchmark_id}" PROPERTIES
                LABELS "benchmark;gltf;private-assets"
            )
            add_test(
                NAME "usd-web-babylon-${_benchmark_id}"
                COMMAND usd-web-babylon-cli
                    --json
                    --iterations 1
                    "${_benchmark_input}"
                    "${CMAKE_BINARY_DIR}/benchmarks/babylon/${_benchmark_name}.babylon"
            )
            set_tests_properties("usd-web-babylon-${_benchmark_id}" PROPERTIES
                LABELS "benchmark;babylon;private-assets"
            )
        endforeach()
    endif()
endif()

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
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "BUILD_JS_LIBRARY requires an Emscripten build")
    endif()
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
    add_dependencies(js-library usd-web-gltf usd-web-babylon)

    message(STATUS "BUILD_JS_LIBRARY=ON — the npm library will be built after the wasm module")
endif()
