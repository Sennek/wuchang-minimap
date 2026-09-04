<#
.SYNOPSIS
    Consistency linter for a WuchangMinimap release. Run by tools\package.ps1; also
    runnable on its own against the repository.

.DESCRIPTION
    Every check here exists because the corresponding mistake actually shipped in
    1.0.0 or was one edit away from shipping:

      * `<AUTHOR>` went out to players on the LICENSE copyright line.
      * The shipped README linked to two files that are not in the package.
      * xmake.lua's set_xmakever said 2.9.3 while the docs said 3.1.1.
      * THIRD_PARTY_NOTICES.md said MinHook 1.3.3, DEVELOPMENT.md said 1.3.4.
      * DEVELOPMENT.md claimed a ~2.9 MB DLL long after it was 3.7 MB.

    What it checks:

      1. One version everywhere - src\version.hpp, xmake.lua's set_version, the top
         RELEASED changelog heading, and (with -PackageRoot) the package folder name.
      2. One UE4SS build string everywhere a reader can see one - BUILD_INFO.txt,
         THIRD_PARTY_NOTICES.md, README.md, tools\INSTALL_GUIDE.html, docs\NEXUS.md.
      3. No unfilled placeholders: `@@...@@`, `<ALLCAPS>` template slots, TODO/FIXME.
      4. Every relative link in a shipped document resolves to a file that is really
         in the package (Markdown links and HTML hrefs).

    Exit code 0 = clean. Non-zero = the number of problems, each printed as one line.
    Nothing is written or changed.

.PARAMETER PackageRoot
    An assembled package tree (dist\WuchangMinimap-<ver>\). When given, the shipped
    copies of the documents are the ones checked, links must resolve inside the
    package, and the folder name must carry the right version. Without it only the
    repository-level checks run - useful before starting a release.

.PARAMETER Ue4ssBuild
    The UE4SS build string every document must agree on. Defaults to the same value
    package.ps1 uses.

.PARAMETER Quiet
    Only print problems and the verdict, not each passing check.
