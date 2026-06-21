# package.ps1 — builds a release zip ready for MO2 / Vortex installation
# Usage: .\package.ps1 [-Version "1.0.0"]
param([string]$Version = "1.0.0")

$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$out   = "$root\dist"
$pkg   = "$out\SkyrimLoadingPercent-$Version"
$dest  = "$pkg\Data\SKSE\Plugins"

# Build
Write-Host "Building Release..."
cmake --build "$root\build\release" --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# Assemble package folder
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item "$root\build\release\Release\SkyrimLoadingPercent.dll" $dest
Copy-Item "$root\SKSE\Plugins\SkyrimLoadingPercent.ini"          $dest

# FOMOD meta so MO2/Vortex pick up the mod info
$fomodDest = "$pkg\fomod"
New-Item -ItemType Directory -Force $fomodDest | Out-Null
Copy-Item "$root\fomod\info.xml" $fomodDest

# Zip it
$zip = "$out\SkyrimLoadingPercent-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip
Write-Host "Package ready: $zip"
