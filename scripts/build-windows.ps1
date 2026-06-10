<#
.SYNOPSIS
    Configures, builds, and deploys voiceTyper on Windows (MSVC + Ninja),
    producing a self-contained executable folder at <BuildDir>\.

.DESCRIPTION
    Runs CMake configure + build with the Ninja generator, then windeployqt so
    the .exe runs standalone: Qt DLLs, Qt plugins, and the MSVC runtime
    (vcruntime140*.dll / msvcp140*.dll, via --compiler-runtime) are all copied
    next to the executable. The resulting folder is self-contained and runs on a
    clean machine with no Visual C++ Redistributable install required. whisper.cpp
    and ggml are statically linked into the .exe already (BUILD_SHARED_LIBS=OFF).

    When the CUDA / Vulkan backends are compiled in, their runtime DLLs
    (cudart64_*.dll + cuBLAS for CUDA, vulkan-1.dll for Vulkan) are also copied
    next to the .exe, so the folder runs on machines without the CUDA toolkit or
    Vulkan SDK installed. Build with
    -ExtraCmakeArgs "-DVOICETYPER_WITH_CUDA=OFF -DVOICETYPER_WITH_VULKAN=OFF"
    for a smaller CPU-only folder.

    Note: full single-file static linking (Qt linked into the .exe) is NOT done
    here - the standard Qt installer ships only shared kits. It would require a
    static Qt build from source.

    The MSVC build environment is initialized automatically (vcvars64.bat, located
    via vswhere), so the script can be run from a plain PowerShell prompt - no
    "Developer PowerShell" needed. Ninja is used rather than the Visual Studio
    generator because the CUDA backend builds by invoking nvcc directly; the VS
    generator instead needs CUDA's MSBuild integration, which is absent for newer
    Visual Studio versions ("No CUDA toolset found").

    Requires:
      - Visual Studio 2022+ (Desktop C++) + Windows SDK
      - CMake >= 3.21
      - Qt 6 MSVC kit (Core, Gui, Widgets, Multimedia, Network)
    Optional GPU backends are auto-detected: Vulkan SDK (via VULKAN_SDK env /
    C:\VulkanSDK\*) and CUDA toolkit (nvcc). A backend is compiled in only when
    its toolchain is present on the build host; otherwise it is switched off so
    the configure step stays quiet.

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
    Skip windeployqt (the .exe will then need Qt's bin and the MSVC runtime on
    PATH to run, and won't be self-contained).

.PARAMETER Clean
    Remove the build directory before configuring.

.PARAMETER SkipVsDevEnv
    Skip auto-initializing the MSVC environment (use when already running inside a
    Developer prompt, or to point at a custom toolchain).

.PARAMETER CudaArch
    CUDA compute architecture(s) to compile for, e.g. "89" (RTX 40xx) or a list
    like "86;89;90". Default "auto" detects the build host's GPU via nvidia-smi.
    This is passed as CMAKE_CUDA_ARCHITECTURES; setting it explicitly avoids
    ggml-cuda's default "native" probe, which silently produces an EMPTY arch (a
    GPU-less, non-functional CUDA backend) whenever the GPU isn't visible to the
    build process.

.PARAMETER ExtraCmakeArgs
    Extra args passed through to CMake configure (e.g. -DVOICETYPER_WITH_CUDA=OFF).

