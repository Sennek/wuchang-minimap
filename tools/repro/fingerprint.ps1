<#
.SYNOPSIS
    Takes an independent fingerprint of everything repro.ps1 may touch, and compares two.

.DESCRIPTION
    repro.ps1 proves its own restores against its own snapshots. That proof is worth exactly
    as much as the snapshot it is measured against, so it cannot answer "is the install where
    it started". This script answers that from outside: it walks the four trees whole - not
    the files a profile happens to name - and hashes every one of them.

        <game>\Project_Plague\Binaries\Win64\        recursive; payloads, UE4SS, both mods
        %LOCALAPPDATA%\WuchangMinimap\               the mod's play state
        %LOCALAPPDATA%\Project_Plague\Saved\<user>\  the save slot and GameConfig
        %LOCALAPPDATA%\Project_Plague\Saved\Config\Windows\

    LastWriteTimeUtc is part of the fingerprint. A restore that put the bytes back and stamped
    "now" on a config is a change the mod's 1 Hz config watch would see, so a comparison that
    ignored the timestamp would pass a round trip that is not one.

    READ ONLY. It writes one JSON file, wherever -Out points, and nothing else.

.PARAMETER Verb
    take | compare

.PARAMETER Out
    take: where the fingerprint JSON goes.

.PARAMETER Before / -After
    compare: the two fingerprint files. Exit 0 when they are identical, 1 when they are not,
    and a throw when either file loaded fewer rows than it claims - a diff that has not sized
    its own input can only report the reassuring answer.

.EXAMPLE
    .\fingerprint.ps1 take -Out before.json
    .\repro.ps1 apply box; .\repro.ps1 apply stock; .\repro.ps1 restore
    .\fingerprint.ps1 take -Out after.json
    .\fingerprint.ps1 compare -Before before.json -After after.json
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('take', 'compare')]
    [string]$Verb,

    [string]$Out,
    [string]$Before,
    [string]$After,
    [string]$LabelA = 'before',
    [string]$LabelB = 'after',

    [string]$GameRoot = $(if ($env:WUCHANG_GAME_ROOT) { $env:WUCHANG_GAME_ROOT }
                          else { 'E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers' })
)

$ErrorActionPreference = 'Stop'

function Get-Roots {
    $saved = $(if ($env:WUCHANG_REPRO_SAVED_DIR) { $env:WUCHANG_REPRO_SAVED_DIR }
               else { Join-Path $env:LOCALAPPDATA 'Project_Plague\Saved' })
    $state = $(if ($env:WUCHANG_REPRO_STATE_DIR) { $env:WUCHANG_REPRO_STATE_DIR }
               else { Join-Path $env:LOCALAPPDATA 'WuchangMinimap' })
    $roots = [ordered]@{
        'bin'   = Join-Path $GameRoot 'Project_Plague\Binaries\Win64'
        'state' = $state
        'ini'   = Join-Path $saved 'Config\Windows'
    }
    # Every numeric user folder under Saved, so a second Steam account's save is fingerprinted too.
    Get-ChildItem -LiteralPath $saved -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+$' } |
        ForEach-Object { $roots["user-$($_.Name)"] = $_.FullName }
    return $roots
}

function Invoke-Take {
    if (-not $Out) { throw "take needs -Out <path>" }
    $roots = Get-Roots
    $rows = New-Object System.Collections.ArrayList
    foreach ($k in $roots.Keys) {
        $r = $roots[$k]
        if (-not (Test-Path -LiteralPath $r)) { Write-Host ("  {0,-12} absent  {1}" -f $k, $r); continue }
        $n = 0
        Get-ChildItem -LiteralPath $r -Recurse -File -Force | ForEach-Object {
            [void]$rows.Add([pscustomobject]@{
                root  = $k
                rel   = $_.FullName.Substring($r.Length).TrimStart('\')
                size  = $_.Length
                mtime = $_.LastWriteTimeUtc.ToString('o')
                sha   = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            })
            $n++
        }
        Write-Host ("  {0,-12} {1,5} file(s)  {2}" -f $k, $n, $r)
    }
    $sorted = @($rows | Sort-Object root, rel)
    [pscustomobject]@{
        taken = [DateTime]::UtcNow.ToString('o')
        game  = $GameRoot
        count = $sorted.Count
        files = $sorted
    } | ConvertTo-Json -Depth 5 | Out-File -LiteralPath $Out -Encoding utf8
    Write-Host ("{0} file(s) -> {1}" -f $sorted.Count, $Out)
}

function Read-Fingerprint([string]$path, [string]$label) {
    $j = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $h = @{}
    foreach ($r in @($j.files)) { $h["$($r.root)|$($r.rel)"] = $r }
    if ($h.Count -ne $j.count) {
        throw "$label : the file claims $($j.count) rows, $($h.Count) loaded - the fingerprint did not parse"
    }
    return ,$h
}

function Invoke-Compare {
    if (-not $Before -or -not $After) { throw "compare needs -Before <path> -After <path>" }
    $a = Read-Fingerprint $Before $LabelA
    $b = Read-Fingerprint $After  $LabelB
    Write-Host ("{0} : {1} file(s)    {2} : {3} file(s)" -f $LabelA, $a.Count, $LabelB, $b.Count)

    $onlyA = @(); $onlyB = @(); $diff = @(); $tdiff = @()
    foreach ($k in $a.Keys) {
        if (-not $b.ContainsKey($k))          { $onlyA += $k }
        elseif ($a[$k].sha -ne $b[$k].sha)    { $diff   += $k }
        elseif ($a[$k].mtime -ne $b[$k].mtime){ $tdiff  += $k }
    }
    foreach ($k in $b.Keys) { if (-not $a.ContainsKey($k)) { $onlyB += $k } }

    Write-Host ("gone   (only in {0}) : {1}" -f $LabelA, $onlyA.Count)
    foreach ($k in ($onlyA | Sort-Object)) { Write-Host "    - $k" }
    Write-Host ("new    (only in {0}) : {1}" -f $LabelB, $onlyB.Count)
    foreach ($k in ($onlyB | Sort-Object)) { Write-Host "    + $k" }
    Write-Host ("content differs      : {0}" -f $diff.Count)
    foreach ($k in ($diff  | Sort-Object)) { Write-Host "    ~ $k" }
    Write-Host ("mtime moved          : {0}" -f $tdiff.Count)
    foreach ($k in ($tdiff | Sort-Object)) { Write-Host "    t $k" }

    $total = $onlyA.Count + $onlyB.Count + $diff.Count + $tdiff.Count
    if ($total -eq 0) {
        Write-Host ("IDENTICAL - {0} file(s), every byte and every LastWriteTimeUtc" -f $a.Count)
        return 0
    }
    Write-Host ("DIFFERS - {0} file(s) do not match" -f $total)
    return 1
}

switch ($Verb) {
    'take'    { Invoke-Take; exit 0 }
    'compare' { exit (Invoke-Compare) }
}
