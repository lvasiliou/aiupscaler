# Builds the Rust engine and the C++ WinUI 3 app, then bundles them together.
# Output: <BuildRoot>\gui\bin\AiUpscaler.exe (with the engine in engine\). The engine always
# includes DirectML and NVIDIA CUDA; CUDA is used only on PCs that have the CUDA runtime.
#
# Build output goes to a short path because MIDL and the MSVC linker are not
# long-path aware; deep project folders otherwise fail with LNK1104 / MIDL5157.
param(
    # Not under %TEMP%: MSBuild's incremental C++ builds misbehave there (MSB8029).
    [string]$BuildRoot = (Join-Path $env:LOCALAPPDATA 'AiUpscalerBuild'),
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    # Also build the MSI installer (<BuildRoot>\installer\AiUpscaler.msi). WiX comes from NuGet.
    [switch]$Installer
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$cargoTarget = Join-Path $BuildRoot 'engine'
$guiRoot = Join-Path $BuildRoot 'gui'
$packages = Join-Path $BuildRoot 'packages'

Write-Host '== Building Rust engine' -ForegroundColor Cyan
Push-Location (Join-Path $root 'upscaler')
try
{
    # upscaler\.cargo\config.toml picks the ONNX Runtime build with CUDA and DirectML.
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
    "-p:OutDir=$guiRoot\bin\",
    "-p:IntDir=$guiRoot\obj\",
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

Write-Host "`nDone: $guiRoot\bin\AiUpscaler.exe" -ForegroundColor Green

if ($Installer)
{
    # The installer's version follows the engine's (upscaler\Cargo.toml).
    $version = (Select-String -Path (Join-Path $root 'upscaler\Cargo.toml') -Pattern '^version\s*=\s*"([^"]+)"' | Select-Object -First 1).Matches[0].Groups[1].Value
    Write-Host "== Building MSI installer $version" -ForegroundColor Cyan
    $installerOut = Join-Path $BuildRoot 'installer'
    dotnet build (Join-Path $root 'installer\AiUpscaler.wixproj') -c Release -nologo -v:minimal -o $installerOut `
        "-p:AppDir=$guiRoot\bin\" "-p:ProductVersion=$version" "-p:BaseIntermediateOutputPath=$BuildRoot\installer-obj\"
    if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }
    Write-Host "Done: $installerOut\AiUpscaler.msi" -ForegroundColor Green
}
