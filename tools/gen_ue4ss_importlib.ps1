<#
.SYNOPSIS
    Generates an MSVC import library (sdk/lib/UE4SS.lib) from a shipped UE4SS.dll.

.DESCRIPTION
    Building UE4SS from source requires the private `Re-UE4SS/UEPseudo` submodule
    (Epic Games GitHub org access), so we cannot produce UE4SS.lib the normal way.
    Instead we read the export table of the *exact* UE4SS.dll that is installed in
    the game and synthesise an import library from it. Because the headers we compile
    against come from the RE-UE4SS checkout at the same commit as that DLL, the ABI
    matches.

.PARAMETER Ue4ssDll
    Path to the UE4SS.dll shipped by the release you are targeting.
#>
[CmdletBinding()]
param(
    # Both defaults are machine-specific (the original dev box). Override with the
    # parameters, or with the WUCHANG_UE4SS_DLL / WUCHANG_VS_PATH environment variables.
    [string]$Ue4ssDll  = $(if ($env:WUCHANG_UE4SS_DLL) { $env:WUCHANG_UE4SS_DLL }
                           else { "F:\Tools\ue4ss\rel\ue4ss\UE4SS.dll" }),
    [string]$VsPath    = $(if ($env:WUCHANG_VS_PATH) { $env:WUCHANG_VS_PATH }
                           else { "C:\Program Files\Microsoft Visual Studio\18\Insiders" }),
    [string]$Toolset   = "14.40"
)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot
$defOut = Join-Path $root 'sdk\UE4SS.def'
$libOut = Join-Path $root 'sdk\lib\UE4SS.lib'

if (-not (Test-Path $Ue4ssDll)) { throw "UE4SS.dll not found at '$Ue4ssDll'" }

Import-Module (Join-Path $VsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll') | Out-Null
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation `
                 -DevCmdArguments "-arch=x64 -host_arch=x64 -vcvars_ver=$Toolset" | Out-Null

Write-Host "Reading exports from $Ue4ssDll ..."
$dump = & dumpbin /nologo /exports $Ue4ssDll

# A mangled MSVC symbol names a *variable* (not a function) when it contains '@@3',
# or when it is one of the compiler-generated data symbols (vftable, RTTI, string
# literal, ...). Import libraries must tag those as DATA or the linker will emit a
# jump thunk where an address load is required.
function Test-IsDataSymbol([string]$n) {
    if ($n -match '@@3')                    { return $true }
    if ($n -match '^\?\?_[78CRS]')          { return $true }
    return $false
}

$exports = New-Object System.Collections.Generic.List[string]
$seen    = New-Object System.Collections.Generic.HashSet[string]
foreach ($line in $dump) {
    # "  ordinal  hint  RVA       name"
    if ($line -notmatch '^\s+(\d+)\s+([0-9A-Fa-f]+)\s+([0-9A-Fa-f]{8})\s+(\S+)\s*$') { continue }
    $name = $Matches[4]
    if ($name -eq '[NONAME]') { continue }
    if (-not $seen.Add($name)) { continue }
    if (Test-IsDataSymbol $name) { $exports.Add("    $name DATA") }
    else                         { $exports.Add("    $name") }
}

if ($exports.Count -eq 0) { throw "Parsed 0 exports out of $($dump.Count) dumpbin lines." }

New-Item -ItemType Directory -Force -Path (Split-Path $defOut), (Split-Path $libOut) | Out-Null
$def = @("LIBRARY UE4SS.dll", "EXPORTS") + $exports
$def | Out-File -Encoding ascii $defOut
Write-Host "Wrote $defOut ($($exports.Count) exports)"

& lib /nologo /def:$defOut /machine:x64 /out:$libOut
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed with exit code $LASTEXITCODE" }
Write-Host "Wrote $libOut ($([math]::Round((Get-Item $libOut).Length / 1KB)) KB)"
