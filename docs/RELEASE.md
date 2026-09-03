# Cutting a release

Everything here happens on the build machine with the **game closed**. A release is
one `tools\package.ps1` run plus a commit and a tag; nothing is assembled by hand.

## 0. Before you start

- [ ] **Close the game.** `deploy.ps1` and the packaging both touch files the game
      locks, and the in-game smoke test at the end needs a fresh launch anyway.
- [ ] `git status` is **clean**. `package.ps1` refuses to run otherwise (it records the
      commit hash in `BUILD_INFO.txt`, which would be a lie for a dirty tree). Use
      `-AllowDirty` only when iterating on the packaging script itself.
- [ ] `tools\CHANGELOG.template.md` has a section for the version you are about to cut,
      with the user-visible changes written for players, not for the repo.
- [ ] `LICENSE` names a real copyright holder (no `<AUTHOR>` placeholder left) and
      `THIRD_PARTY_NOTICES.md` still matches `third_party\` (check the version numbers
      if anything was updated).
- [ ] `README.md` (the short user-facing one) and `tools\INSTALL_GUIDE.html` describe
      the keys the build actually ships.

## 1. Build the package

```powershell
.\tools\package.ps1 -Version 1.0.0
```

This: stamps `src\version.hpp` + `xmake.lua`, runs `build.ps1` (compile + the offline
`markers_test`, which must report **0 failures**), assembles `dist\WuchangMinimap-1.0.0\`,
smoke-checks it, zips it to `dist\WuchangMinimap-1.0.0.zip` and round-trips the zip.

Any failure stops before the zip is written. Do not work around a smoke-check failure by
zipping the folder yourself.

## 2. Eyeball `dist\`

Open `dist\WuchangMinimap-1.0.0\` and confirm:

| Path | What to check |
|---|---|
| `BUILD_INFO.txt` | version, commit hash and UE4SS build are the ones you expect; no `DIRTY` |
| `INSTALL_GUIDE.html` | opens in a browser, version and date filled in (no `@@VERSION@@`) |
| `README.md`, `CHANGELOG.md` | changelog's top section is this version |
| `LICENSE`, `THIRD_PARTY_NOTICES.md` | present, real author name |
| `ue4ss\Mods\WuchangMinimap\dlls\main.dll` | the only file in `dlls\` - **no `main.pdb`** |
| `...\config_wuchang_minimap.txt` | present; **no `..._dev.txt`** (the script refuses, but look) |
| `...\maps\` | `maps.json` + five `chapter<N>\` folders of PNGs |
| `...\markers\` | `chapter1..5.json`, `shrines.json`, `items.json`; **no `*.sample.json`** |
| `...\enabled.txt` | present (empty file - that is correct) |
| the zip | ~50 MB, and the console printed `zip round-trip OK` |

The script also prints the size and file count; a sudden change in either is worth
understanding before uploading.

## 3. Commit and tag

```powershell
git commit -am "release 1.0.0"
git tag v1.0.0
```

The commit exists because `-Version` rewrote `src\version.hpp` and `xmake.lua`. Tag the
commit that carries those numbers, so the tag and `BUILD_INFO.txt` agree about which
tree the zip came from.

> The zip in `dist\` was built from the commit *before* this one (the stamp is the only
> difference). If you want the recorded hash to be the tagged commit exactly, re-run
> `.\tools\package.ps1` with no `-Version` after committing.

## 4. Install it and smoke-test in the game

Install the packaged zip the way a player would (unzip into
`<Game>\Project_Plague\Binaries\Win64\`), not with `deploy.ps1` - the point is to test
what people download. Then launch the game and check:

- [ ] The UE4SS console / `UE4SS.log` shows the start-up line **`WuchangMinimap vX.Y.Z loaded`**
      with the version you just cut.
- [ ] The minimap appears in-world once a save is loaded.
- [ ] **F2** opens the settings panel; the version in its title matches.
- [ ] **M** opens the full map; the chapter you are in is drawn and shrines are listed.
- [ ] The x-ray toggle turns marker see-through mode on and off.
- [ ] Quit and confirm `wuchang_minimap.log` in the mod folder has no errors and no
      per-frame spam.

If anything here fails, the release does not ship - fix, re-tag.

## 5. Upload to Nexus Mods

- File: `dist\WuchangMinimap-1.0.0.zip`, named exactly that (Nexus shows the file name).
- Version field: `1.0.0`, matching the tag and `BUILD_INFO.txt`.
- Changelog: paste the `## 1.0.0` section of `tools\CHANGELOG.template.md` (the rendered
  copy in the package, `CHANGELOG.md`, already has the placeholders expanded) into the
  Nexus changelog box.
- Description: `docs\NEXUS.md`.
- Requirements: state UE4SS (the build named in `BUILD_INFO.txt`) - it is **not** bundled.
- Tick "this mod contains files derived from the game's data" if the upload form asks;
  see the "Game data" section of `THIRD_PARTY_NOTICES.md`.

## 6. After

- [ ] `git push && git push --tags` if there is a remote.
- [ ] Deploy the same build to your own game with `.\deploy.ps1` so your install and the
      published one are the same code.
- [ ] Older `dist\WuchangMinimap-*` folders and zips can be deleted - `dist\` is
      gitignored and every package is reproducible from its tag.