.PARAMETER Installer
    After building, package Windows installers (.exe) with Inno Setup. FOUR
    installers are produced, one per GPU-backend combination:
        voiceTyper-<ver>-cpu-setup.exe      CPU only
        voiceTyper-<ver>-vulkan-setup.exe   CPU + Vulkan
        voiceTyper-<ver>-cuda-setup.exe     CPU + CUDA
        voiceTyper-<ver>-all-setup.exe      CPU + Vulkan + CUDA (universal)
    Each variant is configured and built in its own directory (<BuildDir>-cpu,
    <BuildDir>-vulkan, <BuildDir>-cuda, <BuildDir>-all) so their CMake caches -
    which bake in the GGML_CUDA / GGML_VULKAN choice - don't collide. The full
    self-contained runtime (the executable, all Qt DLLs and plugins, the MSVC
    runtime, and only the backend DLLs that variant uses - CUDA cuBLAS / Vulkan
    loader) is staged via windeployqt into <that dir>\dist, then compiled into a
    setup .exe written to <BuildDir>. A variant whose toolchain is absent on the
    build host (no CUDA toolkit / no Vulkan SDK) is skipped with a warning, since
    CMake silently degrades a missing backend to CPU. The application icon (built
    from the voicetyper_*x*.png sources) is embedded in the .exe and used for the
    installer and Start Menu / Desktop shortcuts. If Inno Setup's ISCC.exe isn't
    found it is installed automatically via winget.

.PARAMETER IncludeModel
    With -Installer, also bundle the speech models (models\ggml-*.bin) into the
    installer so the app works out of the box. Off by default because the models
    are large (180 MB - 1 GB+); without it the user downloads a model on first run.

.PARAMETER AppVersion
    Version string stamped into the installer. Defaults to the project VERSION
    parsed from CMakeLists.txt.

.EXAMPLE
    .\scripts\build-windows.ps1
    .\scripts\build-windows.ps1 -QtPrefix "C:\Qt\6.11.1\msvc2022_64"
    .\scripts\build-windows.ps1 -BuildType Debug -Clean
    .\scripts\build-windows.ps1 -Installer            # 4 installers: CPU / Vulkan / CUDA / All
    .\scripts\build-windows.ps1 -Installer -IncludeModel
#>
param(
    [string]$QtPrefix = $env:QT_PREFIX,
    [ValidateSet("Release", "Debug")]
    [string]$BuildType = "Release",
    [string]$BuildDir = "build",
    [switch]$NoWhisper,
    [switch]$SkipDeploy,
    [switch]$SkipVsDevEnv,
    [string]$CudaArch = "auto",
    [switch]$Clean,
    [string[]]$ExtraCmakeArgs = @(),
    [switch]$Installer,
    [switch]$IncludeModel,
    [string]$AppVersion
)

if ($Installer -and $SkipDeploy) {
    throw "-Installer cannot be combined with -SkipDeploy: the installer needs the deployed runtime DLLs."
}

$ErrorActionPreference = "Stop"

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments)
    # Native tools (cmake, windeployqt) routinely write progress and warnings to
    # stderr. Under $ErrorActionPreference='Stop', Windows PowerShell escalates any
    # native stderr output to a terminating error even when the tool exits 0, so
    # relax the preference for the call and judge success by the real exit code.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @Arguments
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($LASTEXITCODE -ne 0) {
        throw "$Exe failed with exit code $LASTEXITCODE"
    }
}

# Locate vcvars64.bat (the MSVC x64 dev-environment initializer) via vswhere,
# falling back to a scan of the standard install roots.
function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vsRoot) {
            $vc = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vc) { return $vc }
        }
    }
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $pf) { continue }
        $found = Get-ChildItem (Join-Path $pf "Microsoft Visual Studio") -Recurse -Filter "vcvars64.bat" `
            -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

# Run vcvars64.bat in a child cmd and import the resulting environment (PATH,
# INCLUDE, LIB, CUDA_PATH, ...) into this process so Ninja finds cl.exe / nvcc.
function Import-VsDevEnv {
    param([string]$VcVars)
    Write-Host "Initializing MSVC environment: $VcVars"
    $output = & cmd /c "`"$VcVars`" >nul 2>&1 && set"
    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2]
        }
    }
}

