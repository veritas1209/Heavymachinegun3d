param(
    [Parameter(Mandatory = $true)]
    [string]$InputApk,
    [Parameter(Mandatory = $true)]
    [string]$SmaliDirectory,
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) "output"),
    [string]$AndroidSdkRoot = $env:ANDROID_SDK_ROOT,
    [string]$BuildToolsVersion = "28.0.3",
    [string]$JavaExecutable = "java",
    [string]$KeyToolExecutable = "keytool",
    [string]$PythonExecutable = "python",
    [string]$SmaliJar = (Join-Path (Split-Path -Parent $PSScriptRoot) "tools\smali-2.5.2-fat.jar"),
    [switch]$ConvertPalettePngs,
    [switch]$ConvertStartupImagesToTga,
    [switch]$PatchNativeFileGetSize,
    [switch]$ProbeNativeMmapErrno,
    [switch]$ProbeNativeResourceRead,
    [switch]$PatchNativeJniArraySync,
    [switch]$PatchNativeTimerHeadUaf,
    [switch]$PatchNativeCurrentTime,
    [switch]$SkipNativeGyroArraySync
)

$ErrorActionPreference = "Stop"

$android16Port = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($AndroidSdkRoot)) {
    throw "AndroidSdkRoot was not supplied and ANDROID_SDK_ROOT is not set"
}

function Resolve-Executable([string]$NameOrPath) {
    if (Test-Path -LiteralPath $NameOrPath -PathType Leaf) {
        return (Resolve-Path -LiteralPath $NameOrPath).Path
    }
    $command = Get-Command $NameOrPath -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "Executable not found: $NameOrPath"
    }
    return $command.Source
}

$buildTools = Join-Path $AndroidSdkRoot "build-tools\$BuildToolsVersion"
$zipAlign = Join-Path $buildTools "zipalign.exe"
$apkSignerJar = Join-Path $buildTools "lib\apksigner.jar"
$java = Resolve-Executable $JavaExecutable
$smali = $SmaliJar
$python = Resolve-Executable $PythonExecutable
$pngConverter = Join-Path $PSScriptRoot "convert-palette-pngs.py"
$startupTgaConverter = Join-Path $PSScriptRoot "convert-startup-images-to-tga.py"
$nativeFileGetSizePatcher = Join-Path $PSScriptRoot "patch-native-file-getsize-lseek.py"
$nativeMmapErrnoPatcher = Join-Path $PSScriptRoot "patch-native-mmap-errno-probe.py"
$nativeResourceReadPatcher = Join-Path $PSScriptRoot "patch-native-resource-read-probe.py"
$nativeJniArraySyncPatcher = Join-Path $PSScriptRoot "patch-native-jni-int-array-sync.py"
$nativeTimerHeadUafPatcher = Join-Path $PSScriptRoot "patch-native-timer-head-uaf-probe.py"
$nativeCurrentTimePatcher = Join-Path $PSScriptRoot "patch-native-currenttime-gettimeofday.py"
$nativeGyroArraySyncPatcher = Join-Path $PSScriptRoot "patch-native-jni-float-array-sync.py"
$keyTool = Resolve-Executable $KeyToolExecutable
$keyStore = Join-Path (Split-Path -Parent $OutputDirectory) "heavy-gunner-compat-debug.keystore"
$outputStem = if ($PatchNativeTimerHeadUaf) { "heavy-gunner-native-compat" } elseif ($PatchNativeJniArraySync) { "heavy-gunner-native-jni-array-compat" } elseif ($ProbeNativeResourceRead) { "heavy-gunner-native-resource-read-probe" } elseif ($ProbeNativeMmapErrno) { "heavy-gunner-native-mmap-errno-probe" } elseif ($PatchNativeFileGetSize) { "heavy-gunner-native-getsize-probe" } elseif ($ConvertStartupImagesToTga) { "heavy-gunner-startup-tga-probe" } elseif ($ConvertPalettePngs) { "heavy-gunner-palette-compat" } else { "heavy-gunner-fd-instrumented" }
$patchedDex = Join-Path $OutputDirectory "fd-instrumented-classes.dex"
$unsignedApk = Join-Path $OutputDirectory "$outputStem-unsigned.apk"
$alignedApk = Join-Path $OutputDirectory "$outputStem-aligned.apk"
$signedApk = Join-Path $OutputDirectory "$outputStem.apk"

