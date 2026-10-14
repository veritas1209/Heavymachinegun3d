param(
    [string]$OutputDirectory = "C:\Users\hajin\IT_Projects\android emul\android16-port\output\diagnostics"
)

$ErrorActionPreference = "Stop"

$android16Port = Split-Path -Parent $PSScriptRoot
$java = "C:\Program Files\Java\jre1.8.0_431\bin\java.exe"
$smali = Join-Path $android16Port "tools\smali-2.5.2-fat.jar"
$smaliSource = Join-Path $PSScriptRoot "smali"
$outputDex = Join-Path $OutputDirectory "asset-fd-probe.dex"

foreach ($requiredFile in @($java, $smali)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required file not found: $requiredFile"
    }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

if (Test-Path -LiteralPath $outputDex) {
    Remove-Item -LiteralPath $outputDex -Force
}

& $java -jar $smali assemble --api 25 --output $outputDex $smaliSource
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $outputDex -PathType Leaf)) {
    throw "smali failed with exit code $LASTEXITCODE"
}

Write-Output "Created: $outputDex"
