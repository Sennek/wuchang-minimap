#!/usr/bin/env python3
"""Report how much of the hand-written C/C++ in this repo is comments.

Tokenizes src/ and tests/ (line and block comments, string, char and raw-string
literals) and prints a per-file table, a comment-block size histogram, a
header-vs-inline split and the largest blocks.
"""

import argparse
import csv
import os
import sys

CODE = 1
COMMENT = 2

DEFAULT_GLOBS = [
    ("src", (".cpp", ".hpp", ".h")),
    ("tests", (".cpp",)),
]

BUCKETS = [
    ("1 line", 1, 1),
    ("2 lines", 2, 2),
    ("3 lines", 3, 3),
    ("4-5", 4, 5),
    ("6-9", 6, 9),
    ("10-19", 10, 19),
    ("20+", 20, 1 << 30),
]


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_files(root):
    out = []
    for sub, exts in DEFAULT_GLOBS:
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith(exts):
                out.append(os.path.join(d, name))
    return out


def expand(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for dirpath, _, names in os.walk(p):
                for name in sorted(names):
                    if name.endswith((".cpp", ".hpp", ".h", ".c", ".cc", ".cxx")):
                        out.append(os.path.join(dirpath, name))
        else:
            out.append(p)
    return out


def classify(text):
    """Return a list of per-line flag sets (CODE / COMMENT bits, 0 = blank)."""
    lines = text.split("\n")
    flags = [0] * len(lines)
    raw_strings = False
    i = 0
    n = len(text)
    line = 0

    def mark(idx, bit):
        if 0 <= idx < len(flags):
            flags[idx] |= bit

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            # line comment, honouring backslash-newline continuation
            while i < n:
                mark(line, COMMENT)
                if text[i] == "\n":
                    cont = i > 0 and text[i - 1] == "\\"
                    line += 1
                    i += 1
                    if not cont:
                        break
                    continue
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            mark(line, COMMENT)
            while i < n:
                if text[i] == "\n":
                    line += 1
                    mark(line, COMMENT)
                    i += 1
                    continue
                if text[i] == "*" and i + 1 < n and text[i + 1] == "/":
                    i += 2
                    break
                i += 1
            continue
        if c == '"' or c == "'":
            mark(line, CODE)
            # raw string?  R"delim( ... )delim"
            prev = text[i - 1] if i else ""
            prev2 = text[i - 2] if i > 1 else ""
            is_raw = prev == "R" and not (prev2.isalnum() and prev2 not in "uUL8")
            if c == '"' and is_raw:
                j = text.find("(", i + 1)
                delim = text[i + 1:j] if j != -1 else None
                if delim is not None and "\n" not in delim and len(delim) <= 16:
                    raw_strings = True
                    close = ")" + delim + '"'
                    end = text.find(close, j + 1)
                    end = n if end == -1 else end + len(close)
                    line += text.count("\n", i, end)
                    for k in range(text.count("\n", i, end) + 1):
                        mark(line - k, CODE)
                    i = end
                    continue
            quote = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    if i + 1 < n and text[i + 1] == "\n":
                        line += 1
                        mark(line, CODE)
                    i += 2
                    continue
                if text[i] == "\n":
                    line += 1
                    mark(line, CODE)
                    i += 1
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        mark(line, CODE)
        i += 1

    return lines, flags, raw_strings


def analyse(path, root):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read().replace("\r\n", "\n")
    lines, flags, raw = classify(text)
    if lines and lines[-1] == "":
        lines = lines[:-1]
        flags = flags[:-1]
    rel = os.path.relpath(path, root).replace("\\", "/")

    stats = {"file": rel, "total": len(lines), "blank": 0, "code": 0,
             "comment_only": 0, "mixed": 0}
    for f in flags:
        if f == 0:
            stats["blank"] += 1
        elif f == COMMENT:
            stats["comment_only"] += 1
        elif f == CODE:
            stats["code"] += 1
        else:
            stats["mixed"] += 1
    stats["comment_lines"] = stats["comment_only"] + stats["mixed"]
    nonblank = stats["total"] - stats["blank"]
    stats["pct"] = 100.0 * stats["comment_lines"] / stats["total"] if stats["total"] else 0.0
    stats["pct_nb"] = 100.0 * stats["comment_lines"] / nonblank if nonblank else 0.0

    blocks = []
    seen_code = False
    run = None
    for idx, f in enumerate(flags):
        if f == COMMENT:
            if run is None:
                run = {"file": rel, "start": idx + 1, "size": 0,
                       "first": lines[idx].strip()[:70],
                       "header": idx < 3 or not seen_code}
            run["size"] += 1
        else:
            if run is not None:
                blocks.append(run)
                run = None
            if f & CODE:
                seen_code = True
    if run is not None:
        blocks.append(run)
    return stats, blocks, raw


def print_table(rows):
    w = max([len(r["file"]) for r in rows] + [5])
    head = (f"{'file':<{w}}  {'total':>6} {'blank':>6} {'code':>6} {'cmtonly':>7} "
            f"{'mixed':>6} {'cmtlns':>6} {'pct':>7} {'pct_nb':>7}")
    print(head)
    print("-" * len(head))
    for r in rows:
        print(f"{r['file']:<{w}}  {r['total']:>6} {r['blank']:>6} {r['code']:>6} "
              f"{r['comment_only']:>7} {r['mixed']:>6} {r['comment_lines']:>6} "
              f"{r['pct']:>6.1f}% {r['pct_nb']:>6.1f}%")


def total_row(rows):
    t = {"file": "TOTAL", "total": 0, "blank": 0, "code": 0, "comment_only": 0, "mixed": 0}
    for r in rows:
        for k in ("total", "blank", "code", "comment_only", "mixed"):
            t[k] += r[k]
    t["comment_lines"] = t["comment_only"] + t["mixed"]
    nb = t["total"] - t["blank"]
    t["pct"] = 100.0 * t["comment_lines"] / t["total"] if t["total"] else 0.0
    t["pct_nb"] = 100.0 * t["comment_lines"] / nb if nb else 0.0
    return t


def print_histogram(blocks, all_comment_lines):
    print(f"{'bucket':<10} {'blocks':>7} {'lines':>7} {'% cmt lines':>12}")
    print("-" * 39)
    for name, lo, hi in BUCKETS:
        sel = [b for b in blocks if lo <= b["size"] <= hi]
        ln = sum(b["size"] for b in sel)
        pct = 100.0 * ln / all_comment_lines if all_comment_lines else 0.0
        print(f"{name:<10} {len(sel):>7} {ln:>7} {pct:>11.1f}%")
    tot = sum(b["size"] for b in blocks)
    pct = 100.0 * tot / all_comment_lines if all_comment_lines else 0.0
    print(f"{'ALL':<10} {len(blocks):>7} {tot:>7} {pct:>11.1f}%")


def main():
    ap = argparse.ArgumentParser(description="C/C++ comment statistics")
    ap.add_argument("paths", nargs="*", help="files or dirs (default: src/, tests/)")
    ap.add_argument("--top", type=int, default=25, help="how many largest blocks to list")
    ap.add_argument("--csv", action="store_true", help="dump the per-file table as CSV")
    args = ap.parse_args()

    root = repo_root()
    files = expand(args.paths) if args.paths else default_files(root)
    if not files:
        print("no input files", file=sys.stderr)
        return 1

    rows, blocks, raw_seen = [], [], []
    for path in files:
        st, bl, raw = analyse(path, root)
        rows.append(st)
        blocks.extend(bl)
        if raw:
            raw_seen.append(st["file"])
    rows.sort(key=lambda r: (-r["pct"], r["file"]))
    tot = total_row(rows)

    if args.csv:
        w = csv.writer(sys.stdout, lineterminator="\n")
        w.writerow(["file", "total", "blank", "code", "comment_only", "mixed",
                    "comment_lines", "pct", "pct_nonblank"])
        for r in rows + [tot]:
            w.writerow([r["file"], r["total"], r["blank"], r["code"], r["comment_only"],
                        r["mixed"], r["comment_lines"], f"{r['pct']:.2f}", f"{r['pct_nb']:.2f}"])
        return 0

    print(f"comment statistics over {len(files)} files (root {root})")
    print()
    print("== per file ==")
    print_table(rows + [tot])

    cl = tot["comment_lines"]
    print()
    print("== comment blocks by size ==")
    print_histogram(blocks, cl)

    for label, sel in (("file header", [b for b in blocks if b["header"]]),
                       ("inline", [b for b in blocks if not b["header"]])):
        print()
        print(f"== {label} blocks ==")
        print_histogram(sel, cl)

    print()
    print(f"== top {args.top} largest blocks ==")
    for b in sorted(blocks, key=lambda b: (-b["size"], b["file"], b["start"]))[:args.top]:
        print(f"{b['file']}:{b['start']:<5} {b['size']:>4}  {b['first']}")

    print()
    print("raw strings seen in: " + (", ".join(raw_seen) if raw_seen else "(none)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
