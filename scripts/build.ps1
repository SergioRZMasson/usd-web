<#
.SYNOPSIS
    Builds usd-web-gltf: OpenUSD + Adobe's usdGltf plugin, compiled to WebAssembly.

.DESCRIPTION
    Performs the full dependency chain from a clean machine:
      1. oneTBB       -> static wasm library (required by OpenUSD)
      2. OpenUSD      -> monolithic static wasm library, no imaging, no Python
      3. tinygltf     -> header-only, pinned to the version Adobe's plugin expects
      4. this project -> the wasm module and its resource bundle

    Steps are skipped when their output already exists, so re-running is cheap.
    Expect roughly 30-60 minutes for a cold build, almost all of it OpenUSD.

.PARAMETER EmsdkDir
    Emscripten SDK location. Must already be installed and activated.

.PARAMETER DepsDir
    Scratch directory for dependency sources and installs.

.PARAMETER AdobePluginsDir
    Checkout of https://github.com/adobe/USD-Fileformat-plugins

.PARAMETER Clean
    Remove this project's build directory before configuring.

.EXAMPLE
    ./scripts/build.ps1 -AdobePluginsDir E:/Github/USD-Fileformat-plugins
#>
[CmdletBinding()]
param(
    [string]$EmsdkDir = 'E:/emsdk',
    [string]$DepsDir = 'E:/wasmdeps',
    [Parameter(Mandatory = $true)][string]$AdobePluginsDir,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$ProjectDir = Split-Path -Parent $PSScriptRoot
$UsdInstall = Join-Path $DepsDir 'usd26-wasm'
$TbbInstall = Join-Path $DepsDir 'install-wasm'
$TinyGltfDir = Join-Path $DepsDir 'tinygltf'

# Versions are pinned deliberately:
#   OpenUSD 26.08 is the first release that builds pxr/usd for Emscripten. 25.11 and
#   earlier wrap `add_subdirectory(usd)` in `if (NOT EMSCRIPTEN)`, so they yield only
#   pxr/base and cannot open a stage.
#   tinygltf 2.8.21 matches the WriteImageDataFunction signature Adobe's gltf.cpp uses;
#   2.9.x added an FsCallbacks parameter and fails to compile.
$UsdTag = 'v26.08'
$TbbTag = 'v2022.2.0'
$TinyGltfTag = 'v2.8.21'

$EmsdkEnv = Join-Path $EmsdkDir 'emsdk_env.bat'
if (-not (Test-Path $EmsdkEnv)) {
    throw "Emscripten not found at '$EmsdkDir'. Install it and run 'emsdk install latest; emsdk activate latest'."
}
if (-not (Test-Path (Join-Path $AdobePluginsDir 'utils/src/layerRead.cpp'))) {
    throw "'$AdobePluginsDir' does not look like a USD-Fileformat-plugins checkout."
}

function Invoke-Emscripten {
    <#  Runs a command with the Emscripten environment loaded. emsdk_env.bat only
        affects the process it runs in, so the activation and the command must share
        a single cmd.exe invocation. #>
    param([Parameter(Mandatory = $true)][string]$Command)

    Write-Host "  > $Command" -ForegroundColor DarkGray
    & $env:ComSpec /c "call `"$EmsdkEnv`" >nul 2>&1 && $Command"
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Command"
    }
}

function Get-Source {
    param([string]$Url, [string]$Tag, [string]$Destination)

    if (Test-Path $Destination) {
        Write-Host "  already present: $Destination" -ForegroundColor DarkGray
        return
    }
    Write-Host "  cloning $Url @ $Tag" -ForegroundColor DarkGray
    git clone --depth 1 --branch $Tag $Url $Destination 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to clone $Url" }
}

New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null

# --- 1. oneTBB ------------------------------------------------------------
Write-Host "`n[1/4] oneTBB" -ForegroundColor Cyan
if (Test-Path (Join-Path $TbbInstall 'lib/libtbb.a')) {
    Write-Host '  up to date' -ForegroundColor DarkGray
} else {
    $tbbSrc = Join-Path $DepsDir 'oneTBB'
    Get-Source -Url 'https://github.com/oneapi-src/oneTBB.git' -Tag $TbbTag -Destination $tbbSrc
    Invoke-Emscripten "cd /d `"$tbbSrc`" && emcmake cmake -G Ninja -B build-wasm -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DTBB_STRICT=OFF -DTBB_TEST=OFF -DTBB_EXAMPLES=OFF -DTBB_DISABLE_HWLOC_AUTOMATIC_SEARCH=ON -DCMAKE_INSTALL_PREFIX=`"$TbbInstall`" -DCMAKE_CXX_FLAGS=`"-Wno-unused-command-line-argument -pthread`" -DCMAKE_C_FLAGS=`"-Wno-unused-command-line-argument -pthread`""
    Invoke-Emscripten "cd /d `"$tbbSrc`" && cmake --build build-wasm --target install"
}

# --- 2. OpenUSD -----------------------------------------------------------
Write-Host "`n[2/4] OpenUSD $UsdTag (this is the long one)" -ForegroundColor Cyan
if (Test-Path (Join-Path $UsdInstall 'lib/libusd_m.a')) {
    Write-Host '  up to date' -ForegroundColor DarkGray
} else {
    $usdSrc = Join-Path $DepsDir 'OpenUSD26'
    Get-Source -Url 'https://github.com/PixarAnimationStudios/OpenUSD.git' -Tag $UsdTag -Destination $usdSrc

    # PXR_BUILD_MONOLITHIC produces one archive, which is what the wasm link expects.
    # Imaging, Python, tools, tests and validation are all off: none contribute to
    # reading a stage or writing glTF, and each costs binary size.
    $usdFlags = @(
        '-DCMAKE_BUILD_TYPE=Release'
        '-DBUILD_SHARED_LIBS=OFF'
        '-DPXR_BUILD_MONOLITHIC=ON'
        '-DPXR_BUILD_IMAGING=OFF'
        '-DPXR_BUILD_USD_IMAGING=OFF'
        '-DPXR_BUILD_USDVIEW=OFF'
        '-DPXR_ENABLE_PYTHON_SUPPORT=OFF'
        '-DPXR_BUILD_TESTS=OFF'
        '-DPXR_BUILD_EXAMPLES=OFF'
        '-DPXR_BUILD_TUTORIALS=OFF'
        '-DPXR_BUILD_USD_TOOLS=OFF'
        '-DPXR_BUILD_USD_VALIDATION=OFF'
        '-DPXR_ENABLE_GL_SUPPORT=OFF'
        '-DPXR_ENABLE_MATERIALX_SUPPORT=OFF'
        "-DCMAKE_INSTALL_PREFIX=`"$UsdInstall`""
        "-DCMAKE_PREFIX_PATH=`"$TbbInstall`""
        "-DCMAKE_FIND_ROOT_PATH=`"$TbbInstall`""
        "-DTBB_DIR=`"$TbbInstall/lib/cmake/TBB`""
        '-DCMAKE_CXX_FLAGS="-pthread --use-port=zlib -Wno-unused-command-line-argument"'
        '-DCMAKE_C_FLAGS="-pthread --use-port=zlib -Wno-unused-command-line-argument"'
        '-DCMAKE_EXE_LINKER_FLAGS="-pthread"'
    ) -join ' '

    Invoke-Emscripten "cd /d `"$usdSrc`" && emcmake cmake -G Ninja -B build-wasm $usdFlags"
    Invoke-Emscripten "cd /d `"$usdSrc`" && cmake --build build-wasm --target install"
}

# --- 3. tinygltf ----------------------------------------------------------
Write-Host "`n[3/4] tinygltf $TinyGltfTag" -ForegroundColor Cyan
Get-Source -Url 'https://github.com/syoyo/tinygltf.git' -Tag $TinyGltfTag -Destination $TinyGltfDir

# --- 4. usd-web-gltf ------------------------------------------------------
Write-Host "`n[4/4] usd-web-gltf" -ForegroundColor Cyan
$buildDir = Join-Path $ProjectDir 'build'
if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

$projectFlags = @(
    '-DCMAKE_BUILD_TYPE=Release'
    "-DADOBE_PLUGINS_DIR=`"$AdobePluginsDir`""
    "-DTINYGLTF_DIR=`"$TinyGltfDir`""
    "-DCMAKE_PREFIX_PATH=`"$UsdInstall;$TbbInstall`""
    "-DCMAKE_FIND_ROOT_PATH=`"$UsdInstall;$TbbInstall`""
    "-DCMAKE_CXX_FLAGS=`"-pthread --use-port=zlib -Wno-unused-command-line-argument -I $TbbInstall/include`""
    '-DCMAKE_EXE_LINKER_FLAGS="-pthread"'
) -join ' '

Invoke-Emscripten "cd /d `"$ProjectDir`" && emcmake cmake -G Ninja -B build $projectFlags"
Invoke-Emscripten "cd /d `"$ProjectDir`" && cmake --build build"

Write-Host "`nBuild complete." -ForegroundColor Green
Get-ChildItem (Join-Path $buildDir 'bin') |
    Select-Object Name, @{ Name = 'MB'; Expression = { [math]::Round($_.Length / 1MB, 2) } } |
    Format-Table -AutoSize

Write-Host "Next: cd js; npm install; npm run build; node test/e2e.mjs" -ForegroundColor Cyan
