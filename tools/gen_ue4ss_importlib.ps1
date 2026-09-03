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
    Path to the UE4SS.dll shipped by the release you are targeting. There is no sane
    default for this one - it is the DLL out of YOUR game folder, and using someone
    else's would produce an import library for the wrong build. Pass it, or set
    WUCHANG_UE4SS_DLL.

.PARAMETER VsPath
    The Visual Studio install to borrow dumpbin.exe and lib.exe from. Discovered with
    vswhere by default (see tools\vs_detect.ps1); override with -VsPath or
    WUCHANG_VS_PATH.

.PARAMETER Toolset
    MSVC toolset for the dev shell. Any toolset's lib.exe can write this import
    library - it only reads a .def - so the default just follows whatever VsPath has.
#>
[CmdletBinding()]
param(
    [string]$Ue4ssDll  = $env:WUCHANG_UE4SS_DLL,
    [string]$VsPath    = $env:WUCHANG_VS_PATH,
    [string]$Toolset   = $env:WUCHANG_MSVC_TOOLSET
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'vs_detect.ps1')
$root   = Split-Path -Parent $PSScriptRoot
$defOut = Join-Path $root 'sdk\UE4SS.def'
$libOut = Join-Path $root 'sdk\lib\UE4SS.lib'

if (-not $Ue4ssDll) {
    throw ("Pass -Ue4ssDll (or set `$env:WUCHANG_UE4SS_DLL) to the UE4SS.dll installed in " +
           "your game, e.g. '<Game>\Project_Plague\Binaries\Win64\ue4ss\UE4SS.dll'. " +
           "The import library must be synthesised from the exact build you will run.")
}
if (-not (Test-Path $Ue4ssDll)) { throw "UE4SS.dll not found at '$Ue4ssDll'" }

if (-not $Toolset) { $Toolset = Resolve-MsvcToolset }
if (-not $VsPath)  { $VsPath  = Get-VsPathForToolset -Toolset $Toolset }
if (-not $VsPath) {
    throw ("No Visual Studio install found. Pass -VsPath, or set `$env:WUCHANG_VS_PATH " +
           "to the folder that contains Common7\Tools\Microsoft.VisualStudio.DevShell.dll.")
}
Write-Host "Visual Studio: $VsPath  (toolset $Toolset)"

# VS 2026 Insiders ships no vcvars64.bat, so the dev environment has to be entered
# through the DevShell module rather than by calling a batch file.
$devShell = Join-Path $VsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (-not (Test-Path -LiteralPath $devShell)) {
    throw "'$VsPath' has no Common7\Tools\Microsoft.VisualStudio.DevShell.dll - not a VS install?"
}
Import-Module $devShell | Out-Null
# -vcvars_ver takes a major.minor ("14.40"), not the full 14.40.33807 directory name.
$vcvarsVer = ($Toolset -split '\.')[0..1] -join '.'
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation `
                 -DevCmdArguments "-arch=x64 -host_arch=x64 -vcvars_ver=$vcvarsVer" | Out-Null

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
