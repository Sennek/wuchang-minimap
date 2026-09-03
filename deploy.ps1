<#
.SYNOPSIS
    Installs the built mod into the game's UE4SS Mods folder.

.DESCRIPTION
    Copies build\windows\x64\<Mode>\main.dll (and its .pdb) into

        <Game>\Project_Plague\Binaries\Win64\ue4ss\Mods\WuchangMinimap\dlls\main.dll

    and drops an empty enabled.txt next to the dlls folder so UE4SS loads the mod
    without having to edit Mods\mods.txt.

    A copy is also staged into deploy\ue4ss\Mods\WuchangMinimap\ so the repo mirrors
    exactly what ends up in the game.

.PARAMETER GameRoot
    The game's install directory (the one containing Project_Plague). Machine-specific:
    the default is the Steam library on the original dev box. Override with -GameRoot or
    by setting the WUCHANG_GAME_ROOT environment variable.

.PARAMETER Force
    Overwrite even if the target dlls folder does not exist yet (it is created either
    way); use this to skip the "is UE4SS actually installed?" sanity check.

.PARAMETER Pull
    Do not deploy. Instead copy the navmesh dumps the mod has written in the game
    (Mods\WuchangMinimap\navmesh\<agent>\*.json) back into tools\navmesh\dumps\ so
    they can be rendered and committed.
#>
[CmdletBinding()]
param(
    [string]$GameRoot = $(if ($env:WUCHANG_GAME_ROOT) { $env:WUCHANG_GAME_ROOT }
                          else { 'E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers' }),
    [ValidateSet('Game__Shipping__Win64', 'Game__Debug__Win64')]
    [string]$Mode     = 'Game__Shipping__Win64',
    [switch]$NoPdb,
    [switch]$Force,
    [switch]$ForceConfig,
    [switch]$NoMaps,
    [switch]$Pull
)

$ErrorActionPreference = 'Stop'

$ModName  = 'WuchangMinimap'

if ($Pull) {
    $srcNav = Join-Path $GameRoot "Project_Plague\Binaries\Win64\ue4ss\Mods\$ModName\navmesh"
    if (-not (Test-Path $srcNav)) {
        throw "No navmesh dumps yet: '$srcNav' does not exist. Enable navmesh_dump in config.ini, run the game and press F3 first."
    }
    $dstNav = Join-Path $PSScriptRoot 'tools\navmesh\dumps'
    New-Item -ItemType Directory -Force -Path $dstNav | Out-Null
    $n = 0
    foreach ($agentDir in Get-ChildItem -Path $srcNav -Directory) {
        $dst = Join-Path $dstNav $agentDir.Name
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        foreach ($f in Get-ChildItem -Path $agentDir.FullName -Filter '*.json' -File) {
            Copy-Item -Path $f.FullName -Destination $dst -Force
            $n++
        }
        Write-Host "  $($agentDir.Name): $((Get-ChildItem -Path $dst -Filter '*.json' -File).Count) file(s)"
    }
    Write-Host ""
    Write-Host "Pulled $n navmesh JSON file(s) -> $dstNav" -ForegroundColor Green
    Write-Host "Render them with:" -ForegroundColor Green
    Write-Host "  python tools\navmesh\render.py --input tools\navmesh\dumps --out tools\navmesh\out --debug"
    return
}
$builtDll = Join-Path $PSScriptRoot "build\windows\x64\$Mode\main.dll"
$builtPdb = Join-Path $PSScriptRoot "build\windows\x64\$Mode\main.pdb"

if (-not (Test-Path $builtDll)) {
    throw "main.dll not found at '$builtDll'. Run .\build.ps1 -Mode $Mode first."
}

$ue4ssDir = Join-Path $GameRoot 'Project_Plague\Binaries\Win64\ue4ss'
if (-not (Test-Path $ue4ssDir)) {
    if (-not $Force) {
        throw "UE4SS does not appear to be installed: '$ue4ssDir' does not exist. Pass -Force to create it anyway."
    }
    Write-Warning "Creating '$ue4ssDir' - UE4SS itself is not installed there yet."
}

$targets = @(
    (Join-Path $PSScriptRoot "deploy\ue4ss\Mods\$ModName"),   # in-repo mirror
    (Join-Path $ue4ssDir     "Mods\$ModName")                 # the real install
)

