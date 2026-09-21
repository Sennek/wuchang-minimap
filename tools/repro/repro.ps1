<#
.SYNOPSIS
    Puts this box into a named configuration, remembers the exact bytes it replaced, and
    puts them back.

.DESCRIPTION
    A profile is a configuration a bug report was made in: which injectors and overlays
    load into the game, which mod build and config run, what the game's own settings say.
    `apply` makes this box that configuration, `restore` undoes it and proves the undo by
    hash, `status` says what is currently applied and what drifted since. `run` applies a
    profile, launches the game once per cell, decides each launch by
    `.claude/rules/in-game-verification.md`'s criteria, restores, and writes the evidence
    as a manifest; `verify` judges a manifest - any manifest, at any later date - against
    the profile it names.

    WHAT IT TOUCHES

        <game>\Project_Plague\Binaries\Win64\             payload DLLs and inis
        <game>\...\Win64\ue4ss\Mods\WuchangMinimap\       the mod: dll, configs, enabled.txt
        <game>\...\Win64\ue4ss\Mods\WuchangRecon\         the Lua recon mod's enabled.txt
        %LOCALAPPDATA%\WuchangMinimap\                    the mod's own play state
        %LOCALAPPDATA%\Project_Plague\Saved\Config\Windows\GameUserSettings.ini
        <store>\                                          vault, snapshots, payloads, current.json

    WHAT IT MUST NOT DO

        - write anything under the repo working tree. The repo is READ: `dist\` for a
          pinned mod build, `tools\repro\profiles\` for the profiles themselves.
        - write `deploy\`. deploy.ps1 is the only thing that writes the install from the repo.
        - write a save slot, and nothing but the two writers named below may write
          `Saved\<user id>\GameConfig\`. Resolve-RootPath is the guard: it normalises
          `<root>\<rel>` to a full path, refuses one that climbs out of its root, and
          refuses the `GameSlots` and `GameConfig` segments unless the caller states which
          which writer it is - only `saves-restore` writes there now, or
          `saves-restore`. A snapshot entry records that statement, so the restore that
          undoes such a write resolves it the same way and no wider.

          Three writes do not resolve through a root, and this is the whole list: the
          vault copies live files INTO the store (it reads a save slot, never writes one),
          the store's own snapshot copies and manifests, and `current.json`. Everything
          else - payloads, the mod's files, `enabled.txt`, the state dir, the game's ini
          and `GameConfig.sav` - goes through Resolve-RootPath.
        - write the registry, start or stop a process, or install, move or modify
          `UE4SS.dll`. HAGS and the pinned UE4SS build are read and compared; a mismatch is
          reported, never corrected.
        - delete a vault, a snapshot, or anything it did not create in this run.
        - run while the game is running (`status` and `list` excepted).
        - decide a cell by anything but the criteria in `.claude/rules/in-game-verification.md`,
          and never tear a healthy cell down with Stop-Process. The one kill in this script
          runs after a cell has already been decided, because a LowLevelFatalError modal
          cannot be closed any other way.
        - clear the mod's log. It is the player's log too. A run marks it before the launch
          and reads the new lines back against the mark, rotation and all.

    WHERE A RUN'S EVIDENCE GOES

        <store>\runs\<utc>-<profile>-<probe>\, beside the vault and the snapshots, never
        under the repo tree and never in `.workspace` - the store is the whole non-repo
        side of this pipeline and a run is one more thing it keeps.

    `dwmapi.dll` IN Binaries\Win64 IS NOT A WINDOWS DLL

        It is UE4SS's injection proxy - the file the game loads at start-up and the thing
        that in turn loads `UE4SS.dll`. Nothing in this round places or removes it, and no
        profile declares it; a future "no UE4SS at all" profile is the only thing that
        would ever move it, and it would be moving UE4SS, not patching Windows.

    THE UNDO IS WRITTEN BEFORE THE DAMAGE, NEVER AFTER

        An apply creates its snapshot directory and writes `current.json` with
        `complete: false` before it touches the first file, and each snapshot entry is
        persisted the moment its bytes are copied. `complete` turns true only when the
        apply has finished. An I/O error on a 165 MB payload, a held handle or a Ctrl-C
        therefore leaves a record that names every file already replaced: `restore` acts
        on it and says the apply it is undoing did not finish, `status` and `list` show it
        as incomplete, and the next `apply` refuses to start on top of it exactly as it
        refuses on top of a finished one. A record written after the mutations would be a
        record that exists only when it was not needed.

    WHY A RESTORE PUTS THE OLD LastWriteTimeUtc BACK

        The mod's 1 Hz watch does not read the config files; it hashes both files'
        timestamps together and only re-reads when that hash moves. A restore that stamped
        "now" on a config would therefore be a change the mod notices, and a restore that
        keeps the old timestamp is a change it does not - which is exactly right, because
        the game is not running during a restore (apply and restore refuse to run while it
        is) and the next launch reads both files from scratch anyway. Keeping the recorded
        timestamp is what makes "byte-identical to where it started" include the metadata a
        later run is decided on.

    TEST OVERRIDES

        WUCHANG_REPRO_STATE_DIR and WUCHANG_REPRO_SAVED_DIR exist so the whole round trip
        can be exercised against a throwaway tree. Those two directories are the only ones
        not derivable from WUCHANG_GAME_ROOT - they hang off $env:LOCALAPPDATA - and
        without an override every test would be a test against the owner's live play state.
        WUCHANG_REPRO_PROFILES_DIR is the third: `profiles\` is committed, so a test that
        needs a profile of its own would otherwise have to write one into the repo. With
        -GameRoot and -Store pointed at a sandbox, all four move together and an apply /
        restore can be proven on invented files before it is trusted with the install.

.PARAMETER Verb
    status | vault | apply | restore | saves-restore | list | run | verify | runs

.PARAMETER Name
    The profile name for `apply`, the vault stamp for `saves-restore`.

.PARAMETER Mod
    Overrides the profile's `mod.state` for this apply only, and is recorded in
    current.json. One profile therefore covers the four cells of a census run.

.PARAMETER DryRun
    Print every file that would be written and exit 0 having changed nothing. Every verb
    supports it; validation still runs.

.PARAMETER Force
    Proceeds past a `processes` or `manual` precondition that reads back wrong. It does
    NOT force the game-running refusal - a cell is a launch, and there is no such thing as
    applying a profile to a process that is already up.

.PARAMETER Probe
    What a cell collects and how it is decided. `crash` is the criteria of
    `.claude/rules/in-game-verification.md`, the process's module list and the mod's own
    lines from that launch. `present` is all of that plus the two instruments that price a
    frame: PresentMon on the game's own swapchain, and the mod's frame census. It needs the
    right to open an ETW session - administrator, or Performance Log Users - and it says so
    before it applies anything rather than degrading quietly.

.PARAMETER CycleMs
    -Probe present only: milliseconds the overlay spends in each layer of the census
    before it rotates to the next. The rotation is what puts every layer inside ONE
    capture, so scene drift and the mod's warm-up land on all of them equally. 0 measures
    the overlay as shipped and nothing else - the layers stay unpriced, and the overlay
    stops disappearing every couple of seconds, which is the only mode that is pleasant
    to play in.

.PARAMETER PresentMon
    -Probe present only: the PresentMon console build to capture with. Its sha256 is
    checked against `profiles/pinned.json` and a mismatch is named, not enforced.

.PARAMETER Cells
    Launches per run, three by default. A failure that arrives at ~19 s is timing-dependent
    and one launch per condition answers nothing.

.PARAMETER Hold
    Seconds a cell is held after the game has settled before it is called HEALTHY.

.PARAMETER Settle
    Seconds between the launch and the moment the game is resolved by name. Steam hands the
    launch on to a second process, so the game is the newest one of that name still ALIVE
    when this elapses - never whatever existed a moment after the launch.

.PARAMETER Until
    `hold` (the default) holds a cell for -Hold seconds and closes the game with WM_CLOSE.
    `exit` hands the session to the person at the keyboard: the cell ends when they leave
    the game, and the script never tears it down - a kill under a modal box while the game
    is autosaving is how a save is truncated. Use it for anything that has to be played.

.PARAMETER MaxMinutes
    With -Until exit, how long a played session may last before the cell gives up and says
    so. It is a bound on the script's patience, not on the session.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('status', 'vault', 'apply', 'restore', 'saves-restore', 'list',
                 'run', 'verify', 'runs')]
    [string]$Verb,

    [Parameter(Position = 1)]
    [string]$Name,

    [ValidateSet('absent', 'hooks-off', 'off', 'on')]
    [string]$Mod,

    [switch]$DryRun,
    [switch]$Force,

    [ValidateSet('crash', 'present')]
    [string]$Probe = 'crash',

    [ValidateRange(0, 60000)]
    [int]$CycleMs = 2000,

    [ValidateRange(1, 20)]
    [int]$Cells = 3,

    [ValidateRange(5, 3600)]
    [int]$Hold = 60,

    [ValidateRange(3, 120)]
    [int]$Settle = 10,

    [ValidateSet('hold', 'exit')]
    [string]$Until = 'hold',

    [ValidateRange(1, 480)]
    [int]$MaxMinutes = 120,

    [string]$GameRoot = $(if ($env:WUCHANG_GAME_ROOT) { $env:WUCHANG_GAME_ROOT }
                          else { 'E:\Program Files (x86)\Steam\steamapps\common\Wuchang Fallen Feathers' }),
    [string]$Store    = $(if ($env:WUCHANG_REPRO_STORE) { $env:WUCHANG_REPRO_STORE }
                          else { 'F:\Tools\wuchang-repro' }),
    [string]$PresentMon = $(if ($env:WUCHANG_PRESENTMON) { $env:WUCHANG_PRESENTMON }
                            else { 'F:\Tools\PresentMon\PresentMon.exe' })
)

$ErrorActionPreference = 'Stop'

# Every number this script reads was written by a machine with a '.' decimal point - the
# mod's log, PresentMon's CSV, the manifests it writes back. On this box the shell is
# ru-RU, where 25.205 formats as "25,205" and parses as 25205, so the script works in one
# culture from end to end rather than remembering to say so at each call.
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture

#------------------------------------------------------------------------------------
# Paths. Derived, never parameters - the two env vars above are the only dials, plus
# the two test overrides for the directories that hang off LOCALAPPDATA.
#------------------------------------------------------------------------------------
$repo        = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$profilesDir = $(if ($env:WUCHANG_REPRO_PROFILES_DIR) { $env:WUCHANG_REPRO_PROFILES_DIR }
                 else { Join-Path $PSScriptRoot 'profiles' })
$bin         = Join-Path $GameRoot 'Project_Plague\Binaries\Win64'
$modDir      = Join-Path $bin 'ue4ss\Mods\WuchangMinimap'
$reconDir    = Join-Path $bin 'ue4ss\Mods\WuchangRecon'
$ue4ssDll    = Join-Path $bin 'ue4ss\UE4SS.dll'
$stateDir    = $(if ($env:WUCHANG_REPRO_STATE_DIR) { $env:WUCHANG_REPRO_STATE_DIR }
                 else { Join-Path $env:LOCALAPPDATA 'WuchangMinimap' })
$saved       = $(if ($env:WUCHANG_REPRO_SAVED_DIR) { $env:WUCHANG_REPRO_SAVED_DIR }
                 else { Join-Path $env:LOCALAPPDATA 'Project_Plague\Saved' })

$vaultRoot    = Join-Path $Store 'vault'
$snapshotRoot = Join-Path $Store 'snapshots'
$payloadRoot  = Join-Path $Store 'payloads'
$runRoot      = Join-Path $Store 'runs'
$currentJson  = Join-Path $Store 'current.json'
# Written when a run leaves a process up that it started and could not stop; printed by
# every `status` until the process is gone, then deleted by the same reader.
$processNotice = Join-Path $Store 'left_running.json'

$Roots = @{
    'bin'   = $bin
    'mod'   = $modDir
    'recon' = $reconDir
    'state' = $stateDir
    'saved' = $saved
}

$WatchedProcesses = @('Project_Plague-Win64-Shipping', 'CrashReportClient', 'RTSS',
                      'RTSSHooksLoader64', 'EncoderServer', 'MSIAfterburner', 'Discord')
$GameProcesses    = @('Project_Plague-Win64-Shipping', 'CrashReportClient')
$LogGlob          = 'wuchang_minimap.log'

$RunSchema        = 'wuchang-repro-run/1'
$ExpectFields     = @('verdict', 'modules', 'modules_absent', 'log_lines', 'log_lines_absent', 'note')
$ProfileFields    = @('name', 'description', 'note', 'report', 'payloads', 'mod', 'recon', 'state',
                      'game_settings', 'game_config_expect', 'processes', 'expect', 'manual')
$ModFields        = @('build', 'state', 'config', 'config_dev')

#====================================================================================
# Primitives
#====================================================================================

function New-Stamp {
    # 'Z' is not a custom format specifier, so it is concatenated rather than formatted.
    return ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + 'Z')
}

function New-FreeStamp([string]$parent, [string]$suffix) {
    # A stamp is second-resolution, so two runs inside one second would land in one
    # folder - and writing into a folder this run did not create is how a snapshot stops
    # being an undo. The suffix disambiguates rather than overwriting.
    $base  = New-Stamp
    $stamp = $base
    $n = 1
    while (Test-Path -LiteralPath (Join-Path $parent ($stamp + $suffix))) {
        $n++
        $stamp = "$base-$n"
    }
    return $stamp
}

function Get-Hash([string]$path, [string]$algorithm) {
    return (Get-FileHash -LiteralPath $path -Algorithm $algorithm).Hash.ToLowerInvariant()
}

function Get-Sha([string]$path) { return (Get-Hash $path 'SHA256') }
function Get-Md5([string]$path) { return (Get-Hash $path 'MD5') }

function Get-MtimeString([string]$path) {
    return (Get-Item -LiteralPath $path).LastWriteTimeUtc.ToString('o')
}

function Get-RelativeTo([string]$root, [string]$full) {
    # Both sides go through the provider first: a root spelled with an 8.3 component and
    # a child the provider expanded to the long name are the same path at two lengths,
    # and subtracting the wrong one loses a character off the front of every rel.
    $r = (Get-Item -LiteralPath $root).FullName.TrimEnd('\')
    if (-not $full.StartsWith($r, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "'$full' is not under '$r'."
    }
    return $full.Substring($r.Length).TrimStart('\')
}

function Test-Utf8Bom([string]$path) {
    # The mod's two config files are UTF-8 WITH a BOM on this box. A tool that exists to
    # prove byte-exactness may not re-encode a file the mod parses, so the BOM is read off
    # the bytes and written back by whoever rewrites the file.
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
    $head = New-Object byte[] 3
    $fs = [System.IO.File]::OpenRead($path)
    try { $n = $fs.Read($head, 0, 3) } finally { $fs.Dispose() }
    return ($n -eq 3 -and $head[0] -eq 0xEF -and $head[1] -eq 0xBB -and $head[2] -eq 0xBF)
}

function Write-TextFile([string]$path, [string]$text, [bool]$bom = $false) {
    $dir = Split-Path -Parent $path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($bom)))
}

function Write-JsonFile([string]$path, $object) {
    Write-TextFile $path (($object | ConvertTo-Json -Depth 12) + "`r`n")
}

function Read-JsonFile([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Expected a JSON file at '$path' and there is none."
    }
    # ReadAllText, not Get-Content: a UTF-8 file with no BOM is read as the ANSI code
    # page by Get-Content on 5.1, and every em-dash in a description comes back mangled.
    return ([System.IO.File]::ReadAllText($path) | ConvertFrom-Json)
}

# A List[object] is NEVER wrapped in @(). On this box's PowerShell (5.1.19041.6456) the
# array subexpression throws "Argument types do not match" on a generic List, whatever it
# holds - `foreach`, a pipeline and .ToArray() all work, so .ToArray() is what this script
# uses. A wrap that throws inside a hashtable literal silently drops the key when the
# error is non-terminating, which is how a manifest loses its cells.

function AsArray($value) {
    # ConvertFrom-Json yields $null for an absent field and a bare object for a
    # one-element array; @($null) would otherwise be a one-element array of nothing.
    # The leading comma is what makes this a function callers can index and count: a
    # returned array of one element is otherwise unrolled to the element on the way out,
    # and a snapshot of exactly one file would walk zero of them.
    if ($null -eq $value) { return ,@() }
    return ,@($value)
}

function Get-StringList($object, [string]$field) {
    # @($null).Count is 1 in PowerShell, so a field that is not there reads as one entry -
    # an empty string, which every path ends with and every line contains. Every list this
    # script reads out of parsed JSON comes through here: the profile's `expect` lists, and
    # a cell record from a manifest written before the field existed.
    return @(@($object.$field) | Where-Object { $_ -is [string] -and $_.Trim().Length -gt 0 })
}

function Get-PropertyNames($object) {
    if ($null -eq $object) { return @() }
    return @($object.PSObject.Properties | ForEach-Object { $_.Name })
}

#------------------------------------------------------------------------------------
# Every byte this tool copies goes through Copy-Exact, and every mutation through the
# four verbs below it, so -DryRun is one branch and not a hundred.
#------------------------------------------------------------------------------------
function Copy-Exact([string]$from, [string]$to) {
    # A copy nobody re-reads is not a backup. An empty, truncated or short-copied file
    # hashes differently from its source, and the only useful moment to hear that is the
    # moment it was written - not the restore that needed it. Callers hear it as a throw.
    $dir = Split-Path -Parent $to
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    Copy-Item -LiteralPath $from -Destination $to -Force
    $want = Get-Sha $from
    $have = Get-Sha $to
    if ($have -ne $want) {
        throw "the copy '$to' hashes $have; its source '$from' hashes $want. The copy is not the file it was made from."
    }
    return $have
}

function Invoke-Copy([string]$from, [string]$to, [string]$why) {
    Write-Host ("  {0,-6} {1}" -f $(if ($DryRun) { 'would' } else { 'write' }), $to)
    if ($why) { Write-Host ("         {0}" -f $why) -ForegroundColor DarkGray }
    if ($DryRun) { return }
    $null = Copy-Exact $from $to
}

function Invoke-Delete([string]$path, [string]$why) {
    Write-Host ("  {0,-6} {1}" -f $(if ($DryRun) { 'would' } else { 'del  ' }), $path)
    if ($why) { Write-Host ("         {0}" -f $why) -ForegroundColor DarkGray }
    if ($DryRun) { return }
    # -Recurse because the primitive takes whatever path the caller resolved: without it a
    # directory would put PowerShell's "item has children" prompt in front of a script that
    # has no console to answer it. Which paths may be a directory is the caller's rule.
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force -Recurse }
}

function Invoke-Rename([string]$from, [string]$to, [string]$why) {
    Write-Host ("  {0,-6} {1} -> {2}" -f $(if ($DryRun) { 'would' } else { 'move ' }),
                $from, (Split-Path -Leaf $to))
    if ($why) { Write-Host ("         {0}" -f $why) -ForegroundColor DarkGray }
    if ($DryRun) { return }
    if (Test-Path -LiteralPath $to) { Remove-Item -LiteralPath $to -Force }
    Move-Item -LiteralPath $from -Destination $to -Force
}

function Invoke-WriteText([string]$path, [string]$text, [string]$why, [bool]$bom = $false) {
    Write-Host ("  {0,-6} {1}" -f $(if ($DryRun) { 'would' } else { 'write' }), $path)
    if ($why) { Write-Host ("         {0}" -f $why) -ForegroundColor DarkGray }
    if ($DryRun) { return }
    Write-TextFile $path $text $bom
}

#====================================================================================
# Roots, and the one path nothing here may ever write
#====================================================================================

function Resolve-RootPath([string]$root, [string]$rel, [string]$intent = 'general') {
    # The one place a write turns a root and a relative path into a path on disk, and the
    # only place the two protected areas of `saved` are named. Three things are decided
    # here and nowhere else:
    #
    #   containment  - the result is normalised first, so a `rel` that climbs with `..`
    #                  out of its root is refused rather than followed. A rooted `rel`
    #                  ('C:\…') is not a relative path at all and is refused too.
    #   GameSlots    - the one thing on this box that cannot be re-downloaded. Only
    #                  `saves-restore` (intent 'save_area') may write there.
    #   GameConfig   - the in-game graphics selector, and the owner's progress in the same
    #                  blob. Only `saves-restore` (intent 'save_area') may write it; a
    #                  profile READS it through `game_config_expect` and never writes.
    #
    # The intent travels in the snapshot entry, so the restore of such a write resolves it
    # with the same permission and no wider.
    if (-not $Roots.ContainsKey($root)) { throw "Unknown snapshot root '$root'." }
    # A snapshot entry hands its own intent straight back here, and JSON's null for a field
    # a record predates is the unrestricted one.
    if (-not $intent) { $intent = 'general' }
    if ($intent -notin @('general', 'save_area')) {
        throw "Unknown write intent '$intent' for 'root\$rel'."
    }
    if ([System.IO.Path]::IsPathRooted($rel)) {
        throw "'$rel' is an absolute path; a path under root '$root' must be relative to it."
    }
    $base = [System.IO.Path]::GetFullPath($Roots[$root]).TrimEnd('\')
    $full = [System.IO.Path]::GetFullPath((Join-Path $base $rel))
    if ($full -ne $base -and -not $full.StartsWith($base + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "'$rel' resolves to '$full', which is outside root '$root' ('$base'). Refusing to write out of a root."
    }
    $norm = $full.Substring($base.Length).TrimStart('\')
    if ($root -eq 'saved') {
        if ($norm -match '(^|[\\/])GameSlots([\\/]|$)' -and $intent -ne 'save_area') {
            throw "Refusing to touch a save slot: 'saved\$norm'. Only saves-restore writes there."
        }
        if ($norm -match '(^|[\\/])GameConfig([\\/]|$|\.)' -and $intent -ne 'save_area') {
            throw "Refusing to touch the game's own config save: 'saved\$norm'. Only saves-restore writes there - it carries the owner's progress, not just his graphics settings."
        }
    }
    return $full
}

function Test-GameRunning {
    foreach ($p in $GameProcesses) {
        if (@(Get-Process -Name $p -ErrorAction SilentlyContinue).Count -gt 0) { return $p }
    }
    return $null
}

function Assert-GameClosed([string]$verb) {
    $running = Test-GameRunning
    if ($running) {
        throw "'$running' is running. $verb refuses while the game is up - a cell is a launch, and -Force does not cover this."
    }
}

#====================================================================================
# The config files: rewrite a value, never a comment or an ordering - and only when the
# value is not already the one asked for
#====================================================================================

function Set-ConfigKeys([string]$root, [string]$rel, $pairs, [string]$label) {
    # Sets keys in a config, recording the bytes it replaces first - and writes nothing at
    # all when every key already holds the value asked for.
    #
    # The skip is not an optimisation. The mod's 1 Hz watch does not read the config files;
    # it hashes both files' timestamps together and re-reads only when that hash moves. A
    # write that changes no byte but stamps `now` is therefore a change the mod sees and
    # the bytes do not record - and a snapshot entry beside it claims an apply replaced a
    # file it did not. Two profiles that declare the same `game_settings` swapped that ini
    # in and out on every apply for nothing.
    $keys = Get-PropertyNames $pairs
    if ($keys.Count -eq 0) { return }
    $path = Resolve-RootPath $root $rel

    $text = ''
    # ReadAllText consumes a BOM, so whether there was one is read off the bytes and
    # handed back to the writer. Both of the mod's installed configs carry one.
    $bom = (Test-Utf8Bom $path)
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $text = [System.IO.File]::ReadAllText($path)
    }
    $before = $text
    $eol = $(if ($text -match "`r`n") { "`r`n" } else { "`n" })

    foreach ($key in $keys) {
        $value = [string]$pairs.$key
        $pattern = '(?m)^([ \t]*' + [regex]::Escape($key) + '[ \t]*=[ \t]*)[^\r\n]*'
        $rx = New-Object System.Text.RegularExpressions.Regex($pattern)
        if ($rx.IsMatch($text)) {
            # A MatchEvaluator, because a value may legitimately contain '$'.
            $ev = [System.Text.RegularExpressions.MatchEvaluator] {
                param($m) return ($m.Groups[1].Value + $value)
            }
            $text = $rx.Replace($text, $ev, 1)
        } else {
            if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) { $text += $eol }
            $text += ($key + ' = ' + $value + $eol)
        }
    }
    if ($text -eq $before) {
        Write-Host ("  same     {0}" -f $path) -ForegroundColor DarkGray
        Write-Host ("           {0} : {1} already hold the declared value; not written" -f
                    $label, ($keys -join ', ')) -ForegroundColor DarkGray
        return
    }
    Add-SnapshotEntry $root $rel
    Invoke-WriteText $path $text ("$label : " + ($keys -join ', ')) $bom
}

