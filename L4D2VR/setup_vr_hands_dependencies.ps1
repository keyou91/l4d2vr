$ErrorActionPreference = "Stop"

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$solutionDir = Split-Path -Parent $projectDir
$thirdpartyDir = Join-Path $solutionDir "thirdparty"
$cgltfDir = Join-Path $thirdpartyDir "cgltf"
$ozzSourceDir = Join-Path $thirdpartyDir "ozz-animation"
$ozzBuildDir = Join-Path $thirdpartyDir "ozz-animation-build-win32"
$ozzInstallRoot = Join-Path $thirdpartyDir "ozz-animation-install\win32"
$ozzIncludeDir = Join-Path $ozzInstallRoot "include"
$ozzLibRoot = Join-Path $ozzInstallRoot "lib"
$tempDir = Join-Path $env:TEMP "l4d2vr-vr-hands-dependencies"

$cgltfCommit = "85cd62382dfea638278962690cf515023f33ed00"
$ozzVersion = "0.17.0"
# Pin the annotated 0.17.0 tag to its peeled commit so a moved tag cannot
# silently change the third-party code used by release builds.
$ozzCommit = "744eb9d99f606eda849acb0b1204f7a3dc20bca1"
$ozzLicenseFile = Join-Path $ozzInstallRoot "LICENSE.md"
$ozzVersionFile = Join-Path $ozzInstallRoot "VERSION.txt"
$ozzVersionMarker = "ozz-animation $ozzVersion`ncommit $ozzCommit"
$requiredOzzLibraries = @(
    "ozz_animation.lib",
    "ozz_animation_offline.lib",
    "ozz_base.lib"
)
$requiredOzzHeaders = @(
    "ozz\animation\runtime\skeleton.h",
    "ozz\animation\runtime\local_to_model_job.h",
    "ozz\animation\offline\raw_skeleton.h",
    "ozz\animation\offline\skeleton_builder.h",
    "ozz\base\maths\soa_transform.h",
    "ozz\base\maths\simd_math.h",
    "ozz\base\maths\simd_quaternion.h",
    "ozz\base\span.h"
)

function Copy-DirectoryContents([string]$sourceDir, [string]$destinationDir) {
    if (-not (Test-Path $sourceDir)) {
        return
    }

    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    Copy-Item -Recurse -Force -Path (Join-Path $sourceDir "*") -Destination $destinationDir
}

function Copy-RequiredLibraries([string]$sourceDir, [string]$destinationDir) {
    if (-not (Test-Path $sourceDir)) {
        return
    }

    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
    foreach ($library in $requiredOzzLibraries) {
        $sourceFile = Join-Path $sourceDir $library
        if (Test-Path $sourceFile) {
            Copy-Item -Force -Path $sourceFile -Destination (Join-Path $destinationDir $library)
        }
    }
}

function Test-OzzInstallReady {
    if (-not (Test-Path $ozzLicenseFile)) {
        return $false
    }

    if (-not (Test-Path $ozzVersionFile)) {
        return $false
    }

    if ((Get-Content -Raw -Path $ozzVersionFile).Trim() -ne $ozzVersionMarker) {
        return $false
    }

    foreach ($header in $requiredOzzHeaders) {
        if (-not (Test-Path (Join-Path $ozzIncludeDir $header))) {
            return $false
        }
    }

    foreach ($configuration in @("Debug", "Release")) {
        $libraryDir = Join-Path $ozzLibRoot $configuration
        foreach ($library in $requiredOzzLibraries) {
            if (-not (Test-Path (Join-Path $libraryDir $library))) {
                return $false
            }
        }
    }

    return $true
}

function Import-LegacyInstallLayout {
    # Older versions installed a complete include tree under each configuration.
    # Collapse those duplicate trees into a single shared include directory and
    # move the three libraries used by L4D2VR into lib\<Configuration>.
    if (-not (Test-OzzInstallReady)) {
        foreach ($configuration in @("Release", "Debug")) {
            $legacyIncludeDir = Join-Path $ozzInstallRoot "$configuration\include"
            if (Test-Path (Join-Path $legacyIncludeDir "ozz\animation\runtime\skeleton.h")) {
                Copy-DirectoryContents $legacyIncludeDir $ozzIncludeDir
                break
            }
        }
    }

    foreach ($configuration in @("Debug", "Release")) {
        $legacyLibDir = Join-Path $ozzInstallRoot "$configuration\lib"
        $compactLibDir = Join-Path $ozzLibRoot $configuration
        Copy-RequiredLibraries $legacyLibDir $compactLibDir
    }
}

