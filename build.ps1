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

.PARAMETER Rebuild
    Wipe intermediates first.
#>
[CmdletBinding()]
param(
    [ValidateSet('Game__Shipping__Win64', 'Game__Debug__Win64')]
    [string]$Mode      = 'Game__Shipping__Win64',
    [string]$Ue4ssRoot = 'F:/Tools/RE-UE4SS',
    [string]$Xmake     = 'F:\Tools\xmake\xmake.exe',
    [string]$Toolset   = '14.40.33807',
    [int]   $Jobs      = 8,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (-not (Test-Path $Xmake)) { throw "xmake not found at '$Xmake'" }
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

    $dll = Join-Path $PSScriptRoot "build\windows\x64\$Mode\main.dll"
    Write-Host ""
    Write-Host "Built: $dll ($([math]::Round((Get-Item $dll).Length / 1KB)) KB)" -ForegroundColor Green
}
finally {
    Pop-Location
}
