<#
.SYNOPSIS
    Visual Studio / MSVC discovery shared by build.ps1 and gen_ue4ss_importlib.ps1.

.DESCRIPTION
    Dot-source it:

        . (Join-Path $PSScriptRoot 'vs_detect.ps1')

    Nothing here is specific to this project except one constant: the toolset version
    the mod is known to build with. Everything else is plain discovery, so a clone on
    a machine that has never seen this repo can still find a compiler.
#>

# The MSVC toolset this project has actually been built and shipped with. Preferred
# whenever it is installed, so two machines that both have it produce the same binary.
# Anything else works, but is untested - the scripts say so out loud when they fall back.
$script:WuchangPinnedToolset = '14.40.33807'

function Get-VsInstallPaths {
    <#
      Every Visual Studio install on the box, newest first. vswhere ships with the VS
      installer since 2017 and knows about Preview/Insiders builds when asked
      (-prerelease), which matters here: the dev box's only C++ toolchain is VS 2026
      Insiders. If vswhere is missing we fall back to globbing the two standard roots.
    #>
    $found = New-Object System.Collections.Generic.List[string]

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $out = & $vswhere -all -prerelease -products '*' `
                          -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                          -property installationPath 2>$null
        foreach ($line in @($out)) {
            $p = "$line".Trim()
            if ($p -and (Test-Path -LiteralPath $p)) { $found.Add($p) }
        }
        # -requires can come back empty on an install whose component ids differ
        # (Insiders has done this); ask again without the filter before giving up.
        if ($found.Count -eq 0) {
            $out = & $vswhere -all -prerelease -products '*' -property installationPath 2>$null
            foreach ($line in @($out)) {
                $p = "$line".Trim()
                if ($p -and (Test-Path -LiteralPath $p)) { $found.Add($p) }
            }
        }
    }

    if ($found.Count -eq 0) {
        foreach ($root in @((Join-Path $env:ProgramFiles 'Microsoft Visual Studio'),
                            (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio'))) {
            if (-not (Test-Path -LiteralPath $root)) { continue }
            foreach ($d in Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue) {
                foreach ($e in Get-ChildItem -LiteralPath $d.FullName -Directory -ErrorAction SilentlyContinue) {
                    if (Test-Path -LiteralPath (Join-Path $e.FullName 'VC\Tools\MSVC')) { $found.Add($e.FullName) }
                }
            }
        }
    }

    # Newest first, so "latest installed" means what it says.
    return @($found | Sort-Object -Descending)
}

function Get-MsvcToolsets {
    <#
      Every MSVC toolset version installed, as objects with Version / Path / VsPath,
      newest version first. The version IS the directory name under VC\Tools\MSVC -
      that is the same string `xmake f --vs_toolset=` wants.
    #>
    $all = New-Object System.Collections.Generic.List[object]
    foreach ($vs in @(Get-VsInstallPaths)) {
        $msvc = Join-Path $vs 'VC\Tools\MSVC'
        if (-not (Test-Path -LiteralPath $msvc)) { continue }
        foreach ($d in Get-ChildItem -LiteralPath $msvc -Directory -ErrorAction SilentlyContinue) {
            # A toolset without the x64 cl.exe is a broken/partial install; skip it.
            if (-not (Test-Path -LiteralPath (Join-Path $d.FullName 'bin\HostX64\x64\cl.exe'))) { continue }
            $parsed = $null
            [void][version]::TryParse($d.Name, [ref]$parsed)
            $all.Add([pscustomobject]@{
                Version = $d.Name
                Sort    = if ($parsed) { $parsed } else { [version]'0.0.0' }
                Path    = $d.FullName
                VsPath  = $vs
            })
        }
    }
    return @($all | Sort-Object -Property Sort -Descending)
}

function Resolve-MsvcToolset {
    <#
      Decide which toolset version string to hand to xmake. Order:
        1. an explicit -Toolset
        2. $env:WUCHANG_MSVC_TOOLSET
        3. the pinned, tested version, IF it is installed
        4. the newest installed toolset - with a warning, because it is untested here
      Returns the version string, or throws when the box has no C++ toolchain at all.
    #>
    param([string]$Requested)

    # @() is load bearing: PowerShell 5.1 unrolls a one-element array on return, so a box
    # with exactly one toolset would otherwise give a bare object with no .Count - which
    # silently skipped the "not installed" warning until it was tested.
    $installed = @(Get-MsvcToolsets)

    if ($Requested) {
        if ($installed.Count -gt 0 -and $Requested -notin @($installed.Version)) {
            Write-Warning ("Toolset $Requested was asked for but is not installed. Present: {0}" -f
                           (@($installed.Version) -join ', '))
        }
        return $Requested
    }
    if ($env:WUCHANG_MSVC_TOOLSET) { return $env:WUCHANG_MSVC_TOOLSET }

    if ($installed.Count -eq 0) {
        throw ("No MSVC x64 toolset found. Install Visual Studio 2022 or newer with the " +
               "'Desktop development with C++' workload, or pass -Toolset / set " +
               "`$env:WUCHANG_MSVC_TOOLSET to a version under <VS>\VC\Tools\MSVC\.")
    }
    if ($script:WuchangPinnedToolset -in @($installed.Version)) {
        return $script:WuchangPinnedToolset
    }

    $latest = $installed[0].Version
    Write-Warning ("MSVC $script:WuchangPinnedToolset (the tested toolset) is not installed; " +
                   "using the newest one present, $latest. This is untested - if the build " +
                   "fails on a std:: or fmt error, install $script:WuchangPinnedToolset via " +
                   "the VS Installer's 'Individual components', or pass -Toolset.")
    return $latest
}

function Get-VsPathForToolset {
    <#
      The VS install that owns a given toolset version - what Enter-VsDevShell needs.
      With no version, the newest install that has any usable toolset.
    #>
    param([string]$Toolset)

    $installed = @(Get-MsvcToolsets)  # see the note in Resolve-MsvcToolset
    if ($installed.Count -eq 0) { return $null }
    if ($Toolset) {
        $hit = @($installed | Where-Object { $_.Version -eq $Toolset })
        if ($hit.Count -gt 0) { return $hit[0].VsPath }
    }
    return $installed[0].VsPath
}