function Get-ConfigValue([string]$path, [string]$key) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    $text = [System.IO.File]::ReadAllText($path)
    $m = [regex]::Match($text, '(?m)^[ \t]*' + [regex]::Escape($key) + '[ \t]*=[ \t]*([^\r\n;#]*)')
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value.Trim()
}

#====================================================================================
# The vault - the owner's play state, copied before anything else happens
#====================================================================================

function Get-PlayStateFiles {
    $out = New-Object System.Collections.Generic.List[object]

    if (Test-Path -LiteralPath $saved -PathType Container) {
        # A user-id folder: all digits, or one that carries GameSlots / GameConfig.
        foreach ($d in Get-ChildItem -LiteralPath $saved -Directory) {
            $isUser = ($d.Name -match '^\d+$') -or
                      (Test-Path -LiteralPath (Join-Path $d.FullName 'GameSlots')) -or
                      (Get-ChildItem -LiteralPath $d.FullName -Filter 'GameConfig*' -ErrorAction SilentlyContinue)
            if (-not $isUser) { continue }
            foreach ($f in Get-ChildItem -LiteralPath $d.FullName -Recurse -File) {
                $out.Add([pscustomobject]@{
                    root = 'saved'
                    rel  = (Get-RelativeTo $saved $f.FullName)
                    abs  = $f.FullName
                })
            }
        }
        $iniDir = Join-Path $saved 'Config\Windows'
        if (Test-Path -LiteralPath $iniDir -PathType Container) {
            foreach ($f in Get-ChildItem -LiteralPath $iniDir -Filter '*.ini' -File) {
                $out.Add([pscustomobject]@{
                    root = 'saved'
                    rel  = (Get-RelativeTo $saved $f.FullName)
                    abs  = $f.FullName
                })
            }
        }
    }

    if (Test-Path -LiteralPath $stateDir -PathType Container) {
        foreach ($f in Get-ChildItem -LiteralPath $stateDir -Recurse -File) {
            # The logs are not play state, and they are the large half of the folder.
            if ($f.Name -like "$LogGlob*") { continue }
            $out.Add([pscustomobject]@{
                root = 'state'
                rel  = (Get-RelativeTo $stateDir $f.FullName)
                abs  = $f.FullName
            })
        }
    }
    return $out
}

function Get-VaultStamps {
    if (-not (Test-Path -LiteralPath $vaultRoot -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $vaultRoot -Directory |
             Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'vault.json') } |
             Sort-Object Name | ForEach-Object { $_.Name })
}

function Get-NewestVaultStamp {
    # @() on the way in, because a function returning a one-element array returns the
    # element - and [-1] on a bare string is its last character, not the newest stamp.
    $stamps = @(Get-VaultStamps)
    if ($stamps.Count -eq 0) { return $null }
    return $stamps[-1]
}

function Compare-AgainstVault([string]$stamp) {
    # One record per file that differs from that vault; an empty list means the play state
    # is exactly what was vaulted. Each record carries its own root, rel and kind, so a
    # caller that has to tell a changed SAVE SLOT from the GameConfig.sav the game rewrites
    # on every exit asks the record rather than parsing the printed line back apart.
    $diffs = New-Object System.Collections.Generic.List[object]
    $manifest = Read-JsonFile (Join-Path $vaultRoot "$stamp\vault.json")
    $recorded = @{}
    $where    = @{}
    foreach ($e in (AsArray $manifest.files)) {
        $key = "$($e.root)\$($e.rel)"
        $recorded[$key] = $e.sha256
        $where[$key]    = @([string]$e.root, [string]$e.rel)
    }

    $live = @{}
    foreach ($f in (Get-PlayStateFiles)) {
        $key = "$($f.root)\$($f.rel)"
        $live[$key] = $f.abs
        if (-not $where.ContainsKey($key)) { $where[$key] = @([string]$f.root, [string]$f.rel) }
    }

    foreach ($key in ($recorded.Keys + $live.Keys | Sort-Object -Unique)) {
        $how = $null
        if (-not $live.ContainsKey($key))          { $how = 'gone' }
        elseif (-not $recorded.ContainsKey($key))  { $how = 'new' }
        elseif ((Get-Sha $live[$key]) -ne $recorded[$key]) { $how = 'changed' }
        if (-not $how) { continue }

        $root = $where[$key][0]
        $rel  = $where[$key][1]
        $diffs.Add([pscustomobject]@{
            how  = $how
            root = $root
            rel  = $rel
            kind = $(if ($root -eq 'saved') { Get-SavedFileKind $rel } else { $root })
            text = ("{0,-9} {1}" -f $how, $key)
        })
    }
    return $diffs
}

function Invoke-Vault {
    Write-Host "Vault" -ForegroundColor Cyan
    $files = @(Get-PlayStateFiles)
    if ($files.Count -eq 0) {
        Write-Host "  (no play state found under '$saved' or '$stateDir')" -ForegroundColor Yellow
    }

    $entries = New-Object System.Collections.Generic.List[object]
    foreach ($f in $files) {
        $item = Get-Item -LiteralPath $f.abs
        $entries.Add([pscustomobject]@{
            root   = $f.root
            rel    = $f.rel
            len    = $item.Length
            sha256 = (Get-Sha $f.abs)
            mtime  = $item.LastWriteTimeUtc.ToString('o')
        })
    }

    $newest = Get-NewestVaultStamp
    if ($newest) {
        $diffs = @(Compare-AgainstVault $newest)
        if ($diffs.Count -eq 0) {
            Write-Host ("  unchanged      {0} file(s); {1} still stands" -f $entries.Count, $newest)
            return $newest
        }
    }

    $stamp = New-FreeStamp $vaultRoot ''
    $dir   = Join-Path $vaultRoot $stamp
    $byKey = @{}
    foreach ($e in $entries) { $byKey["$($e.root)\$($e.rel)"] = $e.sha256 }
    foreach ($f in $files) {
        # The vault READS a save slot, and only a write is forbidden - so the copy comes
        # off the path the enumeration already handed back, not off Resolve-RootPath. The
        # destination is inside the store, which no root covers.
        $src = $f.abs
        $dst = Join-Path $dir ("files\{0}\{1}" -f $f.root, $f.rel)
        if ($DryRun) {
            Write-Host ("  would    {0}" -f $dst)
            continue
        }
        # Copy-Exact proves the copy against the file it came from; this proves it against
        # the hash the manifest is about to claim, which is the one a later restore trusts.
        $have = Copy-Exact $src $dst
        if ($have -ne $byKey["$($f.root)\$($f.rel)"]) {
            throw "vault: '$src' changed while it was being vaulted - the copy hashes $have, the manifest was about to say $($byKey["$($f.root)\$($f.rel)"])."
        }
    }

    # The array is hoisted: an @(...) subexpression inside an [ordered] literal that is
    # then cast to PSCustomObject throws "Argument types do not match" on 5.1.
    $fileArray = $entries.ToArray()
    $manifest = [pscustomobject][ordered]@{
        stamp = $stamp
        host  = $env:COMPUTERNAME
        saved = $saved
        state = $stateDir
        files = $fileArray
    }
    if ($DryRun) {
        Write-Host ("  would    {0}" -f (Join-Path $dir 'vault.json'))
        Write-Host ("  vault          {0} file(s) would go to {1}" -f $entries.Count, $stamp)
    } else {
        Write-JsonFile (Join-Path $dir 'vault.json') $manifest
        Write-Host ("  vault          {0} file(s) -> {1}" -f $entries.Count, $stamp) -ForegroundColor Green
    }
    return $stamp
}

#====================================================================================
# Snapshots - the exact bytes an apply replaced or removed
#====================================================================================

$script:SnapEntries = $null
$script:SnapDirs    = $null
$script:SnapSeen    = $null
$script:SnapDirSeen = $null
$script:SnapDir     = $null
$script:SnapProfile = $null
$script:SnapStamp   = $null

function Start-Snapshot([string]$dir, [string]$profileName, [string]$stamp) {
    # The directory and an empty snapshot.json exist before the apply touches its first
    # file, so an apply that dies mid-way has left a record rather than a mystery.
    $script:SnapEntries = New-Object System.Collections.Generic.List[object]
    $script:SnapDirs    = New-Object System.Collections.Generic.List[object]
    $script:SnapSeen    = @{}
    $script:SnapDirSeen = @{}
    $script:SnapDir     = $dir
    $script:SnapProfile = $profileName
    $script:SnapStamp   = $stamp
    $null = Save-Snapshot
}

function Save-Snapshot {
    # snapshot.json as it stands right now. Written again after every entry: the cost is a
    # few KB of JSON per file and the return is that the record never trails the damage.
    # Both arrays are hoisted: an expression inside an [ordered] literal that is then cast
    # to PSCustomObject throws "Argument types do not match" on 5.1.
    # `$( ... @() ... )` is not a way to produce an empty array: the subexpression UNROLLS
    # it to $null, and $null in an [ordered] literal serializes as `{}` - an empty object,
    # which reads back as an object with no properties rather than as a list of none. The
    # assignment below does not unroll, so an empty snapshot writes `"dirs": []`.
    $fileArray = $script:SnapEntries.ToArray()
    $dirArray  = @()
    if ($script:SnapDirs) { $dirArray = $script:SnapDirs.ToArray() }
    $rootsObj  = [pscustomobject]$Roots
    $manifest = [pscustomobject][ordered]@{
        stamp   = $script:SnapStamp
        profile = $script:SnapProfile
        host    = $env:COMPUTERNAME
        roots   = $rootsObj
        files   = $fileArray
        # Directories the apply had to CREATE. A tree is restored as its files, and the
        # files alone would leave the folder standing empty - a residue the fingerprint
        # never sees, because it counts files. Recorded so the restore can remove exactly
        # what the apply made and nothing that was already there.
        dirs    = $dirArray
    }
    if (-not $DryRun) { Write-JsonFile (Join-Path $script:SnapDir 'snapshot.json') $manifest }
    return $manifest
}

function Get-FlatName([int]$index, [string]$root, [string]$rel) {
    # A destination path carries '\', so the copy under files\ is flattened. The index
    # prefix is what makes the flattened name unique; snapshot.json carries the real
    # root + relative path beside it, so a restore is never guessing.
    $flat = ($root + '__' + $rel) -replace '[\\/:*?"<>|]', '_'
    if ($flat.Length -gt 120) { $flat = $flat.Substring($flat.Length - 120) }
    return ('{0:d4}__{1}' -f $index, $flat)
}

function Add-SnapshotDirs([string]$root, [string]$rel, [string]$intent = 'general') {
    # Every directory on the way to `rel` that does not exist yet, outermost first, so a
    # restore walking the list backwards removes the deepest one first. Recorded before
    # anything is written into them; creating them is Copy-Exact's job.
    $abs    = Resolve-RootPath $root $rel $intent
    $rootAbs = (Resolve-RootPath $root '.' $intent).TrimEnd('\')
    $missing = New-Object System.Collections.Generic.List[string]
    $dir = Split-Path -Parent $abs
    while ($dir -and $dir.Length -gt $rootAbs.Length -and -not (Test-Path -LiteralPath $dir)) {
        $missing.Insert(0, $dir)
        $dir = Split-Path -Parent $dir
    }
    foreach ($d in $missing) {
        $key = $d.ToLowerInvariant()
        if ($script:SnapDirSeen.ContainsKey($key)) { continue }
        $script:SnapDirSeen[$key] = $true
        $script:SnapDirs.Add([pscustomobject][ordered]@{ path = $d })
    }
}

function Get-TreeFiles([string]$dir) {
    # Every file under `dir`, as paths relative to it. The empty list when it does not
    # exist, because "state this tree absent" is satisfied by a tree that is not there.
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) { return @() }
    # The base comes from the SAME provider call that yields the children, because two
    # PowerShell APIs can spell one path differently: `Resolve-Path` hands back the 8.3
    # short form when it was given one ('A1A85~1.KOZ') while `Get-ChildItem` expands it
    # ('a.kozachenko'), and a Substring by length then slices in the wrong place. The
    # StartsWith is not decoration - it is what turns that class of mistake into a throw
    # instead of a path with a stray separator in front of it.
    $base = (Get-Item -LiteralPath $dir).FullName.TrimEnd([char]92) + [string][char]92
    return @(Get-ChildItem -LiteralPath $dir -File -Recurse -Force | ForEach-Object {
                 if (-not $_.FullName.StartsWith($base, [System.StringComparison]::OrdinalIgnoreCase)) {
                     throw "tree: '$($_.FullName)' is not under '$base'; refusing to guess its relative path."
                 }
                 $_.FullName.Substring($base.Length)
             } | Sort-Object)
}

function Add-SnapshotEntry([string]$root, [string]$rel, [bool]$volatile = $false, [string]$intent = 'general') {
    $abs = Resolve-RootPath $root $rel $intent
    $key = $abs.ToLowerInvariant()
    if ($script:SnapSeen.ContainsKey($key)) { return }
    $script:SnapSeen[$key] = $true
    if ((Test-Path -LiteralPath $abs) -and -not (Test-Path -LiteralPath $abs -PathType Leaf)) {
        throw "snapshot: '$abs' is a directory, not a file. Nothing here replaces or removes a tree."
    }

    $index = $script:SnapEntries.Count + 1
    $file  = Get-FlatName $index $root $rel
    $entry = [ordered]@{
        root     = $root
        rel      = $rel
        file     = $file
        # Which writer the apply declared itself to be. The restore resolves the path with
        # the same intent, so undoing a `saves-restore` write is allowed and nothing else
        # inherits that permission.
        intent   = $intent
        # ReShade rewrites its own ini while the game runs, so a file marked volatile is
        # snapshotted and restored like any other but never counted as drift by `status`.
        volatile = $volatile
        absent   = $true
        len      = 0
        sha256   = $null
        mtime    = $null
    }
    if (Test-Path -LiteralPath $abs -PathType Leaf) {
        $item = Get-Item -LiteralPath $abs
        $entry.absent = $false
        $entry.len    = $item.Length
        $entry.sha256 = (Get-Sha $abs)
        $entry.mtime  = $item.LastWriteTimeUtc.ToString('o')
        $dst = Join-Path $script:SnapDir "files\$file"
        if ($DryRun) {
            Write-Host ("  would    {0}" -f $dst)
        } else {
            $have = Copy-Exact $abs $dst
            if ($have -ne $entry.sha256) {
                throw "snapshot: '$abs' changed while it was being snapshotted - the copy hashes $have, the entry says $($entry.sha256)."
            }
        }
    }
    $script:SnapEntries.Add([pscustomobject]$entry)
    $null = Save-Snapshot
}

function Complete-Snapshot {
    # The after_* fields are what `status` compares against disk: the snapshot is both
    # the undo and the record of what the apply left behind, so there is one file and
    # one truth rather than two that can disagree. Until this runs the entries carry no
    # after_* at all, which is how an unfinished apply is told from a finished one.
    foreach ($e in $script:SnapEntries) {
        $abs = Resolve-RootPath $e.root $e.rel $e.intent
        if ($DryRun) {
            Add-Member -InputObject $e -NotePropertyName 'after_absent' -NotePropertyValue $true -Force
            Add-Member -InputObject $e -NotePropertyName 'after_sha256' -NotePropertyValue $null -Force
            Add-Member -InputObject $e -NotePropertyName 'after_len'    -NotePropertyValue 0 -Force
            Add-Member -InputObject $e -NotePropertyName 'after_mtime'  -NotePropertyValue $null -Force
            continue
        }
        if (Test-Path -LiteralPath $abs -PathType Leaf) {
            $item = Get-Item -LiteralPath $abs
            Add-Member -InputObject $e -NotePropertyName 'after_absent' -NotePropertyValue $false -Force
            Add-Member -InputObject $e -NotePropertyName 'after_sha256' -NotePropertyValue (Get-Sha $abs) -Force
            Add-Member -InputObject $e -NotePropertyName 'after_len'    -NotePropertyValue $item.Length -Force
            Add-Member -InputObject $e -NotePropertyName 'after_mtime'  -NotePropertyValue $item.LastWriteTimeUtc.ToString('o') -Force
        } else {
            Add-Member -InputObject $e -NotePropertyName 'after_absent' -NotePropertyValue $true -Force
            Add-Member -InputObject $e -NotePropertyName 'after_sha256' -NotePropertyValue $null -Force
            Add-Member -InputObject $e -NotePropertyName 'after_len'    -NotePropertyValue 0 -Force
            Add-Member -InputObject $e -NotePropertyName 'after_mtime'  -NotePropertyValue $null -Force
        }
    }
    if ($DryRun) { Write-Host ("  would    {0}" -f (Join-Path $script:SnapDir 'snapshot.json')) }
    return (Save-Snapshot)
}

#====================================================================================
# Profiles
#====================================================================================

function Get-ProfilePath([string]$name) { return (Join-Path $profilesDir "$name\profile.json") }

function Get-ModState($p) {
    # The state this run will launch into: the profile's, unless -Mod overrode it. Asked
    # by the apply that establishes it and by the probe that decides whether there is
    # anything of ours in the process to instrument.
    if ($Mod) { return $Mod }
    if ($p.mod -and $p.mod.state) { return [string]$p.mod.state }
    return 'on'
}

function Read-GameConfigSet([string]$path) {
    # The in-game graphics selector - upscaler, frame generation, the quality sliders -
    # lives in `Saved\<user>\GameConfig\GameConfig.sav`, not in any ini: a 10-byte marker,
    # a zlib block, the same marker as footer, and JSON inside. This READS it.
    #
    # It is never written, and the reason is in the same blob: beside `gameset` the file
    # carries playedtime, equipment, achievements and the player's location. A profile that
    # pinned this file would put the owner's progress back to whenever it was captured,
    # every time it was applied. So the graphics selection is a precondition a profile
    # asserts, like `manual.hags`, and not a payload it installs.
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    # Find the zlib header rather than trusting an offset: the marker's length is a fact
    # about one build of the game, and 0x78 0x9C is a fact about zlib.
    $at = -1
    for ($i = 0; $i -lt [Math]::Min($bytes.Length - 2, 256); $i++) {
        if ($bytes[$i] -eq 0x78 -and $bytes[$i + 1] -eq 0x9C) { $at = $i; break }
    }
    if ($at -lt 0) { return $null }
    try {
        # DeflateStream is raw deflate, so the two zlib header bytes are skipped; the
        # trailing Adler-32 and the footer marker are simply never read.
        $ms = New-Object System.IO.MemoryStream(,$bytes[($at + 2)..($bytes.Length - 1)])
        $ds = New-Object System.IO.Compression.DeflateStream($ms, [System.IO.Compression.CompressionMode]::Decompress)
        $sr = New-Object System.IO.StreamReader($ds, [System.Text.Encoding]::UTF8)
        $txt = $sr.ReadToEnd()
        $sr.Dispose(); $ds.Dispose(); $ms.Dispose()
    } catch { return $null }
    $i = $txt.IndexOf('"gameset"')
    if ($i -lt 0) { return $null }
    $open = $txt.IndexOf('{', $i)
    $close = $txt.IndexOf('}', $open)
    if ($open -lt 0 -or $close -lt 0) { return $null }
    $set = @{}
    foreach ($m in [regex]::Matches($txt.Substring($open, $close - $open), '"([^"]+)"\s*:\s*"([^"]*)"')) {
        $set[$m.Groups[1].Value] = $m.Groups[2].Value
    }
    return $set
}

function Get-Pinned {
    # `pinned.json` is a repo file, and a profiles directory without one is a legitimate
    # state - a test drives this script against a sandbox of its own. Absence is $null
    # here and a named readback everywhere it matters, never a throw halfway through.
    $path = Join-Path $profilesDir 'pinned.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    return (Read-JsonFile $path)
}

function Get-PinnedUe4ss { $p = Get-Pinned; return $(if ($p) { $p.ue4ss } else { $null }) }

function Get-HwSchMode {
    try {
        $p = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' `
                              -Name 'HwSchMode' -ErrorAction Stop
        return [int]$p.HwSchMode
    } catch { return $null }
}