foreach ($modDir in $targets) {
    $dllsDir = Join-Path $modDir 'dlls'
    New-Item -ItemType Directory -Force -Path $dllsDir | Out-Null

    Copy-Item -Path $builtDll -Destination (Join-Path $dllsDir 'main.dll') -Force
    if (-not $NoPdb -and (Test-Path $builtPdb)) {
        Copy-Item -Path $builtPdb -Destination (Join-Path $dllsDir 'main.pdb') -Force
    }

    # An empty enabled.txt is the "no mods.txt editing required" opt-in.
    $enabled = Join-Path $modDir 'enabled.txt'
    if (-not (Test-Path $enabled)) { New-Item -ItemType File -Path $enabled | Out-Null }

    # Config files are never overwritten unless -ForceConfig: the user may have turned
    # the navmesh dumper on for a capture session, or tuned the minimap by hand.
    #   config.ini                     - the runtime navmesh dumper (off by default)
    #   config_wuchang_minimap.txt     - the overlay / minimap / full map settings
    #   config_wuchang_minimap_dev.txt - the developer dials. Installed HERE (this is a
    #                                    dev deploy) but deliberately NOT part of the
    #                                    release zip - tools\package.ps1 refuses to ship
    #                                    it. It is read after the player config and
    #                                    overrides it.
    # wuchang_minimap_found.txt (the collection tracker) and
    # wuchang_minimap_waypoint.txt (the full map waypoint) also live in the mod root and
    # are the player's own state: never shipped, never touched by a deploy.
    foreach ($cfgName in @('config.ini', 'config_wuchang_minimap.txt', 'config_wuchang_minimap_dev.txt')) {
        $cfgSrc = Join-Path $PSScriptRoot "deploy\ue4ss\Mods\WuchangMinimap\$cfgName"
        $cfgDst = Join-Path $modDir $cfgName
        if ((Test-Path $cfgSrc) -and ($cfgSrc -ne $cfgDst) -and ($ForceConfig -or -not (Test-Path $cfgDst))) {
            Copy-Item -Path $cfgSrc -Destination $cfgDst -Force
            Write-Host "Installed default $cfgName -> $cfgDst"
        }
    }

    # The map assets the overlay loads at start-up: maps\maps.json plus
    # maps\<chapter>\*.png, built by tools\navmesh\build_map.py. Always refreshed -
    # they are generated, not user-editable.
    if (-not $NoMaps) {
        $mapsSrc = Join-Path $PSScriptRoot 'maps'
        if (Test-Path $mapsSrc) {
            $mapsDst = Join-Path $modDir 'maps'
            # Wipe first: Copy-Item does not remove files the pipeline stopped
            # producing, and a stale asset set (the old small_f*.png ordinal layers,
            # say) would be loaded alongside the new one.
            if ((Test-Path $mapsDst) -and ($mapsSrc -ne $mapsDst)) {
                Remove-Item -Recurse -Force $mapsDst
            }
            New-Item -ItemType Directory -Force -Path $mapsDst | Out-Null
            if ($mapsSrc -ne $mapsDst) {
                Copy-Item -Path (Join-Path $mapsSrc '*') -Destination $mapsDst -Recurse -Force
            }
            $png = Get-ChildItem -Path $mapsDst -Filter '*.png' -Recurse -File
            Write-Host ("Deployed maps -> {0} ({1} PNG, {2:N1} MB)" -f $mapsDst, $png.Count,
                        (($png | Measure-Object -Property Length -Sum).Sum / 1MB))
        } else {
            Write-Warning "No maps\ directory in the repo - build it with tools\navmesh\build_map.py"
        }
    }

    # The static marker database: markers\<chapter>.json, produced offline by
    # tools\markers. Generated, so it is wiped and re-copied like maps\ - but
    # wuchang_minimap_found.txt lives in the mod root, NOT in here, so the player's
    # collection tracker is never touched by a deploy.
    $markersSrc = Join-Path $PSScriptRoot 'markers'
    if (Test-Path $markersSrc) {
        $markersDst = Join-Path $modDir 'markers'
        if ((Test-Path $markersDst) -and ($markersSrc -ne $markersDst)) {
            Remove-Item -Recurse -Force $markersDst
        }
        New-Item -ItemType Directory -Force -Path $markersDst | Out-Null
        if ($markersSrc -ne $markersDst) {
            Copy-Item -Path (Join-Path $markersSrc '*') -Destination $markersDst -Recurse -Force
        }
        $json = @(Get-ChildItem -Path $markersDst -Filter '*.json' -File)
        $real = @($json | Where-Object { $_.Name -notlike '*.sample.json' })
        Write-Host ("Deployed markers -> {0} ({1} file(s), {2} chapter manifest(s))" -f
                    $markersDst, $json.Count, $real.Count)
        if ($real.Count -eq 0) {
            Write-Warning "markers\ holds only the hand-written sample - the minimap will show LIVE markers only. Build the static database with tools\markers."
        }
    }

    Write-Host "Deployed -> $dllsDir\main.dll"
}

Write-Host ""
Write-Host "Done. Launch the game and look for 'WuchangMinimap loaded' in the UE4SS log/console." -ForegroundColor Green
