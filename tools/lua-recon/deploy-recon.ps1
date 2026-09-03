<#
.SYNOPSIS
    Installs the WuchangRecon Lua recon mod into the game's UE4SS Mods folder.

.DESCRIPTION
    Copies tools\lua-recon\WuchangRecon\ (Scripts\main.lua + enabled.txt) into

        <Game>\Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangRecon\

    and creates the out\ folder the mod writes its dumps into. Existing dumps in
    out\ are left alone unless -Clean is passed.

.PARAMETER GameRoot
    The game's install directory (the one containing Project_Plague).

.PARAMETER Clean
    Delete everything already in out\ before deploying.

.PARAMETER Pull
    Instead of deploying, copy the dumps currently in the game's out\ folder back
    into tools\lua-recon\WuchangRecon\out\ so they can be read/committed.
#>
[CmdletBinding()]
param(
    # Machine-specific default (the original dev box's Steam library). Override with
    # -GameRoot or the WUCHANG_GAME_ROOT environment variable.
    [string]$GameRoot = $(if ($env:WUCHANG_GAME_ROOT) { $env:WUCHANG_GAME_ROOT }
                          else { 'E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers' }),
    [switch]$Clean,
    [switch]$Pull
)

$ErrorActionPreference = 'Stop'

$ModName   = 'WuchangRecon'
$srcMod    = Join-Path $PSScriptRoot $ModName
$ue4ssDir  = Join-Path $GameRoot 'Project_Plague\Binaries\Win64\ue4ss'
$dstMod    = Join-Path $ue4ssDir  "Mods\$ModName"

if (-not (Test-Path $ue4ssDir)) {
    throw "UE4SS is not installed: '$ue4ssDir' does not exist."
}

if ($Pull) {
    $srcOut = Join-Path $dstMod 'out'
    $dstOut = Join-Path $srcMod 'out'
    if (-not (Test-Path $srcOut)) { throw "No dumps yet: '$srcOut' does not exist." }
    New-Item -ItemType Directory -Force -Path $dstOut | Out-Null
    $files = Get-ChildItem -Path $srcOut -File
    foreach ($f in $files) { Copy-Item -Path $f.FullName -Destination $dstOut -Force }
    Write-Host "Pulled $($files.Count) file(s) -> $dstOut"
    return
}

New-Item -ItemType Directory -Force -Path (Join-Path $dstMod 'Scripts') | Out-Null
$outDir = Join-Path $dstMod 'out'
if ($Clean -and (Test-Path $outDir)) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Copy-Item -Path (Join-Path $srcMod 'Scripts\main.lua') `
          -Destination (Join-Path $dstMod 'Scripts\main.lua') -Force

$enabled = Join-Path $dstMod 'enabled.txt'
if (-not (Test-Path $enabled)) { New-Item -ItemType File -Path $enabled | Out-Null }

Write-Host "Deployed -> $dstMod"
Write-Host "  Scripts\main.lua"
Write-Host "  enabled.txt"
Write-Host "  out\            (dumps land here)"
Write-Host ""
Write-Host "Hotkeys in game: F8 world dump, F9 UI dump, F7 tracker toggle, F11 navmesh probe, F12 pickup watch (CTRL+key also works)." -ForegroundColor Green
Write-Host "F6 belongs to the WuchangMinimap C++ mod (navmesh dump); F10 is a game console key." -ForegroundColor DarkGray
