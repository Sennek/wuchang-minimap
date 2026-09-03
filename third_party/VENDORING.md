# Vendoring third_party/

Three libraries are vendored as a **plain copy of upstream sources**, not as git
submodules and not through a package manager. This file is the provenance record: where
each one came from, which version, and what (if anything) was changed locally.

Why copies at all: the mod has to be buildable on a machine that can compile UE4SS's
headers and nothing else (see `docs/DEVELOPMENT.md`, "Why an import library?"), and two
of the three are header-only or near enough that a submodule buys nothing. The copies
are small — 47 files in total.

All three were dropped in one commit, `b0d4b60` ("Bootstrap WuchangMinimap UE4SS C++
mod"), on **2026-09-02**.

> **Honest gap:** no upstream commit sha was recorded when these were copied in, so the
> table below identifies each drop by its *version*, which is checked against a macro or
> a copyright range in the vendored source itself — not by sha. Every version below is
> verifiable from the files in this repository today (the "Verified by" column), and the
> "Re-check" command at the bottom is how you confirm a tree is unmodified. When any of
> these is next updated, record the sha here as well.

| Library | Version | Upstream | Upstream tag | Verified by |
|---|---|---|---|---|
| Dear ImGui | 1.92.9b | <https://github.com/ocornut/imgui> | `v1.92.9b` | `IMGUI_VERSION` in `imgui/imgui.h` (`"1.92.9b"`, `IMGUI_VERSION_NUM 19291`) |
| MinHook (+ HDE32/64) | 1.3.3 | <https://github.com/TsudaKageyu/minhook> | `v1.3.3` | see "MinHook has no version macro" below |
| {fmt} | 11.2.0 | <https://github.com/fmtlib/fmt> | `11.2.0` | `FMT_VERSION` in `fmt/include/fmt/base.h` (`110200`) |

## Local modifications

**None.** No file under `third_party/` has been edited. Everything this project needed
to change is done from the outside:

- ImGui is configured entirely through `xmake.lua` defines and at runtime; `imconfig.h`
  is upstream's, with every `#define` still commented out.
- MinHook is compiled as-is by the `minhook` target in `xmake.lua`.
- fmt is used header-only via `FMT_HEADER_ONLY=1`, set in `xmake.lua`.

If that ever stops being true, the modification must be listed here *and* the entry in
`THIRD_PARTY_NOTICES.md` must say the library is modified — both licences require it.

## What was copied, and what was left out

Each drop is a **subset** of upstream: only the files the build needs.

- **imgui** (18 files): the core (`imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`,
  `imgui_widgets.cpp`, `imgui_demo.cpp`), the headers, the three `imstb_*` headers,
  `misc/cpp/imgui_stdlib.*`, the `imgui_impl_dx12` and `imgui_impl_win32` backends, and
  `LICENSE.txt`. No examples, no other backends, no docs.
- **minhook** (14 files): `include/MinHook.h`, `src/{buffer,hook,trampoline}.c` plus
  their headers, `src/hde/` (hde32, hde64, tables, `pstdint.h`), and `LICENSE.txt`. No
  MSVC project files, no DLL build, no tests.
- **fmt** (15 files): `include/fmt/*.h` and `LICENSE`. No `src/`, since it is compiled
  header-only. fmt 11.2.0 is **not a choice** — UE4SS's `DynamicOutput/Output.hpp`
  includes `<fmt/core.h>` and UE4SS itself pins 11.2.0, so this has to match.

Because these are subsets, a naive `diff -r` against a full upstream checkout reports
the *absent* files as differences. Only files present here are meaningful to compare.

## MinHook has no version macro

`MinHook.h` defines no version, which is why the repo has previously carried two
different answers (`THIRD_PARTY_NOTICES.md` said 1.3.3, `docs/DEVELOPMENT.md` said
1.3.4). It is **1.3.3**, on this evidence:

1. Every source file and `LICENSE.txt` carries `Copyright (C) 2009-2017 Tsuda Kageyu`.
   v1.3.3 is the 2017 release; upstream bumped the range after it.
2. There is no ARM64 support anywhere in the tree (`grep -r ARM64 third_party/minhook`
   finds nothing). Upstream master gained ARM64 handling after 1.3.3, so this predates
   that work.
3. The exported API is exactly 1.3.3's, including `MH_QueueEnableHook`,
   `MH_QueueDisableHook`, `MH_ApplyQueued`, `MH_CreateHookApiEx` and
   `MH_StatusToString`, with nothing added.

**1.3.3 is also the newest tagged MinHook release**, so "latest release" and "1.3.3"
are the same statement — the 1.3.4 in the old docs referred to no released version.

## Re-check a tree against upstream

```powershell
# Example for MinHook. Same shape for imgui (v1.92.9b) and fmt (11.2.0).
git clone --depth 1 --branch v1.3.3 https://github.com/TsudaKageyu/minhook C:\tmp\minhook
# Compare only the files we actually vendor:
Get-ChildItem -Recurse -File third_party\minhook | ForEach-Object {
    $rel = $_.FullName.Substring((Resolve-Path third_party\minhook).Path.Length + 1)
    $up  = Join-Path C:\tmp\minhook $rel
    if (-not (Test-Path $up)) { "ONLY HERE: $rel" }
    elseif ((Get-FileHash $_.FullName).Hash -ne (Get-FileHash $up).Hash) { "DIFFERS:   $rel" }
}
```

Silence means the vendored subset is byte-identical to that tag. Run it when updating a
library, and record the sha in the table above while you have the clone.

## Updating one of these

1. Clone upstream at the new tag, copy over **only** the files already present here
   (keep the subset — adding files means teaching `xmake.lua` about them).
2. Record the new version, tag **and sha** in the table above, with today's date.
3. Update the version and the licence text in `THIRD_PARTY_NOTICES.md`. Both ImGui's MIT
   and MinHook's BSD-2 require the notice to travel with the binary, so the notices file
   is the compliance artefact, not a courtesy.
4. Update the "Vendored versions" table in `docs/DEVELOPMENT.md`.
5. Rebuild and run `tools\check_release.ps1`, which cross-checks the version strings.
6. For fmt specifically: do **not** update it independently of UE4SS. The version must
   equal the one UE4SS pins, or `Output::send` formats with a different fmt than the
   UE4SS DLL was built with.
