#!/usr/bin/env python3
r"""Build `markers/npcs.json` - the game's NPC roster, offline, with no `.usmap`.

WHY
---
Every `npc` marker in `markers/chapter*.json` read the literal label "NPC",
which is exactly as useful as no label at all when the map draws fifty of
them.  Bosses were fixed by finding the game's
own English name for the class (`build_bosses.py`); this does the same for the
78 descendants of `BP_NPC_C`.

THE ROUTE (and the three routes that do NOT work)
-------------------------------------------------
`DT_AiTable` is the boss route and it was checked FIRST because the code
already existed: **0 of the 78 `BP_NPC_C` descendants have a row in it**
(255 rows, 253 distinct classes, none an NPC).  NPCs are not AI-table entries.
Two more dead ends, both measured: there is no `DT_Npc*` table that keys a
blueprint class (`DT_NPCTask`, `DT_NpcFriendData`, `DT_NpcTaskIcon` are
task/friendship data, not a class roster), and pairing the dialogue code in a
class name (`XYZ`, `DY`, `HJE`) with the `NewStringTable/NPC_<code>_<n>`
dialogue lines yields **one** usable vote across 681 dialogue assets - and that
one vote (`DY` -> "Duryal, the King of Ancient Shu") *contradicts* the answer
the `DY_NPC_*` blueprints themselves carry ("Storyteller"), so the whole
code-based idea is unsafe and is not used.

What does work is direct and needs no property decoding at all: **the NPC
blueprint asset embeds its own localisation key.**  A cooked `FText` keeps its
namespace and key as plain strings, so `HJE_NPC_NoWeapon.uasset` literally
contains `npc_Dianame_01`, and `MMGame.locres` maps that to "Huang Jian'e:".
Strip the speaker colon and that is the game's own English name for the class.
The key list came out of a regex histogram over all 7 213 English locres keys
(the `build_items.py` lesson: histogram the prefixes, never hand-list them) -
`npc` is a 61-key family, `npc_Dianame_01..43` (dialogue speaker names) plus
`npc_name_01..18` (the same people without the colon).  Note the two numberings
are NOT the same person (`npc_Dianame_17` is Wu Gang, `npc_name_17` is He
Youzai), so only the key actually embedded in the asset is ever used.

63 of the 78 classes carry a key.  The fallback chain for the rest is ordered
by how WRONG a rule can be, not by how often it fires (the `build_bosses.py`
lesson - a super-chain rule renamed a Chongsheng variant after another boss):

1. `own`     - a key inside the class' own `.uasset` / `.uexp`.
2. `folder`  - another class in the same `Content/Game/AI/npc/<F>/` directory
               resolved one, and every rule-1 class in that directory agrees on
               it.  A directory is one character in this project's layout
               (checked: zero directories disagree), which is why this beats a
               super-chain walk - `DY_NPC_ErHu_C` is the storyteller's fiddle
               variant and sits beside `DY_NPC_C`.
3. `ref`     - the class' package REFERENCES an asset under
               `.../AI/npc/<F>/` whose name resolved, AND the class name shares
               the folder's own token, case-insensitively.  Both halves are
               required: `BP_NPC_xuanyangzi_yandou_C` references
               `AI/npc/NPC_XuanYangZi/ABP_NPC_Xuanyangzi` *and* says
               "xuanyangzi", so it is Xuanyangzi.  A bare substring rule would
               have handed `BP_NPC_cunmin_C` (generic villagers) the name of
               `NPC_Cunminlaofu` ("Qiao Ying", the old village woman), which is
               the exact mistake the boss work already made once.
4. `pinyin`  - a three-entry hand table for tokens whose Chinese is known and
               whose class the data leaves anonymous.  Auditable by design:
               `via` says `pinyin`, so nothing here can be mistaken for a name
               the game supplied.
5. `class-name` - the transliteration in the class name, tidied.

`NO_NAME` classes deliberately keep the generic category label - see the
comment on that table; a two-to-five letter initialism is not a name and
"DKDC" on the map would be worse than the category's own label.

    python build_npcs.py                  # -> ..\..\markers\npcs.json
    python build_npcs.py --report         # + per-class witnesses and placement

Like `items.json` and `bosses.json` this is a **toolchain artifact**: the names
are baked into `chapter*.json` by `extract_markers.py`, the runtime never reads
it and `package.ps1` does not ship it.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys

import build_items as BI
import build_bosses as BB
import class_graph
import pakmaps
import provenance                          # noqa: E402

SCHEMA = "wuchang-minimap-npcs/1"
NPC_BASE = "BP_NPC_C"

# Where the per-character NPC blueprint directories live.  One directory is one
# character (verified: no directory's blueprints disagree about their key).
NPC_DIR = "Content/Game/AI/npc/"

# The locres key families that name a character.  `npc_Dianame_<nn>` is the
# dialogue speaker plate ("Huang Jian'e:") and `npc_name_<nn>` the bare name;
# the two are numbered independently, so only the key embedded in the asset is
# used and the colon is stripped afterwards.
KEY_RX = re.compile(rb"npc_(?:Dianame|name)_\d+")
KEY_STR = re.compile(r"^npc_(?:Dianame|name)_\d+$")

# Asset paths referenced by a package, used by the `ref` rule.
REF_RX = re.compile(rb"/Game/Game/AI/npc/(NPC_[A-Za-z0-9_]+)/")

# Descendants of `BP_NPC_C` that are not people: they have their own marker
# category (`shrine`, `door`, `other`) via `marker_classes.EXACT`, so
# a name from this file would never be used and listing them would only make
# the roster look incomplete.
NOT_PEOPLE = {
    "BP_RebornFire_C",          # shrine
    "ItemCollectionBox_C",      # other - the player's storage box
    "BP_PuzzlesDoor_C",         # door
    "BP_KlesaCleaner_C",        # other - world mechanism
}

# Classes that keep the generic category label on purpose.
#
# The DKDC family is the interesting one, and it is what put the `note`
# category on the map (76 markers).  What the data says:
# every placed instance carries a per-instance `FString` naming a read-point id
# (`NPC_DG_READ08`, `NPC_FYZ_READ03`, `NPC_SWC_READ01`), the blueprint has no
# character mesh at all, its only interaction string is `ui_263` = "Check", and
# it spawns the `NS_Hint01` hint particle.  `ReadPointSP_NPC_C` (14 markers,
# `NS_Hint01_Blue`) and `Letter01_NPC_C` (1) are the same thing.  So they are
# readable inscriptions / notes, and **"Note"** is what they get: a description,
# never a character's name, so it cannot be wrong about identity - and the same
# word the category itself uses.  "Reading point" was our internal description of
# the CLASS; on the map it made a note's glyph and its label disagree about what
# the thing is, and the label is the half the player reads.
# All three are typed `note` by `marker_classes.EXACT` (the user's call on
# 2026-09-03; the `NPC` regex would otherwise have typed the last two `npc`).
#
# The read ids themselves resolve nowhere offline: they are not in
# `MMGame.locres` (the only locres in the game) and a scan of all 2 150
# dialogue / DataTable / StringTable assets for `NPC_DG_READ08` finds nothing,
# so the DialoguePlugin resolves them at runtime from data not in the paks.
READ_POINT = {
    "DKDC_NPC_C": "Note",
    "ReadPointSP_NPC_C": "Note",
    "Letter01_NPC_C": "Note",
}

# `BP_WeaponRefrom_C` is the weapon-reforge station (its only string is
# `ui_219` = "Use" and it references `DialogGroup/WorkBench`).  It is never
# placed in any level, so nothing reads this entry; it is here so the roster
# accounts for all 78 classes.
NO_NAME = {
    "BP_WeaponRefrom_C": "Workbench",
}

# Hand-mapped tokens: the Chinese behind the token is known, the data names the
# class nowhere, and the class IS placed.  Kept to three entries and reported
# as `via: pinyin` so a reviewer can veto any of them without reading code.
#
# * `cunmin` = 村民, villager.  23 placed instances of generic crowd villagers
#   in five levels; the game gives them no dialogue and no name key.
# * `XM` = XinMo = 心魔, the inner demon.  Two steps, both from asset naming:
#   this project abbreviates XinMo as XM (`.../mat/XinMo/M_XM_ANQ_Body`, and
#   `SFX_NPC/SFX_NPC_XM` + `Voice_NPC/NPC_XM` show it is a voiced NPC), and
#   `XM_NPC_C` is placed exactly ONCE per chapter in all five chapters' `_logic`
#   level, which is the Inner Demon's story role.  `npc_Dianame_30` is the
#   game's own "Inner Demon".
PINYIN = {
    "cunmin": "Villager",
    "cunmin2": "Villager",
    "XM": "Inner Demon",
}


# ---------------------------------------------------------------------------
# reading the paks
# ---------------------------------------------------------------------------

def asset_index(src: pakmaps.MapSource) -> dict[str, list[str]]:
    """`asset stem -> the `.uasset` keys with that basename`."""
    out: dict[str, list[str]] = {}
    for k in src.owner:
        if k.endswith(".uasset"):
            out.setdefault(os.path.basename(k)[:-len(".uasset")], []).append(k)
    return out


def blob_of(src: pakmaps.MapSource, key: str) -> bytes:
    """A package's header plus its `.uexp`, as one byte string."""
    try:
        b = src.read(key)
    except Exception:                                           # noqa: BLE001
        return b""
    uexp = key[:-len(".uasset")] + ".uexp"
    if uexp in src.owner:
        try:
            b += src.read(uexp)
        except Exception:                                       # noqa: BLE001
            pass
    return b


