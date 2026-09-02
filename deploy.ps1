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
    The game's install directory (the one containing Project_Plague).

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
    [string]$GameRoot = 'E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers',
    [ValidateSet('Game__Shipping__Win64', 'Game__Debug__Win64')]
    [string]$Mode     = 'Game__Shipping__Win64',
    [switch]$NoPdb,
    [switch]$Force,
    [switch]$Pull
)

$ErrorActionPreference = 'Stop'

$ModName  = 'WuchangMinimap'

if ($Pull) {
    $srcNav = Join-Path $GameRoot "Project_Plague\Binaries\Win64\ue4ss\Mods\$ModName\navmesh"
    if (-not (Test-Path $srcNav)) {
        throw "No navmesh dumps yet: '$srcNav' does not exist. Run the game and press F6 (or just walk around) first."
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

    Write-Host "Deployed -> $dllsDir\main.dll"
}

Write-Host ""
Write-Host "Done. Launch the game and look for 'WuchangMinimap loaded' in the UE4SS log/console." -ForegroundColor Green
