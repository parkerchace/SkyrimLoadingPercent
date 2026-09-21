# package.ps1 — builds a release zip ready for MO2 / Vortex installation
# Usage: .\package.ps1 [-Version "4.0.0"]
param([string]$Version = "4.0.0")

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
# Guard: verify the ESP is form 44 before it can ship. The 43->44 fix was
# committed locally on 2026-08-09 but never published, so every download stayed
# form 43 for six weeks while users reported it. Fail loudly instead.
$espForm = [BitConverter]::ToUInt16([IO.File]::ReadAllBytes("$data\SkyrimLoadingPercent.esp"), 20)
if ($espForm -lt 44) { throw "ESP is form $espForm, expected 44 - refusing to package. See Check-PluginForm.ps1" }
Write-Host "ESP form version: $espForm (ok)"

# MCM-Helper config
Copy-Item "$root\MCM\Config\SkyrimLoadingPercent\config.json"    "$data\MCM\Config\SkyrimLoadingPercent\"
Copy-Item "$root\MCM\Config\SkyrimLoadingPercent\settings.ini"   "$data\MCM\Config\SkyrimLoadingPercent\"
# FOMOD meta so MO2/Vortex show name/version/author, + ModuleConfig.xml so it's a
# valid scripted FOMOD (MO2's simple installer misdetects a bare info.xml sitting
# next to a top-level Data\ folder as an invalid archive layout)
Copy-Item "$root\fomod\info.xml"                                 "$pkg\fomod\"
Copy-Item "$root\fomod\ModuleConfig.xml"                         "$pkg\fomod\"
# Docs / attribution. COPYING.txt and EXCEPTIONS.md must ship with the binary:
# the GPL requires the license text to travel with the distributed work, and the
# additional permissions are only in force if the recipient actually has them.
Copy-Item "$root\README.md"                                      "$pkg\"
Copy-Item "$root\CREDITS.md"                                     "$pkg\"
Copy-Item "$root\LICENSE"                                        "$pkg\"
Copy-Item "$root\COPYING.txt"                                    "$pkg\"
Copy-Item "$root\EXCEPTIONS.md"                                  "$pkg\"

# Zip it
$zip = "$out\SkyrimLoadingPercent-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Package ready: $zip"
