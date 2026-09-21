# package.ps1 — builds a release zip ready for MO2 / Vortex installation
# Usage: .\package.ps1 [-Version "4.0.0"] [-OutDir <path>]
param(
    [string]$Version = "4.0.0",
    [string]$OutDir  = ""
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$out  = if ($OutDir) { $OutDir } else { "$root\dist" }
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Force $out | Out-Null }
$pkg  = "$out\SkyrimLoadingPercent-$Version"

# Build
Write-Host "Building Release..."
cmake --build "$root\build\release" --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# Layout: Data-relative content sits at the ARCHIVE ROOT (SKSE\, Scripts\, MCM\,
# the .esp), not nested under a Data\ folder, and there is no FOMOD.
#
# The earlier Data\-wrapped layout needed a FOMOD to install: MO2's generic
# auto-detector only collapses through single-child folders to find the data
# root, so with Data\ plus anything else at the top level it stopped at the root
# and reported "The content of <data> does not look valid" — rejecting the docs
# along with fomod\. A scripted FOMOD was the workaround.
#
# Rooting the data content directly removes the cause instead. MO2 and Vortex
# both recognise SKSE\, Scripts\, MCM\ and a loose .esp as data-root markers and
# accept stray text files beside them, which is how most Nexus mods ship. Manual
# installs still work: extract the archive into Data\.
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force `
    "$pkg\SKSE\Plugins", `
    "$pkg\Scripts", `
    "$pkg\MCM\Config\SkyrimLoadingPercent" | Out-Null

# Plugin + default INI
Copy-Item "$root\build\release\Release\SkyrimLoadingPercent.dll" "$pkg\SKSE\Plugins\"
Copy-Item "$root\SKSE\Plugins\SkyrimLoadingPercent.ini"          "$pkg\SKSE\Plugins\"
# ESP (data root) + compiled MCM script
Copy-Item "$root\SkyrimLoadingPercent.esp"                       "$pkg\"
Copy-Item "$root\Scripts\SkyrimLoadingPercentMCM.pex"            "$pkg\Scripts\"
# Guard: verify the ESP is form 44 before it can ship. The 43->44 fix was
# committed locally on 2026-08-09 but never published, so every download stayed
# form 43 for six weeks while users reported it. Fail loudly instead.
$espForm = [BitConverter]::ToUInt16([IO.File]::ReadAllBytes("$pkg\SkyrimLoadingPercent.esp"), 20)
if ($espForm -lt 44) { throw "ESP is form $espForm, expected 44 - refusing to package. See Check-PluginForm.ps1" }
Write-Host "ESP form version: $espForm (ok)"

# MCM-Helper config
Copy-Item "$root\MCM\Config\SkyrimLoadingPercent\config.json"    "$pkg\MCM\Config\SkyrimLoadingPercent\"
Copy-Item "$root\MCM\Config\SkyrimLoadingPercent\settings.ini"   "$pkg\MCM\Config\SkyrimLoadingPercent\"

# Licensing. These are not optional extras: the GPL requires its text to travel
# with the distributed work, the additional permissions only bind if the
# recipient actually has them, and the MIT/BSD libraries statically linked into
# the DLL require their notices to accompany the binary.
# Everything ships as .txt so Windows has a default handler and nothing arrives
# extensionless. README.md is not shipped - the Nexus page carries that content.
Copy-Item "$root\LICENSE"                   "$pkg\LICENSE.txt"
Copy-Item "$root\COPYING.txt"               "$pkg\COPYING.txt"
Copy-Item "$root\EXCEPTIONS.md"             "$pkg\EXCEPTIONS.txt"
Copy-Item "$root\THIRD-PARTY-LICENSES.txt"  "$pkg\THIRD-PARTY-LICENSES.txt"
Copy-Item "$root\CREDITS.md"                "$pkg\CREDITS.txt"

# Refuse to ship anything that isn't a known-good file type. Keeps stray build
# artifacts, .md files and extensionless files out of the uploaded archive.
$allowed = @('.dll', '.esp', '.ini', '.pex', '.json', '.txt')
$bad = Get-ChildItem $pkg -Recurse -File | Where-Object { $allowed -notcontains $_.Extension.ToLower() }
if ($bad) { throw "Unexpected file types in package: $($bad.Name -join ', ')" }

# Zip it
$zip = "$out\SkyrimLoadingPercent-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$pkg\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Package ready: $zip"
Get-ChildItem $pkg -Recurse -File | ForEach-Object {
    "  {0,-48} {1,9:N0}" -f $_.FullName.Substring($pkg.Length + 1), $_.Length
}