def scan(src: pakmaps.MapSource, classes: list[str],
         index: dict[str, list[str]], verbose: bool = True):
    """Per class: its asset paths, the locres keys inside them, the NPC
    directories it references."""
    keys: dict[str, set[str]] = {}
    refs: dict[str, set[str]] = {}
    paths: dict[str, list[str]] = {}
    for cls in classes:
        stem = cls[:-2] if cls.endswith("_C") else cls
        ks = index.get(stem, [])
        paths[cls] = ks
        kk: set[str] = set()
        rr: set[str] = set()
        for k in ks:
            b = blob_of(src, k)
            kk |= {m.decode() for m in KEY_RX.findall(b)}
            rr |= {m.decode() for m in REF_RX.findall(b)}
        keys[cls] = kk
        refs[cls] = rr
    if verbose:
        print(f"  {sum(1 for c in classes if keys[c])}/{len(classes)} class(es) "
              f"embed an npc locres key")
    return keys, refs, paths


def folder_of(paths: list[str]) -> str | None:
    """The `NPC_<X>` directory a class' blueprint lives in, if any."""
    for k in paths:
        if k.startswith(NPC_DIR):
            rest = k[len(NPC_DIR):]
            if "/" in rest:
                return rest.split("/")[0]
    return None


# ---------------------------------------------------------------------------
# resolution
# ---------------------------------------------------------------------------

