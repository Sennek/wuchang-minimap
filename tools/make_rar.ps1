<#
.SYNOPSIS
    One-click RAR of the current release: dist\WuchangMinimap-<version>.rar.

.DESCRIPTION
    Runs tools\package.ps1 (build, tests, smoke check, check_release.ps1, zip) and then
    packs the assembled dist\WuchangMinimap-<version>\ tree into a RAR5 archive next to
    the zip, with the same top-level folder the zip has. The archive is tested and
    round-tripped (every entry, name and size, against the tree on disk) before the
    script reports success. Nothing is written on failure.

    WinRAR is located automatically: -Rar, $env:WUCHANG_RAR, PATH, the WinRAR registry
    keys, then the default Program Files folders. Only Rar.exe (the console tool that
    ships with every WinRAR install) is used.

    make_rar.cmd in the repo root runs this with no arguments and keeps the window open.

.PARAMETER FromDist
    Skip package.ps1 and pack the dist tree that is already there. Refused when that
    tree's BUILD_INFO.txt names a commit other than HEAD or the working tree is dirty,
    unless -AllowDirty is also given. For iterating on the archive itself.

.PARAMETER NoBuild, AllowDirty, Mode, OutDir
    Passed through to package.ps1 unchanged (see its help). -OutDir is also where the
    .rar is written.

.PARAMETER Rar
    Path to Rar.exe or to the WinRAR folder, when auto-detection is not wanted.
#>
[CmdletBinding()]
param(
    [switch]$FromDist,
    [switch]$NoBuild,
    [switch]$AllowDirty,
    [ValidateSet('Game__Shipping__Win64', 'Game__Debug__Win64')]
    [string]$Mode = 'Game__Shipping__Win64',
    [string]$OutDir,
    [string]$Rar
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

function Find-Rar([string]$hint) {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($h in @($hint, $env:WUCHANG_RAR)) {
        if (-not $h) { continue }
        $candidates.Add($h)
        $candidates.Add((Join-Path $h 'Rar.exe'))
    }
    foreach ($name in 'rar', 'Rar.exe', 'WinRAR', 'WinRAR.exe') {
        $g = Get-Command $name -ErrorAction SilentlyContinue
        if ($g) { $candidates.Add((Join-Path (Split-Path -Parent $g.Source) 'Rar.exe')) }
    }
    foreach ($key in 'HKLM:\SOFTWARE\WinRAR', 'HKCU:\SOFTWARE\WinRAR',
                     'HKLM:\SOFTWARE\WOW6432Node\WinRAR',
                     'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\WinRAR.exe',
                     'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\WinRAR.exe') {
        if (-not (Test-Path $key)) { continue }
        $p = Get-ItemProperty $key
        foreach ($prop in 'exe64', 'exe32', '(default)', 'Path') {
            $v = $p.$prop
            if (-not $v) { continue }
            $dir = if (Test-Path -LiteralPath $v -PathType Container) { $v } else { Split-Path -Parent $v }
            if ($dir) { $candidates.Add((Join-Path $dir 'Rar.exe')) }
        }
    }
    foreach ($pf in $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432) {
        if ($pf) { $candidates.Add((Join-Path $pf 'WinRAR\Rar.exe')) }
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf) -and
            [System.IO.Path]::GetFileName($c) -ieq 'Rar.exe') {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }
    throw ("Rar.exe not found. Install WinRAR (https://www.win-rar.com), or pass " +
           "-Rar <path to Rar.exe>, or set WUCHANG_RAR.")
}

function Read-ModVersion {
    $header = Join-Path $repo 'src\version.hpp'
    $m = [regex]::Match((Get-Content -Raw -LiteralPath $header),
        '(?m)^#define\s+WUCHANG_MINIMAP_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $m.Success) { throw "Could not read WUCHANG_MINIMAP_VERSION from '$header'." }
    return $m.Groups[1].Value
}

function Invoke-Rar([string]$exe, [string[]]$rarArgs) {
    & $exe @rarArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Rar.exe $($rarArgs[0]) failed with exit code $LASTEXITCODE."
    }
}

