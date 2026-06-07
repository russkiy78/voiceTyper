<#
.SYNOPSIS
    Configures, builds, and deploys voiceTyper on Windows (MSVC), producing a
    self-contained executable folder at <BuildDir>\<BuildType>\.

.DESCRIPTION
    Runs CMake configure + build, then windeployqt so the .exe runs standalone
    (Qt DLLs and plugins are copied next to it). Requires:
      - Visual Studio 2022 (Desktop C++) + Windows SDK
      - CMake >= 3.21
      - Qt 6 MSVC kit (Core, Gui, Widgets, Multimedia, Network)
    Optional GPU backends are auto-detected: Vulkan SDK (glslc) / CUDA toolkit.

.PARAMETER QtPrefix
    Qt MSVC kit prefix (e.g. C:\Qt\6.11.1\msvc2022_64). Auto-detected from
    C:\Qt\*\msvc*_64 if omitted. May also be set via the QT_PREFIX env var.

.PARAMETER BuildType
    CMake config to build: Release (default) or Debug.

.PARAMETER BuildDir
    Build directory (default: build).

.PARAMETER NoWhisper
    Build without whisper.cpp (UI/plumbing only, NullAsrEngine).

.PARAMETER SkipDeploy
    Skip windeployqt (the .exe will then need Qt's bin on PATH to run).

.PARAMETER Clean
    Remove the build directory before configuring.

.PARAMETER ExtraCmakeArgs
    Extra args passed through to CMake configure (e.g. -DVOICETYPER_WITH_CUDA=OFF).

.EXAMPLE
    .\scripts\build-windows.ps1
    .\scripts\build-windows.ps1 -QtPrefix "C:\Qt\6.11.1\msvc2022_64"
    .\scripts\build-windows.ps1 -BuildType Debug -Clean
#>
param(
    [string]$QtPrefix = $env:QT_PREFIX,
    [ValidateSet("Release", "Debug")]
    [string]$BuildType = "Release",
    [string]$BuildDir = "build",
    [switch]$NoWhisper,
    [switch]$SkipDeploy,
    [switch]$Clean,
    [string[]]$ExtraCmakeArgs = @()
)

$ErrorActionPreference = "Stop"

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Exe failed with exit code $LASTEXITCODE"
    }
}

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# Resolve the build directory to an absolute path so messages are copy-pasteable.
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $Root $BuildDir
}

# Auto-detect a Qt MSVC kit (prefer the newest version, prefer the msvc2022 kit).
if ([string]::IsNullOrWhiteSpace($QtPrefix)) {
    $kits = @()
    if (Test-Path "C:\Qt") {
        $kits = Get-ChildItem "C:\Qt" -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                $ver = $_.Name
                Get-ChildItem $_.FullName -Directory -Filter "msvc*_64" -ErrorAction SilentlyContinue |
                    ForEach-Object {
                        [pscustomobject]@{
                            Version = try { [version]$ver } catch { [version]"0.0" }
                            Path    = $_.FullName
                        }
                    }
            }
    }
    $best = $kits | Sort-Object Version | Select-Object -Last 1
    if ($best) {
        $QtPrefix = $best.Path
    }
}

if ([string]::IsNullOrWhiteSpace($QtPrefix) -or -not (Test-Path $QtPrefix)) {
    throw "Qt MSVC kit not found. Pass -QtPrefix 'C:\Qt\6.x\msvc2022_64' or set `$env:QT_PREFIX."
}
Write-Host "Using Qt kit: $QtPrefix"

if ($Clean -and (Test-Path $BuildPath)) {
    Write-Host "Cleaning $BuildPath"
    Remove-Item -Recurse -Force $BuildPath
}

$withWhisper = if ($NoWhisper) { "OFF" } else { "ON" }

$configureArgs = @(
    "-S", $Root,
    "-B", $BuildPath,
    "-DCMAKE_PREFIX_PATH=$QtPrefix",
    "-DVOICETYPER_WITH_WHISPER=$withWhisper"
) + $ExtraCmakeArgs

Write-Host "==> Configuring (whisper=$withWhisper)"
Invoke-Tool "cmake" $configureArgs

Write-Host "==> Building $BuildType"
Invoke-Tool "cmake" @("--build", $BuildPath, "--config", $BuildType)

$exe = Join-Path $BuildPath (Join-Path $BuildType "voiceTyper.exe")
if (-not (Test-Path $exe)) {
    throw "Build reported success but executable not found at $exe"
}

if (-not $SkipDeploy) {
    $windeployqt = Join-Path $QtPrefix "bin\windeployqt.exe"
    if (-not (Test-Path $windeployqt)) {
        throw "windeployqt not found at $windeployqt (use -SkipDeploy to skip Qt deployment)."
    }
    Write-Host "==> Deploying Qt runtime next to the executable"
    $deployFlag = if ($BuildType -eq "Debug") { "--debug" } else { "--release" }
    Invoke-Tool $windeployqt @($deployFlag, "--no-translations", $exe)
}

Write-Host ""
Write-Host "Build complete."
Write-Host "Executable: $exe"

if (-not (Get-ChildItem (Join-Path $Root "models") -Filter "ggml-*.bin" -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "No speech model found. Download one before running:"
    Write-Host "  .\scripts\download-model.ps1                # small-q5_1 (~180 MB)"
    Write-Host "  .\scripts\download-model.ps1 large-v3-turbo-q5_0"
}