def display(loc: dict[str, str], key: str) -> str | None:
    """The localised string for a name key, without the speaker colon."""
    v = loc.get(key)
    if not v:
        return None
    return v.rstrip().rstrip(":：").strip() or None


def prettify(cls: str) -> str:
    """The transliteration inside a class name, as words.

    `BP_NPC_cunmin_C` -> "Cunmin", `Heyouzai_NPC_C` -> "Heyouzai",
    `ZY_N_NPC_ChangQiang_C` -> "ZY Chang Qiang".  Purely lexical: it can look
    clumsy but it can never name the wrong character.
    """
    toks = [t for t in cls.split("_") if t]
    if toks and toks[-1] == "C":
        toks.pop()
    drop = {"BP", "NPC", "N", "AI"}
    kept = [t for t in toks if t not in drop] or [t for t in toks if t != "C"]
    words: list[str] = []
    for t in kept:
        if t.isupper() and len(t) <= 5:
            words.append(t)                     # an initialism stays as it is
            continue
        parts = re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+", t)
        words += [p if p.isupper() else p[:1].upper() + p[1:] for p in parts]
    return " ".join(words).strip()


def token_of(cls: str) -> str:
    """The class' own token: what `prettify` keeps, unspaced and unchanged."""
    toks = [t for t in cls.split("_") if t]
    if toks and toks[-1] == "C":
        toks.pop()
    kept = [t for t in toks if t not in {"BP", "NPC", "N", "AI"}]
    return kept[0] if kept else (toks[0] if toks else cls)