function Get-HagsState {
    $mode = Get-HwSchMode
    if ($null -eq $mode) { return 'unknown' }
    if ($mode -eq 2) { return 'on' }
    if ($mode -eq 1) { return 'off' }
    return "HwSchMode=$mode"
}

function Read-Profile([string]$name) {
    $path = Get-ProfilePath $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "No such profile: '$path'."
    }
    $p = Read-JsonFile $path

    foreach ($field in (Get-PropertyNames $p)) {
        if ($field -notin $ProfileFields) {
            throw "profile '$name': unknown field '$field' in '$path'."
        }
    }
    if (-not $p.name) { throw "profile '$name': field 'name' is missing in '$path'." }
    if ($p.name -ne $name) { throw "profile '$name': field 'name' says '$($p.name)'." }

    foreach ($field in (Get-PropertyNames $p.mod)) {
        if ($field -notin $ModFields) { throw "profile '$name': unknown field 'mod.$field'." }
    }
    $build = $(if ($p.mod -and $p.mod.build) { [string]$p.mod.build } else { 'keep' })
    if ($build -ne 'keep' -and $build -notmatch '^dist:\d+\.\d+\.\d+$') {
        throw "profile '$name': field 'mod.build' is '$build'; expected 'keep' or 'dist:<x.y.z>'."
    }
    $state = $(if ($p.mod -and $p.mod.state) { [string]$p.mod.state } else { 'on' })
    if ($state -notin @('absent', 'hooks-off', 'off', 'on')) {
        throw "profile '$name': field 'mod.state' is '$state'; expected absent|hooks-off|off|on."
    }
    if ($p.recon -and ([string]$p.recon) -notin @('on', 'absent')) {
        throw "profile '$name': field 'recon' is '$($p.recon)'; expected 'on' or 'absent'."
    }
    $stateMode = $(if ($p.state) { [string]$p.state } else { 'keep' })
    if ($stateMode -notin @('keep', 'fresh')) {
        throw "profile '$name': field 'state' is '$stateMode'; expected 'keep' or 'fresh'."
    }
    if ($p.game_config_expect) {
        # There are two user-id folders on this box and the file sits one level deeper than
        # its folder's name suggests, so the profile names the user or it names nothing: a
        # guess here reads the wrong player's graphics settings.
        foreach ($field in (Get-PropertyNames $p.game_config_expect)) {
            if ($field -notin @('user', 'gameset', 'note')) {
                throw "profile '$name': unknown field 'game_config_expect.$field'."
            }
        }
        if (([string]$p.game_config_expect.user) -notmatch '^\d+$') {
            throw "profile '$name': field 'game_config_expect.user' is '$($p.game_config_expect.user)'; expected a user-id folder name, all digits."
        }
        if ((Get-PropertyNames $p.game_config_expect.gameset).Count -eq 0) {
            throw "profile '$name': 'game_config_expect' names no gameset key, so it asserts nothing."
        }
    }
    # An `expect` is what a run is judged against, so a misspelt field in it is a check
    # that silently cannot fail, and an `expect` that asserts nothing is a profile that
    # can never be contradicted by a launch.
    foreach ($field in (Get-PropertyNames $p.expect)) {
        if ($field -notin $ExpectFields) {
            throw "profile '$name': unknown field 'expect.$field'; expected one of $($ExpectFields -join ', ')."
        }
    }
    if ($p.expect.verdict -and ([string]$p.expect.verdict) -notin @('HEALTHY', 'CRASH', 'any')) {
        throw ("profile '$name': field 'expect.verdict' is '$($p.expect.verdict)'; expected HEALTHY, " +
               "CRASH, or 'any' to record the verdict without asserting it.")
    }
    $assertions = 0
    foreach ($field in @('modules', 'modules_absent', 'log_lines', 'log_lines_absent')) {
        $assertions += (Get-StringList $p.expect $field).Count
    }
    if ($assertions -eq 0) {
        throw "profile '$name': 'expect' asserts nothing, so no launch could ever fail it."
    }

    foreach ($entry in (AsArray $p.payloads)) {
        if (-not $entry.dest) { throw "profile '$name': a 'payloads' entry has no 'dest'." }
        foreach ($field in (Get-PropertyNames $entry)) {
            # `note` is the one free-text field: a profile is a recipe a person reads,
            # and JSON has no comments.
            if ($field -notin @('dest', 'source', 'sha256', 'volatile', 'note', 'tree')) {
                throw "profile '$name': unknown field 'payloads[$($entry.dest)].$field'."
            }
        }
        if ($entry.tree -and $entry.source -and -not $entry.sha256) {
            throw ("profile '$name': payload tree 'payloads[$($entry.dest)]' installs files and " +
                   "names no 'sha256'. A tree's sha256 is an object of relative path -> hash, one " +
                   "entry per file it carries.")
        }
        if ($entry.tree -and $entry.sha256 -and ($entry.sha256 -is [string])) {
            throw ("profile '$name': payload tree 'payloads[$($entry.dest)].sha256' is a single " +
                   "string. A tree hashes every file it carries: { ""<relative path>"": ""<sha256>"" }.")
        }
    }
    return $p
}

function Test-ProfilePreconditions($p, [string]$name) {
    # Everything an apply reads back before it writes anything. `manual.hags` is the one
    # entry with a machine answer; the rest of `manual` is printed as a checklist.
    $problems = New-Object System.Collections.Generic.List[string]

    if ($p.manual -and $p.manual.hags) {
        $want = [string]$p.manual.hags
        $have = Get-HagsState
        if ($have -ne $want) {
            $problems.Add("manual.hags says '$want'; HKLM\...\GraphicsDrivers\HwSchMode reads '$have'.")
        }
    }
    # The in-game graphics selection, read out of GameConfig.sav and never written into it -
    # the same blob carries the owner's progress. A configuration that depends on DLSS or on
    # frame generation says so here and the apply refuses when the game disagrees, instead of
    # measuring whatever the menu happened to be left on.
    if ($p.game_config_expect) {
        $gc = Read-GameConfigSet (Join-Path $saved `
                  (Join-Path ([string]$p.game_config_expect.user) 'GameConfig\GameConfig.sav'))
        if ($null -eq $gc) {
            $problems.Add(("game_config_expect names user '$($p.game_config_expect.user)' and its " +
                           "GameConfig.sav could not be read."))
        } else {
            foreach ($k in (Get-PropertyNames $p.game_config_expect.gameset)) {
                $want = [string]$p.game_config_expect.gameset.$k
                $have = [string]$gc[$k]
                if ($have -ne $want) {
                    $problems.Add(("game_config_expect says gameset.$k = '$want'; the game's own " +
                                   "settings read '$have'. Change it in the game's menu."))
                } else {
                    Write-Host ("  game config    {0,-16} {1}" -f $k, $have) -ForegroundColor DarkGray
                }
            }
        }
    }
    # `running` is no longer a refusal on its own: the apply starts what is not up and
    # stops it again on restore. What IS a refusal is being unable to - a profile that
    # names a process this box cannot start would otherwise fail after the snapshot had
    # opened, which is the one place a precondition exists to be ahead of.
    foreach ($proc in (Get-StringList $p.processes 'running')) {
        if (@(Get-Process -Name $proc -ErrorAction SilentlyContinue).Count -gt 0) { continue }
        $start = Get-ProcessStart $proc
        if (-not $start -or -not $start.path) {
            $problems.Add(("processes.running names '$proc', it is not running, and " +
                           "pinned.json does not say where its exe is."))
        } elseif (-not (Test-Path -LiteralPath $start.path -PathType Leaf)) {
            $problems.Add("processes.running names '$proc' and its exe is not at '$($start.path)'.")
        }
    }
    foreach ($proc in (AsArray $p.processes.absent)) {
        if (@(Get-Process -Name $proc -ErrorAction SilentlyContinue).Count -gt 0) {
            $problems.Add("processes.absent names '$proc' and it is running.")
        }
    }

    if ($problems.Count -eq 0) {
        Write-Host "  preconditions  OK" -ForegroundColor Green
        return
    }
    foreach ($msg in $problems) {
        Write-Host ("  {0}  {1}" -f $(if ($Force) { 'FORCED' } else { 'FAIL  ' }), $msg) `
                   -ForegroundColor $(if ($Force) { 'Yellow' } else { 'Red' })
    }
    if (-not $Force) {
        throw ("profile '$name': $($problems.Count) precondition(s) read back wrong - " +
               ($problems -join ' ') + " Fix them, or pass -Force to proceed anyway.")
    }
}

function Test-ProfilePayloads($p, [string]$name) {
    foreach ($entry in (AsArray $p.payloads)) {
        if ($null -eq $entry.source) { continue }
        $src = Join-Path $payloadRoot ([string]$entry.source)
        if ($entry.tree) {
            # A tree's source is a folder in the store, and its `sha256` is a map of
            # relative path -> hash: one hash over a directory would name an ordering and
            # a traversal rather than the files, and could not say WHICH file drifted.
            if (-not (Test-Path -LiteralPath $src -PathType Container)) {
                throw "profile '$name': payload 'payloads[$($entry.dest)].source' = '$($entry.source)' is a tree and there is no such folder in the store ('$src')."
            }
            $have = @(Get-TreeFiles $src)
            if ($have.Count -eq 0) {
                throw "profile '$name': payload tree '$($entry.source)' is an empty folder; a tree that installs nothing is a profile that states nothing."
            }
            foreach ($rel in $have) {
                $want = $entry.sha256.$rel
                if (-not $want) {
                    throw "profile '$name': payload tree '$($entry.source)' holds '$rel' and 'payloads[$($entry.dest)].sha256' does not name it. Every file a tree installs is hashed in the profile."
                }
                $got = Get-Sha (Join-Path $src $rel)
                if ($got -ne ([string]$want).ToLowerInvariant()) {
                    throw "profile '$name': payload '$($entry.source)\$rel' hashes $got, the profile says $want."
                }
            }
            foreach ($rel in (Get-PropertyNames $entry.sha256)) {
                if ($rel -notin $have) {
                    throw "profile '$name': 'payloads[$($entry.dest)].sha256' names '$rel' and the store's tree '$($entry.source)' does not hold it."
                }
            }
            continue
        }
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
            throw "profile '$name': payload 'payloads[$($entry.dest)].source' = '$($entry.source)' is not in the store ('$src'). Re-download it; sha256 $($entry.sha256)."
        }
        if ($entry.sha256) {
            $have = Get-Sha $src
            if ($have -ne ([string]$entry.sha256).ToLowerInvariant()) {
                throw "profile '$name': payload '$($entry.source)' hashes $have, 'payloads[$($entry.dest)].sha256' says $($entry.sha256)."
            }
        }
    }
}

function Get-DistBuilds {
    # Every packaged build in dist\, with the md5 of its main.dll: the only handle on
    # "which release is installed", since the install itself carries no BUILD_INFO.txt.
    $out = New-Object System.Collections.Generic.List[object]
    $dist = Join-Path $repo 'dist'
    if (-not (Test-Path -LiteralPath $dist -PathType Container)) { return $out }
    foreach ($d in Get-ChildItem -LiteralPath $dist -Directory -Filter 'WuchangMinimap-*') {
        $dll = Join-Path $d.FullName 'ue4ss\Mods\WuchangMinimap\dlls\main.dll'
        if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { continue }
        $out.Add([pscustomobject]@{
            version = $d.Name.Substring('WuchangMinimap-'.Length)
            md5     = (Get-Md5 $dll)
        })
    }
    return $out
}

function Get-DistModDir([string]$build, [string]$name) {
    $ver = $build.Substring(5)
    $dir = Join-Path $repo "dist\WuchangMinimap-$ver\ue4ss\Mods\WuchangMinimap"
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        throw "profile '$name': field 'mod.build' = '$build' but '$dir' is not on disk."
    }
    return $dir
}

#====================================================================================
# apply
#====================================================================================

function Set-ModEnabledFile([string]$label, [bool]$present, [string]$root) {
    # The directory is the root's own: asking the caller for it as well would be a second
    # answer to a question $Roots already answers, and the two could disagree.
    $dir  = $Roots[$root]
    $live = Resolve-RootPath $root 'enabled.txt'
    $off  = Resolve-RootPath $root 'enabled.txt.repro-off'
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        if (-not $present) { return }
        throw "$label : '$dir' does not exist, so its enabled.txt cannot be created."
    }
    # Only a branch that actually moves a file records one: a snapshot entry for a file
    # nobody touched is an undo with nothing to undo.
    if ($present) {
        if (Test-Path -LiteralPath $off) {
            Add-SnapshotEntry $root 'enabled.txt'
            Add-SnapshotEntry $root 'enabled.txt.repro-off'
            Invoke-Rename $off $live "$label : the mod loads again"
        } elseif (-not (Test-Path -LiteralPath $live)) {
            Add-SnapshotEntry $root 'enabled.txt'
            Invoke-WriteText $live '' "$label : UE4SS's opt-in"
        }
    } else {
        if (-not (Test-Path -LiteralPath $live)) { return }
        Add-SnapshotEntry $root 'enabled.txt'
        Add-SnapshotEntry $root 'enabled.txt.repro-off'
        Invoke-Rename $live $off "$label : the mod never starts"
    }
}

function Test-ModRuntimeOutput([string]$rel) {
    # What the MOD wrote while a game ran, as opposed to what a release installed. Pinning
    # a build replaces the release's own files and removes the ones another release would
    # have carried; it does not clear out runtime output that no release ever contained.
    #
    # `navmesh\` is the load-bearing case: those are in-game navmesh dumps, pulled back by
    # `deploy.ps1 -Pull`, and they cannot be re-taken without another capture run. They
    # would be snapshotted and restored like anything else - but the cheapest way not to
    # lose irreplaceable evidence is not to delete it.
    if ($rel -match '^(?i)navmesh([\\/]|$)')      { return $true }
    if ($rel -match '(?i)^wuchang_minimap.*\.txt$') { return $true }
    if ($rel -match '(?i)\.log(\.\d+)?$')         { return $true }
    return $false
}

function Get-EffectiveModKey([string]$playerCfg, [string]$devCfg, [string]$key) {
    # The mod reads the dev file after the player file and lets it override, so the value a
    # launch runs on is the dev one when there is one. One question, one answer, here.
    $dev = Get-ConfigValue $devCfg $key
    if ($null -ne $dev) { return $dev }
    return (Get-ConfigValue $playerCfg $key)
}

function Assert-ModStateOnDisk([string]$modState, [string]$playerCfg, [string]$devCfg) {
    # What the label claims, read back off the files as the mod would read them. A profile
    # whose own mod.config or mod.config_dev moves one of the two keys away from the state
    # it declares produces a cell current.json would mislabel, which is an error and not a
    # warning: the label is the thing every later report quotes.
    $want = @{
        'on'        = @('1', '1')
        'hooks-off' = @('1', '0')
        'off'       = @('0', '1')
        'absent'    = @('0', '0')
    }[$modState]
    $have = @((Get-EffectiveModKey $playerCfg $devCfg 'mod_enabled'),
              (Get-EffectiveModKey $playerCfg $devCfg 'overlay_hooks'))
    if ($have[0] -ne $want[0] -or $have[1] -ne $want[1]) {
        throw ("mod.state '$modState' wants mod_enabled = $($want[0]), overlay_hooks = $($want[1]), " +
               "but the installed configs read back $($have[0]) / $($have[1]) - the dev config overrides the player one. " +
               "current.json would name a cell the disk does not carry.")
    }
    $loaded = Test-Path -LiteralPath (Resolve-RootPath 'mod' 'enabled.txt') -PathType Leaf
    if ($loaded -ne ($modState -ne 'absent')) {
        throw "mod.state '$modState' wants enabled.txt $(if ($modState -eq 'absent') { 'renamed away' } else { 'present' }), and it is $(if ($loaded) { 'present' } else { 'not there' })."
    }
}

