# Downloads the NuGet packages listed in packages.config into .\packages
# (same layout `nuget restore` produces), without needing nuget.exe.
param(
    # Pass a short path (and the same -p:PackagesDir=... to MSBuild) if the project lives in a deep folder.
    [string]$PackagesDir = (Join-Path $PSScriptRoot 'packages')
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$packagesDir = $PackagesDir
New-Item -ItemType Directory -Force $packagesDir | Out-Null

foreach ($package in ([xml](Get-Content (Join-Path $root 'packages.config'))).packages.package)
{
    $target = Join-Path $packagesDir "$($package.id).$($package.version)"
    if (Test-Path $target)
    {
        continue
    }

    $id = $package.id.ToLowerInvariant()
    $version = $package.version.ToLowerInvariant()
    $nupkg = "$target.zip"
    Write-Host "Downloading $($package.id) $($package.version)"
    Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$id/$version/$id.$version.nupkg" -OutFile $nupkg
    Expand-Archive $nupkg -DestinationPath $target
    Remove-Item $nupkg
}
