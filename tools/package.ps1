<#
.SYNOPSIS
    Builds a RELEASE package of WuchangMinimap: dist\WuchangMinimap-<version>\ plus the
    matching .zip.

.DESCRIPTION
    This is the *release* path. It is not deploy.ps1.

        build.ps1    compile + run the offline tests
        deploy.ps1   DEV install: copies main.dll (and main.pdb) straight into the game
                     folder, never overwrites a config the player has edited
        package.ps1  RELEASE: build.ps1, then assemble a clean tree that mirrors exactly
                     what a player copies into
                         <Game>\Project_Plague\Binaries\Win64\
                     and zip it. No pdb, no navmesh dumps, no player state files, and it
                     never writes anywhere near the Steam folder.

    The package tree:

        WuchangMinimap-<version>\
          INSTALL_GUIDE.html
          README.md
          CHANGELOG.md
          LICENSE
          THIRD_PARTY_NOTICES.md
          BUILD_INFO.txt
          ue4ss\Mods\WuchangMinimap\
            dlls\main.dll
            maps\maps.json, maps\chapter<1..5>\*.png
            markers\chapter*.json          (the hand-written *.sample.json is excluded)
            markers\items.json             (item display names, when it has been built)
            markers\shrines.json           (the shrine table: names, chapters, destinations)
            config_wuchang_minimap.txt
            config.ini
            enabled.txt

    Before zipping, the script smoke-checks that everything the runtime enumerates is
    actually in the tree: maps.json parses, every image and height-map PNG it names
    exists and is non-empty, the five chapter marker manifests are there and parse, and
    the config / enabled.txt / main.dll are present. Afterwards it round-trips the zip
    (entry count and per-entry length against the tree on disk).

.PARAMETER Version
    Stamp a new version. REWRITES src\version.hpp (the single source of truth that the
    DLL, the F2 panel and this script all read) and xmake.lua's set_version, then builds.
    Omit it to package whatever version.hpp already says.

.PARAMETER NoBuild
    Skip build.ps1 and package the main.dll that is already in build\. For iterating on
    the packaging itself; a release must never use it.

.PARAMETER OutDir
    Where the package folder and the zip are written. Default: <repo>\dist.

.PARAMETER AllowDirty
    Package even though `git status --porcelain` is not empty. Off by default: a zip
    built from a working tree that does not match any commit cannot be reproduced, and
    the commit recorded in BUILD_INFO.txt would be a lie. Use it only while iterating
    on the packaging itself.

.PARAMETER Ue4ssBuild
    The UE4SS release this DLL is ABI-tied to, recorded in BUILD_INFO.txt. The default
    is the build sdk\lib\UE4SS.lib was synthesised from (see sdk\UE4SS.def and
    tools\gen_ue4ss_importlib.ps1); change it when the mod is rebuilt against another
    UE4SS. Override with -Ue4ssBuild or the WUCHANG_UE4SS_BUILD environment variable.
