<#
.SYNOPSIS
    Builds usd-web-gltf: OpenUSD + Adobe's usdGltf plugin, compiled to WebAssembly.

.DESCRIPTION
    A thin convenience wrapper around the CMake `wasm` preset. It:
      1. Initialises the git submodules under dependencies/ (Adobe's plugins + tinygltf).
      2. Ensures Ninja is on PATH, borrowing the copy vcpkg downloads if necessary.
      3. Runs the configure + build workflow. Configuring drives vcpkg to compile
         OpenUSD as a static, monolithic library for the wasm32-emscripten triplet
         (using the overlay port in ports/usd and the overlay triplet in triplets/),
         then the build links the wasm module.

    All dependencies are handled by vcpkg and the submodules, so nothing has to be
    configured on the machine beyond the two prerequisites below.

.PARAMETER Clean
    Remove the build/ directory before configuring.

.NOTES
    Prerequisites (assumed already installed):
      * VCPKG_ROOT  -> a vcpkg checkout.
      * Emscripten  -> emcc on PATH, or the EMSDK / EMSCRIPTEN_ROOT environment variable.

    The first run compiles OpenUSD from source through vcpkg and takes roughly
    30-60 minutes. Later runs reuse vcpkg's binary cache and are fast.

.EXAMPLE
    ./scripts/build.ps1
#>
[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$ProjectDir = Split-Path -Parent $PSScriptRoot

if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT is not set. Point it at your vcpkg checkout."
}
if (-not (Get-Command emcc -ErrorAction SilentlyContinue) -and -not $env:EMSDK) {
    throw "Emscripten not found. Put emcc on PATH, or set EMSDK / EMSCRIPTEN_ROOT."
}

# The wasm preset uses the Ninja generator. Prefer a Ninja on PATH; otherwise reuse the
# one vcpkg downloads for its own builds so a separate install is not required.
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $ninja = Get-ChildItem "$env:VCPKG_ROOT/downloads/tools" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $ninja) {
        throw "Ninja was not found on PATH and none is bundled with vcpkg. Install Ninja."
    }
    $env:PATH = "$($ninja.DirectoryName);$env:PATH"
    Write-Host "Using Ninja from $($ninja.DirectoryName)" -ForegroundColor DarkGray
}

Push-Location $ProjectDir
try {
    Write-Host "`n[1/3] Submodules" -ForegroundColor Cyan
    git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "git submodule update failed." }

    if ($Clean -and (Test-Path "build")) {
        Write-Host "  removing build/" -ForegroundColor DarkGray
        Remove-Item -Recurse -Force "build"
    }

    Write-Host "`n[2/3] Configure  (vcpkg compiles OpenUSD on the first run; 30-60 min cold)" -ForegroundColor Cyan
    cmake --preset wasm
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    Write-Host "`n[3/3] Build" -ForegroundColor Cyan
    cmake --build --preset wasm
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed." }

    Write-Host "`nBuild complete." -ForegroundColor Green
    Get-ChildItem "build/wasm/bin" -ErrorAction SilentlyContinue |
        Select-Object Name, @{ Name = 'MB'; Expression = { [math]::Round($_.Length / 1MB, 2) } } |
        Format-Table -AutoSize

    Write-Host "Next: cd js; npm install; npm run build; node test/e2e.mjs" -ForegroundColor Cyan
}
finally {
    Pop-Location
}