function Invoke-Apply([string]$name, $instrument = $null) {
    # `$instrument` is the probe's own dev-config keys. They are written HERE, inside the
    # snapshot, rather than by the probe afterwards: the undo record is written before the
    # damage, and a snapshot that is already marked complete cannot take another entry.
    Write-Host ""
    Write-Host ("apply {0}" -f $name) -ForegroundColor Cyan

    # 1. the game must be closed, and no -Force covers it.
    Assert-GameClosed 'apply'

    # 2. the vault, always first.
    $vaultStamp = Invoke-Vault

    # 3. anything already applied comes off first, proven, before anything goes on - and an
    #    apply that did not finish is undone on exactly the same terms as one that did.
    if (Test-Path -LiteralPath $currentJson -PathType Leaf) {
        $cur = Read-JsonFile $currentJson
        Write-Host ""
        Write-Host ("  applied now    {0} ({1}){2} - undoing it first" -f $cur.profile, $cur.snapshot,
                    $(if ($cur.complete -eq $false) { ', an apply that did not finish' } else { '' }))
        Invoke-Restore -Quiet
        if (-not $DryRun -and (Test-Path -LiteralPath $currentJson -PathType Leaf)) {
            throw "The restore of '$($cur.profile)' could not be proven exact; not applying '$name' on top of it."
        }
    }

    # 4. resolve and validate.
    Write-Host ""
    Write-Host "  Profile" -ForegroundColor Cyan
    $p = Read-Profile $name
    $profileSha = Get-Sha (Get-ProfilePath $name)
    Write-Host ("  profile.json   {0}" -f $profileSha)
    if ($p.description) { Write-Host ("  description    {0}" -f $p.description) }
    $build     = $(if ($p.mod -and $p.mod.build) { [string]$p.mod.build } else { 'keep' })
    $modState  = Get-ModState $p
    $stateMode = $(if ($p.state) { [string]$p.state } else { 'keep' })

    # Everything that can be known to be wrong is known before a single byte moves: a
    # profile half-applied because its build was missing is worse than one not applied.
    Test-ProfilePayloads $p $name
    $distDir = $null
    if ($build -ne 'keep') { $distDir = Get-DistModDir $build $name }
    Test-ProfilePreconditions $p $name
    foreach ($k in (Get-PropertyNames $p.manual)) {
        if ($k -eq 'hags') { continue }
        Write-Host ("  manual         {0,-16} {1}" -f $k, $p.manual.$k) -ForegroundColor DarkGray
    }
    if ($Mod) { Write-Host ("  -Mod           {0} (overrides mod.state)" -f $Mod) -ForegroundColor Yellow }

    # 5. the undo record, before the first byte moves. current.json says `complete: false`
    #    until the apply has finished; see the header.
    $stamp   = New-FreeStamp $snapshotRoot "-$name"
    $snapDir = Join-Path $snapshotRoot "$stamp-$name"
    Start-Snapshot $snapDir $name $stamp
    $current = [pscustomobject][ordered]@{
        profile        = $name
        profile_sha256 = $profileSha
        snapshot       = $stamp
        vault          = $vaultStamp
        mod_state      = $modState
        mod_build      = $build
        state          = $stateMode
        host           = $env:COMPUTERNAME
        applied_utc    = [DateTime]::UtcNow.ToString('o')
        complete       = $false
        mod_loaded     = $null
        main_dll_md5   = $null
    }
    if (-not $DryRun) { Write-JsonFile $currentJson $current }

    # 6. act, file by file. Every entry is persisted as it is taken.
    Write-Host ""
    Write-Host "  Changes" -ForegroundColor Cyan

    # -- processes -----------------------------------------------------------------
    # Before the files, because a process this apply starts is recorded in `current.json`
    # the same way the files are, and an apply that dies after starting one must still
    # leave a record that says so.
    $procRecords = Start-RequiredProcesses $p
    Add-Member -InputObject $current -NotePropertyName 'processes' -NotePropertyValue $procRecords -Force
    if (-not $DryRun) { Write-JsonFile $currentJson $current }

    # -- payloads ------------------------------------------------------------------
    foreach ($entry in (AsArray $p.payloads)) {
        $rel = [string]$entry.dest
        $dst = Resolve-RootPath 'bin' $rel

        if ($entry.tree) {
            # A TREE. The snapshot format needs nothing new for it - a tree is a set of
            # files, and "this file was not here before" already means "delete it on the
            # way back". What a tree adds is the enumeration, and the directories the
            # apply has to create, which the files alone would leave standing empty.
            $under = @(Get-TreeFiles $dst)
            if ($null -eq $entry.source) {
                # Stated ABSENT. Every file that is there is snapshotted before the tree
                # goes, so the restore puts back a folder nobody had to list by hand -
                # which is the whole reason a directory is declarable at all.
                foreach ($sub in $under) {
                    Add-SnapshotEntry 'bin' (Join-Path $rel $sub) ([bool]$entry.volatile)
                }
                if (Test-Path -LiteralPath $dst) {
                    Invoke-Delete $dst ("payload: this profile states the tree absent ({0} file(s))" -f
                                        $under.Count)
                }
                continue
            }
            # INSTALLED. A tree REPLACES the directory, as a pinned build replaces the mod
            # directory: a file standing there that the source does not carry is not part
            # of this configuration, so it is snapshotted and removed rather than left to
            # load beside ours.
            $src   = Join-Path $payloadRoot ([string]$entry.source)
            $files = @(Get-TreeFiles $src)
            foreach ($sub in $under) {
                if ($sub -notin $files) {
                    Add-SnapshotEntry 'bin' (Join-Path $rel $sub) ([bool]$entry.volatile)
                    Invoke-Delete (Join-Path $dst $sub) "payload: the tree does not carry it"
                }
            }
            foreach ($sub in $files) {
                $subRel = Join-Path $rel $sub
                Add-SnapshotDirs 'bin' $subRel
                Add-SnapshotEntry 'bin' $subRel ([bool]$entry.volatile)
                Invoke-Copy (Join-Path $src $sub) (Join-Path $dst $sub) $null
            }
            Write-Host ("         payload tree: {0}, {1} file(s)" -f $entry.source, $files.Count) `
                       -ForegroundColor DarkGray
            continue
        }

        Add-SnapshotDirs 'bin' $rel
        Add-SnapshotEntry 'bin' $rel ([bool]$entry.volatile)
        if ($null -eq $entry.source) {
            if (Test-Path -LiteralPath $dst -PathType Leaf) {
                Invoke-Delete $dst "payload: this profile states it absent"
            }
        } else {
            # Test-ProfilePayloads has already proven the source against the declared
            # sha256 and Invoke-Copy proves the destination against the source, so the
            # destination is proven to be the declared file without hashing it twice.
            $src = Join-Path $payloadRoot ([string]$entry.source)
            Invoke-Copy $src $dst ("payload: " + $entry.source)
        }
    }

    # -- mod.build -----------------------------------------------------------------
    # Pinning a build REPLACES the mod directory with that release, it does not copy over
    # the top of the one that is there. Anything the install carries that the release does
    # not - a newer marker file, a map the release never shipped, the dev config no release
    # ships at all - is not part of that build, and a cell that ran with it is not that
    # build. Every removal is snapshotted like every replacement, so the restore puts the
    # whole directory back exactly, dev config included.
    if ($build -ne 'keep') {
        $shipped = @{}
        foreach ($f in Get-ChildItem -LiteralPath $distDir -Recurse -File) {
            $rel = (Get-RelativeTo $distDir $f.FullName)
            $shipped[$rel.ToLowerInvariant()] = $true
            Add-SnapshotEntry 'mod' $rel
            Invoke-Copy $f.FullName (Resolve-RootPath 'mod' $rel) $null
        }
        $extra = 0
        $kept  = 0
        foreach ($f in Get-ChildItem -LiteralPath $modDir -Recurse -File) {
            $rel = (Get-RelativeTo $modDir $f.FullName)
            if ($shipped.ContainsKey($rel.ToLowerInvariant())) { continue }
            if (Test-ModRuntimeOutput $rel) { $kept++; continue }
            Add-SnapshotEntry 'mod' $rel
            Invoke-Delete (Resolve-RootPath 'mod' $rel) "mod.build $build : not part of that release"
            $extra++
        }
        Write-Host ("  mod.build      {0} - {1} file(s) of the release, {2} removed, {3} runtime output kept" -f
                    $build, $shipped.Count, $extra, $kept)
    }

    # -- mod.state -----------------------------------------------------------------
    # The spec's table names one key per state; a state that left the other where the
    # previous profile put it would be a cell current.json mislabels, so all four pin both
    # keys and the four pairs are distinct. `absent` pins 0/0 - a pair no loaded cell uses,
    # so even the configs alone cannot be read as another state - and is the only state
    # that renames enabled.txt away, which is what actually stops the DLL loading.
    $playerCfg = Resolve-RootPath 'mod' 'config_wuchang_minimap.txt'
    $devCfg    = Resolve-RootPath 'mod' 'config_wuchang_minimap_dev.txt'
    $stateKeys = [ordered]@{}
    switch ($modState) {
        'on'        { $stateKeys['mod_enabled'] = '1'; $stateKeys['overlay_hooks'] = '1' }
        'hooks-off' { $stateKeys['mod_enabled'] = '1'; $stateKeys['overlay_hooks'] = '0' }
        'off'       { $stateKeys['mod_enabled'] = '0'; $stateKeys['overlay_hooks'] = '1' }
        'absent'    { $stateKeys['mod_enabled'] = '0'; $stateKeys['overlay_hooks'] = '0' }
    }
    Set-ModEnabledFile 'mod.state' ($modState -ne 'absent') 'mod'
    Set-ConfigKeys 'mod' 'config_wuchang_minimap.txt' ([pscustomobject]$stateKeys) "mod.state = $modState"
    Write-Host ("  mod.state      {0}" -f $modState)

    # -- mod.config / mod.config_dev, on top of the state ---------------------------
    if ($p.mod -and (Get-PropertyNames $p.mod.config).Count -gt 0) {
        Set-ConfigKeys 'mod' 'config_wuchang_minimap.txt' $p.mod.config 'mod.config'
    }
    if ($p.mod -and (Get-PropertyNames $p.mod.config_dev).Count -gt 0) {
        Set-ConfigKeys 'mod' 'config_wuchang_minimap_dev.txt' $p.mod.config_dev 'mod.config_dev'
    }

    # -- the probe's instrument, last, so it wins over the profile ------------------
    # Handed in already decided: whether a probe's keys are written at all is the probe's
    # question, and this is only the writer of them.
    if ($instrument -and (Get-PropertyNames $instrument).Count -gt 0) {
        Set-ConfigKeys 'mod' 'config_wuchang_minimap_dev.txt' $instrument "probe $Probe"
    }

    # -- recon ---------------------------------------------------------------------
    if ($p.recon) {
        Set-ModEnabledFile 'recon' (([string]$p.recon) -eq 'on') 'recon'
        Write-Host ("  recon          {0}" -f $p.recon)
    }

    # -- state ---------------------------------------------------------------------
    if ($stateMode -eq 'fresh') {
        if (Test-Path -LiteralPath $stateDir -PathType Container) {
            foreach ($f in Get-ChildItem -LiteralPath $stateDir -Recurse -File) {
                if ($f.Name -like "$LogGlob*") { continue }
                $rel = (Get-RelativeTo $stateDir $f.FullName)
                Add-SnapshotEntry 'state' $rel
                Invoke-Delete (Resolve-RootPath 'state' $rel) "state: fresh"
            }
        }
        Write-Host "  state          fresh"
    } else {
        Write-Host "  state          keep"
    }

    # -- game_settings -------------------------------------------------------------
    # The ini lands in Saved\Config\Windows, which no guard covers. `GameConfig.sav` is in
    # the guarded area and the only writer of it inside an apply is this block, which says
    # so to Resolve-RootPath; the save slot beside it is refused for every intent but
    # saves-restore's.
    if ((Get-PropertyNames $p.game_settings).Count -gt 0) {
        Set-ConfigKeys 'saved' 'Config\Windows\GameUserSettings.ini' $p.game_settings 'game_settings'
    }

    # 7. the apply is finished: the after-state, what a later `run` quotes, and complete.
    $manifest = Complete-Snapshot
    $dllPath  = Resolve-RootPath 'mod' 'dlls\main.dll'
    if (-not $DryRun) {
        if (Test-Path -LiteralPath $dllPath -PathType Leaf) { $current.main_dll_md5 = Get-Md5 $dllPath }
        # An md5 is the identity of a FILE; whether UE4SS loads it is a different fact, and
        # `rules/in-game-verification.md` makes the md5 the first line of every report. A
        # `stock` cell installs no DLL into the process at all, and a report that opened
        # with this md5 alone would name a build that was never in it.
        $current.mod_loaded = ((Test-Path -LiteralPath (Resolve-RootPath 'mod' 'enabled.txt') -PathType Leaf) -and
                               (Test-Path -LiteralPath $dllPath -PathType Leaf))
        $current.complete = $true
        Write-JsonFile $currentJson $current
        Assert-ModStateOnDisk $modState $playerCfg $devCfg
    } else {
        Write-Host ("  would    {0}" -f $currentJson)
    }

    Write-Host ""
    if ($DryRun) {
        Write-Host ("Dry run: '{0}' would touch {1} file(s); nothing was changed." -f
                    $name, $manifest.files.Count) -ForegroundColor Green
    } else {
        Write-Host ("Applied '{0}' - snapshot {1}, {2} file(s) recorded, vault {3}." -f
                    $name, $stamp, $manifest.files.Count, $vaultStamp) -ForegroundColor Green
        Write-Host ("main.dll md5   {0}" -f $(if ($current.main_dll_md5) { $current.main_dll_md5 } else { 'no main.dll installed' }))
        Write-Host ("mod loaded     {0}" -f $(if ($current.mod_loaded) { 'yes - enabled.txt is present, UE4SS loads the DLL' }
                                              else { 'NO - start_mod is never called in this cell' }))
    }
}

#====================================================================================
# restore
#====================================================================================

function Invoke-Restore {
    param([switch]$Quiet)

    # The refusal is the function's own, not its caller's: printing and permission are two
    # questions, and `-Quiet` answers only the first.
    Assert-GameClosed 'restore'
    if (-not $Quiet) {
        Write-Host ""
        Write-Host "restore" -ForegroundColor Cyan
        $null = Invoke-Vault
    }

    if (-not (Test-Path -LiteralPath $currentJson -PathType Leaf)) {
        Write-Host "  nothing applied - no current.json in the store." -ForegroundColor Yellow
        return
    }
    $cur     = Read-JsonFile $currentJson
    $snapDir = Join-Path $snapshotRoot ("{0}-{1}" -f $cur.snapshot, $cur.profile)
    $snap    = Read-JsonFile (Join-Path $snapDir 'snapshot.json')
    $entries = AsArray $snap.files

    Write-Host ""
    Write-Host ("  undoing        {0} ({1}), {2} file(s)" -f $cur.profile, $cur.snapshot, $entries.Count)
    # A record that never turned complete is an apply that died part-way through. It is
    # undone exactly like a finished one - it names every file already replaced - and the
    # difference is said out loud, because "restored" would otherwise describe a tree that
    # was never fully changed in the first place.
    if ($cur.complete -eq $false) {
        Write-Host ("  incomplete     the apply of '{0}' did not finish; these are the files it had already replaced." -f $cur.profile) `
                   -ForegroundColor Yellow
    }

    # Reverse order: a rename chain is undone the way it was made.
    for ($i = $entries.Count - 1; $i -ge 0; $i--) {
        $e   = $entries[$i]
        $abs = Resolve-RootPath $e.root $e.rel $e.intent
        if ($e.absent) {
            if (Test-Path -LiteralPath $abs -PathType Leaf) {
                Invoke-Delete $abs "restore: it did not exist before"
            } elseif (Test-Path -LiteralPath $abs) {
                # Nothing here removes a tree it did not create, and an apply never makes
                # one: a directory standing where a file was recorded is somebody else's,
                # and the proof pass below fails on it by name.
                Write-Host ("  SKIP   {0}" -f $abs) -ForegroundColor Red
                Write-Host "         a directory stands where a file was recorded; restore removes no tree it did not create." -ForegroundColor Red
            }
            continue
        }
        $src = Join-Path $snapDir "files\$($e.file)"
        if (-not $DryRun -and -not (Test-Path -LiteralPath $src -PathType Leaf)) {
            throw "restore: the snapshot copy '$src' for '$abs' is missing; current.json is left in place."
        }
        Invoke-Copy $src $abs $null
        if (-not $DryRun -and $e.mtime) {
            # See the header: the mod's 1 Hz watch hashes both config timestamps, so
            # "back to where it started" has to include LastWriteTimeUtc.
            (Get-Item -LiteralPath $abs).LastWriteTimeUtc = [DateTime]::Parse($e.mtime).ToUniversalTime()
        }
    }

    # The processes this apply started, before the files: a restore that dies half-way has
    # at least put the box's process list back, and nothing below depends on them being up.
    $leftRunning = Stop-StartedProcesses $cur.processes

    # The directories the apply created, deepest first - the list was recorded outermost
    # first. Each one goes only if it is EMPTY: a folder holding anything at all is holding
    # something this restore did not put there, and removing it would be removing somebody
    # else's file under cover of an undo.
    $dirs = AsArray $snap.dirs
    for ($i = $dirs.Count - 1; $i -ge 0; $i--) {
        $d = [string]$dirs[$i].path
        if (-not (Test-Path -LiteralPath $d -PathType Container)) { continue }
        if (@(Get-ChildItem -LiteralPath $d -Force).Count -gt 0) {
            Write-Host ("  KEEP   {0}" -f $d) -ForegroundColor Yellow
            Write-Host "         the apply created this directory and it is not empty; it stays." -ForegroundColor Yellow
            continue
        }
        Invoke-Delete $d "restore: the apply created this directory"
    }

    if ($DryRun) {
        Write-Host ""
        Write-Host ("Dry run: restore would put back {0} file(s) and remove {1} created directory(ies); nothing was changed." -f
                    $entries.Count, $dirs.Count) -ForegroundColor Green
        return
    }

    # Prove it. One mismatch is a failure, and current.json stays where it is.
    $bad = New-Object System.Collections.Generic.List[string]
    foreach ($e in $entries) {
        $abs = Resolve-RootPath $e.root $e.rel $e.intent
        if ($e.absent) {
            if (Test-Path -LiteralPath $abs) { $bad.Add("$abs : should be absent, still exists") }
            continue
        }
        if (-not (Test-Path -LiteralPath $abs -PathType Leaf)) {
            $bad.Add("$abs : expected $($e.sha256), file is missing")
            continue
        }
        $have = Get-Sha $abs
        if ($have -ne $e.sha256) { $bad.Add("$abs : expected $($e.sha256), got $have") }
    }
    # A directory the apply created and the restore could not remove is a residue, and a
    # residue that nothing states is how "byte-identical to where it started" quietly stops
    # being true - the fingerprint counts files and would never see it.
    foreach ($d in $dirs) {
        if (Test-Path -LiteralPath ([string]$d.path) -PathType Container) {
            $bad.Add("$($d.path) : the apply created this directory and it is still here")
        }
    }
    if ($bad.Count -gt 0) {
        foreach ($msg in $bad) { Write-Host ("  FAIL  {0}" -f $msg) -ForegroundColor Red }
        throw "restore could not be proven exact: $($bad.Count) of $($entries.Count) file(s) and $($dirs.Count) directory(ies) disagree. current.json is left in place."
    }
    Write-Host ("  proven         {0} file(s) match the snapshot byte for byte{1}" -f $entries.Count,
                $(if ($dirs.Count -gt 0) { ", and {0} created directory(ies) are gone" -f $dirs.Count }
                  else { '' })) -ForegroundColor Green
    Remove-Item -LiteralPath $currentJson -Force

    # The play state, against the vault taken at the start of the matching apply.
    if ($cur.vault -and (Test-Path -LiteralPath (Join-Path $vaultRoot "$($cur.vault)\vault.json"))) {
        $diffs = @(Compare-AgainstVault ([string]$cur.vault))
        if ($diffs.Count -eq 0) {
            Write-Host ("  play state     unchanged since vault {0}" -f $cur.vault) -ForegroundColor Green
        } else {
            Write-Host ("  play state     {0} change(s) since vault {1}:" -f $diffs.Count, $cur.vault) `
                       -ForegroundColor Yellow
            foreach ($d in $diffs) { Write-Host ("                 {0}" -f $d.text) -ForegroundColor Yellow }
        }
    }
    Write-Host ""
    Write-Host ("Restored '{0}' - {1} file(s){2}." -f $cur.profile, $entries.Count,
                $(if ($cur.complete -eq $false) { ', from an apply that did not finish' } else { '' })) `
               -ForegroundColor Green
    Set-ProcessNotice $leftRunning
}

#====================================================================================
# saves-restore
#====================================================================================

function Get-SavedFileKind([string]$rel) {
    # What a file under `saved` actually is. The vault holds four kinds of thing and only
    # one of them is a save.
    if ($rel -match '(^|[\\/])GameSlots([\\/]|$)')        { return 'save slot' }
    if ($rel -match '(^|[\\/])GameConfig([\\/]|$|\.)')    { return 'GameConfig' }
    if ($rel -match '(?i)^Config[\\/]Windows[\\/].+\.ini$') { return 'game ini' }
    return 'other'
}

function Get-VdfBlockAt([string]$text, [int]$i) {
    # The braced block that belongs to the token at $i, or $null when the token is not a
    # section header. An app id appears many times in localconfig.vdf - in licence blobs,
    # in playtime lists - and only the occurrence followed by '{' is the app's settings.
    # Steam puts the brace on the line after the key, so whitespace and newlines are
    # skipped and the first thing that is not whitespace decides: '{' is a section, a quote
    # is a key/value pair - the shape the licence blobs take - and anything else is neither.
    $j = $i
    while ($j -lt $text.Length -and [char]::IsWhiteSpace($text[$j])) { $j++ }
    if ($j -ge $text.Length -or $text[$j] -ne '{') { return $null }
    $depth = 0
    for ($k = $j; $k -lt $text.Length; $k++) {
        if ($text[$k] -eq '{') { $depth++ }
        elseif ($text[$k] -eq '}') {
            $depth--
            if ($depth -eq 0) { return $text.Substring($j, $k - $j + 1) }
        }
    }
    return $null
}

function Get-SteamCloudSetting {
    # The app id off the appmanifest beside the install, then EVERY occurrence of that id
    # in each user's localconfig.vdf - the first one on this box is a licence blob with no
    # cloud setting in it, and a check that reads that one can never fail. Returns what was
    # actually found plus `off`, which is true only when a setting was positively read as
    # off; anything else - not stated, not readable, an exception - is not off, and the
    # caller warns. Both reads, never a write, and Steam itself is not touched.
    $findings = New-Object System.Collections.Generic.List[string]
    try {
        $steamapps = Split-Path -Parent (Split-Path -Parent $GameRoot)
        $leaf      = Split-Path -Leaf $GameRoot
        $appId     = $null
        foreach ($acf in Get-ChildItem -LiteralPath $steamapps -Filter 'appmanifest_*.acf' -File -ErrorAction Stop) {
            $text = [System.IO.File]::ReadAllText($acf.FullName)
            if ($text -match '"installdir"\s*"([^"]+)"' -and $Matches[1] -eq $leaf) {
                if ($acf.Name -match 'appmanifest_(\d+)\.acf') { $appId = $Matches[1] }
                break
            }
        }
        if (-not $appId) {
            return [pscustomobject]@{ off = $false; text = 'not readable - no appmanifest matched the install folder; assume it may be on' }
        }

        $steamPath = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name 'SteamPath' -ErrorAction Stop).SteamPath
        $token = '"' + $appId + '"'
        foreach ($cfg in Get-ChildItem -LiteralPath (Join-Path $steamPath 'userdata') -Directory -ErrorAction Stop) {
            $local = Join-Path $cfg.FullName 'config\localconfig.vdf'
            if (-not (Test-Path -LiteralPath $local -PathType Leaf)) { continue }
            $text = [System.IO.File]::ReadAllText($local)
            $i = $text.IndexOf($token)
            while ($i -ge 0) {
                $block = Get-VdfBlockAt $text ($i + $token.Length)
                if ($block) {
                    if ($block -match '"cloudenabled"\s*"(\d)"') {
                        if ($Matches[1] -eq '0') {
                            return [pscustomobject]@{ off = $true; text = "off (app $appId, user $($cfg.Name), cloudenabled = 0)" }
                        }
                        return [pscustomobject]@{ off = $false; text = "ON (app $appId, user $($cfg.Name), cloudenabled = $($Matches[1]))" }
                    }
                    # Steam writes `cloudenabled` only once the per-app toggle has been
                    # moved, so its absence is not "off". `last_sync_state` is the nearest
                    # real evidence, and it is still not the setting.
                    if ($block -match '"last_sync_state"\s*"([^"]+)"') {
                        $findings.Add("user $($cfg.Name), last_sync_state = $($Matches[1])")
                    }
                }
                $i = $text.IndexOf($token, $i + $token.Length)
            }
        }
        if ($findings.Count -gt 0) {
            return [pscustomobject]@{
                off  = $false
                text = "not stated (app $appId; " + ($findings -join '; ') + ") - assume it may be on"
            }
        }
        return [pscustomobject]@{
            off  = $false
            text = "not readable - app $appId appears in no localconfig.vdf block; assume it may be on"
        }
    } catch {
        return [pscustomobject]@{ off = $false; text = "not readable - $($_.Exception.Message); assume it may be on" }
    }
}

function Invoke-SavesRestore([string]$stamp) {
    Write-Host ""
    Write-Host ("saves-restore {0}" -f $stamp) -ForegroundColor Cyan
    Assert-GameClosed 'saves-restore'

    $dir = Join-Path $vaultRoot $stamp
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'vault.json') -PathType Leaf)) {
        throw "No such vault stamp: '$dir'."
    }

    $cloud = Get-SteamCloudSetting
    Write-Host ("  steam cloud    {0}" -f $cloud.text)
    if (-not $cloud.off) {
        Write-Host "  WARNING        Steam Cloud was not positively read as off for this app. Close Steam before restoring, or the cloud copy can put the old file back." `
                   -ForegroundColor Yellow
    }

    # The thing being replaced is itself vaulted first.
    $null = Invoke-Vault

    $manifest = Read-JsonFile (Join-Path $dir 'vault.json')
    $entries  = @((AsArray $manifest.files) | Where-Object { $_.root -eq 'saved' })

    # Everything under `saved` is put back, which is right - the game's ini files and its
    # own `GameConfig.sav` are as much of a configuration as the slot is. What is wrong is
    # calling all of it "save files", so it is counted by what it actually is.
    $kinds = [ordered]@{ 'save slot' = 0; 'GameConfig' = 0; 'game ini' = 0; 'other' = 0 }
    foreach ($e in $entries) { $kinds[(Get-SavedFileKind ([string]$e.rel))] += 1 }
    Write-Host ""
    Write-Host ("  replacing      {0} file(s) under '{1}' from {2}:" -f $entries.Count, $saved, $stamp)
    foreach ($k in $kinds.Keys) {
        if ($kinds[$k] -gt 0) { Write-Host ("                 {0,-11} {1}" -f $k, $kinds[$k]) }
    }

    # A restore that silently leaves a newer file standing is not the stamp's state. The
    # vault is not narrowed to fix that - it is said.
    $covered = @{}
    foreach ($e in $entries) { $covered[([string]$e.rel).ToLowerInvariant()] = $true }
    $extra = @(Get-PlayStateFiles | Where-Object { $_.root -eq 'saved' -and
                                                   -not $covered.ContainsKey($_.rel.ToLowerInvariant()) })
    if ($extra.Count -eq 0) {
        Write-Host "  coverage       the stamp carries every file that is live under 'saved'"
    } else {
        Write-Host ("  coverage       {0} live file(s) this stamp does NOT carry; they are left exactly as they are:" -f $extra.Count) `
                   -ForegroundColor Yellow
        foreach ($f in ($extra | Sort-Object rel)) {
            Write-Host ("                 {0}  ({1})" -f $f.rel, (Get-SavedFileKind $f.rel)) -ForegroundColor Yellow
        }
    }

    foreach ($e in $entries) {
        $dst = Resolve-RootPath 'saved' ([string]$e.rel) 'save_area'
        $src = Join-Path $dir ("files\{0}\{1}" -f $e.root, $e.rel)
        if (-not (Test-Path -LiteralPath $src -PathType Leaf)) {
            throw "saves-restore: the vaulted copy '$src' is missing."
        }
        # The vaulted bytes are proven against the manifest before they are put anywhere:
        # a corrupted vault copy must not reach a save folder at all.
        $vaulted = Get-Sha $src
        if ($vaulted -ne $e.sha256) {
            throw "saves-restore: the vaulted copy '$src' hashes $vaulted, vault.json says $($e.sha256). This vault cannot be trusted; nothing was written."
        }
        $was = 'absent'
        if (Test-Path -LiteralPath $dst -PathType Leaf) { $was = Get-Sha $dst }
        Write-Host ("  {0,-6} {1}" -f $(if ($DryRun) { 'would' } else { 'write' }), $dst)
        Write-Host ("         was {0}" -f $was) -ForegroundColor DarkGray
        Write-Host ("         now {0}" -f $e.sha256) -ForegroundColor DarkGray
        if ($DryRun) { continue }
        $null = Copy-Exact $src $dst
        if ($e.mtime) {
            (Get-Item -LiteralPath $dst).LastWriteTimeUtc = [DateTime]::Parse($e.mtime).ToUniversalTime()
        }
    }

    Write-Host ""
    if ($DryRun) {
        Write-Host ("Dry run: {0} file(s) under 'saved' would be replaced from {1}." -f $entries.Count, $stamp) -ForegroundColor Green
    } else {
        Write-Host ("Restored {0} file(s) under 'saved' from {1}, each verified by sha256." -f $entries.Count, $stamp) -ForegroundColor Green
    }
}