def resolve(classes: list[str], keys, refs, paths, loc) -> dict[str, dict]:
    """class -> {name, key, via}, in the fallback order documented at the top."""
    out: dict[str, dict] = {}

    # rule 1 - the class' own asset.
    for cls in classes:
        for k in sorted(keys[cls]):
            nm = display(loc, k)
            if nm:
                out[cls] = {"name": nm, "key": k, "via": "own"}
                break

    # a directory's answer, but only when every rule-1 class in it agrees.
    by_dir: dict[str, set[str]] = collections.defaultdict(set)
    for cls in classes:
        f = folder_of(paths[cls])
        if f and cls in out:
            by_dir[f].add(out[cls]["key"])
    dir_key = {f: next(iter(ks)) for f, ks in by_dir.items() if len(ks) == 1}
    conflicts = {f: sorted(ks) for f, ks in by_dir.items() if len(ks) > 1}

    # rule 2 - the same directory.
    for cls in classes:
        if cls in out:
            continue
        f = folder_of(paths[cls])
        if f and f in dir_key:
            nm = display(loc, dir_key[f])
            if nm:
                out[cls] = {"name": nm, "key": dir_key[f], "via": "folder:" + f}

    # rule 3 - a reference to another character's directory, corroborated by
    # the class' own name.  Both halves required.
    for cls in classes:
        if cls in out:
            continue
        flat = cls.lower().replace("_", "")
        for f in sorted(refs[cls]):
            if f not in dir_key:
                continue
            tok = f[len("NPC_"):].lower()
            if tok and tok in flat:
                nm = display(loc, dir_key[f])
                if nm:
                    out[cls] = {"name": nm, "key": dir_key[f], "via": "ref:" + f}
                    break

    # rules 4 and 5.
    for cls in classes:
        if cls in out:
            continue
        if cls in READ_POINT:
            out[cls] = {"name": READ_POINT[cls], "key": None, "via": "read-point"}
        elif cls in NO_NAME:
            out[cls] = {"name": NO_NAME[cls], "key": None, "via": "no-name"}
        elif token_of(cls) in PINYIN:
            out[cls] = {"name": PINYIN[token_of(cls)], "key": None, "via": "pinyin"}
        else:
            out[cls] = {"name": prettify(cls) or cls, "key": None,
                        "via": "class-name"}
    return out, dir_key, conflicts


# ---------------------------------------------------------------------------
# validation: score the answer against a witness the data already carries
# ---------------------------------------------------------------------------

def _flat(s: str) -> str:
    return re.sub(r"[^a-z0-9]", "", s.lower())


def _pieces(name: str) -> list[str]:
    """A localised name split into romanised syllable-ish pieces.

    "Huang Jian'e" -> [Huang, Jian, e]; "Xuanyangzi" -> [Xuanyangzi].  Good
    enough to test an initialism against, which is all it is used for.
    """
    out: list[str] = []
    for word in re.split(r"[\s\-]+", name):
        for part in word.split("'"):
            out += re.findall(r"[A-Z][a-z0-9]*|[a-z0-9]+", part) or ([part] if part else [])
    return [p for p in out if p]


