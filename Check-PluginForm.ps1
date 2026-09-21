# Check-PluginForm.ps1 — report the record form version of Skyrim plugins.
#
# Form 44 is the current maximum for Skyrim (Bethesda's own 1.7.104 masters are
# form 44; there is no 45). MO2 warns about form 43 plugins; Vortex does not
# surface it at all, which is why this exists.
#
# Usage:
#   .\Check-PluginForm.ps1                  # every copy of this mod's ESP
#   .\Check-PluginForm.ps1 -Path <file|dir> # any plugin, or all plugins in a folder
#   .\Check-PluginForm.ps1 -Zip <file.zip>  # plugins inside a release archive
param(
    [string]$Path,
    [string]$Zip
)

$ErrorActionPreference = "Stop"

function Read-PluginHeader([byte[]]$b, [string]$label) {
    if ($b.Length -lt 34 -or [Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'TES4') {
        return [PSCustomObject]@{ Plugin = $label; Form = '-'; ESL = '-'; HEDR = '-'; Note = 'not a plugin' }
    }
    $flags = [BitConverter]::ToUInt32($b, 8)
    [PSCustomObject]@{
        Plugin = $label
        Form   = [BitConverter]::ToUInt16($b, 20)
        ESL    = [bool]($flags -band 0x200)
        HEDR   = [BitConverter]::ToSingle($b, 30)
        Note   = if ([BitConverter]::ToUInt16($b, 20) -lt 44) { 'FORM 43 - MO2 will warn' } else { 'ok' }
    }
}

$results = @()

if ($Zip) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead((Resolve-Path $Zip))
    try {
        foreach ($e in $archive.Entries) {
            if ($e.Name -match '\.(esp|esm|esl)$') {
                $ms = New-Object IO.MemoryStream
                $s = $e.Open(); $s.CopyTo($ms); $s.Close()
                $results += Read-PluginHeader $ms.ToArray() $e.FullName
            }
        }
    } finally { $archive.Dispose() }
}
elseif ($Path) {
    $items = if (Test-Path -LiteralPath $Path -PathType Container) {
        Get-ChildItem -LiteralPath $Path -Include *.esp, *.esm, *.esl -File -Recurse
    } else { Get-Item -LiteralPath $Path }
    foreach ($f in $items) { $results += Read-PluginHeader ([IO.File]::ReadAllBytes($f.FullName)) $f.FullName }
}
else {
    # Default: every copy of this mod's ESP worth caring about.
    $root = $PSScriptRoot
    $candidates = @(
        "$root\SkyrimLoadingPercent.esp",
        "$env:SKYRIM_DATA_PATH\SkyrimLoadingPercent.esp",
        "H:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data\SkyrimLoadingPercent.esp"
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
    foreach ($f in $candidates) { $results += Read-PluginHeader ([IO.File]::ReadAllBytes($f)) $f }
}

$results | Format-Table -AutoSize
if ($results | Where-Object { $_.Form -ne '-' -and $_.Form -lt 44 }) {
    Write-Warning "At least one plugin is below form 44."
}
