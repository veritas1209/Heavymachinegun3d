[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vsRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$cxx = 'C:/mingw64/bin/g++.exe'
$cc = 'C:/mingw64/bin/gcc.exe'
$build = Join-Path $root 'build'

foreach ($required in @($cmake, $ninja, $cc, $cxx)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required Visual Studio build tool was not found: $required"
    }
}

New-Item -ItemType Directory -Force -Path $build | Out-Null
& $cmake -S $root -B $build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_C_COMPILER=$cc" `
    "-DCMAKE_CXX_COMPILER=$cxx" `
    "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -eq 0) {
    & $cmake --build $build --config $Configuration
}
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

$exe = Join-Path $build 'bin\heavy_gunner_port.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Build completed without the expected executable: $exe"
}

Write-Host "Built: $exe"
$probe = Join-Path $build 'bin\pact_format_probe.exe'
if (Test-Path -LiteralPath $probe) {
    Write-Host "Built: $probe"
}
$openGl = Join-Path $build 'bin\heavy_gunner_gl.exe'
if (Test-Path -LiteralPath $openGl) {
    Write-Host "Built: $openGl"
}