function Remove-UnneededOzzFiles {
    # The plugin compiles against installed headers and links the three static
    # libraries above. The downloaded source tree and CMake build tree are not
    # needed after installation.
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ozzSourceDir
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ozzBuildDir
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $ozzInstallRoot "Debug")
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $ozzInstallRoot "Release")
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tempDir
}

New-Item -ItemType Directory -Force -Path $thirdpartyDir, $cgltfDir, $ozzInstallRoot, $ozzIncludeDir, $ozzLibRoot | Out-Null

$cgltfHeader = Join-Path $cgltfDir "cgltf.h"
if (-not (Test-Path $cgltfHeader)) {
    Write-Host "Downloading cgltf $cgltfCommit..."
    Invoke-WebRequest `
        -Uri "https://raw.githubusercontent.com/jkuhlmann/cgltf/$cgltfCommit/cgltf.h" `
        -OutFile $cgltfHeader
}

Import-LegacyInstallLayout

if (-not (Test-OzzInstallReady)) {
    New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

    Write-Host "Downloading ozz-animation $ozzVersion ($ozzCommit)..."
    $ozzZip = Join-Path $tempDir "ozz-animation-$ozzCommit.zip"
    $ozzExtract = Join-Path $tempDir "ozz-animation-extract"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ozzExtract
    Invoke-WebRequest `
        -Uri "https://github.com/guillaumeblanc/ozz-animation/archive/$ozzCommit.zip" `
        -OutFile $ozzZip
    Expand-Archive -Force -Path $ozzZip -DestinationPath $ozzExtract
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ozzSourceDir
    Move-Item -Path (Join-Path $ozzExtract "ozz-animation-$ozzCommit") -Destination $ozzSourceDir

    $ozzSourceLicense = Join-Path $ozzSourceDir "LICENSE.md"
    if (-not (Test-Path $ozzSourceLicense)) {
        throw "ozz-animation $ozzVersion source archive does not contain LICENSE.md."
    }

    # Clear the old compact package before installing a different pinned
    # version, otherwise headers removed upstream could survive an upgrade.
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ozzIncludeDir
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ozzLibRoot
    New-Item -ItemType Directory -Force -Path $ozzIncludeDir, $ozzLibRoot | Out-Null

    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $ozzBuildDir
    New-Item -ItemType Directory -Force -Path $ozzBuildDir | Out-Null

    Write-Host "Configuring ozz-animation Win32 static libraries..."
    cmake -S $ozzSourceDir -B $ozzBuildDir -A Win32 -T v142 `
        -DBUILD_SHARED_LIBS=OFF `
        -DCMAKE_CXX_STANDARD=17 `
        -DCMAKE_CXX_STANDARD_REQUIRED=ON `
        -DCMAKE_CXX_EXTENSIONS=OFF `
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF `
        -Dozz_build_tools=OFF `
        -Dozz_build_fbx=OFF `
        -Dozz_build_gltf=OFF `
        -Dozz_build_data=OFF `
        -Dozz_build_samples=OFF `
        -Dozz_build_howtos=OFF `
        -Dozz_build_tests=OFF `
        -Dozz_build_postfix=OFF `
        -Dozz_build_msvc_rt_dll=ON
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to configure ozz-animation $ozzVersion Win32/v142."
    }

    foreach ($configuration in @("Debug", "Release")) {
        Write-Host "Building and installing ozz-animation $configuration Win32..."
        $stageDir = Join-Path $tempDir "ozz-animation-install-$configuration"
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stageDir
        cmake --build $ozzBuildDir --config $configuration -- /m
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to build ozz-animation $ozzVersion $configuration Win32/v142."
        }

        cmake --install $ozzBuildDir --config $configuration --prefix $stageDir
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to install ozz-animation $ozzVersion $configuration Win32/v142."
        }

        Copy-DirectoryContents (Join-Path $stageDir "include") $ozzIncludeDir
        Copy-RequiredLibraries (Join-Path $stageDir "lib") (Join-Path $ozzLibRoot $configuration)
    }

    Copy-Item -Force -Path $ozzSourceLicense -Destination $ozzLicenseFile
    Set-Content -NoNewline -Encoding ASCII -Path $ozzVersionFile -Value $ozzVersionMarker
}

if (-not (Test-OzzInstallReady)) {
    throw "ozz-animation compact install is incomplete. Required headers or static libraries are missing."
}

Remove-UnneededOzzFiles

Write-Host "VR hand dependencies are ready. ozz-animation $ozzVersion ($ozzCommit), its license, installed headers, and static libraries were kept."
Write-Host "Rebuild L4D2VR Win32."
