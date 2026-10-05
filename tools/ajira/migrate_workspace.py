#!/usr/bin/env python3
"""Перенос таск-системы `.workspace/` (скилл `/task`) в AgentJira.

Записи таск-системы и во что они превращаются:

    <task>/CLAUDE.md        -> эпик: описание = цель и вводные, служебные разделы сняты
    <task>/CURRENT.md       -> подзадача эпика, статус `ready` (активный подэтап)
    <task>/backlog/<s>.md   -> подзадача эпика, статус `backlog`
    <task>/archive/<s>.md   -> подзадача эпика, статус `done`
    <task>/log.md           -> комментарий к одноимённой подзадаче (иначе к эпику)
    <task>/lessons.md       -> страница плана
    <task>/context/**/*.md  -> страницы плана
    _done/<task>/           -> то же самое, эпик закрыт

Переносится ЗАПИСЬ, а не эвиденс: png, csv, дампы и прочие крупные файлы остаются на
диске, а их число и объём называются в `meta` эпика.

    python tools/ajira/migrate_workspace.py build --out %TEMP%\\wm-import.jsonl
    python tools/ajira/migrate_workspace.py docs --project WM

`build` пишет JSONL для `ajira import --file`; `docs` заводит страницы плана через API
(в MCP запись страниц есть, но перенос идёт один раз и из скрипта). `docs` повторный запуск
переживает - уже заведённые slug'и пропускаются; `import` повторный запуск не переживает и
заведёт дубли, поэтому он идёт один раз.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

#: Разделы CLAUDE.md, которые описывали саму таск-систему. В трекере их роль играет
#: протокол AgentJira, поэтому в описание задачи они не переезжают.
SERVICE_SECTIONS = ("Scope", "Rules", "Workflow")
#: Разделы-оглавления из директив `@файл`: их содержимое и так едет страницами плана.
INCLUDE_SECTIONS = ("Completed work", "Lessons and critical constraints", "Common context")
DROP_SECTIONS = SERVICE_SECTIONS + INCLUDE_SECTIONS

LABEL = "workspace-history"
DONE_DIR = "_done"
SKIP_DIRS = {"_done", "_legacy", "_handoff"}

DATE_RE = re.compile(r"(\d{4}-\d{2}-\d{2})")
HEADING_RE = re.compile(r"^##\s+(.+?)\s*$", re.M)


def read_md(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig").replace("\r\n", "\n")


def drop_sections(md: str) -> str:
    """Снимает разделы по заголовкам второго уровня; остальное сохраняет дословно."""
    lines = md.splitlines()
    kept: list[str] = []
    index = 0
    while index < len(lines):
        heading = re.match(r"^##\s+(.*?)\s*$", lines[index])
        if heading and heading.group(1) in DROP_SECTIONS:
            index += 1
            while index < len(lines) and not re.match(r"^##\s", lines[index]):
                index += 1
            continue
        kept.append(lines[index])
        index += 1
    text = "\n".join(kept)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip() + "\n"


def section(md: str, name: str) -> str:
    match = re.search(rf"^##\s+{re.escape(name)}\s*$", md, re.M)
    if match is None:
        return ""
    rest = md[match.end() :]
    nxt = re.search(r"^##\s+", rest, re.M)
    return (rest[: nxt.start()] if nxt else rest).strip()


def gloss(text: str, limit: int = 88) -> str:
    plain = re.sub(r"\*\*|__|`", "", text)
    plain = re.sub(r"\s+", " ", plain).strip()
    if len(plain) <= limit:
        return plain
    return plain[:limit].rsplit(" ", 1)[0] + "…"


def subtask_slug(md: str, fallback: str) -> str:
    match = re.search(r"^#\s*Subtask:\s*(\S+)\s*$", md, re.M)
    return match.group(1) if match else fallback


def iso_date(stamp: float) -> str:
    return datetime.fromtimestamp(stamp, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def iso_day(day: str) -> str:
    return f"{day}T00:00:00Z"


@dataclass
class LogEntry:
    name: str
    heading: str
    date: str | None
    body: str


def split_log(md: str) -> list[LogEntry]:
    """`log.md` — записи по `## <подзадача> — <дата>`; `###` остаётся внутри записи."""
    marks = list(HEADING_RE.finditer(md))
    entries: list[LogEntry] = []
    for number, mark in enumerate(marks):
        heading = mark.group(1)
        end = marks[number + 1].start() if number + 1 < len(marks) else len(md)
        body = md[mark.end() : end].strip()
        found = DATE_RE.search(heading)
        name = re.split(r"\s+—\s+", heading)[0].strip()
        entries.append(
            LogEntry(
                name=name,
                heading=heading,
                date=found.group(1) if found else None,
                body=body,
            )
        )
    if not entries and md.strip():
        entries.append(LogEntry(name="", heading="", date=None, body=md.strip()))
    return entries


@dataclass
class Record:
    source_key: str
    title: str
    kind: str  # epic | current | backlog | archive
    path: Path
    body: str
    status: str
    slug: str
    parent: str | None = None
    comments: list[str] = field(default_factory=list)
    closed_at: str | None = None
    created_at: str | None = None
    meta: dict = field(default_factory=dict)


@dataclass
class Task:
    slug: str
    root: Path
    archived: bool
    records: list[Record]
    log: list[LogEntry]
    lessons_path: Path | None
    context_paths: list[Path]
    evidence_files: int
    evidence_bytes: int


def collect_task(path: Path, archived: bool) -> Task:
    prefix = f"done:{path.name}" if archived else f"task:{path.name}"
    claude_path = path / "CLAUDE.md"
    claude = drop_sections(read_md(claude_path)) if claude_path.is_file() else ""
    goal = section(claude, "Goal")
    title = f"{path.name} — {gloss(goal)}" if goal else path.name

    log_path = path / "log.md"
    log = split_log(read_md(log_path)) if log_path.is_file() else []

    records: list[Record] = []
    children = list(path.glob("archive/*.md")) + list(path.glob("backlog/*.md"))
    current_path = path / "CURRENT.md"

    # Даты закрытия записей известны из заголовков log.md: `## <подзадача> — <дата>`.
    log_dates = {entry.name: entry.date for entry in log if entry.name and entry.date}
    log_by_name = {entry.name: entry for entry in log if entry.name}

    body_of_epic = (
        f"Перенесено из таск-системы: `{'.workspace/' + (DONE_DIR + '/') * archived}{path.name}/`.\n\n"
        f"{claude}".rstrip()
        + "\n"
    )

    def child(kind: str, file: Path, status: str) -> Record:
        text = read_md(file)
        slug = subtask_slug(text, file.stem)
        stamp = iso_date(file.stat().st_mtime)
        closed = log_dates.get(slug) if kind == "archive" else None
        return Record(
            source_key=f"{prefix}/{kind}:{slug}",
            title=slug,
            kind=kind,
            path=file,
            body=text,
            status=status,
            slug=slug,
            parent=prefix,
            closed_at=iso_day(closed) if closed else (stamp if kind == "archive" else None),
            created_at=iso_day(closed) if closed else stamp,
        )

    if current_path.is_file():
        records.append(child("current", current_path, "ready"))
    for file in sorted(path.glob("backlog/*.md")):
        records.append(child("backlog", file, "backlog"))
    for file in sorted(path.glob("archive/*.md")):
        records.append(child("archive", file, "done"))

    # Комментарий записи log.md едет к одноимённой подзадаче, иначе остаётся на эпике.
    epic_comments: list[str] = []
    by_slug = {record.slug: record for record in records}
    for entry in log:
        text = f"### {entry.heading}\n\n{entry.body}".strip() if entry.heading else entry.body
        target = by_slug.get(entry.name)
        if target is not None:
            target.comments.append(text)
        else:
            epic_comments.append(text)

    live = [r for r in records if r.status != "done"]
    if archived:
        epic_status = "done"
        stamps = [r.closed_at or r.created_at for r in records] or [iso_date(claude_path.stat().st_mtime)]
        created_at = min(stamps)
        closed_at = max(stamps)
    else:
        epic_status = "in_progress" if live else "backlog"
        created_at = iso_date(claude_path.stat().st_mtime)
        closed_at = None

    files = [f for f in path.rglob("*") if f.is_file()]
    records.insert(
        0,
        Record(
            source_key=prefix,
            title=title,
            kind="epic",
            path=claude_path,
            body=body_of_epic,
            status=epic_status,
            slug=path.name,
            comments=epic_comments,
            created_at=created_at,
            closed_at=closed_at,
            meta={
                "workspace_path": f".workspace/{DONE_DIR + '/' if archived else ''}{path.name}",
                "migrated_from": "task-workspace",
                "evidence_files": len(files),
                "evidence_bytes": sum(f.stat().st_size for f in files),
            },
        ),
    )

    lessons = path / "lessons.md"
    context_paths = sorted(
        f for f in (path / "context").rglob("*.md") if f.is_file()
    ) if (path / "context").is_dir() else []

    return Task(
        slug=path.name,
        root=path,
        archived=archived,
        records=records,
        log=log,
        lessons_path=lessons if lessons.is_file() and lessons.stat().st_size else None,
        context_paths=context_paths,
        evidence_files=len(files),
        evidence_bytes=sum(f.stat().st_size for f in files),
    )


def collect(workspace: Path) -> list[Task]:
    tasks = [
        collect_task(directory, archived=False)
        for directory in sorted(workspace.iterdir())
        if directory.is_dir() and directory.name not in SKIP_DIRS and (directory / "CLAUDE.md").is_file()
    ]
    done_root = workspace / DONE_DIR
    if done_root.is_dir():
        tasks += [
            collect_task(directory, archived=True)
            for directory in sorted(done_root.iterdir())
            if directory.is_dir() and (directory / "CLAUDE.md").is_file()
        ]
    return tasks


def payload(record: Record) -> dict:
    item = {
        "source_key": record.source_key,
        "title": record.title,
        "type": "epic" if record.kind == "epic" else "task",
        "status": record.status,
        "priority": "medium",
        "description_md": record.body,
        "labels": [LABEL],
        "meta": record.meta,
    }
    if record.parent:
        item["parent"] = record.parent
    if record.comments:
        item["comments"] = record.comments
    if record.created_at:
        item["created_at"] = record.created_at
    if record.closed_at:
        item["closed_at"] = record.closed_at
    return item


def cmd_build(args: argparse.Namespace) -> int:
    workspace = Path(args.workspace)
    tasks = collect(workspace)
    out = Path(args.out)
    lines = [json.dumps(payload(record), ensure_ascii=False) for task in tasks for record in task.records]
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")

    epics = sum(1 for task in tasks for record in task.records if record.kind == "epic")
    docs = sum(len(task.context_paths) + (1 if task.lessons_path else 0) for task in tasks)
    print(f"задач: {len(tasks)} (эпиков {epics}), записей всего: {len(lines)}")
    print(f"страниц плана к заведению: {docs + len(tasks)}")
    print(f"JSONL: {out}")
    return 0


# --- страницы плана ---------------------------------------------------------


def api(method: str, path: str, body: dict | None, url: str, project: str) -> dict:
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(
        f"{url.rstrip('/')}/api/v1{path}",
        data=data,
        headers={"Content-Type": "application/json", "X-Ajira-Project": project},
        method=method,
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            raw = response.read().decode("utf-8")
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8")
        raise SystemExit(f"{method} {path}: HTTP {exc.code} {detail}") from exc


def doc_slug(task_slug: str, *parts: str) -> str:
    raw = "-".join(["task", task_slug, *parts])
    return re.sub(r"[^a-z0-9._-]+", "-", raw.lower()).strip("-")[:120]


def page_title(path: Path, task_root: Path) -> str:
    text = read_md(path)
    heading = re.search(r"^#\s+(.+?)\s*$", text, re.M)
    relative = path.relative_to(task_root).with_suffix("").as_posix()
    if heading:
        name = re.sub(r"^Subtask:\s*", "", heading.group(1)).strip()
        return f"{relative} — {name}"
    return relative


def context_slug_part(path: Path, task_root: Path) -> str:
    return path.relative_to(task_root / "context").with_suffix("").as_posix().replace("/", "-")


def cmd_docs(args: argparse.Namespace) -> int:
    workspace = Path(args.workspace)
    tasks = collect(workspace)
    existing = {item["slug"] for item in api("GET", "/docs", None, args.url, args.project)["items"]}
    epics = {item["title"]: item["key"] for item in api("GET", "/issues?limit=500", None, args.url, args.project)["items"]}

    created = 0
    for task in tasks:
        key = next((k for t, k in epics.items() if t.startswith(f"{task.slug} — ")), None)
        parent_slug = doc_slug(task.slug)
        if parent_slug not in existing:
            body = (
                f"Материалы задачи `{task.slug}` таск-системы.\n\n"
                f"- Задача в трекере: **{key or '—'}**\n"
                f"- Было: `{task.records[0].meta['workspace_path']}/`\n"
                f"- Эвиденс на диске: {task.evidence_files} файлов, "
                f"{task.evidence_bytes / 1048576:.1f} МиБ (png, csv, дампы, логи) — не переносился\n"
            )
            if args.dry_run:
                created += 1
            else:
                api(
                    "POST",
                    "/docs",
                    {"slug": parent_slug, "title": f"Материалы: {task.slug}", "body_md": body},
                    args.url,
                    args.project,
                )
                created += 1
            existing.add(parent_slug)

        pages: list[tuple[str, str, Path]] = []
        if task.lessons_path is not None:
            pages.append((doc_slug(task.slug, "lessons"), "lessons", task.lessons_path))
        for path in task.context_paths:
            pages.append(
                (
                    doc_slug(task.slug, "context", context_slug_part(path, task.root)),
                    page_title(path, task.root),
                    path,
                )
            )
        for slug, title, path in pages:
            if slug in existing:
                continue
            if args.dry_run:
                created += 1
                continue
            api(
                "POST",
                "/docs",
                {
                    "slug": slug,
                    "title": title,
                    "body_md": read_md(path),
                    "parent_slug": parent_slug,
                },
                args.url,
                args.project,
            )
            existing.add(slug)
            created += 1

    handoff = workspace / "_handoff" / "master.md"
    if handoff.is_file() and "handoff" not in existing:
        if not args.dry_run:
            api(
                "POST",
                "/docs",
                {"slug": "handoff", "title": "Handoff: 2026-09-05", "body_md": read_md(handoff)},
                args.url,
                args.project,
            )
        created += 1

    print(f"страниц заведено: {created}{' (dry-run)' if args.dry_run else ''}")
    return 0


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--workspace",
        default=str(Path(__file__).resolve().parents[2] / ".workspace"),
        help="корень таск-системы (по умолчанию .workspace репозитория)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    build = sub.add_parser("build", help="собрать JSONL для `ajira import`")
    build.add_argument("--out", required=True, help="куда писать JSONL")
    build.set_defaults(func=cmd_build)

    docs = sub.add_parser("docs", help="завести страницы плана через API")
    docs.add_argument("--url", default="http://127.0.0.1:8787")
    docs.add_argument("--project", default="WM")
    docs.add_argument("--dry-run", action="store_true")
    docs.set_defaults(func=cmd_docs)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