def witness(npcs: dict[str, dict], keys: dict[str, set[str]],
            verbose: bool = True) -> dict:
    """Score the answer against witnesses the data already carries.

    Four numbers, none of which is used to CHOOSE a name:

    * **ambiguous** - classes embedding more than one name key.  This is the
      acceptance test for the whole scan: "the key inside the asset" is only a
      well-defined answer while it is unique.  Measured 0 of 74.
    * **dir unanimity** - directories where two or more blueprints each carry a
      key of their own.  They are separate assets authored separately, so a
      disagreement would be a real failure; the count of directories that
      disagree is the number that matters, and it is 0.
    * **initials** - a class whose all-caps code has as many letters as the
      localised name has romanised syllables is a testable case: `HJE` =
      Huang-Jian-e, `SKW` = Sun-Ke-Wang, `LWS` = Li-Wan-San.  Cases where the
      name is an English title (`CF` -> "Boatman") or a European name (`AWS` =
      An-Wen-Si -> "Magalhaes") are not testable and are not counted either way.
    * **spelled** - positive evidence only: a directory or class token written
      out in romanised Chinese that equals the localised name with punctuation
      dropped (`NPC_HuangJianE` = "Huang Jian'e", `NPC_Heyouzai` = "He Youzai",
      `NPC_XuanYangZi` = "Xuanyangzi").  Many directories are named for what the
      character IS rather than who (`NPC_YaoTong`, the medicine boy, is "Young
      Boy"), so a miss here is not evidence of an error.
    """
    by_dir: dict[str, set[str]] = collections.defaultdict(set)
    for cls, e in npcs.items():
        if e["via"] == "own" and e.get("dir"):
            by_dir[e["dir"]].add(e["key"])
    multi = {d: ks for d, ks in by_dir.items() if len(ks) >= 1
             and sum(1 for c, e in npcs.items()
                     if e["via"] == "own" and e.get("dir") == d) > 1}
    dir_bad = {d: sorted(ks) for d, ks in multi.items() if len(ks) > 1}

    init_ok, init_n, init_bad = 0, 0, []
    spelled = []
    for cls, e in sorted(npcs.items()):
        if e["via"] not in ("own", "folder", "ref"):
            continue
        name = e["name"]
        pieces = _pieces(name)
        inits = "".join(p[0] for p in pieces).upper()
        for tok in {e.get("dir", "")[len("NPC_"):], token_of(cls)} - {""}:
            if tok.isupper() and 2 <= len(tok) <= 6:
                if len(tok) == len(pieces):
                    init_n += 1
                    if tok == inits:
                        init_ok += 1
                    else:
                        init_bad.append((cls, tok, name))
            elif len(tok) >= 4 and _flat(tok) == _flat(name):
                spelled.append((cls, tok, name))

    amb = sorted(c for c in npcs if len(keys.get(c, ())) > 1)
    if verbose:
        print(f"  witness: {len(amb)} class(es) with an ambiguous key; "
              f"{len(dir_bad)} of {len(multi)} multi-blueprint directories "
              f"disagree; initials {init_ok}/{init_n}; "
              f"spelled-out matches {len(spelled)}")
        for cls, tok, n in init_bad:
            print(f"      ? initials {tok} vs {n!r} ({cls})")
        if dir_bad:
            print(f"      ! {dir_bad}")
    return {
        "ambiguous_keys": amb,
        "dirs_multi_blueprint": len(multi),
        "dirs_disagreeing": dir_bad,
        "initials": {"agree": init_ok, "checked": init_n,
                     "disagree": [{"cls": c, "code": t, "name": n}
                                  for c, t, n in init_bad]},
        "spelled_out_matches": [{"cls": c, "token": t, "name": n}
                                for c, t, n in spelled],
    }