Push-Location $repo
try {
    $rarExe = Find-Rar $Rar
    Write-Host "Rar.exe: $rarExe" -ForegroundColor Cyan

    $ver = Read-ModVersion
    if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }
    $pkgName = "WuchangMinimap-$ver"
    $pkgRoot = Join-Path $OutDir $pkgName
    $rarPath = Join-Path $OutDir "$pkgName.rar"

    #--------------------------------------------------------------------------------
    # 1. The package tree: build it, or verify the one that is there
    #--------------------------------------------------------------------------------
    # A stale rar from an earlier run must not survive a failed one.
    if (Test-Path -LiteralPath $rarPath) { Remove-Item -Force -LiteralPath $rarPath }
    if (-not $FromDist) {
        $pkgArgs = @{ Mode = $Mode; OutDir = $OutDir }
        if ($NoBuild) { $pkgArgs.NoBuild = $true }
        if ($AllowDirty) { $pkgArgs.AllowDirty = $true }
        & (Join-Path $repo 'tools\package.ps1') @pkgArgs
        if ($LASTEXITCODE) { throw "package.ps1 failed ($LASTEXITCODE)." }
        Write-Host ""
    }

    if (-not (Test-Path -LiteralPath $pkgRoot -PathType Container)) {
        throw "Package tree '$pkgRoot' does not exist. Run without -FromDist."
    }
    # Absolute from here on: the round-trip derives archive-relative names from it.
    $OutDir = (Resolve-Path -LiteralPath $OutDir).Path
    $pkgRoot = Join-Path $OutDir $pkgName
    $rarPath = Join-Path $OutDir "$pkgName.rar"
    $buildInfo = Join-Path $pkgRoot 'BUILD_INFO.txt'
    if (-not (Test-Path -LiteralPath $buildInfo)) { throw "'$buildInfo' is missing." }
    $info = Get-Content -Raw -LiteralPath $buildInfo
    $infoVer = [regex]::Match($info, '(?m)^version\s+(\S+)').Groups[1].Value
    if ($infoVer -ne $ver) {
        throw "BUILD_INFO.txt says version '$infoVer' but src\version.hpp says '$ver'. Run without -FromDist."
    }
    if ($FromDist -and -not $AllowDirty) {
        $infoCommit = [regex]::Match($info, '(?m)^commit\s+([0-9a-f]+)').Groups[1].Value
        $head = (git rev-parse HEAD).Trim()
        $dirty = @(git status --porcelain)
        if ($infoCommit -ne $head -or $dirty.Count -gt 0) {
            throw ("-FromDist: dist tree was packaged from commit $infoCommit, HEAD is $head" +
                   $(if ($dirty.Count) { " and the tree is dirty" }) +
                   ". Run without -FromDist (or add -AllowDirty to pack it anyway).")
        }
    }
    $stray = Get-ChildItem -LiteralPath $pkgRoot -Recurse -File |
             Where-Object { $_.Name -like 'wuchang_minimap*.txt' -or $_.Name -like 'wuchang_minimap.log*' -or
                            $_.Extension -ieq '.pdb' }
    if ($stray) {
        throw "Player state / debug files in the package tree: $(($stray | ForEach-Object FullName) -join ', ')"
    }

    #--------------------------------------------------------------------------------
    # 2. Archive
    #--------------------------------------------------------------------------------
    $ok = $false
    try {
    Push-Location $OutDir
    try {
        # Relative name from OutDir so the archive holds WuchangMinimap-<ver>\... exactly
        # as the zip does. RAR5, best compression, 3% recovery record.
        Invoke-Rar $rarExe @('a', '-ma5', '-m5', '-rr3p', '-r', '-y', '-idp', '--', $rarPath, $pkgName)
    } finally { Pop-Location }

    #--------------------------------------------------------------------------------
    # 3. Test and round-trip
    #--------------------------------------------------------------------------------
    Invoke-Rar $rarExe @('t', '-idq', '--', $rarPath)

    $onDisk = @{}
    Get-ChildItem -LiteralPath $pkgRoot -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($OutDir.TrimEnd('\').Length + 1)
        $onDisk[$rel] = $_.Length
    }
    $listing = & $rarExe vt -idc -- $rarPath
    if ($LASTEXITCODE -ne 0) { throw "Rar.exe vt failed with exit code $LASTEXITCODE." }
    $inRar = @{}
    $name = $null; $type = $null
    foreach ($line in $listing) {
        if ($line -match '^\s*Name:\s*(.+?)\s*$') { $name = $Matches[1]; $type = $null; continue }
        if ($line -match '^\s*Type:\s*(\S+)') { $type = $Matches[1]; continue }
        if ($line -match '^\s*Size:\s*(\d+)' -and $name -and $type -eq 'File') {
            $inRar[$name] = [long]$Matches[1]
            $name = $null
        }
    }
    if ($inRar.Count -ne $onDisk.Count) {
        throw "rar round-trip: $($inRar.Count) file entries, $($onDisk.Count) files on disk."
    }
    foreach ($k in $onDisk.Keys) {
        if (-not $inRar.ContainsKey($k)) { throw "rar round-trip: '$k' is missing from the archive." }
        if ($inRar[$k] -ne $onDisk[$k]) {
            throw "rar round-trip: size mismatch for '$k' ($($inRar[$k]) vs $($onDisk[$k]))."
        }
    }
    Write-Host ("  rar            round-trip OK ({0} entries)" -f $onDisk.Count) -ForegroundColor Green
    $ok = $true
    }
    finally {
        # A rar that failed its own test or the round-trip must not be left lying around
        # looking like a release.
        if (-not $ok -and (Test-Path -LiteralPath $rarPath)) { Remove-Item -Force -LiteralPath $rarPath }
    }

    $rarMB = (Get-Item -LiteralPath $rarPath).Length / 1MB
    Write-Host ""
    Write-Host ("Archive: {0}" -f $rarPath) -ForegroundColor Green
    Write-Host ("         {0:N1} MB, {1} files" -f $rarMB, $onDisk.Count)
}
finally {
    Pop-Location
}