#====================================================================================
# status
#====================================================================================

function Invoke-Status {
    Write-Host ""
    Write-Host "Paths" -ForegroundColor Cyan
    Write-Host ("  game           {0}" -f $GameRoot)
    Write-Host ("  store          {0}" -f $Store)
    Write-Host ("  state dir      {0}" -f $stateDir)
    Write-Host ("  saved          {0}" -f $saved)

    Write-Host ""
    Write-Host "Applied" -ForegroundColor Cyan
    if (-not (Test-Path -LiteralPath $currentJson -PathType Leaf)) {
        Write-Host "  profile        (none - no current.json in the store)"
    } else {
        $cur = Read-JsonFile $currentJson
        Write-Host ("  profile        {0}" -f $cur.profile)
        Write-Host ("  snapshot       {0}" -f $cur.snapshot)
        Write-Host ("  vault          {0}" -f $cur.vault)
        Write-Host ("  mod state      {0}   build {1}   state {2}" -f $cur.mod_state, $cur.mod_build, $cur.state)
        Write-Host ("  main.dll md5   {0}   mod loaded {1}" -f
                    $(if ($cur.main_dll_md5) { $cur.main_dll_md5 } else { '(none recorded)' }),
                    $(if ($null -eq $cur.mod_loaded) { '(not recorded)' } elseif ($cur.mod_loaded) { 'yes' } else { 'NO' }))
        Write-Host ("  applied        {0} on {1}" -f $cur.applied_utc, $cur.host)
        $snapFile = Join-Path $snapshotRoot ("{0}-{1}\snapshot.json" -f $cur.snapshot, $cur.profile)
        if (-not (Test-Path -LiteralPath $snapFile -PathType Leaf)) {
            Write-Host ("  DRIFT          the snapshot '{0}' is gone - nothing can undo this apply." -f $snapFile) -ForegroundColor Red
        } elseif ($cur.complete -eq $false) {
            # An apply that never finished has no after-state to compare disk against; what
            # it has is a list of the files it already replaced, and a restore to run.
            $snap = Read-JsonFile $snapFile
            Write-Host ("  INCOMPLETE     the apply of '{0}' did not finish. {1} file(s) were already replaced and are recorded; run 'restore' to put them back." -f
                        $cur.profile, (AsArray $snap.files).Count) -ForegroundColor Red
            foreach ($e in (AsArray $snap.files)) {
                Write-Host ("                 {0}\{1}" -f $e.root, $e.rel) -ForegroundColor Red
            }
        } else {
            $snap  = Read-JsonFile $snapFile
            $drift = New-Object System.Collections.Generic.List[string]
            $info  = New-Object System.Collections.Generic.List[string]
            foreach ($e in (AsArray $snap.files)) {
                $abs = Join-Path $Roots[[string]$e.root] $e.rel
                $exists = Test-Path -LiteralPath $abs -PathType Leaf
                # Plain assignment, not $(if ...): a subexpression enumerates the list
                # and hands back its elements instead of the list itself.
                $bucket = $drift
                if ($e.volatile) { $bucket = $info }
                if ($e.after_absent) {
                    if ($exists) { $bucket.Add("$abs : should be absent, exists") }
                    continue
                }
                if (-not $exists) { $bucket.Add("$abs : missing"); continue }
                if ((Get-Sha $abs) -ne $e.after_sha256) { $bucket.Add("$abs : edited since the apply") }
            }
            if ($drift.Count -eq 0) {
                Write-Host ("  on disk        all {0} file(s) still as applied" -f (AsArray $snap.files).Count) -ForegroundColor Green
            } else {
                Write-Host ("  DRIFT          {0} file(s) changed since the apply:" -f $drift.Count) -ForegroundColor Yellow
                foreach ($d in $drift) { Write-Host ("                 {0}" -f $d) -ForegroundColor Yellow }
            }
            # A volatile payload is one the configuration itself rewrites while the game
            # runs - ReShade.ini is the case this exists for. Reported, never a failure.
            foreach ($d in $info) { Write-Host ("  INFO           volatile: {0}" -f $d) }
        }
    }

    Write-Host ""
    Write-Host "Installed mod" -ForegroundColor Cyan
    $dll = Join-Path $modDir 'dlls\main.dll'
    if (Test-Path -LiteralPath $dll -PathType Leaf) {
        Write-Host ("  main.dll md5   {0}" -f (Get-Md5 $dll))
        Write-Host ("  main.dll       {0}" -f $dll) -ForegroundColor DarkGray
    } else {
        Write-Host ("  main.dll       not installed ({0})" -f $dll) -ForegroundColor Yellow
    }
    # The installed mod folder carries no BUILD_INFO.txt - that file sits at the ROOT of
    # a dist package, not inside ue4ss\Mods\WuchangMinimap\ - so the installed build is
    # identified the only way it can be: the md5 of the DLL against the dist builds'.
    if (Test-Path -LiteralPath $dll -PathType Leaf) {
        $md5 = Get-Md5 $dll
        $match = $null
        foreach ($d in (Get-DistBuilds)) {
            if ($d.md5 -eq $md5) { $match = $d; break }
        }
        if ($match) {
            Write-Host ("  build          {0}  (dist\WuchangMinimap-{0})" -f $match.version)
        } else {
            Write-Host "  build          local build, not a release (md5 matches no dist\WuchangMinimap-*)"
        }
    }
    $enabled = Join-Path $modDir 'enabled.txt'
    $enabledOff = Join-Path $modDir 'enabled.txt.repro-off'
    if (Test-Path -LiteralPath $enabled -PathType Leaf) {
        Write-Host "  enabled.txt    present"
    } elseif (Test-Path -LiteralPath $enabledOff -PathType Leaf) {
        Write-Host "  enabled.txt    renamed to enabled.txt.repro-off (mod state 'absent')" -ForegroundColor Yellow
    } else {
        Write-Host "  enabled.txt    absent" -ForegroundColor Yellow
    }
    foreach ($pair in @(@('player', 'config_wuchang_minimap.txt'), @('dev', 'config_wuchang_minimap_dev.txt'))) {
        $cfg = Join-Path $modDir $pair[1]
        if (-not (Test-Path -LiteralPath $cfg -PathType Leaf)) {
            Write-Host ("  {0,-14} (no {1})" -f $pair[0], $pair[1])
            continue
        }
        Write-Host ("  {0,-14} mod_enabled = {1}   overlay_hooks = {2}" -f
                    $pair[0],
                    $(if ($null -ne (Get-ConfigValue $cfg 'mod_enabled')) { Get-ConfigValue $cfg 'mod_enabled' } else { '-' }),
                    $(if ($null -ne (Get-ConfigValue $cfg 'overlay_hooks')) { Get-ConfigValue $cfg 'overlay_hooks' } else { '-' }))
    }
    if (Test-Path -LiteralPath $reconDir -PathType Container) {
        $on = Test-Path -LiteralPath (Join-Path $reconDir 'enabled.txt') -PathType Leaf
        Write-Host ("  recon          {0}" -f $(if ($on) { 'enabled.txt present' } else { 'absent' }))
    }

    Write-Host ""
    Write-Host "UE4SS" -ForegroundColor Cyan
    $pinned = Get-PinnedUe4ss
    if (-not (Test-Path -LiteralPath $ue4ssDll -PathType Leaf)) {
        Write-Host ("  UE4SS.dll      not installed ({0})" -f $ue4ssDll) -ForegroundColor Yellow
    } else {
        $have = Get-Sha $ue4ssDll
        Write-Host ("  sha256         {0}" -f $have)
        if (-not $pinned) {
            Write-Host "  pinned         profiles\pinned.json is missing" -ForegroundColor Yellow
        } elseif (-not $pinned.sha256) {
            Write-Host ("  pinned         {0} - not pinned yet (pinned.json carries no sha256)" -f $pinned.version) -ForegroundColor Yellow
        } elseif ($have -eq ([string]$pinned.sha256).ToLowerInvariant()) {
            Write-Host ("  pinned         {0}  MATCH" -f $pinned.version) -ForegroundColor Green
        } else {
            Write-Host ("  pinned         {0}  MISMATCH - expected {1}" -f $pinned.version, $pinned.sha256) -ForegroundColor Red
        }
    }

    Write-Host ""
    Write-Host "Payloads under Binaries\Win64" -ForegroundColor Cyan
    $known = @{}
    if (Test-Path -LiteralPath $payloadRoot -PathType Container) {
        foreach ($f in Get-ChildItem -LiteralPath $payloadRoot -File) { $known[(Get-Sha $f.FullName)] = $f.Name }
    }
    $dests = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $profilesDir -PathType Container) {
        foreach ($d in Get-ChildItem -LiteralPath $profilesDir -Directory) {
            $pf = Join-Path $d.FullName 'profile.json'
            if (-not (Test-Path -LiteralPath $pf -PathType Leaf)) { continue }
            foreach ($e in (AsArray (Read-JsonFile $pf).payloads)) {
                if (-not $e.dest) { continue }
                # A tree's dest is a directory; it is listed by what it holds, not hashed
                # as if it were one file.
                $d0 = [string]$e.dest + $(if ($e.tree) { [string][char]92 } else { '' })
                if (-not $dests.Contains($d0)) { $dests.Add($d0) }
            }
        }
    }
    if ($dests.Count -eq 0) {
        Write-Host "  (no profile declares a payload yet)"
    }
    foreach ($dest in ($dests | Sort-Object)) {
        $abs = Join-Path $bin ($dest.TrimEnd([char]92))
        if ($dest.EndsWith([string][char]92)) {
            if (-not (Test-Path -LiteralPath $abs -PathType Container)) {
                Write-Host ("  {0,-28} absent" -f $dest)
            } else {
                $held = @(Get-TreeFiles $abs)
                Write-Host ("  {0,-28} {1} file(s)" -f $dest, $held.Count)
            }
            continue
        }
        if (-not (Test-Path -LiteralPath $abs -PathType Leaf)) {
            Write-Host ("  {0,-28} absent" -f $dest)
            continue
        }
        $sha = Get-Sha $abs
        $who = $(if ($known.ContainsKey($sha)) { $known[$sha] } else { 'NOT IN THE STORE' })
        Write-Host ("  {0,-28} {1}  {2}" -f $dest, $sha.Substring(0, 16), $who)
    }

    Write-Host ""
    Write-Host "Machine" -ForegroundColor Cyan
    $mode = Get-HwSchMode
    Write-Host ("  HAGS           {0} (HwSchMode = {1})" -f (Get-HagsState), $(if ($null -eq $mode) { '-' } else { $mode }))
    foreach ($proc in $WatchedProcesses) {
        $n = @(Get-Process -Name $proc -ErrorAction SilentlyContinue).Count
        Write-Host ("  {0,-28} {1}" -f $proc, $(if ($n -gt 0) { "running ($n)" } else { '-' }))
    }

    Show-ProcessNotice

    Write-Host ""
    Write-Host "Vault" -ForegroundColor Cyan
    $newest = Get-NewestVaultStamp
    if (-not $newest) {
        Write-Host "  (no vault yet)" -ForegroundColor Yellow
    } else {
        Write-Host ("  newest         {0}" -f $newest)
        $diffs = @(Compare-AgainstVault $newest)
        if ($diffs.Count -eq 0) {
            Write-Host "  play state     matches it exactly" -ForegroundColor Green
        } else {
            Write-Host ("  play state     {0} difference(s):" -f $diffs.Count) -ForegroundColor Yellow
            foreach ($d in $diffs) { Write-Host ("                 {0}" -f $d.text) -ForegroundColor Yellow }
        }
    }
}

#====================================================================================
# list
#====================================================================================

function Invoke-List {
    Write-Host ""
    Write-Host "Profiles" -ForegroundColor Cyan
    $any = $false
    if (Test-Path -LiteralPath $profilesDir -PathType Container) {
        foreach ($d in Get-ChildItem -LiteralPath $profilesDir -Directory | Sort-Object Name) {
            $pf = Join-Path $d.FullName 'profile.json'
            if (-not (Test-Path -LiteralPath $pf -PathType Leaf)) { continue }
            $any = $true
            $desc = (Read-JsonFile $pf).description
            Write-Host ("  {0,-18} {1}" -f $d.Name, $desc)
        }
    }
    if (-not $any) { Write-Host ("  (none in '{0}')" -f $profilesDir) }

    Write-Host ""
    Write-Host "Vault stamps" -ForegroundColor Cyan
    $stamps = @(Get-VaultStamps)
    if ($stamps.Count -eq 0) { Write-Host "  (none)" }
    foreach ($s in $stamps) {
        $m = Read-JsonFile (Join-Path $vaultRoot "$s\vault.json")
        Write-Host ("  {0}   {1} file(s)" -f $s, (AsArray $m.files).Count)
    }

    Write-Host ""
    Write-Host "Snapshot stamps" -ForegroundColor Cyan
    $found = $false
    if (Test-Path -LiteralPath $snapshotRoot -PathType Container) {
        foreach ($d in Get-ChildItem -LiteralPath $snapshotRoot -Directory | Sort-Object Name) {
            $sf = Join-Path $d.FullName 'snapshot.json'
            if (-not (Test-Path -LiteralPath $sf -PathType Leaf)) { continue }
            $found = $true
            $m = Read-JsonFile $sf
            Write-Host ("  {0,-34} {1} file(s)" -f $d.Name, (AsArray $m.files).Count)
        }
    }
    if (-not $found) { Write-Host "  (none)" }

    Write-Host ""
    if (Test-Path -LiteralPath $currentJson -PathType Leaf) {
        $cur = Read-JsonFile $currentJson
        if ($cur.complete -eq $false) {
            Write-Host ("Applied: {0} (snapshot {1}) - INCOMPLETE, the apply did not finish; run 'restore'" -f
                        $cur.profile, $cur.snapshot) -ForegroundColor Red
        } else {
            Write-Host ("Applied: {0} (snapshot {1})" -f $cur.profile, $cur.snapshot) -ForegroundColor Green
        }
    } else {
        Write-Host "Applied: nothing"
    }
}

#====================================================================================
# run - one launch per cell, and the evidence a verdict is made of
#====================================================================================
#
# A cell is a process launch. Applying a profile and then toggling into the state it
# names is a different state - once the overlay's composition target has existed in a
# process the window never returns to Hardware: Independent Flip - so `run` applies,
# launches, decides, tears down, and launches again. Nothing is reused.
#
# The criteria are `.claude/rules/in-game-verification.md`'s and no others: CRASH on the
# mod's WATCHDOG line, a CrashReportClient process, a window titled "...has crashed", a
# new Saved\Crashes entry, or the process exiting; HEALTHY only if none of those appear
# for the whole hold. "The process is alive" is not health - a LowLevelFatalError modal
# keeps it alive and keeps Saved\Crashes empty until someone clicks OK.
#
# The mod's log is the player's log. It is never cleared: a mark is taken before the
# launch and the new lines are read back against it, rotation and all.

function Get-GameExe {
    $exe = Join-Path $bin 'Project_Plague-Win64-Shipping.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "The game exe is not at '$exe'." }
    return $exe
}

function Get-LogPaths {
    # TWO places, because a profile may pin an old build. Builds after 96a2c4c write to
    # %LOCALAPPDATA%\WuchangMinimap; 1.1.1 and earlier wrote beside the DLL. A run that
    # marked only one of them would judge a crash profile on a log that never moved, and
    # report `log_lines` absent from a process that had written every one of them.
    return @((Join-Path $stateDir $LogGlob), (Join-Path $modDir $LogGlob))
}

function Get-FileMark([string]$path) {
    # The mod rotates its log on start, so neither a length nor a line count survives a
    # launch on its own. The mark carries the length and the head bytes: a file shorter
    # than the mark, or whose head no longer hashes the same, is a different file and all
    # of its content belongs to the process just launched.
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return [pscustomobject]@{ path = $path; exists = $false; len = 0; head = '' }
    }
    $len  = (Get-Item -LiteralPath $path).Length
    $head = ''
    if ($len -gt 0) {
        $take  = [int][Math]::Min(4096, $len)
        $bytes = New-Object byte[] $take
        $fs    = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
        try { [void]$fs.Read($bytes, 0, $take) } finally { $fs.Dispose() }
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $head = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '') }
        finally { $sha.Dispose() }
    }
    return [pscustomobject]@{ path = $path; exists = $true; len = $len; head = $head }
}

function Get-LogMark {
    $marks = foreach ($path in (Get-LogPaths)) { Get-FileMark $path }
    return ,@($marks)
}

function Get-FileSince($mark) {
    # The game holds the log open while it writes, so it is opened share-all.
    $path = $mark.path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return [pscustomobject]@{ lines = @(); rotated = $false; wrote = $false }
    }
    $now     = Get-FileMark $path
    $rotated = (-not $mark.exists) -or ($now.len -lt $mark.len) -or ($now.head -ne $mark.head)
    $from    = $(if ($rotated) { 0 } else { $mark.len })
    if (-not $rotated -and $now.len -eq $mark.len) {
        return [pscustomobject]@{ lines = @(); rotated = $false; wrote = $false }
    }
    $text = ''
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        if ($from -gt 0) { [void]$fs.Seek([long]$from, 'Begin') }
        # UTF-8, with the BOM detected if there is one. `mm::log` takes wide strings, but
        # what lands in wuchang_minimap.log is UTF-8 bytes with no BOM - read as UTF-16 a
        # 22 KB log comes back as one unsplittable line of mojibake, which is a log slice
        # that every `log_lines` check then judges and none can ever match.
        $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8, $true)
        try { $text = $sr.ReadToEnd() } finally { $sr.Dispose() }
    } finally { $fs.Dispose() }
    $lines = @($text -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 })
    return [pscustomobject]@{ lines = $lines; rotated = $rotated; wrote = ($lines.Count -gt 0) }
}

function Get-LogSince($mark) {
    # The union over both locations, with the one that actually moved naming itself: a
    # build writes to one of them, never to both, so "which file" is evidence rather than
    # bookkeeping.
    $lines   = @()
    $rotated = $false
    $where   = @()
    foreach ($m in @($mark)) {
        $slice = Get-FileSince $m
        if (-not $slice.wrote) { continue }
        $lines   += $slice.lines
        $rotated  = $rotated -or $slice.rotated
        $where   += $m.path
    }
    $note = 'no log was written'
    if ($where.Count -gt 0) {
        $note = $(if ($rotated) { 'the log rotated - every line is this process' }
                  else { 'appended to the log that was already there' })
        $note += (' (' + ($where -join '; ') + ')')
    }
    return [pscustomobject]@{ lines = @($lines); rotated = $rotated; note = $note }
}

function Get-CrashDirNames {
    $crashes = Join-Path $saved 'Crashes'
    return @(Get-ChildItem -LiteralPath $crashes -Directory -ErrorAction SilentlyContinue |
             Select-Object -ExpandProperty Name)
}

function Test-CrashWindow {
    if (@(Get-Process -Name 'CrashReportClient*' -ErrorAction SilentlyContinue).Count -gt 0) {
        return 'a CrashReportClient process'
    }
    $w = @(Get-Process -ErrorAction SilentlyContinue |
           Where-Object { $_.MainWindowTitle -match 'has crashed|LowLevelFatalError' })
    if ($w.Count -gt 0) { return ("a window titled '{0}'" -f $w[0].MainWindowTitle) }
    return $null
}

function Get-LiveGameProcesses([DateTime]$notBefore) {
    return @(Get-Process -Name 'Project_Plague-Win64-Shipping' -ErrorAction SilentlyContinue |
             Where-Object {
                 try { (-not $_.HasExited) -and $_.StartTime -ge $notBefore.AddSeconds(-2) }
                 catch { $false }
             })
}

function Resolve-GameProcess([DateTime]$notBefore, [int]$timeoutSec) {
    # The game is resolved ONCE IT HAS SETTLED, which is the whole point. Steam hands the
    # launch on: the first process to carry this name is a stub that starts the real game
    # and exits within seconds. Binding to whatever exists half a second after the launch
    # and settling afterwards is how three healthy launches were reported as three crashes,
    # with `Saved\Crashes` unmoved and the mod's log carrying a full start-up block.
    #
    # Returns the newest process of that name that is still alive at the end of the wait.
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $seen = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if ((Get-LiveGameProcesses $notBefore).Count -gt 0) { $seen = $true }
    }
    $live = Get-LiveGameProcesses $notBefore
    if ($live.Count -eq 0) {
        if ($seen) { return 'gone' }
        return $null
    }
    return (@($live | Sort-Object StartTime -Descending)[0])
}

function Get-ProcessModules($proc) {
    # FULL PATHS, lower-cased, never module names. Windows loads its own dxgi.dll and
    # dwmapi.dll from System32 into every process, and those are the two names the
    # injectors in these profiles take - a check on the name alone would pass in a
    # configuration that carries neither.
    #
    # Guarded, because a process that is exiting throws here and an empty list would read
    # as "nothing was loaded", which is exactly the answer a stock cell wants to hear.
    try {
        $proc.Refresh()
        return @($proc.Modules |
                 ForEach-Object { $_.FileName.ToLowerInvariant() } |
                 Sort-Object -Unique)
    } catch {
        return $null
    }
}

