# Builds the Rust engine and the C++ WinUI 3 app, then bundles them together.
# Output: <BuildRoot>\gui\bin\AiUpscaler.exe (with the engine in engine\).
#
# Build output goes to a short path because MIDL and the MSVC linker are not
# long-path aware; deep project folders otherwise fail with LNK1104 / MIDL5157.
param(
    # Not under %TEMP%: MSBuild's incremental C++ builds misbehave there (MSB8029).
    [string]$BuildRoot = (Join-Path $env:LOCALAPPDATA 'AiUpscalerBuild'),
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$cargoTarget = Join-Path $BuildRoot 'engine'
$packages = Join-Path $BuildRoot 'packages'

Write-Host '== Building Rust engine' -ForegroundColor Cyan
Push-Location (Join-Path $root 'upscaler')
try
{
    $env:CARGO_TARGET_DIR = $cargoTarget
    cargo build --release
    if ($LASTEXITCODE -ne 0) { throw 'cargo build failed' }
}
finally
{
    Pop-Location
}

Write-Host '== Restoring NuGet packages' -ForegroundColor Cyan
& (Join-Path $root 'gui\restore-packages.ps1') -PackagesDir $packages

Write-Host '== Building WinUI 3 app' -ForegroundColor Cyan
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found; install Visual Studio with the C++ desktop workload.' }

$msbuildArgs = @(
    (Join-Path $root 'gui\AiUpscaler.vcxproj'), '-m', '-nologo', '-v:minimal',
    "-p:Configuration=$Configuration", '-p:Platform=x64',
    "-p:PackagesDir=$packages\",
    "-p:OutDir=$BuildRoot\gui\bin\",
    "-p:IntDir=$BuildRoot\gui\obj\",
    "-p:EngineDir=$cargoTarget\release"
)
& $msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) {
    # Incremental builds sometimes miss a changed precompiled header (error C1382 with
    # link-time code generation); a clean rebuild fixes that. A real error fails again.
    Write-Host 'Incremental build failed; retrying with a clean rebuild...' -ForegroundColor Yellow
    & $msbuild @msbuildArgs -t:Rebuild
    if ($LASTEXITCODE -ne 0) { throw 'MSBuild failed' }
}

Write-Host "`nDone: $BuildRoot\gui\bin\AiUpscaler.exe" -ForegroundColor Green