# Build a multi-resolution .ico (resources\voicetyper.ico) from the repo's
# voicetyper_*x*.png sources, so the .exe gets an embedded application icon and
# the installer/shortcuts have one to use. Each PNG frame is embedded directly
# (Windows Vista+ accepts PNG-compressed icon entries), so no ImageMagick or PIL
# is needed. Returns the .ico path, or $null when no source PNGs are present.
function New-AppIcon {
    param([string]$Root)
    $resDir = Join-Path $Root "resources"
    $icoPath = Join-Path $resDir "voicetyper.ico"
    $frames = @()
    foreach ($size in 16, 32, 48, 64, 128, 256) {
        $png = Join-Path $Root "voicetyper_${size}x${size}.png"
        if (Test-Path $png) {
            $frames += [pscustomobject]@{ Size = $size; Bytes = [System.IO.File]::ReadAllBytes($png) }
        }
    }
    if (-not $frames) {
        Write-Warning "No voicetyper_*x*.png sources found in $Root; skipping app-icon generation."
        return $null
    }
    if (-not (Test-Path $resDir)) { New-Item -ItemType Directory -Path $resDir | Out-Null }

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    try {
        # ICONDIR header.
        $bw.Write([uint16]0)             # reserved
        $bw.Write([uint16]1)             # type = icon
        $bw.Write([uint16]$frames.Count) # image count
        # ICONDIRENTRY records; PNG blobs are appended after all entries.
        $offset = 6 + (16 * $frames.Count)
        foreach ($f in $frames) {
            $dim = if ($f.Size -ge 256) { 0 } else { $f.Size }  # 0 encodes 256
            $bw.Write([byte]$dim)        # width
            $bw.Write([byte]$dim)        # height
            $bw.Write([byte]0)           # palette size
            $bw.Write([byte]0)           # reserved
            $bw.Write([uint16]1)         # color planes
            $bw.Write([uint16]32)        # bits per pixel
            $bw.Write([uint32]$f.Bytes.Length)
            $bw.Write([uint32]$offset)
            $offset += $f.Bytes.Length
        }
        foreach ($f in $frames) { $bw.Write($f.Bytes) }
        $bw.Flush()
        [System.IO.File]::WriteAllBytes($icoPath, $ms.ToArray())
    } finally {
        $bw.Dispose()
        $ms.Dispose()
    }
    Write-Host "Generated app icon: $icoPath ($($frames.Count) sizes)"
    return $icoPath
}