function Test-ModuleLoaded($mods, [string]$needle) {
    # A profile names as much of the path as makes it unambiguous - 'Binaries\Win64\dxgi.dll'
    # for ReShade's proxy, 'dxgi.dll' alone would also match the Windows one.
    # -replace, not String.Replace: two single-character strings make PowerShell pick the
    # (char, char) overload and it then refuses the call.
    $n = ($needle -replace '/', '\').ToLowerInvariant()
    return (@($mods | Where-Object { $_.EndsWith($n) }).Count -gt 0)
}

function Close-GameWindow($proc, [int]$timeoutSec) {
    # WM_CLOSE, never Stop-Process: killing the game skips the OFF / ON / exit paths, and
    # those are where the bugs are. A game that does not answer is a finding with a name.
    $t0   = Get-Date
    $sent = $false
    try { $proc.Refresh(); $sent = $proc.CloseMainWindow() } catch { $sent = $false }
    if (-not $sent) {
        return [pscustomobject]@{ closed = $false; seconds = 0
                                  how = 'WM_CLOSE could not be sent - no main window' }
    }
    $exited = $proc.WaitForExit($timeoutSec * 1000)
    $secs   = [int]((Get-Date) - $t0).TotalSeconds
    if ($exited) { return [pscustomobject]@{ closed = $true; seconds = $secs; how = 'WM_CLOSE' } }
    return [pscustomobject]@{ closed = $false; seconds = $secs
                              how = "did not exit within $timeoutSec s of WM_CLOSE" }
}

function Stop-DecidedCell($proc) {
    # The only kill in this script, and it runs after a cell has already been decided. A
    # killed game does not give back an exclusive audio endpoint: after several of these
    # the audio engine can be left clicking with no game running. That is a testing
    # artifact, and the run says so rather than letting it be hunted in the mod.
    foreach ($n in @('Project_Plague-Win64-Shipping', 'CrashReportClient')) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | ForEach-Object {
            try { $_.Kill() } catch { }
        }
    }
    Start-Sleep -Seconds 3
}

function Get-Monitors {
    # `manual.monitors` is the owner's word; this is what Windows says, so a report can be
    # checked against it rather than trusted.
    try {
        return @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object {
            [pscustomobject]@{
                name    = $_.Name
                mode    = ("{0}x{1}" -f $_.CurrentHorizontalResolution, $_.CurrentVerticalResolution)
                refresh = $_.CurrentRefreshRate
                driver  = $_.DriverVersion
            }
        })
    } catch {
        return @()
    }
}

function Get-WatchedProcessState {
    $o = [ordered]@{}
    foreach ($p in $WatchedProcesses) {
        $o[$p] = @(Get-Process -Name $p -ErrorAction SilentlyContinue).Count
    }
    return [pscustomobject]$o
}

#------------------------------------------------------------------------------------
# The probes. One descriptor per probe, and it is the only place that answers what a
# probe instruments, what it needs before a byte moves, and what it collects. The
# runner below knows only that a probe has those three fields.
#------------------------------------------------------------------------------------

function Get-Probe([string]$name) {
    switch ($name) {
        'crash' {
            return [pscustomobject][ordered]@{
                name       = 'crash'
                what       = ("the criteria of rules/in-game-verification.md, the process's module " +
                              "list and the mod's own lines")
                config_dev = $null
                tool       = $null
                min_hold   = 0
            }
        }
        'present' {
            # The interleaving lives INSIDE the mod. `dev_frame_cycle_ms` rotates the
            # overlay through its five layers and the Present hook sorts the game's own
            # present interval into a histogram per layer, so the comparison is between
            # phases of ONE capture: scene drift, the mod's warm-up and the order of the
            # layers then land on every one of them equally. That is why this is a single
            # launch and not five, and it is what three earlier rounds of cells got wrong.
            $keys = [ordered]@{ 'log_level' = 'verbose'; 'dev_frame_stop' = '0' }
            $keys['dev_frame_cycle_ms'] = [string]$CycleMs
            return [pscustomobject][ordered]@{
                name       = 'present'
                what       = "PresentMon on the game's own swapchain, and the mod's own frame census"
                config_dev = [pscustomobject]$keys
                tool       = 'presentmon'
                # Below this the mod never reaches its 30 s census table and the capture is
                # mostly the loading screen.
                min_hold   = 60
            }
        }
    }
    throw "unknown probe '$name'."
}

function Get-Instrument($spec, $p) {
    # Whether this probe writes its keys into the install, and which. The one rule: a
    # state in which nothing of ours runs has no census to switch on, and writing the keys
    # anyway would be a mutation that measures nothing.
    if (-not $spec.config_dev) { return $null }
    if ((Get-ModState $p) -ne 'on') { return $null }
    return $spec.config_dev
}

function Get-EtwSessionRight {
    # Who may open an ETW session: an administrator, or a member of Performance Log Users
    # (S-1-5-32-559) - the group exists so that a capture needs no elevation. It is read by
    # SID because the group's name is localised, and off the TOKEN rather than off the group's
    # membership list: a membership granted since this logon is not in this token and opens
    # nothing. Returns the grant that satisfies it, or $null.
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if ((New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator))
    {
        return 'administrator'
    }
    foreach ($g in $id.Groups)
    {
        if ($g.Value -eq 'S-1-5-32-559') { return 'performance log users' }
    }
    return $null
}

#------------------------------------------------------------------------------------
# The processes a configuration needs. The rule is the same one the files have: the
# script touches only what it changed. It starts what a profile requires and is not
# already up, records that in the snapshot, and on restore stops exactly those - a
# process the owner already had running is neither started nor stopped.
#
# Stopping is the asymmetric half. RTSS's manifest is `requireAdministrator`, so an
# unelevated shell starts it (it elevates itself) and then cannot stop it: it has no main
# window to close and `Stop-Process` is Access denied across the integrity boundary. The
# escape is a Scheduled Task the owner registers once; without it, a run that started such
# a process says so, and `status` keeps saying so until the process is gone. Nobody has to
# remember.
#------------------------------------------------------------------------------------

function Get-PinnedProcesses { $p = Get-Pinned; return $(if ($p) { $p.processes } else { $null }) }

function Get-ProcessStart([string]$name) {
    # How to start a process is a path AND its arguments; a config that carried only the
    # path could not express a process that needs one. `processes.exe.<name>` is either the
    # path as a bare string or `{ "path": …, "args": [ … ] }`.
    $cfg = Get-PinnedProcesses
    if (-not $cfg -or -not $cfg.exe) { return $null }
    $e = $cfg.exe.$name
    if (-not $e) { return $null }
    if ($e -is [string]) { return [pscustomobject]@{ path = [string]$e; args = @() } }
    return [pscustomobject]@{ path = [string]$e.path; args = @(Get-StringList $e 'args') }
}

function Get-ProcessFamily([string]$name) {
    # A process and the helpers it brings up with it. RTSS starts RTSSHooksLoader64 and
    # EncoderServer, and leaving those behind is the same residue as leaving RTSS itself.
    $cfg = Get-PinnedProcesses
    $kids = @()
    if ($cfg -and $cfg.children) { $kids = @(Get-StringList $cfg.children $name) }
    return @(@($name) + $kids)
}

function Start-RequiredProcesses($p) {
    # Returns one record per name in `processes.running`, saying whether THIS apply is what
    # put it there. Called inside the snapshot window, because starting a process is a
    # change to the box like any other.
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($name in (Get-StringList $p.processes 'running')) {
        if (@(Get-Process -Name $name -ErrorAction SilentlyContinue).Count -gt 0) {
            Write-Host ("  process        {0,-16} already running; this run did not start it and will not stop it" -f $name) `
                       -ForegroundColor DarkGray
            $records.Add([pscustomobject][ordered]@{ name = $name; started = $false })
            continue
        }
        $start = Get-ProcessStart $name
        if (-not $start -or -not $start.path) {
            throw ("profile requires '$name' running and pinned.json does not say where its " +
                   "exe is, so it cannot be started. Add it under processes.exe.")
        }
        if (-not (Test-Path -LiteralPath $start.path -PathType Leaf)) {
            throw "profile requires '$name' running and its exe is not at '$($start.path)'."
        }
        if ($DryRun) {
            Write-Host ("  would  start   {0,-16} {1}" -f $name, $start.path) -ForegroundColor DarkGray
            $records.Add([pscustomobject][ordered]@{ name = $name; started = $true })
            continue
        }
        Write-Host ("  process        {0,-16} starting it - this run will stop it again" -f $name) `
                   -ForegroundColor DarkGray
        # Hidden always: this pipeline never puts a window on the owner's desktop. RTSS goes
        # to the tray either way, and a process that insists on a window still gets one.
        $sp = @{ FilePath = $start.path; WorkingDirectory = (Split-Path -Parent $start.path)
                 WindowStyle = 'Hidden' }
        if (@($start.args).Count -gt 0) { $sp.ArgumentList = @($start.args) }
        Start-Process @sp | Out-Null
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline -and
               @(Get-Process -Name $name -ErrorAction SilentlyContinue).Count -eq 0) {
            Start-Sleep -Milliseconds 400
        }
        if (@(Get-Process -Name $name -ErrorAction SilentlyContinue).Count -eq 0) {
            throw "started '$exe' and '$name' did not appear within 20 s."
        }
        $records.Add([pscustomobject][ordered]@{ name = $name; started = $true })
    }
    return $records.ToArray()
}

function Stop-StartedProcesses($records) {
    # Stops only what an apply started, plus the helpers that came up with it. Returns the
    # names it could not stop, which the caller turns into a standing notice.
    $left = New-Object System.Collections.Generic.List[string]
    $cfg  = Get-PinnedProcesses
    foreach ($r in (AsArray $records)) {
        if (-not $r.started) { continue }
        foreach ($n in (Get-ProcessFamily ([string]$r.name))) {
            $procs = @(Get-Process -Name $n -ErrorAction SilentlyContinue)
            if ($procs.Count -eq 0) { continue }
            if ($DryRun) { Write-Host ("  would  stop    {0}" -f $n) -ForegroundColor DarkGray; continue }
            try { $procs | Stop-Process -Force -ErrorAction Stop } catch { }
            Start-Sleep -Milliseconds 400
            if (@(Get-Process -Name $n -ErrorAction SilentlyContinue).Count -eq 0) {
                Write-Host ("  process        {0,-16} stopped - this run had started it" -f $n) `
                           -ForegroundColor DarkGray
                continue
            }
            # It runs above this shell - `requireAdministrator` processes do. Two ways down,
            # cheapest first.
            #
            # A registered task, if there is one: `Start-ScheduledTask` fires it without
            # elevating US at all, so it cannot put a prompt in the middle of a run. That is
            # the only reason it is tried before the direct route.
            $task = $(if ($cfg) { [string]$cfg.stop_task } else { $null })
            if ($task -and (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue)) {
                Start-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
                Start-Sleep -Seconds 2
                if (@(Get-Process -Name $n -ErrorAction SilentlyContinue).Count -eq 0) {
                    Write-Host ("  process        {0,-16} stopped through '{1}'" -f $n, $task) `
                               -ForegroundColor DarkGray
                    continue
                }
            }
            # Otherwise elevate exactly one fixed system binary with fixed arguments:
            # `taskkill.exe /F /IM <name>.exe`. Not a shell - an elevated shell is a general
            # capability asked for a specific job, and it reads that way to anything watching.
            # On a box whose UAC is set to consent this raises a prompt; that is the box
            # saying so, not a fault, and the notice below catches a declined one.
            try {
                Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\taskkill.exe') `
                              -Verb RunAs -Wait -ArgumentList '/F', '/IM', ($n + '.exe') `
                              -ErrorAction Stop
                Start-Sleep -Milliseconds 800
            } catch { }
            if (@(Get-Process -Name $n -ErrorAction SilentlyContinue).Count -eq 0) {
                Write-Host ("  process        {0,-16} stopped - elevated taskkill; this run had started it" -f $n) `
                           -ForegroundColor DarkGray
                continue
            }
            $left.Add($n)
        }
    }
    # The leading comma: a returned array of none unrolls to $null on the way out, and
    # @($null).Count is 1 - the caller would then report one nameless process left running.
    return ,$left.ToArray()
}

function Set-ProcessNotice($names) {
    # The standing notice. A process this pipeline started and could not stop is a change
    # to the box that outlives the run, so it is written down rather than remembered: every
    # `status` reprints it, and clears it by itself once the processes are gone.
    $names = @(@($names) | Where-Object { $_ })
    if ($names.Count -eq 0) { return }
    $notice = [pscustomobject][ordered]@{
        when  = [DateTime]::UtcNow.ToString('o')
        names = $names
        why   = ("started by a repro run and not stoppable from an unelevated shell " +
                 "(requireAdministrator). Close it from its tray icon, or register the " +
                 "stop task named in profiles\pinned.json.")
    }
    Write-JsonFile $processNotice $notice
    Write-Host ""
    Write-Host ("  LEFT RUNNING   {0}" -f ($names -join ', ')) -ForegroundColor Yellow
    Write-Host ("                 this run started it and cannot stop it. `status` will keep " +
                "saying so until it is gone.") -ForegroundColor Yellow
}

function Show-ProcessNotice {
    if (-not (Test-Path -LiteralPath $processNotice -PathType Leaf)) { return }
    $n = Read-JsonFile $processNotice
    $still = @(@(Get-StringList $n 'names') | Where-Object {
                   @(Get-Process -Name $_ -ErrorAction SilentlyContinue).Count -gt 0 })
    if ($still.Count -eq 0) {
        Remove-Item -LiteralPath $processNotice -Force -ErrorAction SilentlyContinue
        return
    }
    Write-Host ""
    Write-Host "Left running by a repro run" -ForegroundColor Yellow
    Write-Host ("  {0}" -f ($still -join ', ')) -ForegroundColor Yellow
    Write-Host ("  since {0} - {1}" -f $n.when, $n.why) -ForegroundColor DarkGray
}

function Get-PinnedPresentMon { $p = Get-Pinned; return $(if ($p) { $p.presentmon } else { $null }) }

function Test-ProbeReady($spec) {
    # Everything a probe needs, read back BEFORE the profile is applied: a run that
    # discovers halfway that it cannot measure has still changed the box. PresentMon needs
    # an ETW session, and the right to open one is stated here and not fought, because the
    # alternatives are a UAC prompt in the middle of a measurement or a cell that quietly
    # returns no numbers.
    if (-not $spec.tool) { return $null }

    $problems = New-Object System.Collections.Generic.List[string]
    $exe = $PresentMon
    $sha = $null
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        $problems.Add("PresentMon is not at '$exe'; pass -PresentMon <path> or set WUCHANG_PRESENTMON.")
    } else {
        $sha = Get-Sha $exe
    }
    $etwRight = Get-EtwSessionRight
    if (-not $etwRight) {
        $problems.Add(("PresentMon needs an ETW session, which this token may not open: it is neither " +
                       "an administrator nor a member of Performance Log Users (S-1-5-32-559). Add the " +
                       "account to that group and log on again, or run as administrator."))
    }

    $pinned = Get-PinnedPresentMon
    $match  = $null
    if ($sha -and $pinned -and $pinned.sha256) {
        $match = ($sha -eq [string]$pinned.sha256)
        if (-not $match) {
            Write-Host (("  presentmon     {0} is not the pinned build ({1}); the numbers are still " +
                         "collected and the manifest says which binary took them.") -f
                        $sha.Substring(0, 16), $pinned.version) -ForegroundColor Yellow
        }
    }

    # A dry run is a plan, not an attempt: it names what would stop the run and still
    # prints the rest of it.
    $fatal = ($problems.Count -gt 0 -and -not $Force -and -not $DryRun)
    foreach ($msg in $problems) {
        Write-Host ("  {0}  {1}" -f $(if ($fatal) { 'FAIL  ' } else { 'WOULD FAIL' }), $msg) `
                   -ForegroundColor $(if ($fatal) { 'Red' } else { 'Yellow' })
    }
    if ($fatal) {
        throw ("probe '$($spec.name)': " + ($problems -join ' ') +
               " Or pass -Force to run the cells with the census alone and no capture.")
    }

    return [pscustomobject][ordered]@{
        exe        = $exe
        sha256     = $sha
        pinned     = $(if ($pinned) { [string]$pinned.version } else { $null })
        pinned_match = $match
        etw_right  = $etwRight
        # A capture is taken only when everything it needs is true. Anything else records
        # WHY there is no capture, so a cell with no numbers can never read as a cell whose
        # numbers were fine.
        capturing  = ($problems.Count -eq 0)
        why_not    = $(if ($problems.Count -eq 0) { $null } else { ($problems -join ' ') })
    }
}

#------------------------------------------------------------------------------------
# The capture. PresentMon against the resolved pid, bounded twice - it stops when the
# game exits, and stops again on its own clock if the game never does - so nothing in
# this script ever has to kill it to get the CSV flushed.
#------------------------------------------------------------------------------------

function Start-PresentCapture($tool, [string]$runDir, [int]$index, $proc, [int]$maxSeconds) {
    if (-not $tool -or -not $tool.capturing) { return $null }
    $csv = Join-Path $runDir ("cell-{0}.csv" -f $index)
    $out = Join-Path $runDir ("cell-{0}.presentmon.txt" -f $index)
    $err = Join-Path $runDir ("cell-{0}.presentmon.err.txt" -f $index)
    $pmArgs = @(
        '--process_id', $proc.Id,
        '--output_file', $csv,
        '--session_name', 'wuchang-repro',
        '--stop_existing_session',
        '--terminate_on_proc_exit',
        '--timed', $maxSeconds,
        '--terminate_after_timed',
        '--no_console_stats'
    )
    $p = Start-Process -FilePath $tool.exe -ArgumentList $pmArgs -PassThru -NoNewWindow `
                       -RedirectStandardOutput $out -RedirectStandardError $err
    Write-Host ("    capture      PresentMon pid {0} on game pid {1}, up to {2} s" -f
                $p.Id, $proc.Id, $maxSeconds) -ForegroundColor DarkGray
    return [pscustomobject]@{ proc = $p; csv = $csv; stdout = $out; stderr = $err }
}

function Stop-PresentCapture($cap, [int]$timeoutSec = 30) {
    if (-not $cap) { return $null }
    # Waiting is what flushes the CSV; killing is what loses its tail. It is killed only
    # after it has outlived both of its own bounds, and the record says so, because a
    # short CSV that nobody knows is short is the wrong kind of evidence.
    $killed = $false
    if (-not $cap.proc.WaitForExit($timeoutSec * 1000)) {
        Write-Host (("    capture      PresentMon did not exit within {0} s - stopping it; the CSV " +
                     "may be short") -f $timeoutSec) -ForegroundColor Yellow
        try { Stop-Process -Id $cap.proc.Id -Force -ErrorAction Stop } catch {}
        $killed = $true
    }
    $note = $null
    if (Test-Path -LiteralPath $cap.stderr -PathType Leaf) {
        $note = ((Get-Content -LiteralPath $cap.stderr -Raw -ErrorAction SilentlyContinue) + '').Trim()
        if (-not $note) { $note = $null }
    }
    if (-not (Test-Path -LiteralPath $cap.csv -PathType Leaf)) {
        return [pscustomobject]@{ csv = $null; killed = $killed
                                  why = ("PresentMon wrote no CSV" + $(if ($note) { ": $note" } else { '' })) }
    }
    # A capture that cannot be read is a finding about the capture, not the end of the run.
    # The cells of a played session cost an hour of someone's evening and the CSV is on
    # disk either way; a parse that throws here would take the manifest with it.
    try {
        $r = Measure-PresentCsv $cap.csv
    } catch {
        $r = [pscustomobject][ordered]@{ csv = $cap.csv; presents = 0; streams = @()
                                         why = ("the CSV could not be read: " + $_.Exception.Message) }
    }
    Add-Member -InputObject $r -NotePropertyName 'killed' -NotePropertyValue $killed
    return $r
}

#------------------------------------------------------------------------------------
# Reading a capture. Split by `SwapChainAddress` FIRST: PresentMon records every
# swapchain in the process, and a build that presents one of its own turns a pooled
# median into a number that describes no frame anyone saw - 173 fps on a game running
# at 100.
#------------------------------------------------------------------------------------

$PresentColumns = [ordered]@{
    'frame_time_ms'      = @('FrameTime', 'msBetweenPresents', 'MsBetweenPresents')
    'cpu_busy_ms'        = @('CPUBusy', 'msCPUBusy', 'MsCPUBusy')
    'cpu_wait_ms'        = @('CPUWait', 'msCPUWait', 'MsCPUWait')
    'gpu_busy_ms'        = @('GPUBusy', 'msGPUBusy', 'MsGPUBusy')
    'gpu_latency_ms'     = @('GPULatency', 'msGPULatency', 'MsGPULatency')
    'display_latency_ms' = @('DisplayLatency', 'msUntilDisplayed', 'MsUntilDisplayed')
    'in_present_api_ms'  = @('InPresentAPITime', 'msInPresentAPI', 'MsInPresentAPI')
}

function ConvertTo-Double([string]$text) {
    # Invariant at the point of use as well as at the top of the script: a reader of
    # machine-written numbers states which numbers it reads. $null when the text is not one.
    $v = 0.0
    if ([double]::TryParse($text, [Globalization.NumberStyles]::Float,
                           [Globalization.CultureInfo]::InvariantCulture, [ref]$v)) { return $v }
    return $null
}

function Get-Percentile($sorted, [double]$q) {
    if ($sorted.Count -eq 0) { return $null }
    $i = [int][Math]::Floor($q * $sorted.Count)
    if ($i -ge $sorted.Count) { $i = $sorted.Count - 1 }
    return [Math]::Round($sorted[$i], 3)
}

function Measure-Column($rows, [string]$column) {
    $vals = New-Object System.Collections.Generic.List[double]
    foreach ($r in $rows) {
        $v = ConvertTo-Double ([string]$r.$column)
        if ($null -ne $v) { $vals.Add($v) }
    }
    if ($vals.Count -eq 0) { return $null }
    $sorted = @($vals.ToArray() | Sort-Object)
    $sum = 0.0
    foreach ($v in $sorted) { $sum += $v }
    return [pscustomobject][ordered]@{
        column  = $column
        samples = $sorted.Count
        mean    = [Math]::Round($sum / $sorted.Count, 3)
        median  = (Get-Percentile $sorted 0.5)
        p90     = (Get-Percentile $sorted 0.9)
        p99     = (Get-Percentile $sorted 0.99)
        max     = [Math]::Round($sorted[$sorted.Count - 1], 3)
    }
}

