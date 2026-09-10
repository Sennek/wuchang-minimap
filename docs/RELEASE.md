# Cutting a release

Everything here happens on the build machine with the **game closed**. A release is a version
stamp, a commit, a tag, one `tools\package.ps1` run and an upload; nothing is assembled by hand.
`x.y.z` below is the version you are cutting.

## 0. Before you start

Hard preconditions. `tools\check_release.ps1` enforces the mechanical ones — run it now and fix
whatever it prints, before touching a version number:

```powershell
.\tools\check_release.ps1
```

- [ ] **`LICENSE` names a real copyright holder.** No `<AUTHOR>` and no other `<PLACEHOLDER>`: a
      placeholder makes the MIT grant unattributable. Both scripts fail on it; check it by eye
      anyway.
- [ ] **Close the game.** `deploy.ps1` and the packaging both touch files the game locks, and the
      in-game smoke test at the end needs a fresh launch.
- [ ] `git status` is **clean**. Every step below refuses to run otherwise.
- [ ] `tools\CHANGELOG.template.md`'s top `## x.y.z` heading is the version you are about to cut,
      and its body is one line per change, and only changes.
- [ ] `THIRD_PARTY_NOTICES.md` still matches `third_party\`, and `third_party\VENDORING.md` with
      it.
- [ ] `README.md` describes the keys the build actually ships, and names the UE4SS build it is
      compiled against.

## 1. Stamp the version, commit, tag

Stamp first, as its own step: `package.ps1` refuses a dirty tree because `BUILD_INFO.txt` names a
commit hash, and `-Version` itself dirties the tree, so stamping and packaging in one run would
record the commit from *before* the stamp.

```powershell
.\tools\package.ps1 -StampOnly -Version x.y.z
git commit -am "release x.y.z"
git tag vx.y.z
```

`-StampOnly` rewrites `src\version.hpp` and `xmake.lua`'s `set_version` and stops. Nothing else may
change either one.

> Passing `-Version` to a normal packaging run works and warns about exactly this. Do not use it
> for a release.

## 2. Build the package

```powershell
.\tools\package.ps1
```

No `-Version`: the tree already carries the right number and now matches the tagged commit, so
`BUILD_INFO.txt` names **that** commit exactly.

This runs `build.ps1` (compile + the offline `markers_test`, which must report **0 failures**),
assembles `dist\WuchangMinimap-x.y.z\`, smoke-checks it, runs `check_release.ps1` over the
assembled tree, zips it and round-trips the zip. It writes two archives:

| File | What it is |
|---|---|
| `dist\WuchangMinimap-x.y.z.zip` | what a player downloads |
| `dist\WuchangMinimap-x.y.z-symbols.zip` | `main.pdb` + `BUILD_INFO.txt` |

Any failure stops before the zip is written. **Do not work around a smoke-check or consistency
failure by zipping the folder yourself.**

Need a `.rar` as well? Double-click `make_rar.cmd` (or run `.\tools\make_rar.ps1`) instead of
`package.ps1`: same packaging, same checks, same refusals, plus a tested and round-tripped
`dist\WuchangMinimap-x.y.z.rar`.

**Keep the symbols zip.** It is the only way to read a crash dump from that exact build, and
`main.pdb` otherwise lives only in the gitignored `build\` folder. Upload it as an optional file or
archive it with the tag, never as the main download.

## 3. Eyeball `dist\`

| Path | What to check |
|---|---|
| `BUILD_INFO.txt` | version, commit hash and UE4SS build are the ones you expect; **no `DIRTY`**, and the commit is the tagged one |
| `README.md`, `CHANGELOG.md` | the changelog's top section is this version |
| `LICENSE` | real author name, no placeholder |
| `THIRD_PARTY_NOTICES.md` | present |
| `ue4ss\Mods\WuchangMinimap\dlls\main.dll` | the only file in `dlls\` — **no `main.pdb`** |
| `...\config_wuchang_minimap.txt` | present; **no `..._dev.txt`** (the script refuses, but look) |
| `...\maps\` | `maps.json` + five `chapter<N>\` folders of PNGs |
| `...\markers\` | `chapter1..5.json`, `chapterdlc.json`, `shrines.json`, `items.json`; **no `*.sample.json`** |
| `...\enabled.txt` | present (empty file — that is correct) |
| the zip | the console printed `zip round-trip OK` |

The script prints the size and file count; a sudden change in either is worth understanding before
uploading.

## 4. Install it and smoke-test in the game

Install the packaged zip **the way a player would** — unzip into
`<Game>\Project_Plague\Binaries\Win64\` — not with `deploy.ps1`, which installs a different,
PDB-bearing layout and cannot catch a packaging mistake. Then launch the game and check:

- [ ] `UE4SS.log` shows **`WuchangMinimap vX.Y.Z loaded`** with the version you just cut.
- [ ] The minimap appears in-world once a save is loaded.
- [ ] **F2** opens the settings panel; the version in its title matches.
- [ ] **M** opens the full map; the chapter you are in is drawn and shrines are listed.
- [ ] The x-ray toggle turns marker see-through mode on and off.
- [ ] Quit and confirm `wuchang_minimap.log` in the mod folder has no errors and no per-frame spam.

If anything here fails, the release does not ship — fix, re-tag.

## 5. Upload to Nexus Mods

- File: `dist\WuchangMinimap-x.y.z.zip`, named exactly that (Nexus shows the file name).
- Version field: `x.y.z`, matching the tag and `BUILD_INFO.txt`.
- Changelog: paste the `## x.y.z` section of `tools\CHANGELOG.template.md`.
- Description: `docs\NEXUS.md`, BBCode editor (not rich text).
- **Requirements tab**: UE4SS for Wuchang: Fallen Feathers, mod **384**, file version **1.79** —
  build `v3.0.1-934-gcac01ee2`. Put the file version in the requirement note as well as the
  description: a mismatch produces no overlay and no in-game message. It is **not** bundled.
- Tick "this mod contains files derived from the game's data" if the upload form asks; see the
  "Game data" section of `THIRD_PARTY_NOTICES.md`.
- Screenshots: the checklist at the end of `docs\NEXUS.md`. The first image is the mod page
  thumbnail.

## 6. After

- [ ] `git push && git push --tags`.
- [ ] Deploy the same build to your own game with `.\deploy.ps1`, so your install and the published
      one are the same code.
- [ ] Older `dist\WuchangMinimap-*` folders and zips can be deleted — `dist\` is gitignored and
      every package is reproducible from its tag. **Keep the symbols zip of any version still in
      the wild.**