# Make the folder containing $TargetExe self-contained: windeployqt copies Qt
# DLLs, plugins and the MSVC runtime, then the GPU backend runtime DLLs (CUDA
# cuBLAS / Vulkan loader) that windeployqt doesn't know about are copied in.
function Invoke-RuntimeDeploy {
    param(
        [string]$TargetExe,
        [string]$Windeployqt,
        [string]$BuildType,
        [string]$CudaCompiler,
        [bool]$HaveVulkan,
        [string]$VulkanSdk
    )
    Write-Host "==> Deploying Qt + MSVC runtime next to $TargetExe"
    $deployFlag = if ($BuildType -eq "Debug") { "--debug" } else { "--release" }
    # --compiler-runtime copies the MSVC runtime DLLs (vcruntime140*.dll, msvcp140*.dll)
    # next to the .exe, so the folder is self-contained and runs on a clean machine
    # without installing the Visual C++ Redistributable.
    Invoke-Tool $Windeployqt @($deployFlag, "--no-translations", "--compiler-runtime", $TargetExe)

    $exeDir = Split-Path -Parent $TargetExe

    # CUDA: ggml-cuda needs the CUDA runtime plus cuBLAS. The version suffix
    # (e.g. _12 / _13) tracks the toolkit, so match by wildcard. CUDA <=12 ships
    # these in the toolkit's bin (alongside nvcc); CUDA 13 moved them into
    # bin\x64, so search both.
    if ($CudaCompiler) {
        $cudaBin = Split-Path -Parent $CudaCompiler
        $cudaSearchDirs = @($cudaBin, (Join-Path $cudaBin "x64")) | Where-Object { Test-Path $_ }
        $cudaDlls = @()
        foreach ($dir in $cudaSearchDirs) {
            foreach ($pat in @("cudart64_*.dll", "cublas64_*.dll", "cublasLt64_*.dll")) {
                $cudaDlls += Get-ChildItem (Join-Path $dir $pat) -ErrorAction SilentlyContinue
            }
        }
        if ($cudaDlls) {
            Write-Host "==> Copying CUDA runtime DLLs"
            foreach ($dll in $cudaDlls) {
                Copy-Item $dll.FullName -Destination $exeDir -Force
                Write-Host "    $($dll.Name)  (from $(Split-Path -Parent $dll.FullName))"
            }
        } else {
            Write-Warning "CUDA backend was built but no cudart/cublas DLLs found under $cudaBin (searched: $($cudaSearchDirs -join ', ')) - the GPU build may need them at runtime."
        }
    }

    # Vulkan: ggml-vulkan loads vulkan-1.dll. On a machine with up-to-date GPU
    # drivers this loader is already in System32, so bundling it is usually
    # unnecessary; copy it as a fallback for clean machines. Vulkan SDK 1.4+ no
    # longer ships the loader (it's driver-delivered), so fall back to the
    # System32 copy when the SDK's Bin doesn't have one.
    if ($HaveVulkan) {
        $vkDll = @(
            (Join-Path $VulkanSdk "Bin\vulkan-1.dll"),
            (Join-Path $env:SystemRoot "System32\vulkan-1.dll")
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($vkDll) {
            Write-Host "==> Copying Vulkan loader (vulkan-1.dll) from $(Split-Path -Parent $vkDll)"
            Copy-Item $vkDll -Destination $exeDir -Force
        } else {
            Write-Warning "Vulkan backend built but no vulkan-1.dll found in the SDK or System32 - the loader will need to come from the target's GPU drivers."
        }
    }
}

# Search PATH and the known Inno Setup install roots (machine-wide and per-user)
# for ISCC.exe; returns its path or $null.
function Find-ISCC {
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # Inno Setup installs to "Inno Setup 6" by default, but can land in Program
    # Files, Program Files (x86), or %LOCALAPPDATA%\Programs for a per-user
    # install (winget's default). Glob the version folder to be future-proof.
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)},
            (Join-Path $env:LOCALAPPDATA "Programs"))) {
        if (-not $root) { continue }
        $found = Get-ChildItem $root -Directory -Filter "Inno Setup*" -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName "ISCC.exe" } |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}

