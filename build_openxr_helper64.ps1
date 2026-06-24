Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectPath = Join-Path $repoRoot "OpenXRHelper64\OpenXRHelper64.vcxproj"
$loaderPackage = Join-Path $repoRoot "thirdparty\openxr\loader\OpenXR.Loader.1.1.60.nupkg"
$helperOutDir = Join-Path $repoRoot "Release\openxr_helper64"
$gameHelperDir = "E:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2\vr\openxr_helper64"

if (-not (Test-Path -LiteralPath $projectPath)) {
    Write-Error "Project file not found: $projectPath"
}

if (-not (Test-Path -LiteralPath $loaderPackage)) {
    Write-Error "OpenXR loader package not found: $loaderPackage"
}

$msbuildCmd = Get-Command msbuild -ErrorAction SilentlyContinue
if (-not $msbuildCmd) {
    Write-Error "msbuild not found in PATH. Open a Visual Studio Developer PowerShell and retry."
}

Write-Host "Building OpenXR helper: Release|x64"
& $msbuildCmd.Source `
    $projectPath `
    /t:Build `
    /p:Configuration=Release `
    /p:Platform=x64 `
    /m

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

New-Item -ItemType Directory -Force -Path $helperOutDir | Out-Null

$extractDir = Join-Path $repoRoot "thirdparty\openxr\loader\OpenXR.Loader.1.1.60.extract"
if (Test-Path -LiteralPath $extractDir) {
    Remove-Item -LiteralPath $extractDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

try {
    $zipPackage = Join-Path $extractDir "OpenXR.Loader.1.1.60.zip"
    Copy-Item -LiteralPath $loaderPackage -Destination $zipPackage -Force
    Expand-Archive -LiteralPath $zipPackage -DestinationPath $extractDir -Force
    $x64Loader = Join-Path $extractDir "native\x64\release\bin\openxr_loader.dll"
    if (-not (Test-Path -LiteralPath $x64Loader)) {
        Write-Error "x64 openxr_loader.dll not found inside package: $x64Loader"
    }

    Copy-Item -LiteralPath $x64Loader -Destination (Join-Path $helperOutDir "openxr_loader.dll") -Force
}
finally {
    if (Test-Path -LiteralPath $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }
}

if (Test-Path -LiteralPath $gameHelperDir) {
    Remove-Item -LiteralPath $gameHelperDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $gameHelperDir | Out-Null

Copy-Item -LiteralPath (Join-Path $helperOutDir "OpenXRHelper64.exe") -Destination $gameHelperDir -Force
Copy-Item -LiteralPath (Join-Path $helperOutDir "openxr_loader.dll") -Destination $gameHelperDir -Force

Write-Host "OpenXR helper build/deploy succeeded:"
Write-Host "  $helperOutDir"
Write-Host "  $gameHelperDir"
