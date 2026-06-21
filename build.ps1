<#
.SYNOPSIS
    Build SkyrimLoadingPercent — both the SKSE C++ plugin and the LoadingMenu SWF.

.DESCRIPTION
    Step 1: Build the SKSE DLL with CMake + vcpkg.
    Step 2: Download MTASC (free AS2 compiler) if not present, then compile LoadingMenu.as -> LoadingMenu.swf.
    Step 3 (optional): Copy outputs to a Skyrim Data folder for testing.

.PARAMETER SkyrimPath
    Path to your Skyrim SE/AE "Data" folder. If supplied, outputs are deployed automatically.

.PARAMETER Config
    Build configuration: Release (default) or Debug.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -SkyrimPath "C:\Steam\steamapps\common\Skyrim Special Edition\Data" -Config Release
#>

param(
    [string]$SkyrimPath = "",
    [ValidateSet("Release","Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

Write-Host "=== SkyrimLoadingPercent Build ===" -ForegroundColor Cyan

# ── Verify prerequisites ──────────────────────────────────────────────────────

if (-not $env:VCPKG_ROOT) {
    Write-Error "VCPKG_ROOT environment variable is not set. Install vcpkg and set VCPKG_ROOT."
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue)?.Source
if (-not $cmake) {
    Write-Error "cmake not found in PATH. Install CMake 3.21+ and ensure it is on your PATH."
}

# ── Step 1: Build the SKSE plugin DLL ────────────────────────────────────────

Write-Host "`n[1/2] Configuring CMake (preset: $Config.ToLower())..." -ForegroundColor Yellow

$preset = $Config.ToLower()
$buildDir = Join-Path $Root "build\$preset"

$cmakeArgs = @(
    "--preset", $preset
)
if ($SkyrimPath) {
    $cmakeArgs += "-DSKYRIM_PATH=$SkyrimPath"
}

& cmake $Root @cmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed." }

Write-Host "[1/2] Building..." -ForegroundColor Yellow
& cmake --build $Root --preset $preset
if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed." }

$dllPath = Join-Path $buildDir "SkyrimLoadingPercent.dll"
if (Test-Path $dllPath) {
    Write-Host "[1/2] Plugin built: $dllPath" -ForegroundColor Green
} else {
    Write-Warning "DLL not found at expected path $dllPath — check CMake output above."
}

# ── Step 2: Compile LoadingMenu.swf with MTASC ───────────────────────────────

Write-Host "`n[2/2] Compiling LoadingMenu.as -> LoadingMenu.swf..." -ForegroundColor Yellow

$mtascDir  = Join-Path $Root "tools\mtasc"
$mtascExe  = Join-Path $mtascDir "mtasc.exe"
$interfaceDir = Join-Path $Root "interface"
$asSource     = Join-Path $interfaceDir "LoadingMenu.as"
$swfOut       = Join-Path $interfaceDir "LoadingMenu.swf"

if (-not (Test-Path $mtascExe)) {
    Write-Host "  Downloading MTASC 1.14 (free AS2 compiler)..." -ForegroundColor DarkYellow
    $null = New-Item -ItemType Directory -Force $mtascDir
    $zipUrl  = "https://github.com/nickmain/mtasc/releases/download/v1.14/mtasc-1.14-win.zip"
    $zipPath = Join-Path $mtascDir "mtasc.zip"

    try {
        Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -UseBasicParsing
        Expand-Archive -LiteralPath $zipPath -DestinationPath $mtascDir -Force
        Remove-Item $zipPath
    } catch {
        Write-Warning "Auto-download of MTASC failed: $_"
        Write-Host @"
  Please download MTASC manually:
    URL: http://mtasc.org
    1. Extract mtasc.exe to: $mtascDir
    2. Re-run this script.
"@ -ForegroundColor DarkYellow
        exit 1
    }
}

if (-not (Test-Path $mtascExe)) {
    Write-Error "mtasc.exe not found at $mtascExe after download attempt."
}

# Compile: 1280x720 stage, 30fps, Flash 8 (AS2), main-class entry
& $mtascExe -swf "$swfOut" -main -header "1280:720:30" -version 8 -mx "$asSource"
if ($LASTEXITCODE -ne 0) { Write-Error "MTASC compilation failed." }
Write-Host "[2/2] SWF built: $swfOut" -ForegroundColor Green

# ── Step 3 (optional): Deploy to Skyrim Data folder ──────────────────────────

if ($SkyrimPath) {
    Write-Host "`n[Deploy] Copying to $SkyrimPath..." -ForegroundColor Yellow

    # SKSE plugin
    $pluginDst = Join-Path $SkyrimPath "SKSE\Plugins"
    $null = New-Item -ItemType Directory -Force $pluginDst
    if (Test-Path $dllPath) {
        Copy-Item $dllPath (Join-Path $pluginDst "SkyrimLoadingPercent.dll") -Force
    }
    $iniSrc = Join-Path $Root "SKSE\Plugins\SkyrimLoadingPercent.ini"
    if (Test-Path $iniSrc) {
        Copy-Item $iniSrc (Join-Path $pluginDst "SkyrimLoadingPercent.ini") -Force
    }

    # SWF (loose file overrides BSA)
    $ifaceDst = Join-Path $SkyrimPath "Interface"
    $null = New-Item -ItemType Directory -Force $ifaceDst
    Copy-Item $swfOut (Join-Path $ifaceDst "LoadingMenu.swf") -Force

    Write-Host "[Deploy] Done." -ForegroundColor Green
}

Write-Host "`nBuild complete." -ForegroundColor Cyan
Write-Host "To install manually, copy these files to your Skyrim Data folder:"
Write-Host "  $dllPath  ->  Data\SKSE\Plugins\SkyrimLoadingPercent.dll"
Write-Host "  $swfOut   ->  Data\Interface\LoadingMenu.swf"
Write-Host "  $iniSrc   ->  Data\SKSE\Plugins\SkyrimLoadingPercent.ini"