# Locate Inno Setup's command-line compiler, installing it via winget if absent.
function Get-ISCC {
    $iscc = Find-ISCC
    if ($iscc) { return $iscc }

    $manual = "Install Inno Setup 6 manually from https://jrsoftware.org/isdl.php (or 'winget install -e --id JRSoftware.InnoSetup --source winget') and re-run with -Installer."
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "Inno Setup (ISCC.exe) not found and winget is unavailable. $manual"
    }
    Write-Host "==> Inno Setup not found; attempting install via winget (JRSoftware.InnoSetup)"
    # Don't route winget through Invoke-Tool: it routinely returns non-zero for
    # benign reasons (source agreements, msstore fallback, already-installed), and
    # we'd rather fall back to a clear manual-install message than abort the build.
    # --source winget avoids the msstore source entirely.
    & winget install -e --id JRSoftware.InnoSetup --source winget `
        --accept-source-agreements --accept-package-agreements
    $wingetExit = $LASTEXITCODE

    $iscc = Find-ISCC
    if ($iscc) { return $iscc }
    throw "Could not locate ISCC.exe after winget (exit $wingetExit). $manual"
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
                        Version = [version]$ver
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

$withWhisper = if ($NoWhisper) { "OFF" } else { "ON" }

# Bring the MSVC toolchain (cl.exe, Ninja, link.exe) onto PATH unless we're
# already inside a Developer prompt or the caller opted out.
if (-not $SkipVsDevEnv -and -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = Find-VcVars
    if (-not $vcvars) {
        throw "vcvars64.bat not found. Install Visual Studio with the 'Desktop development with C++' workload, or run from a Developer prompt and pass -SkipVsDevEnv."
    }
    Import-VsDevEnv $vcvars
}

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw "ninja not found on PATH. It ships with the VS C++ workload (CMake component); ensure the MSVC environment initialized correctly."
}

# CUDA: ggml-cuda needs nvcc. Pass its path explicitly so CMake's CUDA language
# check succeeds even when the toolkit's bin isn't on PATH at configure time.
$nvcc = Get-Command nvcc -ErrorAction SilentlyContinue
if ($nvcc) {
    $cudaCompiler = $nvcc.Source
    Write-Host "Using CUDA toolkit: $(Split-Path (Split-Path $cudaCompiler))"

    # Resolve CMAKE_CUDA_ARCHITECTURES. ggml-cuda otherwise defaults to "native",
    # whose device probe yields an EMPTY arch when no GPU is visible to the build
    # process - producing a CUDA backend that compiles but enumerates zero devices
    # at runtime (the "CUDA missing from the backend list" symptom).
    $cudaArchResolved = $CudaArch
    if ($CudaArch -eq "auto") {
        $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
        if (-not $smi -and (Test-Path "$env:SystemRoot\System32\nvidia-smi.exe")) {
            $smi = "$env:SystemRoot\System32\nvidia-smi.exe"
        }
        if ($smi) {
            $cap = (& $smi --query-gpu=compute_cap --format=csv,noheader 2>$null |
                Select-Object -First 1)
            if ($cap -match '(\d+)\.(\d+)') {
                $cudaArchResolved = "$($matches[1])$($matches[2])"
            }
        }
        if ($cudaArchResolved -eq "auto") {
            # nvidia-smi unavailable: build a fat binary covering Turing..Blackwell
            # so the result still runs on the target GPU.
            $cudaArchResolved = "75;80;86;89;90"
            Write-Host "CUDA arch: GPU not detected, building for $cudaArchResolved"
        } else {
            Write-Host "CUDA arch: $cudaArchResolved (detected)"
        }
    } else {
        Write-Host "CUDA arch: $cudaArchResolved (requested)"
    }
}

# Vulkan: find_package(Vulkan) locates the loader (Lib\vulkan-1.lib) and the
# glslc shader compiler (Bin\glslc.exe) through the VULKAN_SDK env var, which the
# SDK installer normally sets. If it's missing, auto-detect the newest
# C:\VulkanSDK\* so the Vulkan backend can still be compiled in. Note: the lib
# lives in <SDK>\Lib (NOT \Lib\x64) - let find_package resolve it; don't hardcode.
if ([string]::IsNullOrWhiteSpace($env:VULKAN_SDK) -or -not (Test-Path $env:VULKAN_SDK)) {
    $vk = Get-ChildItem "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Name } | Select-Object -Last 1
    if ($vk) { $env:VULKAN_SDK = $vk.FullName }
}
$haveVulkan = $env:VULKAN_SDK -and (Test-Path (Join-Path $env:VULKAN_SDK "Bin\glslc.exe"))
if ($haveVulkan) {
    Write-Host "Using Vulkan SDK: $env:VULKAN_SDK"
}

# Generate the embedded app icon once, before any configure, so CMake compiles
# it into the .exe (see the resources\voicetyper.rc block in CMakeLists.txt).
$icoPath = New-AppIcon $Root

$windeployqt = Join-Path $QtPrefix "bin\windeployqt.exe"

# ----------------------------------------------------------------------------
# Decide what to build.
#
#   * Without -Installer: a single dev build into $BuildPath using whatever GPU
#     backends this host's toolchains support (the historical behaviour).
#   * With -Installer: four RELEASE builds, one per backend combination, each
#     packaged into its own installer -
#         CPU only | CPU+Vulkan | CPU+CUDA | All (CPU+Vulkan+CUDA)
#     Each variant gets its own build dir ($BuildPath-cpu / -vulkan / -cuda /
#     -all) so their CMake caches (which bake in GGML_CUDA / GGML_VULKAN) don't
#     collide, and all four setup .exes are written side by side into $BuildPath.
#
# A CUDA / Vulkan variant can only be built where that toolchain is present;
# since CMake silently degrades a missing backend to CPU (cmake\WhisperCpp.cmake),
# we must gate here rather than ship a "cuda" installer that is secretly CPU.
# ----------------------------------------------------------------------------
$haveCuda = [bool]$cudaCompiler

if ($Installer) {
    if ([string]::IsNullOrWhiteSpace($AppVersion)) {
        $cml = Get-Content (Join-Path $Root "CMakeLists.txt") -Raw
        $AppVersion = if ($cml -match 'project\(voiceTyper[\s\S]*?VERSION\s+([0-9][0-9.]*)') {
            $matches[1]
        } else {
            "0.0.0"
        }
    }

    $variants = @(
        [pscustomobject]@{ Name = "cpu";    Label = "CPU only";            Cuda = $false; Vulkan = $false }
        [pscustomobject]@{ Name = "vulkan"; Label = "CPU + Vulkan";        Cuda = $false; Vulkan = $true  }
        [pscustomobject]@{ Name = "cuda";   Label = "CPU + CUDA";          Cuda = $true;  Vulkan = $false }
        [pscustomobject]@{ Name = "all";    Label = "CPU + Vulkan + CUDA"; Cuda = $true;  Vulkan = $true  }
    )

    $jobs = @()
    $skipped = @()
    foreach ($v in $variants) {
        if ($v.Cuda -and -not $haveCuda) {
            $skipped += "$($v.Label) - CUDA toolkit (nvcc) not found on this build host"
            Write-Warning "Skipping '$($v.Label)' installer: no CUDA toolkit (nvcc) on this build host."
            continue
        }
        if ($v.Vulkan -and -not $haveVulkan) {
            $skipped += "$($v.Label) - Vulkan SDK (glslc) not found on this build host"
            Write-Warning "Skipping '$($v.Label)' installer: no Vulkan SDK (glslc) on this build host."
            continue
        }
        $jobs += [pscustomobject]@{
            Name      = $v.Name
            Label     = $v.Label
            Cuda      = $v.Cuda
            Vulkan    = $v.Vulkan
            BuildPath = "$BuildPath-$($v.Name)"
            Package   = $true
        }
    }

    # $BuildPath itself isn't a build dir in installer mode (each variant builds
    # into $BuildPath-<name>); ensure it exists to collect the setup .exes.
    if (-not (Test-Path $BuildPath)) { New-Item -ItemType Directory -Path $BuildPath | Out-Null }

    # Resolve ISCC once up front (may trigger a one-time winget install) rather
    # than re-probing per variant.
    $iscc = Get-ISCC
} else {
    $jobs = @(
        [pscustomobject]@{
            Name      = "dev"
            Label     = "dev build"
            Cuda      = $haveCuda
            Vulkan    = [bool]$haveVulkan
            BuildPath = $BuildPath
            Package   = $false
        }
    )
}

$produced = @()

foreach ($job in $jobs) {
    $jobBuildPath = $job.BuildPath

    if ($job.Package) {
        Write-Host ""
        Write-Host "############################################################"
        Write-Host "## Variant: $($job.Label)  ->  $jobBuildPath"
        Write-Host "############################################################"
    }

    if ($Clean -and (Test-Path $jobBuildPath)) {
        Write-Host "Cleaning $jobBuildPath"
        Remove-Item -Recurse -Force $jobBuildPath
    }

    # Ninja is a single-config generator, so the build type is fixed at configure
    # time via CMAKE_BUILD_TYPE (there is no per-build --config selection). The
    # GPU backends are pinned ON/OFF per variant; the explicit OFF is what makes a
    # "CPU only" / "CPU+Vulkan" build exclude CUDA even on a host that has the
    # CUDA toolkit (and likewise for Vulkan).
    $configureArgs = @(
        "-S", $Root,
        "-B", $jobBuildPath,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_PREFIX_PATH=$QtPrefix",
        "-DVOICETYPER_WITH_WHISPER=$withWhisper"
    )
    if ($job.Cuda) {
        $configureArgs += "-DVOICETYPER_WITH_CUDA=ON"
        $configureArgs += "-DCMAKE_CUDA_COMPILER=$cudaCompiler"
        $configureArgs += "-DCMAKE_CUDA_ARCHITECTURES=$cudaArchResolved"
    } else {
        $configureArgs += "-DVOICETYPER_WITH_CUDA=OFF"
    }
    if ($job.Vulkan) {
        $configureArgs += "-DVOICETYPER_WITH_VULKAN=ON"
    } else {
        $configureArgs += "-DVOICETYPER_WITH_VULKAN=OFF"
    }
    $configureArgs += $ExtraCmakeArgs

    Write-Host "==> Configuring $($job.Label) (whisper=$withWhisper, cuda=$($job.Cuda), vulkan=$($job.Vulkan))"
    Invoke-Tool "cmake" $configureArgs

    Write-Host "==> Building $BuildType"
    Invoke-Tool "cmake" @("--build", $jobBuildPath)

    # Locate the produced exe. Ninja (single-config) emits <BuildDir>\voiceTyper.exe;
    # the second candidate covers a multi-config generator if one is forced in via
    # -ExtraCmakeArgs.
    $exeCandidates = @(
        (Join-Path $jobBuildPath "voiceTyper.exe"),
        (Join-Path $jobBuildPath (Join-Path $BuildType "voiceTyper.exe"))
    )
    $exe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $exe) {
        throw "Build reported success but executable not found (looked in: $($exeCandidates -join '; '))"
    }

    if (-not $SkipDeploy) {
        if (-not (Test-Path $windeployqt)) {
            throw "windeployqt not found at $windeployqt (use -SkipDeploy to skip Qt deployment)."
        }
        # The .exe statically links ggml/whisper, but the CUDA and Vulkan backends
        # still pull in shared runtime DLLs that windeployqt doesn't know about;
        # Invoke-RuntimeDeploy copies those next to the .exe too, keeping the folder
        # self-contained on machines that lack the CUDA toolkit / Vulkan SDK. Only
        # the backends this variant was actually built with get their DLLs bundled.
        Invoke-RuntimeDeploy -TargetExe $exe -Windeployqt $windeployqt -BuildType $BuildType `
            -CudaCompiler $(if ($job.Cuda) { $cudaCompiler } else { $null }) `
            -HaveVulkan ([bool]$job.Vulkan) -VulkanSdk $env:VULKAN_SDK
    }

    if (-not $job.Package) {
        Write-Host ""
        Write-Host "Build complete."
        Write-Host "Executable: $exe"
        continue
    }

    # ------------------------------------------------------------------------
    # Package this variant with Inno Setup. The whole runtime is staged into
    # <variant build dir>\dist (a clean tree, separate from CMake's build junk)
    # via a fresh windeployqt run, then ISCC compiles it into a setup .exe named
    # voiceTyper-<version>-<variant>-setup.exe in $BuildPath. All four variants
    # share the same AppName / install dir, so installing one replaces another -
    # the user picks the single build matching their hardware.
    # ------------------------------------------------------------------------
    Write-Host ""
    Write-Host "==> Packaging $($job.Label) installer (version $AppVersion)"

    $stage = Join-Path $jobBuildPath "dist"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Path $stage | Out-Null

    Copy-Item $exe -Destination $stage -Force
    Copy-Item (Join-Path $Root "config\commands.default.json") -Destination $stage -Force
    $stageExe = Join-Path $stage "voiceTyper.exe"
    Invoke-RuntimeDeploy -TargetExe $stageExe -Windeployqt $windeployqt -BuildType $BuildType `
        -CudaCompiler $(if ($job.Cuda) { $cudaCompiler } else { $null }) `
        -HaveVulkan ([bool]$job.Vulkan) -VulkanSdk $env:VULKAN_SDK

    if ($icoPath -and (Test-Path $icoPath)) {
        Copy-Item $icoPath -Destination (Join-Path $stage "voicetyper.ico") -Force
    }

    if ($IncludeModel) {
        $models = Get-ChildItem (Join-Path $Root "models") -Filter "ggml-*.bin" -ErrorAction SilentlyContinue
        if ($models) {
            $stageModels = Join-Path $stage "models"
            New-Item -ItemType Directory -Path $stageModels | Out-Null
            foreach ($m in $models) {
                Write-Host "    bundling model $($m.Name) ($([math]::Round($m.Length / 1MB)) MB)"
                Copy-Item $m.FullName -Destination $stageModels -Force
            }
        } else {
            Write-Warning "-IncludeModel set but no models\ggml-*.bin found; the installer will ship without a model."
        }
    }

    # Build the Inno Setup script. {app}, {autopf}, etc. are Inno constants, not
    # PowerShell - they pass through this here-string untouched (no '$'). Only the
    # output filename carries the backend variant; AppName / install dir stay
    # identical across variants so they cleanly replace one another.
    $outputBase = "voiceTyper-$AppVersion-$($job.Name)-setup"
    $setupIconLine = if ($icoPath -and (Test-Path $icoPath)) { "SetupIconFile=$icoPath" } else { "" }
    $iss = @"
[Setup]
AppName=voiceTyper
AppVersion=$AppVersion
AppPublisher=voiceTyper
DefaultDirName={autopf}\voiceTyper
DefaultGroupName=voiceTyper
DisableProgramGroupPage=yes
OutputDir=$BuildPath
OutputBaseFilename=$outputBase
$setupIconLine
UninstallDisplayIcon={app}\voiceTyper.exe
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "$stage\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\voiceTyper"; Filename: "{app}\voiceTyper.exe"
Name: "{autodesktop}\voiceTyper"; Filename: "{app}\voiceTyper.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\voiceTyper.exe"; Description: "Launch voiceTyper"; Flags: nowait postinstall skipifsilent
"@
    $issPath = Join-Path $BuildPath "voiceTyper-$($job.Name).iss"
    Set-Content -Path $issPath -Value $iss -Encoding UTF8

    Write-Host "==> Compiling installer with $iscc"
    Invoke-Tool $iscc @($issPath)

    $setupExe = Join-Path $BuildPath "$outputBase.exe"
    if (Test-Path $setupExe) {
        Write-Host "Installer: $setupExe"
        $produced += [pscustomobject]@{ Label = $job.Label; Path = $setupExe }
    } else {
        Write-Warning "ISCC reported success but installer not found at $setupExe."
    }
}

if ($Installer) {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "Installers produced ($($produced.Count)):"
    foreach ($p in $produced) {
        Write-Host ("  {0,-20} {1}" -f $p.Label, $p.Path)
    }
    if ($skipped) {
        Write-Host ""
        Write-Host "Variants skipped (toolchain missing on this build host):"
        foreach ($s in $skipped) { Write-Host "  - $s" }
    }
}

if (-not (Get-ChildItem (Join-Path $Root "models") -Filter "ggml-*.bin" -ErrorAction SilentlyContinue)) {
    Write-Host ""
    Write-Host "No speech model found. Download one before running:"
    Write-Host "  .\scripts\download-model.ps1                # small-q5_1 (~180 MB)"
    Write-Host "  .\scripts\download-model.ps1 large-v3-turbo-q5_0"
}