function Get-CsvColumn($header, $names) {
    foreach ($n in $names) { if ($header -contains $n) { return $n } }
    return $null
}

function Get-AddressKey([string]$address) {
    $a = ([string]$address).Trim()
    if ($a -match '^0[xX]') { $a = $a.Substring(2) }
    $a = $a.TrimStart('0')
    if (-not $a) { $a = '0' }
    return $a.ToLowerInvariant()
}

function Measure-PresentCsv([string]$path) {
    $all = @(Import-Csv -LiteralPath $path)
    if ($all.Count -eq 0) {
        return [pscustomobject]@{ csv = $path; presents = 0; streams = @(); truncated = 0
                                  why = 'the CSV has a header and no frames' }
    }
    $header = @($all[0].PSObject.Properties.Name)

    # A capture that was stopped rather than allowed to finish ends mid-record - the last
    # line of a killed PresentMon can be a single character - and `Import-Csv` hands that
    # back as a row whose remaining columns are $null. Such a row is not a present: it
    # cannot be attributed to a swapchain, and its empty PresentMode is not a mode. They are
    # dropped once, here, and counted, so no reader downstream has to know they can exist.
    # The test is the last column, because a truncation cuts everything after a point.
    $lastCol = $header[-1]
    $rows = @($all | Where-Object { $null -ne $_.$lastCol })
    $truncated = $all.Count - $rows.Count
    if ($rows.Count -eq 0) {
        return [pscustomobject]@{ csv = $path; presents = 0; streams = @(); truncated = $truncated
                                  why = ("every one of the {0} row(s) is an incomplete record - the " +
                                         "capture was stopped mid-write") -f $all.Count }
    }
    $addrCol = Get-CsvColumn $header @('SwapChainAddress')
    if (-not $addrCol) {
        throw ("'$path' has no SwapChainAddress column, so it cannot be split by swapchain; its " +
               "columns are: " + ($header -join ', ') + ".")
    }
    $modeCol = Get-CsvColumn $header @('PresentMode')
    $syncCol = Get-CsvColumn $header @('SyncInterval')

    $picked = [ordered]@{}
    foreach ($k in $PresentColumns.Keys) {
        $c = Get-CsvColumn $header $PresentColumns[$k]
        if ($c) { $picked[$k] = $c }
    }

    $byAddr = @{}
    foreach ($r in $rows) {
        $key = Get-AddressKey $r.$addrCol
        if (-not $byAddr.ContainsKey($key)) { $byAddr[$key] = New-Object System.Collections.Generic.List[object] }
        $byAddr[$key].Add($r)
    }

    $streams = New-Object System.Collections.Generic.List[object]
    $counted = 0
    foreach ($key in ($byAddr.Keys | Sort-Object { -$byAddr[$_].Count })) {
        $sub = $byAddr[$key].ToArray()
        $counted += $sub.Count
        $modes = [ordered]@{}
        if ($modeCol) {
            foreach ($r in $sub) {
                $m = [string]$r.$modeCol
                if ($modes.Contains($m)) { $modes[$m] = $modes[$m] + 1 } else { $modes[$m] = 1 }
            }
        }
        $stats = [ordered]@{}
        foreach ($k in $picked.Keys) {
            $s = Measure-Column $sub $picked[$k]
            if ($s) { $stats[$k] = $s }
        }
        $streams.Add([pscustomobject][ordered]@{
            address        = ([string]$sub[0].$addrCol)
            presents       = $sub.Count
            present_modes  = [pscustomobject]$modes
            sync_intervals = $(if ($syncCol) { @(@($sub | ForEach-Object { [string]$_.$syncCol }) |
                                                 Sort-Object -Unique) } else { @() })
            stats          = [pscustomobject]$stats
        })
    }
    # The verifier sizes its own input: a split that lost rows would report a clean median
    # over a fraction of the capture, and it would report it with the same confidence.
    if ($counted -ne $rows.Count) {
        throw "'$path' has $($rows.Count) frame(s) and the split by swapchain accounts for $counted."
    }

    return [pscustomobject][ordered]@{
        csv       = $path
        presents  = $rows.Count
        truncated = $truncated
        columns   = [pscustomobject]$picked
        streams   = $streams.ToArray()
        why       = $null
    }
}

function Resolve-GameStream($capture, $lines) {
    # Which of the streams is the GAME'S. The overlay presents no swapchain of its own - its
    # DirectComposition surface is the null-address stream - so the game is the busiest stream
    # carrying a real address, and that is the rule.
    #
    # The mod's hook-discovery line is a cross-check, never a gate. It logs the swapchain
    # OBJECT the mod hooked, and on a box carrying an interposer - `sl.interposer.dll` ships
    # with this game, so on every box here - that object is the wrapper standing in front of
    # the runtime's swapchain, while ETW reports the runtime's own. The two addresses differ
    # for a reason that says nothing about which stream the numbers come off; the rule records
    # which case this cell is.
    if (-not $capture -or @($capture.streams).Count -eq 0) { return $null }

    $real = @(@($capture.streams) | Where-Object { (Get-AddressKey $_.address) -ne '0' } |
              Sort-Object presents -Descending)
    if ($real.Count -eq 0) {
        return [pscustomobject]@{ address = $null; presents = 0; hooked = $null
                                  rule = 'every stream in the capture has a null swapchain address' }
    }

    $hooked = $null
    foreach ($l in @($lines)) {
        if ($l -match "the engine's own swapchain (0[xX][0-9a-fA-F]+)") { $hooked = $Matches[1]; break }
    }

    $rule = "the busiest of {0} non-zero stream(s)" -f $real.Count
    if (-not $hooked) {
        $rule += '; no mod in the process to cross-check it'
    } elseif ((Get-AddressKey $hooked) -eq (Get-AddressKey $real[0].address)) {
        $rule += "; the mod's own hook-discovery line names the same swapchain"
    } else {
        $rule += (("; the mod hooked {0}, the wrapper in front of it - ETW reports the runtime's " +
                   "own object") -f $hooked)
    }
    return [pscustomobject]@{ address = $real[0].address; presents = $real[0].presents
                              hooked = $hooked; rule = $rule }
}

function Show-PresentCapture($capture) {
    if (-not $capture -or $capture.presents -eq 0) {
        Write-Host ("    capture      no frames: {0}" -f
                    $(if ($capture -and $capture.why) { $capture.why } else { 'the capture is empty' })) `
                   -ForegroundColor Yellow
        return
    }
    if ($capture.truncated -gt 0) {
        Write-Host ((("    capture      {0} incomplete record(s) dropped - the capture was stopped " +
                      "mid-write, so its tail is not the game's last frames") -f $capture.truncated)) `
                   -ForegroundColor Yellow
    }
    $game = $capture.game_stream
    foreach ($s in @($capture.streams)) {
        $mine = ($game -and $game.address -and (Get-AddressKey $s.address) -eq (Get-AddressKey $game.address))
        $ft   = $s.stats.frame_time_ms
        Write-Host ("    {0} {1,-14} {2,6} present(s)  {3}" -f
                    $(if ($mine) { 'GAME  ' } else { '      ' }), $s.address, $s.presents,
                    $(if ($ft -and $ft.mean -gt 0) {
                          "median {0:N3} ms  ({1:N1} fps from the mean)" -f $ft.median, (1000.0 / $ft.mean) }
                      elseif ($ft) { "median {0:N3} ms" -f $ft.median }
                      else { 'no frame-time column' })) `
                   -ForegroundColor $(if ($mine) { 'Gray' } else { 'DarkGray' })
        if ($mine -and $s.present_modes) {
            foreach ($m in (Get-PropertyNames $s.present_modes)) {
                Write-Host ("                   present mode {0,-28} {1,5:N1} %" -f
                            $m, (100.0 * $s.present_modes.$m / $s.presents)) -ForegroundColor DarkGray
            }
        }
    }
    if (-not $game -or -not $game.address) {
        Write-Host ("    capture      the game's own stream could not be named: {0}" -f
                    $(if ($game) { $game.rule } else { 'the capture has no streams' })) -ForegroundColor Yellow
    }
}

function Show-Census($census) {
    Write-Host ("    census       {0} table(s); the last one:" -f $census.tables) -ForegroundColor DarkGray
    foreach ($ph in @($census.phases)) {
        Write-Host ("                   {0,-18} {1,6} frame(s)  median {2,7:N3} ms  p90 {3,7:N3}" -f
                    $ph.phase, $ph.samples, $ph.median_ms, $ph.p90_ms) -ForegroundColor DarkGray
    }
    $d = $census.per_frame_median
    if ($d) {
        Write-Host (("                   per frame, median: composition {0:+0.000;-0.000} ms, the " +
                     "overlay's frame {1:+0.000;-0.000}, the compositor {2:+0.000;-0.000}, our " +
                     "threads {3:+0.000;-0.000}; all of it {4:+0.000;-0.000}") -f
                    $d.composition, $d.overlay_frame, $d.compositor, $d.background, $d.all_of_it)
    }
}

#------------------------------------------------------------------------------------
# The census. The mod's own table, parsed out of the cell's log slice so a comparison
# across runs reads numbers rather than prose. The last table of the cell is the one
# recorded: it is the widest, because the histogram accumulates for the whole session.
#------------------------------------------------------------------------------------

$CensusPhases = @('full', 'nothing composed', 'no frame at all', 'no visual either',
                  'no background either')

function Get-CensusDifferences([string]$line) {
    $nums = @([regex]::Matches($line, '[-+]\d+\.\d+') | ForEach-Object { ConvertTo-Double $_.Value })
    if ($nums.Count -ne 5) { return $null }
    return [pscustomobject][ordered]@{
        composition    = $nums[0]
        overlay_frame  = $nums[1]
        compositor     = $nums[2]
        background     = $nums[3]
        all_of_it      = $nums[4]
    }
}

function Get-CensusTable($lines) {
    $all   = @($lines)
    $heads = New-Object System.Collections.Generic.List[int]
    for ($i = 0; $i -lt $all.Count; $i++) {
        if ($all[$i] -match "frame census: the game's own present interval") { $heads.Add($i) }
    }
    if ($heads.Count -eq 0) { return $null }

    $start  = $heads[$heads.Count - 1]
    $phases = New-Object System.Collections.Generic.List[object]
    $median = $null
    $p90    = $null
    for ($i = $start + 1; $i -lt $all.Count; $i++) {
        $l = $all[$i]
        $m = [regex]::Match($l, ('\s(' + ($CensusPhases -join '|') + ')\s+(\d+) frame\(s\)\s+median\s+' +
                                 '([-\d.]+) ms\s+mean\s+([-\d.]+)\s+p90\s+([-\d.]+)\s+p99\s+([-\d.]+)' +
                                 '\s+over 40 ms: (\d+)'))
        if ($m.Success) {
            $phases.Add([pscustomobject][ordered]@{
                phase     = $m.Groups[1].Value
                samples   = [int]$m.Groups[2].Value
                median_ms = (ConvertTo-Double $m.Groups[3].Value)
                mean_ms   = (ConvertTo-Double $m.Groups[4].Value)
                p90_ms    = (ConvertTo-Double $m.Groups[5].Value)
                p99_ms    = (ConvertTo-Double $m.Groups[6].Value)
                over_40ms = [int]$m.Groups[7].Value
            })
            continue
        }
        if ($l -match 'per frame, median:') { $median = Get-CensusDifferences $l; continue }
        if ($l -match 'per frame, p90:')    { $p90    = Get-CensusDifferences $l; continue }
        # Anything else ends the table: the census is contiguous, and the next line of the
        # log is the next thing the mod had to say.
        break
    }
    if ($phases.Count -eq 0) { return $null }
    return [pscustomobject][ordered]@{
        tables           = $heads.Count
        phases           = $phases.ToArray()
        per_frame_median = $median
        per_frame_p90    = $p90
    }
}

#------------------------------------------------------------------------------------
# The judgement. One home: `run` calls it so the manifest carries a verdict, `verify`
# calls it so a manifest written three weeks ago can be re-judged against the profile it
# names. Nothing else decides a PASS.
#------------------------------------------------------------------------------------


function Add-ExpectCheck($list, [string]$what, [string]$detail, $ok) {
    $list.Add([pscustomobject]@{ check = $what; detail = $detail; pass = [bool]$ok })
}

function Test-Expect($expect, $cell) {
    $checks = New-Object System.Collections.Generic.List[object]
    $mods   = $cell.modules

    foreach ($m in (Get-StringList $expect 'modules')) {
        if ($null -eq $mods) {
            Add-ExpectCheck $checks 'module' "$m - the module list could not be read" $false
        } else {
            Add-ExpectCheck $checks 'module' "$m loaded" (Test-ModuleLoaded $mods $m)
        }
    }
    foreach ($m in (Get-StringList $expect 'modules_absent')) {
        if ($null -eq $mods) {
            Add-ExpectCheck $checks 'module absent' "$m - the module list could not be read" $false
        } else {
            Add-ExpectCheck $checks 'module absent' "$m not loaded" (-not (Test-ModuleLoaded $mods $m))
        }
    }

    # The lines are this process's slice of the log, which is what catches a state reached
    # by toggling: `composition:` is latched once per process, so its absence from a
    # process's own lines means the overlay never built a target in it.
    $lines = @($cell.log_lines)
    foreach ($pat in (Get-StringList $expect 'log_lines')) {
        $hit = @($lines | Where-Object { $_ -like "*$pat*" })
        Add-ExpectCheck $checks 'log line' "'$pat' appears ($($hit.Count)x)" ($hit.Count -gt 0)
    }
    foreach ($pat in (Get-StringList $expect 'log_lines_absent')) {
        $hit = @($lines | Where-Object { $_ -like "*$pat*" })
        Add-ExpectCheck $checks 'log line absent' "'$pat' does not appear ($($hit.Count)x)" ($hit.Count -eq 0)
    }

    # The present mode is NOT among them. It is recorded per stream, with its counts, and
    # it is not a profile's promise: a played cell showed the game's window going
    # `Composed: Flip` in the menu and `Hardware: Independent Flip` on 100 % of its presents
    # once gameplay began, with the overlay's own surface presenting throughout. The mode is
    # a property of what the player is doing, and a profile describes an install.

    # A profile that reproduces a bug expects a CRASH, and a cell of it that comes back
    # HEALTHY is the repro failing - or the bug being fixed - not a pass. HEALTHY is only
    # the default because most profiles are configurations that should simply work.
    #
    # `any` is the third answer, and it is not a weaker CRASH. Some cells are asked a
    # question whose every answer is information: a build old enough to carry a bug that
    # has never been reproduced on this box tells us something whether it crashes or not.
    # Reporting FAIL for a result that is not a failure of the box is how a verifier stops
    # being read, so such a cell records its verdict and asserts nothing about it. The
    # profile must still assert something else - `expect` that asserts nothing is refused.
    $want = $(if ($expect.verdict) { [string]$expect.verdict } else { 'HEALTHY' })
    if ($want -eq 'any') {
        Add-ExpectCheck $checks 'verdict' ("the cell is {0}; this profile asserts no verdict" -f
                                           $cell.verdict) $true
    } else {
        Add-ExpectCheck $checks 'verdict' ("the cell is {0}, expected {1}" -f $cell.verdict, $want) `
                        ($cell.verdict -eq $want)
    }

    $fails = @($checks | Where-Object { -not $_.pass })
    return [pscustomobject]@{
        pass   = ($fails.Count -eq 0)
        failed = $fails.Count
        checks = $checks.ToArray()
    }
}

#------------------------------------------------------------------------------------
# One cell
#------------------------------------------------------------------------------------

function New-CellRecord([int]$index, [string]$verdict, [string]$why, [int]$at, [DateTime]$launchedAt,
                        $proc, $modules, $slice, $teardown, $saveDiffs) {
    return [pscustomobject][ordered]@{
        cell         = $index
        verdict      = $verdict
        why          = $why
        at_seconds   = $at
        launched_utc = $launchedAt.ToUniversalTime().ToString('o')
        game_pid     = $(if ($proc) { $proc.Id } else { $null })
        modules      = $modules
        log_rotated  = $(if ($slice) { $slice.rotated } else { $false })
        log_lines    = $(if ($slice) { @($slice.lines) } else { @() })
        teardown     = $teardown
        # Split, because these are not one kind of fact. The game rewrites Engine.ini and
        # GameConfig.sav on every exit and the mod rewrites _last_stage.txt on every
        # session; printing those four beside the one that matters is how the one that
        # matters gets missed. `save_slot_changed` is the finding - nothing should write a
        # save at a title screen, and a cell that has to be played will write one, which is
        # why a played cell is never a crash cell.
        save_changed      = @(@($saveDiffs) | ForEach-Object { $_.text })
        save_slot_changed = @(@($saveDiffs) | Where-Object { $_.kind -eq 'save slot' } |
                              ForEach-Object { $_.text })
    }
}

function Invoke-Cell([int]$index, [int]$total, [string]$runDir, [int]$hold, [int]$settle,
                     [string]$vaultStamp, $tool) {
    Write-Host ""
    Write-Host ("  cell {0}/{1}" -f $index, $total) -ForegroundColor Cyan

    $mark        = Get-LogMark
    $crashBefore = Get-CrashDirNames
    $exe         = Get-GameExe
    $launchedAt  = Get-Date

    Start-Process -FilePath $exe -WorkingDirectory $bin | Out-Null
    Write-Host ("    launched, settling {0} s before resolving the game" -f $settle)
    $proc = Resolve-GameProcess $launchedAt $settle
    if ($proc -eq 'gone') {
        Write-Host "    CRASH        a game process appeared and was gone before the settle ended" -ForegroundColor Red
        $slice = Get-LogSince $mark
        return (New-CellRecord $index 'CRASH' 'the game exited during the settle' $settle $launchedAt `
                               $null $null $slice $null (Compare-AgainstVault $vaultStamp))
    }
    if (-not $proc) {
        Write-Host ("    NO PROCESS   nothing named Project_Plague-Win64-Shipping was alive after {0} s" -f
                    $settle) -ForegroundColor Red
        return (New-CellRecord $index 'NO PROCESS' 'the game never started' 0 $launchedAt `
                               $null $null $null $null @())
    }
    Write-Host ("    pid {0}" -f $proc.Id)

    # The capture starts once the pid is known, so it is this launch's frames and not
    # whatever Steam's first process presented. Its own bound is the cell's: it outlives
    # neither the game nor the cell, and nothing here has to kill it to read the CSV.
    $capBound = $(if ($Until -eq 'exit') { $MaxMinutes * 60 } else { $hold + 30 })
    $cap = Start-PresentCapture $tool $runDir $index $proc $capBound

    # `hold` holds for a fixed time and closes the game itself. `exit` hands the session to
    # the person at the keyboard and ends when THEY leave the game - a cell that has to be
    # played cannot be timed, and it is never torn down by this script: a kill under a modal
    # box while the game is autosaving is exactly how a save is truncated.
    $session  = ($Until -eq 'exit')
    $deadline = $(if ($session) { (Get-Date).AddMinutes($MaxMinutes) } else { (Get-Date).AddSeconds($hold) })
    if ($session) {
        Write-Host (("    PLAY         the game is yours - quit it from its own menu when you are " +
                     "done (waiting up to {0} min)") -f $MaxMinutes) -ForegroundColor Cyan
    }

    $verdict = $null; $why = ''; $modules = $null; $handoffs = 0
    $t = 0
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 1
        $t++

        $box = Test-CrashWindow
        if ($box) { $verdict = 'CRASH'; $why = $box; break }

        $slice = Get-LogSince $mark
        if (@($slice.lines | Where-Object { $_ -match 'WATCHDOG' }).Count -gt 0) {
            $verdict = 'CRASH'; $why = "the mod's WATCHDOG line"; break
        }
        $new = @(Get-CrashDirNames | Where-Object { $crashBefore -notcontains $_ })
        if ($new.Count -gt 0) {
            $verdict = 'CRASH'; $why = ("a new Saved\Crashes entry: " + ($new -join ', ')); break
        }

        if ($proc.HasExited) {
            # A tracked process that exits while another game process is alive is Steam's
            # hand-off, not a crash. Re-resolve and keep watching.
            $other = @(Get-LiveGameProcesses $launchedAt)
            if ($other.Count -gt 0) {
                $proc = (@($other | Sort-Object StartTime -Descending)[0])
                $handoffs++
                Write-Host ("    hand-off     the process was replaced by pid {0}" -f $proc.Id) -ForegroundColor DarkGray
                $modules = $null
                continue
            }
            if ($session) { $verdict = 'HEALTHY'; $why = "played and left the game after $t s" }
            else          { $verdict = 'CRASH';   $why = 'the process exited' }
            break
        }

        # Read while the process is certainly alive: a teardown that has begun unloads
        # modules, and an empty list would read as "it was never loaded".
        if (-not $modules -and $t -ge 3) { $modules = Get-ProcessModules $proc }
    }
    if (-not $modules) { $modules = Get-ProcessModules $proc }
    if (-not $verdict) {
        if ($session) { $verdict = 'NO VERDICT'; $why = "still in the game after $MaxMinutes min" }
        else          { $verdict = 'HEALTHY';    $why = "held $hold s with none of the crash criteria" }
    }
    $at = $settle + $t

    Write-Host ("    {0,-10} at ~{1} s : {2}" -f $verdict, $at, $why) `
               -ForegroundColor $(if ($verdict -eq 'HEALTHY') { 'Green' } else { 'Red' })

    if ($session) {
        # Nothing to tear down: either the person left the game, or it is still up and the
        # script says so rather than reaching for it.
        $teardown = [pscustomobject]@{ closed = $proc.HasExited; seconds = 0
                                       how = 'a played session is never torn down by this script' }
        if (-not $proc.HasExited) {
            Write-Host "    the game is still running - left alone deliberately" -ForegroundColor Yellow
        }
    } elseif ($verdict -eq 'HEALTHY') {
        $teardown = Close-GameWindow $proc 30
        Write-Host ("    teardown     {0} ({1} s)" -f $teardown.how, $teardown.seconds)
        if (-not $teardown.closed) {
            Write-Host "    it did not answer - taking it off the screen so the next cell is a clean launch" `
                       -ForegroundColor Yellow
            Stop-DecidedCell $proc
        }
    } else {
        Write-Host "    teardown     decided CRASH - killing it out from under its modal box" -ForegroundColor Yellow
        Stop-DecidedCell $proc
        $teardown = [pscustomobject]@{ closed = $true; seconds = 3
                                       how = 'killed - a decided CRASH cannot be closed any other way' }
    }

    $slice = Get-LogSince $mark
    if ($slice.lines.Count -gt 0) {
        Set-Content -LiteralPath (Join-Path $runDir ("cell-{0}.log" -f $index)) `
                    -Value $slice.lines -Encoding UTF8
    }
    Write-Host ("    log          {0} line(s), {1}" -f $slice.lines.Count, $slice.note)

    # The two instruments, read after the game is gone: the capture is complete only once
    # PresentMon has flushed it, and the game's swapchain is named by a line of this
    # cell's own log slice.
    $capture = Stop-PresentCapture $cap
    if ($tool -and -not $capture) {
        # The probe was asked for and there is no capture. The cell says why rather than
        # carrying a silent null that reads, later, like a cell nobody measured.
        $capture = [pscustomobject][ordered]@{ csv = $null; presents = 0; streams = @()
                                               why = $tool.why_not }
    }
    if ($capture) {
        Add-Member -InputObject $capture -NotePropertyName 'game_stream' `
                   -NotePropertyValue (Resolve-GameStream $capture $slice.lines)
        Show-PresentCapture $capture
    }
    $census = $null
    try { $census = Get-CensusTable $slice.lines } catch {
        Write-Host ("    census       could not be read: {0}" -f $_.Exception.Message) -ForegroundColor Yellow
    }
    if ($census) {
        Show-Census $census
    } elseif ($tool) {
        Write-Host ("    census       the mod logged no census table in this cell") -ForegroundColor DarkGray
    }

    $saveDiffs = @(Compare-AgainstVault $vaultStamp)
    $slotDiffs = @($saveDiffs | Where-Object { $_.kind -eq 'save slot' })
    if ($saveDiffs.Count -gt 0) {
        Write-Host ("    play state   {0} file(s) changed: {1}" -f $saveDiffs.Count,
                    ((@($saveDiffs | ForEach-Object { $_.kind }) | Sort-Object -Unique) -join ', ')) `
                   -ForegroundColor DarkGray
    }
    if ($slotDiffs.Count -gt 0) {
        Write-Host ("    SAVE SLOT    {0} file(s) in the save slot changed during this cell" -f $slotDiffs.Count) `
                   -ForegroundColor $(if ($session) { 'DarkGray' } else { 'Yellow' })
        foreach ($d in $slotDiffs) { Write-Host ("                 {0}" -f $d.text) -ForegroundColor DarkGray }
    }

    $rec = New-CellRecord $index $verdict $why $at $launchedAt $proc $modules $slice $teardown $saveDiffs
    Add-Member -InputObject $rec -NotePropertyName 'handoffs' -NotePropertyValue $handoffs
    # The probe's own fields hang off the crash record rather than widening it: every cell
    # is a launch judged by the same criteria first, and a probe adds what it measured.
    if ($tool)    { Add-Member -InputObject $rec -NotePropertyName 'present' -NotePropertyValue $capture }
    if ($census)  { Add-Member -InputObject $rec -NotePropertyName 'census'  -NotePropertyValue $census }
    return $rec
}