def placements(src: pakmaps.MapSource, classes: set[str]) -> dict[str, list[str]]:
    """class -> the `Content/Maps/...umap` packages that place an instance."""
    out: dict[str, list[str]] = {}
    for k in sorted(src.owner):
        if not (k.startswith("Content/Maps/") and k.endswith(".umap")):
            continue
        try:
            pkg = src.package(k)
        except Exception:                                       # noqa: BLE001
            continue
        for ex in pkg.exports:
            if ex.class_name in classes:
                out.setdefault(ex.class_name, []).append(os.path.basename(k)[:-5])
    return out


def build(src: pakmaps.MapSource, verbose: bool = True, report: bool = False,
          prov: dict | None = None) -> dict:
    graph = BB.build_graph(src, verbose)
    if NPC_BASE not in graph and NPC_BASE not in graph.values():
        raise SystemExit(f"{NPC_BASE} not in the class graph - wrong prefixes?")
    everyone = class_graph.descendants(graph, NPC_BASE)
    classes = [c for c in everyone if c not in NOT_PEOPLE]
    if verbose:
        print(f"  {len(everyone)} descendant(s) of {NPC_BASE}, "
              f"{len(classes)} of them people")

    index = asset_index(src)
    keys, refs, paths = scan(src, classes, index, verbose)
    loc = BI.read_locres(src.read(BI.LOCRES.format(lang="en")))

    # the AI table is the boss route; it is recorded here as a measured
    # negative so nobody re-derives it.
    ai = BB.ai_ids(src, verbose=False)
    in_ai = sorted(c for c in classes if c in ai)

    names, dir_key, conflicts = resolve(classes, keys, refs, paths, loc)
    via = collections.Counter(n["via"].split(":")[0] for n in names.values())

    npcs = {}
    for cls in classes:
        e = dict(names[cls])
        f = folder_of(paths[cls])
        if f:
            e["dir"] = f
        npcs[cls] = e
    checks = witness(npcs, keys, verbose)

    doc = {
        "schema": SCHEMA,
        "base": NPC_BASE,
        "count": len(npcs),
        "localised": sum(1 for e in npcs.values() if e["key"]),
        "via": dict(sorted(via.items())),
        "dt_aitable_rows": len(in_ai),
        "witness": checks,
        "source": ("cooked .uasset export maps (class graph) + the npc_Dianame_* / "
                   "npc_name_* FText keys embedded in each NPC blueprint + "
                   "MMGame.locres, offline pak extraction"),
        "npcs": npcs,
        "generated_by": "tools/markers/build_npcs.py",
        **(prov or {}),
    }
    if conflicts:
        doc["dir_conflicts"] = conflicts
    if report:
        doc["placed_in"] = placements(src, set(classes))
        doc["dir_key"] = dir_key
    if verbose:
        print(f"  {len(npcs)} npc class(es), {doc['localised']} with a localised "
              f"name; via {dict(sorted(via.items()))}")
        print(f"  DT_AiTable rows for npc classes: {len(in_ai)} "
              f"(the boss route does not apply)")
        if conflicts:
            print(f"  ! directory key conflicts: {conflicts}")
    return doc


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--pak", default=pakmaps.DEFAULT_PAK)
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "markers", "npcs.json"))
    ap.add_argument("--report", action="store_true",
                    help="also record where every npc class is placed")
    provenance.add_arg(ap)
    a = ap.parse_args(argv)
    ms = pakmaps.MapSource(a.pak)
    doc = build(ms, report=a.report,
                prov=provenance.stamp(ms, a.pak, not a.no_pak_hash))
    out = os.path.normpath(a.out)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write("\n")
    print(f"wrote {out}")
    for cls, e in sorted(doc["npcs"].items()):
        print(f"  {cls:<32} {str(e.get('dir') or ''):<22} "
              f"{e['name']:<34} [{e['via']}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