#>
[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,
    [ValidateSet('Game__Shipping__Win64', 'Game__Debug__Win64')]
    [string]$Mode = 'Game__Shipping__Win64',
    [string]$OutDir,
    [switch]$NoBuild,
    [switch]$AllowDirty,
    [string]$Ue4ssBuild = $(if ($env:WUCHANG_UE4SS_BUILD) { $env:WUCHANG_UE4SS_BUILD }
                           else { 'v3.0.1-1111-g97b7e501' })
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$versionHeader = Join-Path $repo 'src\version.hpp'
$xmakeLua = Join-Path $repo 'xmake.lua'

function Read-ModVersion {
    $text = Get-Content -Raw -LiteralPath $versionHeader
    $m = [regex]::Match($text, '(?m)^#define\s+WUCHANG_MINIMAP_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $m.Success) { throw "Could not read WUCHANG_MINIMAP_VERSION from '$versionHeader'." }
    return $m.Groups[1].Value
}

function Write-ModVersion([string]$v) {
    $text = Get-Content -Raw -LiteralPath $versionHeader
    $text = [regex]::Replace($text,
        '(?m)^(#define\s+WUCHANG_MINIMAP_VERSION\s+")[0-9]+\.[0-9]+\.[0-9]+(")',
        "`${1}$v`${2}")
    [System.IO.File]::WriteAllText($versionHeader, $text, (New-Object System.Text.UTF8Encoding($false)))

    # xmake's set_version is metadata only, but a release with two different numbers in
    # it is a support ticket waiting to happen.
    $lua = Get-Content -Raw -LiteralPath $xmakeLua
    $lua = [regex]::Replace($lua, '(?m)^(set_version\(")[0-9]+\.[0-9]+\.[0-9]+("\))', "`${1}$v`${2}")
    [System.IO.File]::WriteAllText($xmakeLua, $lua, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Version stamped: $v  (src\version.hpp, xmake.lua)" -ForegroundColor Cyan
}

Push-Location $repo
try {
    #--------------------------------------------------------------------------------
    # 0. The tree must match a commit
    #--------------------------------------------------------------------------------
    # A release zip that cannot be rebuilt from a commit is not a release, and
    # BUILD_INFO.txt names a commit hash - which would be a lie if the tree that
    # produced the DLL had uncommitted edits in it. Checked BEFORE -Version stamps
    # version.hpp / xmake.lua, because that stamp is itself an uncommitted edit (the
    # release commit comes after packaging; see docs\RELEASE.md).
    $gitCommit = 'unknown'
    $gitBranch = 'unknown'
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        if (-not $AllowDirty) { throw "git is not on PATH - cannot verify the tree is clean. Pass -AllowDirty to package anyway." }
        Write-Warning "git not found: the package's BUILD_INFO.txt will not name a commit."
    } else {
        $dirty = @(& git -C $repo status --porcelain)
        if ($LASTEXITCODE -ne 0) { throw "git status failed ($LASTEXITCODE) in '$repo'." }
        if ($dirty.Count -gt 0 -and -not $AllowDirty) {
            foreach ($line in ($dirty | Select-Object -First 20)) { Write-Host "  $line" -ForegroundColor Yellow }
            if ($dirty.Count -gt 20) { Write-Host ("  ... and {0} more" -f ($dirty.Count - 20)) -ForegroundColor Yellow }
            throw ("Working tree has {0} uncommitted change(s) - commit them first, or pass -AllowDirty (the package would name a commit it was not built from)." -f $dirty.Count)
        }
        $gitCommit = (& git -C $repo rev-parse HEAD).Trim()
        $gitBranch = (& git -C $repo rev-parse --abbrev-ref HEAD).Trim()
        if ($dirty.Count -gt 0) { $gitCommit = "$gitCommit (DIRTY: $($dirty.Count) uncommitted change(s))" }
    }

    #--------------------------------------------------------------------------------
    # 1. Version
    #--------------------------------------------------------------------------------
    if ($Version) { Write-ModVersion $Version }
    $ver = Read-ModVersion

    $luaVer = [regex]::Match((Get-Content -Raw -LiteralPath $xmakeLua),
                             '(?m)^set_version\("([0-9]+\.[0-9]+\.[0-9]+)"\)')
    if ($luaVer.Success -and $luaVer.Groups[1].Value -ne $ver) {
        Write-Warning ("xmake.lua set_version is {0} but src\version.hpp says {1}; run package.ps1 -Version {1} to sync them." -f $luaVer.Groups[1].Value, $ver)
    }

    Write-Host ""
    Write-Host "Packaging WuchangMinimap $ver ($Mode)" -ForegroundColor Green

    #--------------------------------------------------------------------------------
    # 2. Build (compiles the DLL and runs the offline markers_test)
    #--------------------------------------------------------------------------------
    if (-not $NoBuild) {
        & (Join-Path $repo 'build.ps1') -Mode $Mode
        if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed ($LASTEXITCODE)" }
    } else {
        Write-Warning "-NoBuild: packaging the existing build\ output without rebuilding or testing."
    }

    $builtDll = Join-Path $repo "build\windows\x64\$Mode\main.dll"
    if (-not (Test-Path $builtDll)) { throw "main.dll not found at '$builtDll'." }

    #--------------------------------------------------------------------------------
    # 3. Assemble the tree
    #--------------------------------------------------------------------------------
    if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }
    $pkgName = "WuchangMinimap-$ver"
    $pkgRoot = Join-Path $OutDir $pkgName
    $zipPath = Join-Path $OutDir "$pkgName.zip"

    if (Test-Path $pkgRoot) { Remove-Item -Recurse -Force -LiteralPath $pkgRoot }
    if (Test-Path $zipPath) { Remove-Item -Force -LiteralPath $zipPath }
    $modDir = Join-Path $pkgRoot 'ue4ss\Mods\WuchangMinimap'
    New-Item -ItemType Directory -Force -Path (Join-Path $modDir 'dlls') | Out-Null

    # 3a. the DLL - and only the DLL. main.pdb is a dev artefact; it doubles the
    #     download and tells a player nothing.
    Copy-Item -LiteralPath $builtDll -Destination (Join-Path $modDir 'dlls\main.dll') -Force

    # 3b. map assets: maps.json + chapter<N>\*.png, straight out of the repo.
    $mapsSrc = Join-Path $repo 'maps'
    if (-not (Test-Path (Join-Path $mapsSrc 'maps.json'))) {
        throw "maps\maps.json is missing - build the assets with tools\navmesh\build_map.py first."
    }
    Copy-Item -LiteralPath $mapsSrc -Destination (Join-Path $modDir 'maps') -Recurse -Force

    # 3c. marker database. chapter*.json only: chapter1.sample.json is a hand-written
    #     schema example for the repo and must not ship (it would be loaded as a real
    #     chapter file).
    $markersDst = Join-Path $modDir 'markers'
    New-Item -ItemType Directory -Force -Path $markersDst | Out-Null
    $markerFiles = @(Get-ChildItem -LiteralPath (Join-Path $repo 'markers') -Filter 'chapter*.json' -File |
                     Where-Object { $_.Name -notlike '*.sample.json' })
    if ($markerFiles.Count -eq 0) { throw "No markers\chapter*.json - build them with tools\markers." }
    foreach ($f in $markerFiles) { Copy-Item -LiteralPath $f.FullName -Destination $markersDst -Force }

    #     items.json (the item display-name database, schema wuchang-minimap-items/1) is
    #     shipped alongside them when it exists. The runtime does not need it - the names
    #     are already baked into chapter*.json - but it is what a later tooltip/search
    #     feature reads, and the loader skips any file whose schema is not a marker
    #     manifest, so shipping it is free.
    $itemsSrc = Join-Path $repo 'markers\items.json'
    if (Test-Path $itemsSrc) { Copy-Item -LiteralPath $itemsSrc -Destination $markersDst -Force }

    #     shrines.json (schema wuchang-minimap-shrines/1, from
    #     tools\markers\extract_shrines.py) is REQUIRED from 0.9.4: it is what the full
    #     map's shrine list shows (the localised names and the chapter) and what fast
    #     travel targets. Without it the panel says so, which reads as a broken feature -
    #     so a package that lacks it is a packaging bug, not a degraded build.
    $shrinesSrc = Join-Path $repo 'markers\shrines.json'
    if (-not (Test-Path $shrinesSrc)) {
        throw "markers\shrines.json is missing - build it with tools\markers\extract_shrines.py."
    }
    Copy-Item -LiteralPath $shrinesSrc -Destination $markersDst -Force

    # 3d. the two shipped config files, and the enabled.txt opt-in UE4SS looks for.
    foreach ($cfg in @('config_wuchang_minimap.txt', 'config.ini')) {
        $src = Join-Path $repo "deploy\ue4ss\Mods\WuchangMinimap\$cfg"
        if (-not (Test-Path $src)) { throw "Shipped config '$src' is missing." }
        Copy-Item -LiteralPath $src -Destination (Join-Path $modDir $cfg) -Force
    }
    [System.IO.File]::WriteAllText((Join-Path $modDir 'enabled.txt'), '')

    # The developer overlay is NOT part of a release. It carries dials that only make
    # sense while bringing the mod up, and the mod reads it after the player config, so
    # one shipped by accident would silently override what a player edits.
    $devCfg = Join-Path $modDir 'config_wuchang_minimap_dev.txt'
    if (Test-Path $devCfg) { throw "config_wuchang_minimap_dev.txt must never be packaged." }

    # 3e. docs at the package root.
    $guideSrc = Join-Path $PSScriptRoot 'INSTALL_GUIDE.html'
    if (-not (Test-Path $guideSrc)) { throw "tools\INSTALL_GUIDE.html is missing." }
    $guide = Get-Content -Raw -LiteralPath $guideSrc
    $guide = $guide.Replace('@@VERSION@@', $ver).Replace('@@DATE@@', (Get-Date -Format 'yyyy-MM-dd'))
    [System.IO.File]::WriteAllText((Join-Path $pkgRoot 'INSTALL_GUIDE.html'), $guide,
                                   (New-Object System.Text.UTF8Encoding($false)))

    $changelogSrc = Join-Path $PSScriptRoot 'CHANGELOG.template.md'
    $changelog = (Get-Content -Raw -LiteralPath $changelogSrc).
                    Replace('@@VERSION@@', $ver).Replace('@@DATE@@', (Get-Date -Format 'yyyy-MM-dd'))
    [System.IO.File]::WriteAllText((Join-Path $pkgRoot 'CHANGELOG.md'), $changelog,
                                   (New-Object System.Text.UTF8Encoding($false)))

    #     The repo's README.md (the short, user-facing one), the licence and the
    #     third-party attributions. The notices file is not optional politeness: the
    #     DLL statically contains Dear ImGui and MinHook, whose licences both require
    #     the notice to travel with a binary redistribution.
    foreach ($doc in @('README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md')) {
        $docSrc = Join-Path $repo $doc
        if (-not (Test-Path -LiteralPath $docSrc -PathType Leaf)) { throw "'$doc' is missing from the repo root." }
        Copy-Item -LiteralPath $docSrc -Destination (Join-Path $pkgRoot $doc) -Force
    }

    #     BUILD_INFO.txt - what this exact zip was built from. It is the first thing to
    #     ask for in a bug report ("which build are you on?") and the only way to tie a
    #     download back to a commit once several 1.0.x zips are in the wild.
    $dllInfo = Get-Item -LiteralPath $builtDll
    $buildInfo = @(
        "WuchangMinimap $ver"
        ""
        "version    $ver"
        "commit     $gitCommit"
        "branch     $gitBranch"
        "packaged   $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')"
        "mode       $Mode"
        "main.dll   $([math]::Round($dllInfo.Length / 1KB)) KB, compiled $($dllInfo.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
        "UE4SS      $Ue4ssBuild  (compiled against this build's exports - sdk/UE4SS.def;"
        "           UE4SS itself is NOT part of this package, the player installs it)"
        ""
        "Licence: see LICENSE. Third-party attributions: see THIRD_PARTY_NOTICES.md."
    ) -join "`r`n"
    [System.IO.File]::WriteAllText((Join-Path $pkgRoot 'BUILD_INFO.txt'), $buildInfo + "`r`n",
                                   (New-Object System.Text.UTF8Encoding($false)))

    #--------------------------------------------------------------------------------
    # 4. Smoke check: is everything the runtime enumerates actually in the tree?
    #--------------------------------------------------------------------------------
    Write-Host ""
    Write-Host "Smoke check" -ForegroundColor Cyan
    $problems = New-Object System.Collections.Generic.List[string]

    function Require-File([string]$path, [string]$what) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $script:problems.Add("missing $what : $path"); return $false
        }
        if ((Get-Item -LiteralPath $path).Length -le 0 -and $path -notlike '*enabled.txt') {
            $script:problems.Add("empty $what : $path"); return $false
        }
        return $true
    }
    Require-File (Join-Path $modDir 'dlls\main.dll') 'mod DLL' | Out-Null
    # The package-root documents. A zip without LICENSE / THIRD_PARTY_NOTICES.md is not
    # shippable (see the notices file), and one without the README/guide is a support
    # ticket.
    Require-File (Join-Path $pkgRoot 'INSTALL_GUIDE.html') 'install guide' | Out-Null
    Require-File (Join-Path $pkgRoot 'README.md') 'README' | Out-Null
    Require-File (Join-Path $pkgRoot 'CHANGELOG.md') 'changelog' | Out-Null
    Require-File (Join-Path $pkgRoot 'LICENSE') 'licence' | Out-Null
    Require-File (Join-Path $pkgRoot 'THIRD_PARTY_NOTICES.md') 'third-party notices' | Out-Null
    Require-File (Join-Path $pkgRoot 'BUILD_INFO.txt') 'build info' | Out-Null
    # Nothing may still carry an unexpanded template placeholder.
    foreach ($tpl in @('INSTALL_GUIDE.html', 'CHANGELOG.md')) {
        $t = Join-Path $pkgRoot $tpl
        if ((Test-Path -LiteralPath $t -PathType Leaf) -and
            (Get-Content -Raw -LiteralPath $t) -match '@@[A-Z]+@@') {
            $problems.Add("$tpl still contains an @@PLACEHOLDER@@")
        }
    }
    Require-File (Join-Path $modDir 'config_wuchang_minimap.txt') 'overlay config' | Out-Null
    Require-File (Join-Path $modDir 'config.ini') 'navmesh config' | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $modDir 'enabled.txt') -PathType Leaf)) {
        $problems.Add('missing enabled.txt')
    }

    # maps.json: every image and every height map it names must exist. This is exactly
    # the list mapdata.cpp walks at start-up, so a missing PNG here is a missing PNG there.
    $manifestPath = Join-Path $modDir 'maps\maps.json'
    $pngCount = 0
    if (Require-File $manifestPath 'maps.json') {
        $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
        if ($manifest.schema -ne 'wuchang-minimap-maps/3') {
            $problems.Add("maps.json schema is '$($manifest.schema)', expected 'wuchang-minimap-maps/3'")
        }
        $chapters = @($manifest.chapters.PSObject.Properties)
        if ($chapters.Count -lt 5) { $problems.Add("maps.json lists $($chapters.Count) chapters, expected 5") }
        foreach ($ch in $chapters) {
            $c = $ch.Value
            foreach ($rel in @($c.image) + @($c.height_maps)) {
                if (-not $rel) { continue }
                $p = Join-Path (Join-Path $modDir 'maps') ($rel -replace '/', '\')
                if (Require-File $p "map image for $($ch.Name)") { $pngCount++ }
            }
        }
        Write-Host ("  maps.json      {0} chapter(s), {1} PNG referenced and present" -f $chapters.Count, $pngCount)
    }

    # A PNG in maps\ that maps.json does NOT name is dead weight in the download.
    $allPng = @(Get-ChildItem -LiteralPath (Join-Path $modDir 'maps') -Filter '*.png' -Recurse -File)
    if ($allPng.Count -ne $pngCount) {
        Write-Warning ("maps\ holds {0} PNG but maps.json names {1}" -f $allPng.Count, $pngCount)
    }

    # The five chapter marker manifests (chapterdlc.json is optional - the DLC has
    # markers but no map).
    $expected = 1..5 | ForEach-Object { "chapter$_.json" }
    foreach ($name in $expected) {
        $p = Join-Path $markersDst $name
        if (Require-File $p 'marker manifest') {
            $mk = Get-Content -Raw -LiteralPath $p | ConvertFrom-Json
            if ($mk.schema -ne 'wuchang-minimap-markers/1') {
                $problems.Add("$name schema is '$($mk.schema)'")
            }
        }
    }
    # The shrine table, with the same schema check the runtime applies.
    $shrinesPkg = Join-Path $markersDst 'shrines.json'
    if (Require-File $shrinesPkg 'shrine table') {
        $sh = Get-Content -Raw -LiteralPath $shrinesPkg | ConvertFrom-Json
        if ($sh.schema -ne 'wuchang-minimap-shrines/1') {
            $problems.Add("shrines.json schema is '$($sh.schema)'")
        }
        $realShrines = @($sh.shrines | Where-Object { $_.shrine }).Count
        if ($realShrines -lt 40) {
            $problems.Add("shrines.json holds only $realShrines real shrine(s) - the extractor regressed")
        }
        Write-Host ("  shrines        {0} row(s), {1} shrine(s)" -f $sh.count, $realShrines)
    }

    $shippedMarkers = @(Get-ChildItem -LiteralPath $markersDst -Filter '*.json' -File)
    if (@($shippedMarkers | Where-Object { $_.Name -like '*sample*' }).Count -gt 0) {
        $problems.Add('a *.sample.json leaked into the package')
    }
    Write-Host ("  markers        {0} manifest(s): {1}" -f $shippedMarkers.Count,
                (($shippedMarkers | ForEach-Object { $_.Name }) -join ', '))

    # Nothing that must never ship. Every file the MOD ITSELF writes at runtime is
    # named wuchang_minimap*  (the log, the collection tracker and its per-save
    # variants, the waypoint, firstrun/hookaddr/last_stage/watchdog breadcrumbs and the
    # recon dumps) - none of them is ever a shipped file, so the whole prefix is
    # forbidden in one rule. The two configs the release DOES carry are named
    # config_wuchang_minimap*.txt and do not match it. This matters because the same
    # folder layout is what the game writes into when someone runs the game from the
    # deploy mirror.
    $forbidden = @(Get-ChildItem -LiteralPath $pkgRoot -Recurse -File |
                   Where-Object { $_.Extension -in @('.pdb', '.exp', '.lib', '.ilk', '.orig', '.bak') -or
                                  $_.Name -like 'wuchang_minimap*' -or
                                  $_.Name -like 'tiles_*.json' -or
                                  $_.Name -like '*.sample.json' -or
                                  $_.Name -eq '.gitkeep' })
    foreach ($f in $forbidden) { $problems.Add("must not ship: $($f.FullName)") }
    if ((Test-Path (Join-Path $modDir 'navmesh'))) { $problems.Add('navmesh\ dump folder leaked into the package') }

    # Allow-list, not just a deny-list: anything that appears in the package without
    # this script having been taught about it is a leak by definition. (The tree is
    # assembled file by file from the repo, so this can only fire after an edit here -
    # which is exactly when it should.)
    $allowedRoot = @('INSTALL_GUIDE.html', 'README.md', 'CHANGELOG.md', 'LICENSE',
                     'THIRD_PARTY_NOTICES.md', 'BUILD_INFO.txt', 'ue4ss')
    foreach ($e in Get-ChildItem -LiteralPath $pkgRoot) {
        if ($e.Name -notin $allowedRoot) { $problems.Add("unexpected at package root: $($e.Name)") }
    }
    $allowedMod = @('dlls', 'maps', 'markers', 'config_wuchang_minimap.txt', 'config.ini', 'enabled.txt')
    foreach ($e in Get-ChildItem -LiteralPath $modDir) {
        if ($e.Name -notin $allowedMod) { $problems.Add("unexpected in the mod folder: $($e.Name)") }
    }
    $dllsExtra = @(Get-ChildItem -LiteralPath (Join-Path $modDir 'dlls') | Where-Object { $_.Name -ne 'main.dll' })
    foreach ($e in $dllsExtra) { $problems.Add("unexpected in dlls\: $($e.Name)") }

    if ($problems.Count -gt 0) {
        foreach ($p in $problems) { Write-Host "  FAIL  $p" -ForegroundColor Red }
        throw "Smoke check failed with $($problems.Count) problem(s) - nothing was zipped."
    }
    Write-Host "  OK             everything the runtime enumerates is present" -ForegroundColor Green

    #--------------------------------------------------------------------------------
    # 5. Zip, then round-trip it
    #--------------------------------------------------------------------------------
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $pkgRoot, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)

    $onDisk = @{}
    foreach ($f in Get-ChildItem -LiteralPath $pkgRoot -Recurse -File) {
        $rel = ($pkgName + '/' + $f.FullName.Substring($pkgRoot.Length + 1).Replace('\', '/'))
        $onDisk[$rel] = $f.Length
    }
    $zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $entries = @($zip.Entries | Where-Object { $_.Name -ne '' })
        if ($entries.Count -ne $onDisk.Count) {
            throw "zip round-trip: $($entries.Count) entries, $($onDisk.Count) files on disk."
        }
        foreach ($e in $entries) {
            # .NET Framework's ZipFile writes '\' separators on Windows; normalise.
            $key = $e.FullName.Replace('\', '/')
            if (-not $onDisk.ContainsKey($key)) { throw "zip round-trip: unexpected entry '$key'." }
            if ($onDisk[$key] -ne $e.Length) {
                throw "zip round-trip: size mismatch for '$key' ($($e.Length) vs $($onDisk[$key]))."
            }
        }
        # Actually decompress one entry, so a corrupt stream cannot pass on metadata alone.
        $probe = $entries | Where-Object { $_.Name -eq 'maps.json' } | Select-Object -First 1
        $sr = New-Object System.IO.StreamReader($probe.Open())
        try { $null = $sr.ReadToEnd() | ConvertFrom-Json } finally { $sr.Dispose() }
    } finally { $zip.Dispose() }
    Write-Host ("  zip            round-trip OK ({0} entries)" -f $onDisk.Count) -ForegroundColor Green

    #--------------------------------------------------------------------------------
    # 6. Report
    #--------------------------------------------------------------------------------
    $zipMB = (Get-Item -LiteralPath $zipPath).Length / 1MB
    $treeMB = ((Get-ChildItem -LiteralPath $pkgRoot -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB
    Write-Host ""
    Write-Host ("Package: {0}" -f $zipPath) -ForegroundColor Green
    Write-Host ("         {0:N1} MB zipped, {1:N1} MB unpacked, {2} files" -f $zipMB, $treeMB, $onDisk.Count)
    Write-Host ("Folder:  {0}" -f $pkgRoot)
}
finally {
    Pop-Location
}