#------------------------------------------------------------------------------------
# run
#------------------------------------------------------------------------------------

function Invoke-Run([string]$name) {
    Write-Host ""
    Write-Host ("run {0} -Probe {1} -Cells {2} -Until {3}" -f $name, $Probe, $Cells, $Until) -ForegroundColor Cyan

    Assert-GameClosed 'run'
    $p          = Read-Profile $name
    $profileSha = Get-Sha (Get-ProfilePath $name)

    # The probe, and everything it needs, before the profile is applied: a run that finds
    # out halfway that it cannot measure has still changed the box.
    $spec = Get-Probe $Probe
    if ($Until -eq 'hold' -and $spec.min_hold -gt 0 -and $Hold -lt $spec.min_hold) {
        throw ("probe '$($spec.name)' needs at least $($spec.min_hold) s in the game: below that the " +
               "mod never reaches its 30 s census table and the capture is mostly the loading screen. " +
               "Pass -Hold $($spec.min_hold) or more, or -Until exit to play the cell.")
    }
    Write-Host ("  probe          {0} - {1}" -f $spec.name, $spec.what)
    $inst = Get-Instrument $spec $p
    if ($spec.config_dev) {
        Write-Host ("  instrument     {0}" -f $(
            if ($inst) { (Get-PropertyNames $inst | ForEach-Object { "$_ = $($inst.$_)" }) -join ', ' }
            else { ("nothing - mod.state is '{0}', so nothing of ours runs in this cell to " +
                    "instrument; the capture is still taken") -f (Get-ModState $p) })) `
                   -ForegroundColor $(if ($inst) { 'Gray' } else { 'DarkGray' })
    }
    $tool = Test-ProbeReady $spec
    if ($tool) {
        Write-Host ("  presentmon     {0}{1}" -f $tool.exe,
                    $(if ($tool.capturing) { '' } else { ' - NOT capturing: ' + $tool.why_not })) `
                   -ForegroundColor $(if ($tool.capturing) { 'Gray' } else { 'Yellow' })
    }
    if ($spec.name -eq 'present' -and $Until -eq 'hold') {
        Write-Host (("  note           a held cell sits where the game puts it after a launch, so its " +
                     "PresentMode is the window's and its frame times are not a scene anyone plays. " +
                     "-Until exit is the measurement.")) -ForegroundColor Yellow
    }
    if ($inst -and $CycleMs -gt 0) {
        Write-Host (("  note           the overlay freezes and disappears every {0} ms - that is the " +
                     "census rotating through its layers, not a fault. Stand still and leave the " +
                     "panel closed.") -f $CycleMs) -ForegroundColor Yellow
    }

    if ($DryRun) {
        Test-ProfilePayloads $p $name
        Test-ProfilePreconditions $p $name
        Write-Host ""
        if ($Until -eq 'exit') {
            Write-Host (("  would apply '{0}' and launch it {1} time(s), each held until the game is " +
                         "left (up to {2} min)") -f $name, $Cells, $MaxMinutes)
        } else {
            Write-Host ("  would apply '{0}' and launch it {1} time(s), holding {2} s each" -f $name, $Cells, $Hold)
        }
        Write-Host ("  would write  {0}" -f (Join-Path $runRoot "<stamp>-$name-$Probe"))
        Write-Host ""
        Write-Host "Dry run: nothing was applied and nothing was launched." -ForegroundColor Green
        return
    }

    Invoke-Apply $name $inst
    $cur = Read-JsonFile $currentJson

    $stamp  = New-FreeStamp $runRoot "-$name-$Probe"
    $runDir = Join-Path $runRoot "$stamp-$name-$Probe"
    New-Item -ItemType Directory -Path $runDir -Force | Out-Null

    # A pinned build writes its log where THAT build wrote it, which for 1.1.1 and earlier
    # is beside the DLL - a path the current build never uses. Whatever of those was there
    # before the run stays; whatever the run creates is removed after it, because the lines
    # are already in this run's own cell-N.log and a `wuchang_minimap.log` left in the mod
    # folder reads, later, like the mod regressed to the old path.
    $logsBefore = @{}
    foreach ($f in @(Get-ChildItem -LiteralPath $modDir -Filter "$LogGlob*" -File -ErrorAction SilentlyContinue)) {
        $logsBefore[$f.Name.ToLowerInvariant()] = $true
    }

    # NOT $cells: PowerShell variable names are case-insensitive, so a local $cells is the
    # -Cells parameter, and `$i -le $Cells` then compares an int to a List.
    $cellRecords = New-Object System.Collections.Generic.List[object]
    try {
        for ($i = 1; $i -le $Cells; $i++) {
            $cellRecords.Add((Invoke-Cell $i $Cells $runDir $Hold $Settle $cur.vault $tool))
        }
    } finally {
        # The install comes back whatever the cells did. A run that threw halfway is still
        # a run that applied a profile, and leaving it applied is how the next measurement
        # gets taken against someone else's floor.
        Invoke-Restore

        $stray = @(Get-ChildItem -LiteralPath $modDir -Filter "$LogGlob*" -File -ErrorAction SilentlyContinue |
                   Where-Object { -not $logsBefore.ContainsKey($_.Name.ToLowerInvariant()) })
        foreach ($f in $stray) {
            Invoke-Delete (Resolve-RootPath 'mod' $f.Name) "run: a log this run's build wrote beside the DLL"
        }
        if ($stray.Count -gt 0) {
            Write-Host ("  cleaned        {0} log file(s) the run's build left beside the DLL; the lines are in {1}" -f
                        $stray.Count, $runDir) -ForegroundColor DarkGray
        }
    }

    $ue4ssSha = $(if (Test-Path -LiteralPath $ue4ssDll -PathType Leaf) { Get-Sha $ue4ssDll } else { $null })
    $pinned   = Get-PinnedUe4ss
    $manifest = [pscustomobject][ordered]@{
        schema         = $RunSchema
        stamp          = $stamp
        profile        = $name
        profile_sha256 = $profileSha
        probe          = $Probe
        # What the probe put into the install and what took its numbers: a run three weeks
        # old is compared with today's only if both say how they were instrumented.
        probe_config   = $inst
        presentmon     = $tool
        until          = $Until
        cells_asked    = $Cells
        hold_seconds   = $(if ($Until -eq 'exit') { $null } else { $Hold })
        max_minutes    = $(if ($Until -eq 'exit') { $MaxMinutes } else { $null })
        started_utc    = $cur.applied_utc
        finished_utc   = [DateTime]::UtcNow.ToString('o')
        host           = $env:COMPUTERNAME
        game_root      = $GameRoot
        mod            = [pscustomobject][ordered]@{
            main_dll_md5       = $cur.main_dll_md5
            mod_loaded         = $cur.mod_loaded
            build              = $cur.mod_build
            state              = $cur.mod_state
            ue4ss_sha256       = $ue4ssSha
            ue4ss_pinned       = $pinned.version
            ue4ss_pinned_match = ($ue4ssSha -eq $pinned.sha256)
        }
        machine        = [pscustomobject][ordered]@{
            hags        = (Get-HagsState)
            hw_sch_mode = (Get-HwSchMode)
            monitors    = (Get-Monitors)
            processes   = (Get-WatchedProcessState)
        }
        manual         = $p.manual
        vault          = $cur.vault
        expect         = $p.expect
        cells          = $cellRecords.ToArray()
    }
    foreach ($c in $manifest.cells) {
        Add-Member -InputObject $c -NotePropertyName 'expect_result' `
                   -NotePropertyValue (Test-Expect $p.expect $c)
    }
    $h    = @($manifest.cells | Where-Object { $_.verdict -eq 'HEALTHY' }).Count
    $x    = @($manifest.cells | Where-Object { $_.verdict -eq 'CRASH' }).Count
    $pass = @($manifest.cells | Where-Object { $_.expect_result.pass }).Count
    Add-Member -InputObject $manifest -NotePropertyName 'verdict' -NotePropertyValue (
        [pscustomobject][ordered]@{
            healthy = $h
            crash   = $x
            other   = ($manifest.cells.Count - $h - $x)
            pass    = $pass
            fail    = ($manifest.cells.Count - $pass)
        })
    Write-JsonFile (Join-Path $runDir 'manifest.json') $manifest

    Write-Host ""
    Write-Host ("Run {0} - {1}/{2} healthy, {3} crash" -f $stamp, $h, $manifest.cells.Count, $x) `
               -ForegroundColor $(if ($x -gt 0) { 'Red' } else { 'Green' })
    Write-Host ("manifest       {0}" -f (Join-Path $runDir 'manifest.json'))
    if ($x -gt 0) {
        Write-Host ("A killed game does not give back an exclusive audio endpoint. After several CRASH " +
                    "cells the audio engine can be left clicking with no game running - restart the audio " +
                    "device; it is not the mod.") -ForegroundColor Yellow
    }
    Show-RunVerdict $manifest
}

#------------------------------------------------------------------------------------
# verify - the same judgement, against a manifest written at any time
#------------------------------------------------------------------------------------

function Show-RunVerdict($manifest) {
    Write-Host ""
    Write-Host ("verify {0}" -f $manifest.stamp) -ForegroundColor Cyan
    Write-Host ("  profile        {0}  ({1})" -f $manifest.profile, $manifest.profile_sha256.Substring(0, 16))
    Write-Host ("  probe          {0}{1}" -f $manifest.probe,
                $(if ($manifest.probe_config) {
                      '  instrumented: ' +
                      ((Get-PropertyNames $manifest.probe_config |
                        ForEach-Object { "$_ = $($manifest.probe_config.$_)" }) -join ', ')
                  } else { '' }))
    Write-Host ("  main.dll md5   {0}" -f $(if ($manifest.mod.main_dll_md5) { $manifest.mod.main_dll_md5 }
                                            else { 'no main.dll installed' }))
    Write-Host ("  mod loaded     {0}" -f $(if ($manifest.mod.mod_loaded) { 'yes' } else { 'NO' }))
    Write-Host ("  UE4SS          {0}" -f $(if ($manifest.mod.ue4ss_pinned_match)
                                            { "$($manifest.mod.ue4ss_pinned)  MATCH" }
                                            else { 'NOT the pinned build' }))
    # Which binary took the numbers, for the same reason the build's md5 is the first line
    # of every report: a measurement is comparable with another only if both say so.
    if ($manifest.presentmon) {
        Write-Host ("  presentmon     {0}" -f $(
            if (-not $manifest.presentmon.capturing) { 'no capture: ' + $manifest.presentmon.why_not }
            elseif ($manifest.presentmon.pinned_match) { "$($manifest.presentmon.pinned)  MATCH" }
            else { "$($manifest.presentmon.exe) - NOT the pinned build" })) `
                   -ForegroundColor $(if ($manifest.presentmon.capturing) { 'Gray' } else { 'Yellow' })
    }
    foreach ($c in $manifest.cells) {
        $r = $c.expect_result
        Write-Host ("  cell {0}  {1}  {2,-10} {3}" -f $c.cell,
                    $(if ($r.pass) { 'PASS' } else { 'FAIL' }), $c.verdict, $c.why) `
                   -ForegroundColor $(if ($r.pass) { 'Green' } else { 'Red' })
        foreach ($chk in @($r.checks | Where-Object { -not $_.pass })) {
            Write-Host ("      x {0}: {1}" -f $chk.check, $chk.detail) -ForegroundColor Red
        }
        if ($c.present) {
            $gs = $c.present.game_stream
            $st = $null
            if ($gs -and $gs.address) {
                foreach ($s in @($c.present.streams)) {
                    if ((Get-AddressKey $s.address) -eq (Get-AddressKey $gs.address)) { $st = $s; break }
                }
            }
            if ($st) {
                $top = ''
                foreach ($m in (Get-PropertyNames $st.present_modes)) {
                    if (-not $top) { $top = ("{0} {1:N1} %" -f $m, (100.0 * $st.present_modes.$m / $st.presents)) }
                }
                Write-Host ("      present: {0} on {1}, {2} present(s), median {3:N3} ms; {4} - {5}" -f
                            $top, $st.address, $st.presents, $st.stats.frame_time_ms.median,
                            $gs.rule, $c.present.csv) -ForegroundColor DarkGray
            } else {
                Write-Host ("      present: no capture of the game's own swapchain - {0}" -f
                            $(if ($c.present.why) { $c.present.why }
                              elseif ($gs) { $gs.rule } else { 'no streams' })) -ForegroundColor Yellow
            }
        }
        if ($c.census -and $c.census.per_frame_median) {
            $d = $c.census.per_frame_median
            Write-Host (("      census:  per frame, median - composition {0:+0.000;-0.000} ms, the " +
                         "overlay's frame {1:+0.000;-0.000}, the compositor {2:+0.000;-0.000}, our " +
                         "threads {3:+0.000;-0.000}; all of it {4:+0.000;-0.000}") -f
                        $d.composition, $d.overlay_frame, $d.compositor, $d.background, $d.all_of_it) `
                       -ForegroundColor DarkGray
        }
        $slot = Get-StringList $c 'save_slot_changed'
        $play = Get-StringList $c 'save_changed'
        if ($slot.Count -gt 0) {
            Write-Host ("      ! the SAVE SLOT changed during this cell: {0}" -f ($slot -join '; ')) `
                       -ForegroundColor Yellow
        } elseif ($play.Count -gt 0) {
            Write-Host ("      play state: {0} file(s) the game and the mod rewrite on every session" -f
                        $play.Count) -ForegroundColor DarkGray
        }
    }
    Write-Host ("  {0}/{1} cell(s) PASS" -f $manifest.verdict.pass, $manifest.cells.Count) `
               -ForegroundColor $(if ($manifest.verdict.fail -eq 0) { 'Green' } else { 'Red' })
}

function Resolve-RunDir([string]$stamp) {
    if (-not $stamp) {
        $newest = @(Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue |
                    Sort-Object Name -Descending | Select-Object -First 1)
        if ($newest.Count -eq 0) { throw "No run has been recorded under '$runRoot'." }
        return $newest[0].FullName
    }
    if (Test-Path -LiteralPath $stamp -PathType Container) { return (Resolve-Path -LiteralPath $stamp).Path }
    $hits = @(Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -like "$stamp*" })
    if ($hits.Count -eq 0) { throw "No run under '$runRoot' matches '$stamp'." }
    if ($hits.Count -gt 1) {
        throw ("'{0}' matches {1} runs: {2}." -f $stamp, $hits.Count,
               ((@($hits | ForEach-Object { $_.Name })) -join ', '))
    }
    return $hits[0].FullName
}

function Invoke-Verify([string]$stamp) {
    $dir  = Resolve-RunDir $stamp
    $path = Join-Path $dir 'manifest.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "'$dir' has no manifest.json." }
    $manifest = Read-JsonFile $path
    if ($manifest.schema -ne $RunSchema) {
        throw "'$path' is schema '$($manifest.schema)', not $RunSchema."
    }
    if (@($manifest.cells).Count -ne $manifest.cells_asked) {
        throw ("'{0}' records {1} cell(s) but asked for {2} - the run did not finish." -f
               $path, @($manifest.cells).Count, $manifest.cells_asked)
    }

    # Re-judged from the profile on disk now, not from the verdict the run wrote: keeping
    # the evidence is what makes that possible. A profile that changed since the run says
    # so rather than being judged against silently.
    $p      = Read-Profile $manifest.profile
    $shaNow = Get-Sha (Get-ProfilePath $manifest.profile)
    if ($shaNow -ne $manifest.profile_sha256) {
        Write-Host ("  profile.json changed since this run ({0} -> {1}); judging against it as it is now." -f
                    $manifest.profile_sha256.Substring(0, 16), $shaNow.Substring(0, 16)) -ForegroundColor Yellow
    }
    foreach ($c in $manifest.cells) {
        $r = Test-Expect $p.expect $c
        if ($c.PSObject.Properties.Name -contains 'expect_result') { $c.expect_result = $r }
        else { Add-Member -InputObject $c -NotePropertyName 'expect_result' -NotePropertyValue $r }
    }
    $pass = @($manifest.cells | Where-Object { $_.expect_result.pass }).Count
    $manifest.verdict.pass = $pass
    $manifest.verdict.fail = (@($manifest.cells).Count - $pass)
    Show-RunVerdict $manifest
    if ($manifest.verdict.fail -gt 0) {
        throw "$($manifest.verdict.fail) cell(s) did not match the profile's expect."
    }
}

function Invoke-Runs {
    $dirs = @(Get-ChildItem -LiteralPath $runRoot -Directory -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending)
    if ($dirs.Count -eq 0) { Write-Host "  (no runs yet)"; return }
    foreach ($d in $dirs) {
        $m = Join-Path $d.FullName 'manifest.json'
        if (-not (Test-Path -LiteralPath $m -PathType Leaf)) {
            Write-Host ("  {0,-44} no manifest - the run did not finish" -f $d.Name) -ForegroundColor Yellow
            continue
        }
        $j = Read-JsonFile $m
        # The pass count is the one recorded when the run was judged. `verify` is the judge,
        # and it re-judges against the profile as it stands now - so a profile that has moved
        # since is said here rather than leaving two numbers to disagree in silence.
        $moved = $false
        $pf = Get-ProfilePath ([string]$j.profile)
        if (Test-Path -LiteralPath $pf -PathType Leaf) { $moved = ((Get-Sha $pf) -ne $j.profile_sha256) }
        Write-Host ("  {0,-44} {1} healthy / {2} crash, {3} pass when judged{4}" -f
                    $d.Name, $j.verdict.healthy, $j.verdict.crash, $j.verdict.pass,
                    $(if ($moved) { " - the profile has moved since; run verify $($j.stamp)" } else { '' })) `
                   -ForegroundColor $(if ($moved) { 'Yellow' } else { 'Gray' })
    }
}

#====================================================================================
# Dispatch
#====================================================================================

try {
    if ($DryRun) {
        Write-Host "-DryRun: every write below is printed and not performed." -ForegroundColor Yellow
    }
    switch ($Verb) {
        'status' { Invoke-Status }
        'list'   { Invoke-List }
        'vault'  {
            Assert-GameClosed 'vault'
            Write-Host ""
            $null = Invoke-Vault
        }
        'apply' {
            if (-not $Name) { throw "apply needs a profile name: repro.ps1 apply <profile>." }
            Invoke-Apply $Name
        }
        'restore' { Invoke-Restore }
        'saves-restore' {
            if (-not $Name) { throw "saves-restore needs a vault stamp: repro.ps1 saves-restore <stamp>." }
            Invoke-SavesRestore $Name
        }
        'run' {
            if (-not $Name) { throw "run needs a profile name: repro.ps1 run <profile>." }
            Invoke-Run $Name
        }
        'verify' { Invoke-Verify $Name }
        'runs'   { Write-Host "Runs" -ForegroundColor Cyan; Invoke-Runs }
    }
} catch {
    Write-Host ""
    Write-Host ("FAILED  {0}" -f $_.Exception.Message) -ForegroundColor Red
    # A .NET message on its own ("Argument types do not match") names neither the verb nor
    # the line, and this script's failures are read by someone who was not watching.
    if ($_.InvocationInfo) {
        Write-Host ("        at line {0}: {1}" -f $_.InvocationInfo.ScriptLineNumber,
                    $_.InvocationInfo.Line.Trim()) -ForegroundColor DarkGray
    }
    exit 1
}
exit 0