foreach ($requiredFile in @($InputApk, $java, $smali, $zipAlign, $apkSignerJar)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required file not found: $requiredFile"
    }
}
if (-not (Test-Path -LiteralPath $SmaliDirectory -PathType Container)) {
    throw "Smali directory not found: $SmaliDirectory"
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

if (-not (Test-Path -LiteralPath $keyStore -PathType Leaf)) {
    & $keyTool -genkeypair -keystore $keyStore -storepass android -keypass android -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000
    if ($LASTEXITCODE -ne 0) {
        throw "keytool failed with exit code $LASTEXITCODE"
    }
}

foreach ($oldOutput in @($patchedDex, $unsignedApk, $alignedApk, $signedApk)) {
    if (Test-Path -LiteralPath $oldOutput) {
        Remove-Item -LiteralPath $oldOutput -Force
    }
}

& $java -jar $smali assemble --api 25 --output $patchedDex $SmaliDirectory
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $patchedDex -PathType Leaf)) {
    throw "smali failed with exit code $LASTEXITCODE"
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("heavy-gunner-fd-probe-" + [Guid]::NewGuid().ToString("N"))
$unpacked = Join-Path $tempRoot "unpacked"
$zipCopy = Join-Path $tempRoot "source.zip"

try {
    New-Item -ItemType Directory -Force -Path $unpacked | Out-Null
    Copy-Item -LiteralPath $InputApk -Destination $zipCopy
    Expand-Archive -LiteralPath $zipCopy -DestinationPath $unpacked
    Copy-Item -LiteralPath $patchedDex -Destination (Join-Path $unpacked "classes.dex") -Force

    if ($PatchNativeFileGetSize -or $ProbeNativeMmapErrno -or $ProbeNativeResourceRead -or $PatchNativeJniArraySync -or $PatchNativeTimerHeadUaf -or $PatchNativeCurrentTime) {
        $legacyAbiDirectory = Join-Path $unpacked "lib\armeabi"
        $visibleAbiDirectory = Join-Path $unpacked "lib\armeabi-v7a"
        if (-not (Test-Path -LiteralPath $legacyAbiDirectory -PathType Container)) {
            throw "Legacy armeabi library directory not found: $legacyAbiDirectory"
        }
        New-Item -ItemType Directory -Force -Path $visibleAbiDirectory | Out-Null
        Copy-Item -Path (Join-Path $legacyAbiDirectory "*.so") -Destination $visibleAbiDirectory -Force

        foreach ($requiredFile in @($python, $nativeFileGetSizePatcher)) {
            if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
                throw "Required file not found: $requiredFile"
            }
        }
        $nativeLibraries = @(Get-ChildItem -LiteralPath (Join-Path $unpacked "lib") -Recurse -File -Filter "libgame.so")
        if ($nativeLibraries.Count -eq 0) {
            throw "No libgame.so was found in the unpacked APK"
        }
        foreach ($nativeLibrary in $nativeLibraries) {
            & $python $nativeFileGetSizePatcher $nativeLibrary.FullName
            if ($LASTEXITCODE -ne 0) {
                throw "Native File::GetSize patch failed with exit code $LASTEXITCODE"
            }
            if ($ProbeNativeMmapErrno) {
                if (-not (Test-Path -LiteralPath $nativeMmapErrnoPatcher -PathType Leaf)) {
                    throw "Required file not found: $nativeMmapErrnoPatcher"
                }
                & $python $nativeMmapErrnoPatcher $nativeLibrary.FullName
                if ($LASTEXITCODE -ne 0) {
                    throw "Native mmap errno probe patch failed with exit code $LASTEXITCODE"
                }
            }
            if ($ProbeNativeResourceRead) {
                if (-not (Test-Path -LiteralPath $nativeResourceReadPatcher -PathType Leaf)) {
                    throw "Required file not found: $nativeResourceReadPatcher"
                }
                & $python $nativeResourceReadPatcher $nativeLibrary.FullName
                if ($LASTEXITCODE -ne 0) {
                    throw "Native resource-read probe patch failed with exit code $LASTEXITCODE"
                }
            }
            if ($PatchNativeJniArraySync -or $PatchNativeTimerHeadUaf) {
                if (-not (Test-Path -LiteralPath $nativeJniArraySyncPatcher -PathType Leaf)) {
                    throw "Required file not found: $nativeJniArraySyncPatcher"
                }
                & $python $nativeJniArraySyncPatcher $nativeLibrary.FullName
                if ($LASTEXITCODE -ne 0) {
                    throw "Native JNI int[] synchronization patch failed with exit code $LASTEXITCODE"
                }
            }
            if ($PatchNativeTimerHeadUaf) {
                if (-not (Test-Path -LiteralPath $nativeTimerHeadUafPatcher -PathType Leaf)) {
                    throw "Required file not found: $nativeTimerHeadUafPatcher"
                }
                & $python $nativeTimerHeadUafPatcher $nativeLibrary.FullName
                if ($LASTEXITCODE -ne 0) {
                    throw "Native TM_Run head-node UAF patch failed with exit code $LASTEXITCODE"
                }
            }
            if ($PatchNativeCurrentTime -or $PatchNativeTimerHeadUaf) {
                if (-not (Test-Path -LiteralPath $nativeCurrentTimePatcher -PathType Leaf)) {
                    throw "Required file not found: $nativeCurrentTimePatcher"
                }
                & $python $nativeCurrentTimePatcher $nativeLibrary.FullName
                if ($LASTEXITCODE -ne 0) {
                    throw "Native current-time compatibility patch failed with exit code $LASTEXITCODE"
                }
            }
            if ($PatchNativeTimerHeadUaf -and -not $SkipNativeGyroArraySync) {
                if (-not (Test-Path -LiteralPath $nativeGyroArraySyncPatcher -PathType Leaf)) {
                    throw "Required file not found: $nativeGyroArraySyncPatcher"
                }
                & $python $nativeGyroArraySyncPatcher $nativeLibrary.FullName
                if ($LASTEXITCODE -ne 0) {
                    throw "Native JNI float[3] accelerometer synchronization patch failed with exit code $LASTEXITCODE"
                }
            }
        }
    }

    if ($PatchNativeTimerHeadUaf) {
        Write-Output "This compatibility build includes File::GetSize, JNI asset/accelerometer synchronization, a safe TM_Run head removal, and an ART-compatible native clock."
    } elseif ($PatchNativeJniArraySync) {
        Write-Output "This compatibility build patches File::GetSize and refreshes the legacy JNI int[] asset metadata after each Java call."
    } elseif ($ProbeNativeResourceRead) {
        Write-Output "This diagnostic build patches File::GetSize and exposes CS_knlGetResource failures as fault address 0x12."
    } elseif ($ProbeNativeMmapErrno) {
        Write-Output "This diagnostic build patches File::GetSize and exposes mmap errno through the tombstone fault address."
    } elseif ($PatchNativeFileGetSize) {
        Write-Output "This diagnostic build replaces only the original File::GetSize implementation with File::Seek(0, SEEK_END)."
    } elseif ($ConvertPalettePngs) {
        foreach ($requiredFile in @($python, $pngConverter)) {
            if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
                throw "Required file not found: $requiredFile"
            }
        }
        & $python $pngConverter (Join-Path $unpacked "assets\common\game_res") (Join-Path $unpacked "assets\common\game_res_kor")
        if ($LASTEXITCODE -ne 0) {
            throw "PNG conversion failed with exit code $LASTEXITCODE"
        }
    }

    if ($ConvertStartupImagesToTga) {
        foreach ($requiredFile in @($python, $startupTgaConverter)) {
            if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
                throw "Required file not found: $requiredFile"
            }
        }
        & $python $startupTgaConverter (Join-Path $unpacked "assets\common\game_res") (Join-Path $unpacked "assets\common\game_res_kor")
        if ($LASTEXITCODE -ne 0) {
            throw "Startup TGA conversion failed with exit code $LASTEXITCODE"
        }
    }

    $oldSignature = Join-Path $unpacked "META-INF"
    if (Test-Path -LiteralPath $oldSignature) {
        Remove-Item -LiteralPath $oldSignature -Recurse -Force
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $unpacked,
        $unsignedApk,
        [System.IO.Compression.CompressionLevel]::NoCompression,
        $false
    )

    & $zipAlign -f 4 $unsignedApk $alignedApk
    if ($LASTEXITCODE -ne 0) {
        throw "zipalign failed with exit code $LASTEXITCODE"
    }

    & $java -jar $apkSignerJar sign --ks $keyStore --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android --v1-signing-enabled true --v2-signing-enabled true --out $signedApk $alignedApk
    if ($LASTEXITCODE -ne 0) {
        throw "apksigner failed with exit code $LASTEXITCODE"
    }

    & $java -jar $apkSignerJar verify --verbose $signedApk
    if ($LASTEXITCODE -ne 0) {
        throw "APK signature verification failed with exit code $LASTEXITCODE"
    }

    Write-Output "Created: $signedApk"
    if ($PatchNativeTimerHeadUaf) {
        Write-Output "This build preserves the original timer, redraw, and accelerometer calculations while refreshing values that ART exposes through detached JNI array copies."
    } elseif ($PatchNativeJniArraySync) {
        Write-Output "This build preserves the original resource values while making their JNI transfer compatible with ART."
    } elseif ($ProbeNativeResourceRead) {
        Write-Output "This is a native resource-read probe for diagnosis, not distribution."
    } elseif ($ProbeNativeMmapErrno) {
        Write-Output "This is an errno probe build for diagnosis, not distribution."
    } elseif ($PatchNativeFileGetSize) {
        Write-Output "This is a native File::GetSize compatibility probe, not distribution."
    } elseif ($ConvertPalettePngs) {
        Write-Output "This diagnostic build also rewrites paletted PNG storage to pixel-identical RGBA PNG."
    } elseif ($ConvertStartupImagesToTga) {
        Write-Output "This diagnostic build rewrites only four startup images to pixel-identical RGBA TGA payloads."
    } else {
        Write-Output "This build changes only WrapperJinterface logging and is for diagnosis, not distribution."
    }
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
