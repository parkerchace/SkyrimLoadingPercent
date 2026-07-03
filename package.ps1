# package.ps1 — builds a release zip ready for MO2 / Vortex installation
# Usage: .\package.ps1 [-Version "3.0.0"]
param([string]$Version = "3.0.0")

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out  = "$root\dist"
$pkg  = "$out\SkyrimLoadingPercent-$Version"
$data = "$pkg\Data"

# Build
Write-Host "Building Release..."
cmake --build "$root\build\release" --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# Assemble package folder (Data-relative layout under a Data\ root + fomod meta)
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force `
    "$data\SKSE\Plugins", `
    "$data\Scripts", `
    "$data\MCM\Config\SkyrimLoadingPercent", `
    "$pkg\fomod" | Out-Null

# Plugin + default INI
Copy-Item "$root\build\release\Release\SkyrimLoadingPercent.dll" "$data\SKSE\Plugins\"
Copy-Item "$root\SKSE\Plugins\SkyrimLoadingPercent.ini"          "$data\SKSE\Plugins\"
# ESP (Data root) + compiled MCM script
Copy-Item "$root\SkyrimLoadingPercent.esp"                       "$data\"
Copy-Item "$root\Scripts\SkyrimLoadingPercentMCM.pex"            "$data\Scripts\"
# MCM-Helper config
Copy-Item "$root\MCM\Config\SkyrimLoadingPercent\config.json"    "$data\MCM\Config\SkyrimLoadingPercent\"
Copy-Item "$root\MCM\Config\SkyrimLoadingPercent\settings.ini"   "$data\MCM\Config\SkyrimLoadingPercent\"
# FOMOD meta so MO2/Vortex show name/version/author
Copy-Item "$root\fomod\info.xml"                                 "$pkg\fomod\"
# Docs / attribution
Copy-Item "$root\README.md"                                      "$pkg\"
Copy-Item "$root\CREDITS.md"                                     "$pkg\"

# Zip it
$zip = "$out\SkyrimLoadingPercent-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Package ready: $zip"
