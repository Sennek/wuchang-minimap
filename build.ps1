<#
.SYNOPSIS
    Configures and builds the WuchangMinimap UE4SS C++ mod.

.DESCRIPTION
    Thin wrapper around xmake so the toolset, mode and UE4SS checkout path do not have
    to be retyped. Equivalent to:

        xmake f -m Game__Shipping__Win64 -p windows -a x64 --vs_toolset=14.40.33807 `
                --ue4ss_root=F:/Tools/RE-UE4SS -y
        xmake -j 8

.PARAMETER Mode
    Game__Shipping__Win64 (default) or Game__Debug__Win64.

.PARAMETER Ue4ssRoot
    The RE-UE4SS checkout the headers come from. Machine-specific: the default is where
    it lives on the original dev box. Override with -Ue4ssRoot or by setting the
    WUCHANG_UE4SS_ROOT environment variable.

.PARAMETER Xmake
    xmake.exe. Machine-specific like the above; override with -Xmake or WUCHANG_XMAKE
    (or just put xmake on PATH and pass -Xmake xmake.exe).

.PARAMETER Rebuild
    Wipe intermediates first.
#>
[CmdletBinding()]
param(
    [ValidateSet('Game__Shipping__Win64', 'Game__Debug__Win64')]
    [string]$Mode      = 'Game__Shipping__Win64',
    [string]$Ue4ssRoot = $(if ($env:WUCHANG_UE4SS_ROOT) { $env:WUCHANG_UE4SS_ROOT } else { 'F:/Tools/RE-UE4SS' }),
    [string]$Xmake     = $(if ($env:WUCHANG_XMAKE)      { $env:WUCHANG_XMAKE }      else { 'F:\Tools\xmake\xmake.exe' }),
    [string]$Toolset   = '14.40.33807',
    [int]   $Jobs      = 8,
    [switch]$Rebuild,
    [switch]$NoTests
)

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not (Test-Path $Xmake)) {
        # Also accept a bare name that is on PATH, so -Xmake xmake.exe works.
        $onPath = Get-Command $Xmake -ErrorAction SilentlyContinue
        if (-not $onPath) { throw "xmake not found at '$Xmake' (set -Xmake or `$env:WUCHANG_XMAKE)" }
        $Xmake = $onPath.Source
    }
    if (-not (Test-Path (Join-Path $Ue4ssRoot 'UE4SS/include/Mod/CppUserModBase.hpp'))) {
        throw "'$Ue4ssRoot' does not look like an RE-UE4SS checkout (Mod/CppUserModBase.hpp missing)."
    }
    if (-not (Test-Path (Join-Path $PSScriptRoot 'sdk\lib\UE4SS.lib'))) {
        throw "sdk\lib\UE4SS.lib is missing. Run tools\gen_ue4ss_importlib.ps1 first."
    }

    # `xmake clean --all` drops the cached Visual Studio environment along with the
    # intermediates, so it has to run BEFORE `xmake f`, not after - otherwise the next
    # compile runs with an empty INCLUDE and fails on `#include <memory>`.
    if ($Rebuild) {
        & $Xmake clean --all
    }

    & $Xmake f -m $Mode -p windows -a x64 --vs_toolset=$Toolset --ue4ss_root=$Ue4ssRoot -y
    if ($LASTEXITCODE -ne 0) { throw "xmake config failed ($LASTEXITCODE)" }

    & $Xmake -j $Jobs
    if ($LASTEXITCODE -ne 0) { throw "xmake build failed ($LASTEXITCODE)" }

    # Offline tests. They link only src\markers_db.cpp (no UE4SS, no D3D12), so they
    # run here on the build machine with the game closed - which is the only place
    # anything about this mod can be verified without burning a play session.
    if (-not $NoTests) {
        & $Xmake build markers_test
        if ($LASTEXITCODE -ne 0) { throw "xmake build markers_test failed ($LASTEXITCODE)" }
        & $Xmake run markers_test (Join-Path $PSScriptRoot 'markers')
        if ($LASTEXITCODE -ne 0) { throw "markers_test FAILED ($LASTEXITCODE)" }
    }

    $dll = Join-Path $PSScriptRoot "build\windows\x64\$Mode\main.dll"
    Write-Host ""
    Write-Host "Built: $dll ($([math]::Round((Get-Item $dll).Length / 1KB)) KB)" -ForegroundColor Green
}
finally {
    Pop-Location
}