#>
[CmdletBinding()]
param(
    [string]$PackageRoot,
    [string]$Ue4ssBuild = $(if ($env:WUCHANG_UE4SS_BUILD) { $env:WUCHANG_UE4SS_BUILD }
                           else { 'v3.0.1-1111-g97b7e501' }),
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

$problems = New-Object System.Collections.Generic.List[string]
function Add-Problem([string]$m) { $script:problems.Add($m) }
function Say([string]$m) { if (-not $Quiet) { Write-Host "  $m" } }

function Read-TextOrNull([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    return Get-Content -Raw -LiteralPath $path
}

Write-Host ""
Write-Host "Release consistency check" -ForegroundColor Cyan

#--------------------------------------------------------------------------------------
# 1. One version everywhere
#--------------------------------------------------------------------------------------
$verHpp = $null
$t = Read-TextOrNull (Join-Path $repo 'src\version.hpp')
if (-not $t) { Add-Problem 'src\version.hpp is missing' }
else {
    $m = [regex]::Match($t, '(?m)^#define\s+WUCHANG_MINIMAP_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $m.Success) { Add-Problem 'src\version.hpp: no WUCHANG_MINIMAP_VERSION define found' }
    else { $verHpp = $m.Groups[1].Value; Say "version.hpp        $verHpp" }
}

$t = Read-TextOrNull (Join-Path $repo 'xmake.lua')
if (-not $t) { Add-Problem 'xmake.lua is missing' }
else {
    $m = [regex]::Match($t, '(?m)^set_version\("([0-9]+\.[0-9]+\.[0-9]+)"\)')
    if (-not $m.Success) { Add-Problem 'xmake.lua: no set_version("x.y.z") found' }
    else {
        Say "xmake.lua          $($m.Groups[1].Value)"
        if ($verHpp -and $m.Groups[1].Value -ne $verHpp) {
            Add-Problem "version mismatch: xmake.lua set_version is $($m.Groups[1].Value), version.hpp says $verHpp"
        }
    }
}

# The changelog's top "## x.y.z" heading. Any other heading is skipped; a version
# heading must exist and must match version.hpp.
$changelogSrc = Join-Path $PSScriptRoot 'CHANGELOG.template.md'
$t = Read-TextOrNull $changelogSrc
if (-not $t) { Add-Problem 'tools\CHANGELOG.template.md is missing' }
else {
    $headings = [regex]::Matches($t, '(?m)^##\s+(.+?)\s*$') | ForEach-Object { $_.Groups[1].Value }
    $released = @($headings | Where-Object { $_ -match '^[0-9]+\.[0-9]+\.[0-9]+' })
    if ($released.Count -eq 0) { Add-Problem 'CHANGELOG.template.md has no released "## x.y.z" section' }
    else {
        $topVer = ([regex]::Match($released[0], '^([0-9]+\.[0-9]+\.[0-9]+)')).Groups[1].Value
        Say "changelog top      $topVer"
        if ($verHpp -and $topVer -ne $verHpp) {
            Add-Problem "version mismatch: the top released changelog section is $topVer, version.hpp says $verHpp"
        }
    }
}

#--------------------------------------------------------------------------------------
# 2. Where the documents live: the package if we have one, the repo otherwise
#--------------------------------------------------------------------------------------
# name -> path. Documents that only exist in the repo (NEXUS.md) stay repo-side either
# way; the rest come out of the package when one is given, because the shipped copy is
# the one a player reads.
$docs = [ordered]@{}
if ($PackageRoot) {
    if (-not (Test-Path -LiteralPath $PackageRoot -PathType Container)) {
        throw "-PackageRoot '$PackageRoot' is not a folder."
    }
    $PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
    $pkgName = Split-Path -Leaf $PackageRoot
    Say "package            $pkgName"
    if ($verHpp -and $pkgName -ne "WuchangMinimap-$verHpp") {
        Add-Problem "package folder is '$pkgName', expected 'WuchangMinimap-$verHpp'"
    }
    foreach ($n in @('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'CHANGELOG.md',
                     'INSTALL_GUIDE.html', 'BUILD_INFO.txt')) {
        $docs[$n] = Join-Path $PackageRoot $n
    }
} else {
    $docs['README.md']              = Join-Path $repo 'README.md'
    $docs['LICENSE']                = Join-Path $repo 'LICENSE'
    $docs['THIRD_PARTY_NOTICES.md'] = Join-Path $repo 'THIRD_PARTY_NOTICES.md'
    $docs['CHANGELOG.md']           = $changelogSrc
    $docs['INSTALL_GUIDE.html']     = Join-Path $PSScriptRoot 'INSTALL_GUIDE.html'
}
$docs['docs\NEXUS.md'] = Join-Path $repo 'docs\NEXUS.md'

# Repo-side, two of these are TEMPLATES: package.ps1 expands @@VERSION@@ / @@DATE@@ when
# it copies them in. An unexpanded placeholder is a bug in the PACKAGE and completely
# normal in the repo, so the @@...@@ rule only applies to a package tree.
$templatesInRepo = @('INSTALL_GUIDE.html', 'CHANGELOG.md')

#--------------------------------------------------------------------------------------
# 3. One UE4SS build string everywhere a reader can see one
#--------------------------------------------------------------------------------------
# The mod is ABI-tied to one UE4SS build, so a document naming a different one (or none)
# is the single most expensive support mistake available. BUILD_INFO.txt only exists in
# a package, so it is required only there.
$ue4ssWanted = @('README.md', 'THIRD_PARTY_NOTICES.md', 'INSTALL_GUIDE.html', 'docs\NEXUS.md')
if ($PackageRoot) { $ue4ssWanted += 'BUILD_INFO.txt' }
foreach ($n in $ue4ssWanted) {
    $t = Read-TextOrNull $docs[$n]
    if ($null -eq $t) { Add-Problem "$n is missing (cannot check the UE4SS build string)"; continue }
    if ($t -notlike "*$Ue4ssBuild*") {
        Add-Problem "$n does not name the UE4SS build '$Ue4ssBuild'"
    }
    # A *different* UE4SS build string anywhere is worse than a missing one.
    foreach ($other in [regex]::Matches($t, 'v3\.0\.[0-9]+-[0-9]+-g[0-9a-f]{8}')) {
        if ($other.Value -ne $Ue4ssBuild) {
            Add-Problem "$n names a different UE4SS build '$($other.Value)' (expected '$Ue4ssBuild')"
        }
    }
}
Say "UE4SS build        $Ue4ssBuild"

#--------------------------------------------------------------------------------------
# 4. No unfilled placeholders
#--------------------------------------------------------------------------------------
# `<ALLCAPS>` is the template shape (<AUTHOR>, <VERSION>, <YOUR NAME>). It deliberately
# does NOT match the angle-bracket path placeholders the docs use on purpose - <Steam>,
# <Game>, <N> - because those have a lowercase letter or are a single character, and it
# does not match <https://...> autolinks.
$placeholderRules = @(
    @{ Name = '@@PLACEHOLDER@@'; Pattern = '@@[A-Za-z_]+@@'; PackageOnly = $true }
    @{ Name = '<ALLCAPS> template slot'; Pattern = '<[A-Z][A-Z0-9_]{2,}(?:\s[A-Z0-9_]+)*>'; PackageOnly = $false }
    @{ Name = 'TODO/FIXME/XXX'; Pattern = '\b(?:TODO|FIXME|XXX)\b'; PackageOnly = $false }
)
foreach ($n in $docs.Keys) {
    $t = Read-TextOrNull $docs[$n]
    if ($null -eq $t) { continue }
    foreach ($rule in $placeholderRules) {
        if ($rule.PackageOnly -and -not $PackageRoot -and $n -in $templatesInRepo) { continue }
        foreach ($hit in [regex]::Matches($t, $rule.Pattern)) {
            Add-Problem "$n still contains a $($rule.Name): '$($hit.Value)'"
        }
    }
}
Say "placeholders       none"

#--------------------------------------------------------------------------------------
# 5. Every relative link in a shipped document resolves inside the package
#--------------------------------------------------------------------------------------
# Only meaningful against a real package: in the repo, README's CHANGELOG.md link is
# expected to dangle, because that file is generated at packaging time.
if ($PackageRoot) {
    $linkChecked = 0
    foreach ($n in @('README.md', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.md', 'INSTALL_GUIDE.html')) {
        $path = $docs[$n]
        $t = Read-TextOrNull $path
        if ($null -eq $t) { continue }
        $dir = Split-Path -Parent $path

        $targets = New-Object System.Collections.Generic.List[string]
        # Markdown [text](target) - the target may carry a #anchor or a "title".
        foreach ($m in [regex]::Matches($t, '\]\(\s*([^)\s]+)')) { $targets.Add($m.Groups[1].Value) }
        # HTML href="target" / src="target"
        foreach ($m in [regex]::Matches($t, '(?:href|src)\s*=\s*"([^"]+)"')) { $targets.Add($m.Groups[1].Value) }

        foreach ($raw in $targets) {
            $target = $raw
            if ($target -match '^(?:[a-z][a-z0-9+.-]*:|//|#)') { continue }  # url, protocol-relative, anchor
            $target = ($target -split '#')[0]
            if (-not $target) { continue }
            $target = $target -replace '/', '\'
            $resolved = Join-Path $dir $target
            $linkChecked++
            if (-not (Test-Path -LiteralPath $resolved)) {
                Add-Problem "$n links to '$raw', which is not in the package"
            }
        }
    }
    Say "relative links     $linkChecked checked"
}

#--------------------------------------------------------------------------------------
# Verdict
#--------------------------------------------------------------------------------------
if ($problems.Count -gt 0) {
    Write-Host ""
    foreach ($p in $problems) { Write-Host "  FAIL  $p" -ForegroundColor Red }
    Write-Host ""
    Write-Host ("Release consistency check FAILED: {0} problem(s)." -f $problems.Count) -ForegroundColor Red
    exit $problems.Count
}
Write-Host "  OK                 versions, UE4SS build, placeholders and links all agree" -ForegroundColor Green
exit 0
