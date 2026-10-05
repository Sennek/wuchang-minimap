"""Автономный MCP-сервер AgentJira: один файл, только стандартная библиотека.

Файл копируют в произвольный проект, чтобы агент того проекта работал с уже запущенной
AgentJira по HTTP. Ни зависимостей, ни импортов пакета `ajira`: достаточно Python 3.9+.

Это СБОРКА: не правьте её. Инструменты, обработчики и слой JSON-RPC живут в `ajira/mcp_core.py`,
транспорт и точка входа — в `scripts/standalone_parts/runtime.py`, а складывает их
`scripts/build_standalone.py`. Проверить, что файл не разошёлся с исходниками:

    python scripts/build_standalone.py --check

Транспорт — stdio: одна строка JSON-RPC 2.0 на входе, одна строка JSON на выходе. В stdout не
попадает ничего, кроме JSON-RPC: предупреждения и отладка идут в stderr.

Настройки берутся из окружения (те же значения можно передать аргументами `--url`, `--token`,
`--token-file`, `--project`, `--timeout`):

* `AJIRA_URL` — адрес трекера, по умолчанию `http://127.0.0.1:8787`;
* `AJIRA_TOKEN` — токен агента; если задан, уходит заголовком `Authorization: Bearer`;
* `AJIRA_TOKEN_FILE` — файл с токеном: так секрет не попадает в `.mcp.json`, который коммитят;
* `AJIRA_PROJECT` — ключ проекта; уходит заголовком `X-Ajira-Project`;
* `AJIRA_TIMEOUT` — таймаут одного запроса в секундах, по умолчанию 30.

Каждый запрос уходит с заголовком `X-Ajira-Source: mcp`: так событие в истории трекера показывает,
что действие сделано через MCP. Условная правка задачи (`issue_update` с `version`) отправляет
`If-Match` — на этом стоит оптимистическая блокировка сервера.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence
import argparse
import mimetypes
import os
import socket
import uuid
from typing import Any, Mapping, Sequence
from urllib import error as urlerror
from urllib import parse as urlparse
from urllib import request as urlrequest

PROTOCOL_VERSION = "2024-11-05"

SERVER_NAME = "ajira"
SERVER_VERSION = "0.1.0"

#: Коды ошибок JSON-RPC 2.0.
PARSE_ERROR = -32700
INVALID_REQUEST = -32600
METHOD_NOT_FOUND = -32601
INVALID_PARAMS = -32602
INTERNAL_ERROR = -32603

#: Размер строки stdin, который сервер согласен принять: защита от мусора в потоке.
MAX_LINE_BYTES = 4 * 1024 * 1024


def _tool(
    name: str,
    description: str,
    properties: Mapping[str, Any],
    required: Sequence[str] = (),
) -> dict[str, Any]:
    """Описание инструмента в формате MCP: имя, назначение и JSON Schema аргументов."""
    schema: dict[str, Any] = {
        "type": "object",
        "properties": dict(properties),
        "additionalProperties": False,
    }
    if required:
        schema["required"] = list(required)
    return {"name": name, "description": description, "inputSchema": schema}


_ISSUE_KEY = {"type": "string", "description": "ключ задачи, например AJ-42"}
_STATUS = {
    "type": "string",
    "description": "статус задачи",
    "enum": [
        "backlog",
        "ready",
        "in_progress",
        "blocked",
        "review",
        "failed",
        "done",
        "cancelled",
    ],
}
#: При создании доступны только эти два: остальные означают историю и достигаются переходами.
_CREATION_STATUS = {
    "type": "string",
    "description": (
        "статус при создании: backlog или ready. Остальные состояния достигаются переходами, "
        "а создание сразу в ready требует описания и хотя бы одного критерия приёмки"
    ),
    "enum": ["backlog", "ready"],
}
_ISSUE_TYPE = {
    "type": "string",
    "description": "тип задачи",
    "enum": ["epic", "story", "task", "bug", "chore", "spike", "question"],
}
_PRIORITY = {
    "type": "string",
    "description": "приоритет",
    "enum": ["lowest", "low", "medium", "high", "highest", "critical"],
}
_LINK_TYPE = {
    "type": "string",
    "description": "тип связи; обратный тип связи сервер выводит сам",
    "enum": ["blocks", "relates_to", "duplicates", "implements", "caused_by", "spawned_from"],
}
_STRING_LIST = {"type": "array", "items": {"type": "string"}}
#: Ключ идемпотентности записи: агент, не знающий, дошёл ли вызов, повторяет его с тем же
#: ключом и получает прежний результат вместо дубля задачи, комментария, worklog, вложения или
#: связи.
_IDEMPOTENCY_KEY = {
    "type": "string",
    "description": (
        "ключ идемпотентности (любая уникальная строка, например uuid): повтори вызов с тем же "
        "ключом, если не знаешь, дошёл ли первый, — дубля не будет"
    ),
}

#: Каталог инструментов: имя → описание и схема аргументов.
TOOLS: tuple[dict[str, Any], ...] = (
    _tool(
        "issue_list",
        "Список задач с фильтрами: что можно взять в работу, что в review, что у агента.",
        {
            "status": {**_STRING_LIST, "description": "фильтр по статусам"},
            "type": {**_STRING_LIST, "description": "фильтр по типам"},
            "priority": {**_STRING_LIST, "description": "фильтр по приоритетам"},
            "label": {**_STRING_LIST, "description": "метки, например agent-ready"},
            "assignee": {"type": "string", "description": "имя актора-исполнителя"},
            "milestone": {"type": "string", "description": "веха"},
            "parent": {"type": "string", "description": "ключ родительской задачи"},
            "q": {"type": "string", "description": "подстрока в заголовке и описании"},
            "updated_since": {"type": "string", "description": "ISO8601, курсор синхронизации"},
            "limit": {"type": "integer", "description": "сколько задач вернуть"},
            "cursor": {
                "type": "string",
                "description": "курсор следующей страницы из next_cursor предыдущего ответа",
            },
            "sort": {
                "type": "string",
                "enum": ["rank", "updated", "created", "priority"],
                "description": "порядок выдачи",
            },
            "compact": {
                "type": "boolean",
                "description": (
                    "только поля выбора задачи: key, title, status, parent, labels и прочее "
                    "без описания. Полное описание — в issue_get"
                ),
            },
            "fields": {
                **_STRING_LIST,
                "description": (
                    "какие поля вернуть, точно: например [\"key\",\"title\",\"status\"]. "
                    "Сильнее compact"
                ),
            },
        },
    ),
    _tool(
        "issue_get",
        "Карточка задачи: поля, лиза, доступные переходы, вложения и комментарии.",
        {"issue_key": _ISSUE_KEY},
        required=("issue_key",),
    ),
    _tool(
        "issue_create",
        "Создать задачу или подзадачу. Метка agent-ready разрешает агентам её взять.",
        {
            "title": {"type": "string", "description": "заголовок"},
            "type": _ISSUE_TYPE,
            "status": _CREATION_STATUS,
            "priority": _PRIORITY,
            "description_md": {"type": "string", "description": "описание в markdown"},
            "labels": {**_STRING_LIST, "description": "метки, например agent-ready"},
            "checklist": {**_STRING_LIST, "description": "критерии приёмки"},
            "parent": {"type": "string", "description": "ключ родительской задачи"},
            "milestone": {"type": "string", "description": "веха"},
            "assignee": {"type": "string", "description": "имя актора"},
            "estimate_minutes": {"type": "integer", "description": "оценка в минутах"},
            "meta": {"type": "object", "description": "ветка, коммит, PR, URL"},
            "idempotency_key": _IDEMPOTENCY_KEY,
        },
        required=("title",),
    ),
    _tool(
        "issue_update",
        "Правка полей задачи. version уходит в If-Match: при рассинхроне сервер ответит 409.",
        {
            "issue_key": _ISSUE_KEY,
            "patch": {
                "type": "object",
                "description": "изменяемые поля: title, description_md, priority, labels, meta, ...",
            },
            "version": {"type": "integer", "description": "версия из issue_get"},
        },
        required=("issue_key", "patch"),
    ),
    _tool(
        "issue_claim",
        "Взять задачу в работу: создаёт лиз. 409 — задача занята, 429 — исчерпан лимит агента.",
        {
            "issue_key": _ISSUE_KEY,
            "ttl_seconds": {"type": "integer", "description": "срок лиза, по умолчанию серверный"},
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_heartbeat",
        "Продлить лиз. Пропущенный heartbeat — лиз истекает и задача возвращается в ready.",
        {
            "issue_key": _ISSUE_KEY,
            "ttl_seconds": {"type": "integer", "description": "новый срок лиза"},
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_release",
        "Освободить задачу явно, не дожидаясь TTL.",
        {
            "issue_key": _ISSUE_KEY,
            "reason": {"type": "string", "description": "почему отпускаю"},
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_comment",
        "Комментарий в markdown: прогресс, найденная проблема, результат.",
        {
            "issue_key": _ISSUE_KEY,
            "body_md": {"type": "string", "description": "текст в markdown"},
            "attachments": {**_STRING_LIST, "description": "id уже загруженных вложений"},
            "idempotency_key": _IDEMPOTENCY_KEY,
        },
        required=("issue_key", "body_md"),
    ),
    _tool(
        "issue_worklog",
        "Записать расход по задаче: что сделано, сколько времени, токенов и денег. От этой записи "
        "наполняются суточные бюджеты агента: не записал — лимит токенов и денег не сработает. "
        "Зови после каждого законченного шага работы, до перехода в review.",
        {
            "issue_key": _ISSUE_KEY,
            "note": {"type": "string", "description": "что именно сделано"},
            "seconds": {"type": "integer", "description": "сколько заняло времени"},
            "tokens_in": {"type": "integer", "description": "входные токены"},
            "tokens_out": {"type": "integer", "description": "выходные токены"},
            "cost_usd": {"type": "number", "description": "стоимость в долларах"},
            "idempotency_key": _IDEMPOTENCY_KEY,
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_transition",
        "Сменить статус. Гварды проверяют доказательства: для review нужен след работы.",
        {
            "issue_key": _ISSUE_KEY,
            "to": _STATUS,
            "note": {"type": "string", "description": "объяснение перехода"},
        },
        required=("issue_key", "to"),
    ),
    _tool(
        "issue_link",
        "Связать две задачи. Обратный тип связи сервер выводит сам.",
        {
            "issue_key": _ISSUE_KEY,
            "to": {"type": "string", "description": "ключ второй задачи"},
            "type": _LINK_TYPE,
            "idempotency_key": _IDEMPOTENCY_KEY,
        },
        required=("issue_key", "to", "type"),
    ),
    _tool(
        "issue_attach",
        "Приложить файл к задаче: диф, лог, скриншот. Путь — на этой машине.",
        {
            "issue_key": _ISSUE_KEY,
            "path": {"type": "string", "description": "путь к файлу"},
            "idempotency_key": _IDEMPOTENCY_KEY,
        },
        required=("issue_key", "path"),
    ),
    _tool(
        "issue_events",
        "История задачи: переходы, комментарии, worklog, правки чеклиста. Зови, чтобы понять, "
        "что уже делали до тебя и чего не хватает гвардам.",
        {
            "issue_key": _ISSUE_KEY,
            "limit": {"type": "integer", "description": "сколько событий вернуть"},
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_checklist_add",
        "Добавить критерий приёмки в задачу. Без описания и хотя бы одного критерия задача не "
        "пройдёт гвард backlog → ready.",
        {
            "issue_key": _ISSUE_KEY,
            "text": {"type": "string", "description": "формулировка критерия"},
            "sort_order": {"type": "number", "description": "позиция в списке, иначе в конец"},
        },
        required=("issue_key", "text"),
    ),
    _tool(
        "issue_checklist_tick",
        "Отметить критерий приёмки выполненным или снять отметку. item_id берётся из карточки "
        "issue_get.",
        {
            "issue_key": _ISSUE_KEY,
            "item_id": {"type": "string", "description": "id пункта чеклиста"},
            "done": {
                "type": "boolean",
                "description": "true отмечает выполнение, false снимает; по умолчанию true",
            },
        },
        required=("issue_key", "item_id"),
    ),
    _tool(
        "issue_checklist_edit",
        "Переименовать критерий приёмки или поменять его позицию в списке.",
        {
            "issue_key": _ISSUE_KEY,
            "item_id": {"type": "string", "description": "id пункта чеклиста"},
            "text": {"type": "string", "description": "новая формулировка"},
            "sort_order": {"type": "number", "description": "новая позиция в списке"},
        },
        required=("issue_key", "item_id"),
    ),
    _tool(
        "issue_checklist_waive",
        "Отметить критерий «не требуется»: пункт остаётся видимым, но закрыт без выполнения. "
        "Нужна причина; undo=true снимает отметку. Гвард закрытия засчитывает такой пункт "
        "закрытым.",
        {
            "issue_key": _ISSUE_KEY,
            "item_id": {"type": "string", "description": "id пункта чеклиста"},
            "reason": {"type": "string", "description": "почему критерий не требуется"},
            "undo": {"type": "boolean", "description": "true снимает отметку «не требуется»"},
        },
        required=("issue_key", "item_id"),
    ),
    _tool(
        "issue_checklist_remove",
        "Удалить критерий приёмки физически — в отличие от «не требуется», следа в списке не "
        "остаётся. Причину можно назвать в reason.",
        {
            "issue_key": _ISSUE_KEY,
            "item_id": {"type": "string", "description": "id пункта чеклиста"},
            "reason": {"type": "string", "description": "почему пункт убран"},
        },
        required=("issue_key", "item_id"),
    ),
    _tool(
        "attention_request",
        "Позвать человека: утверждение, вопрос с вариантами, нужны правки, блокер.",
        {
            "body_md": {"type": "string", "description": "что именно решает человек"},
            "kind": {
                "type": "string",
                "description": "что именно нужно от человека",
                "enum": ["approval", "question", "review", "blocked", "mention"],
            },
            "issue_key": {"type": "string", "description": "ключ задачи, к которой относится запрос"},
            "options": {**_STRING_LIST, "description": "варианты ответа кнопками"},
        },
        required=("body_md",),
    ),
    _tool(
        "inbox_list",
        "Очередь «нужен человек»: что человек ещё не разобрал и чем закончились твои вопросы. "
        "Зови, прежде чем звать человека повторно.",
        {
            "status": {
                "type": "string",
                "enum": ["open", "resolved", "dismissed"],
                "description": "какие записи показать, по умолчанию open",
            },
            "addressee": {
                "type": "string",
                "description": (
                    "упоминания этого актора вместо очереди человека: имя актора или me — "
                    "свои (по токену)"
                ),
            },
        },
    ),
    _tool(
        "search",
        "Полнотекстовый поиск по задачам, комментариям и страницам планов.",
        {
            "q": {"type": "string", "description": "поисковый запрос"},
            "limit": {"type": "integer", "description": "сколько результатов вернуть"},
        },
        required=("q",),
    ),
    _tool(
        "metrics",
        "Сводка за период: время цикла, простои, возвраты из review, расход агентов и остаток их "
        "суточных бюджетов. Зови, чтобы понять, куда уходит время и деньги.",
        {"days": {"type": "integer", "description": "период в днях, по умолчанию 30"}},
    ),
    _tool(
        "docs_list",
        "Страницы «Планы»: дерево документов проекта с содержимым. Зови, чтобы прочитать "
        "постановку, роадмап или решения до начала работы. compact=true — только дерево, "
        "без текстов.",
        {
            "compact": {
                "type": "boolean",
                "description": "только слаги и заголовки, без текстов страниц",
            }
        },
    ),
    _tool(
        "doc_get",
        "Одна страница плана целиком по слагу, вместе с markdown. У страницы может быть "
        "`source_path`: тогда текст читается из файла репозитория, и поле `source` говорит, "
        "прочитан ли он.",
        {"slug": {"type": "string", "description": "слаг страницы, например roadmap"}},
        required=("slug",),
    ),
    _tool(
        "doc_create",
        "Создать страницу плана: постановку, решение, план работ. С `source_path` страница "
        "становится указателем на файл репозитория — текст тогда живёт в git, а не в базе.",
        {
            "slug": {"type": "string", "description": "слаг страницы, например plan-mvp"},
            "title": {"type": "string", "description": "заголовок"},
            "body_md": {"type": "string", "description": "текст страницы в markdown"},
            "parent_slug": {"type": "string", "description": "слаг родительской страницы"},
            "sort_order": {"type": "number", "description": "позиция в дереве страниц"},
            "source_path": {
                "type": "string",
                "description": (
                    "путь к файлу репозитория, который показывает страница, "
                    "например design/02-mvp.md; body_md тогда не используется"
                ),
            },
        },
        required=("slug", "title"),
    ),
    _tool(
        "doc_update",
        "Правка страницы плана. version уходит в If-Match: при рассинхроне сервер ответит 409. "
        "У страницы, которая показывает файл репозитория, текст не правится — только заголовок "
        "и путь.",
        {
            "slug": {"type": "string", "description": "слаг страницы"},
            "patch": {
                "type": "object",
                "description": (
                    "изменяемые поля: title, body_md, parent_slug, sort_order, source_path"
                ),
            },
            "version": {"type": "integer", "description": "версия из doc_get"},
        },
        required=("slug", "patch"),
    ),
    _tool(
        "milestones_list",
        "Вехи роадмапа: срок, статус и прогресс по входящим задачам. Зови, чтобы понять, к какой "
        "вехе относится задача и что ещё в ней не закрыто.",
        {},
    ),
    _tool(
        "me",
        "Кто я: остаток суточного бюджета и мои активные лизы.",
        {},
    ),
)

TOOLS_BY_NAME: dict[str, dict[str, Any]] = {tool["name"]: tool for tool in TOOLS}


class ToolError(Exception):
    """Ошибка вызова инструмента: уходит клиенту как isError, процесс не падает."""


# --- приведение аргументов ---------------------------------------------------


def _require(args: Mapping[str, Any], name: str, tool: str) -> Any:
    value = args.get(name)
    if value is None:
        raise ToolError(f"{tool}: обязательный аргумент {name} не задан")
    return value


def _as_int(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise ToolError(f"{name}: ожидалось целое число")
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise ToolError(f"{name}: ожидалось целое число, получено {value!r}") from exc


def _as_float(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise ToolError(f"{name}: ожидалось число")
    try:
        return float(value)
    except (TypeError, ValueError) as exc:
        raise ToolError(f"{name}: ожидалось число, получено {value!r}") from exc


def _as_str_list(value: Any, name: str) -> list[str]:
    """Список строк. Агент может прислать и строку — разбираем её как одну метку."""
    if value is None:
        return []
    if isinstance(value, str):
        return [item.strip() for item in value.split(",") if item.strip()]
    if isinstance(value, (list, tuple)):
        return [str(item) for item in value]
    raise ToolError(f"{name}: ожидался массив строк")


def _as_text(value: Any, name: str) -> str:
    """Обязательный текст без окружающих пробелов. Пустой — отказ до похода в API."""
    text = str(value).strip()
    if not text:
        raise ToolError(f"{name}: пустой текст недопустим")
    return text


# --- обработчики инструментов ------------------------------------------------


def _idempotency_of(args: Mapping[str, Any]) -> dict[str, str]:
    """Аргумент `idempotency_key` для клиента. Без ключа клиент зовётся как раньше."""
    value = str(args.get("idempotency_key") or "").strip()
    return {"idempotency_key": value} if value else {}


def _op_issue_list(client: Any, args: Mapping[str, Any]) -> Any:
    filters = dict(args)
    for name in ("status", "type", "label", "priority", "fields"):
        if name in filters:
            filters[name] = _as_str_list(filters[name], name)
    if "limit" in filters:
        filters["limit"] = _as_int(filters["limit"], "limit")
    if "compact" in filters and not isinstance(filters["compact"], bool):
        raise ToolError("issue_list: compact должен быть true или false")
    return client.list_issues(**filters)


def _op_issue_get(client: Any, args: Mapping[str, Any]) -> Any:
    return client.get_issue(str(_require(args, "issue_key", "issue_get")))


def _op_issue_create(client: Any, args: Mapping[str, Any]) -> Any:
    fields = dict(args)
    fields.pop("issue_key", None)
    # Ключ — не поле задачи: уходит заголовком, а пустой не уходит вовсе.
    fields.pop("idempotency_key", None)
    fields["title"] = _as_text(fields.get("title", ""), "issue_create: заголовок")
    if "labels" in fields:
        fields["labels"] = _as_str_list(fields["labels"], "labels")
    if "checklist" in fields:
        fields["checklist"] = _as_str_list(fields["checklist"], "checklist")
    if "estimate_minutes" in fields:
        fields["estimate_minutes"] = _as_int(fields["estimate_minutes"], "estimate_minutes")
    known = {
        "title",
        "type",
        "status",
        "priority",
        "description_md",
        "labels",
        "checklist",
        "parent",
        "milestone",
        "assignee",
        "estimate_minutes",
        "meta",
    }
    unknown = sorted(set(fields) - known)
    if unknown:
        raise ToolError("issue_create: неизвестные аргументы: " + ", ".join(unknown))
    return client.create_issue(**fields, **_idempotency_of(args))


def _op_issue_update(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_update"))
    patch = args.get("patch")
    if not isinstance(patch, Mapping) or not patch:
        raise ToolError("issue_update: patch должен быть непустым объектом")
    version = args.get("version")
    return client.update_issue(key, patch, version=version)


def _op_issue_claim(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_claim"))
    ttl = args.get("ttl_seconds")
    return client.claim(key, ttl_seconds=_as_int(ttl, "ttl_seconds") if ttl is not None else None)


def _op_issue_heartbeat(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_heartbeat"))
    ttl = args.get("ttl_seconds")
    return client.heartbeat(key, ttl_seconds=_as_int(ttl, "ttl_seconds") if ttl is not None else None)


def _op_issue_release(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_release"))
    reason = args.get("reason")
    return client.release(key, reason=str(reason) if reason is not None else None)


def _op_issue_comment(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_comment"))
    body = _as_text(_require(args, "body_md", "issue_comment"), "issue_comment: body_md")
    attachments = _as_str_list(args.get("attachments"), "attachments")
    return client.comment(key, body, attachments=attachments or None, **_idempotency_of(args))


def _op_issue_worklog(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_worklog"))
    note = args.get("note")
    seconds = args.get("seconds")
    tokens_in = args.get("tokens_in")
    tokens_out = args.get("tokens_out")
    cost = args.get("cost_usd")
    return client.worklog(
        key,
        str(note) if note is not None else "",
        seconds=_as_int(seconds, "seconds") if seconds is not None else None,
        tokens_in=_as_int(tokens_in, "tokens_in") if tokens_in is not None else 0,
        tokens_out=_as_int(tokens_out, "tokens_out") if tokens_out is not None else 0,
        cost_usd=_as_float(cost, "cost_usd") if cost is not None else 0.0,
        **_idempotency_of(args),
    )


def _op_issue_transition(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_transition"))
    to = str(_require(args, "to", "issue_transition"))
    note = args.get("note")
    return client.transition(key, to, note=str(note) if note is not None else None)


def _op_issue_link(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_link"))
    to = str(_require(args, "to", "issue_link"))
    link_type = str(_require(args, "type", "issue_link"))
    return client.link(key, to, link_type, **_idempotency_of(args))


def _op_issue_attach(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_attach"))
    path = Path(str(_require(args, "path", "issue_attach")))
    if not path.is_file():
        raise ToolError(f"issue_attach: файл не найден: {path}")
    return client.attach(key, path, **_idempotency_of(args))


def _op_issue_events(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_events"))
    limit = args.get("limit")
    return client.issue_events(key, limit=_as_int(limit, "limit") if limit is not None else None)


def _op_issue_checklist_add(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_checklist_add"))
    text = str(_require(args, "text", "issue_checklist_add")).strip()
    if not text:
        raise ToolError("issue_checklist_add: текст критерия не может быть пустым")
    order = args.get("sort_order")
    return client.checklist_add(
        key, text, sort_order=_as_float(order, "sort_order") if order is not None else None
    )


def _op_issue_checklist_tick(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_checklist_tick"))
    item_id = str(_require(args, "item_id", "issue_checklist_tick"))
    done = args.get("done")
    if done is None:
        done = True
    if not isinstance(done, bool):
        raise ToolError("issue_checklist_tick: done должен быть true или false")
    return client.checklist_tick(key, item_id, done=done)


def _op_issue_checklist_edit(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_checklist_edit"))
    item_id = str(_require(args, "item_id", "issue_checklist_edit"))
    text = args.get("text")
    order = args.get("sort_order")
    if text is None and order is None:
        raise ToolError("issue_checklist_edit: нужен text или sort_order")
    new_text = _as_text(text, "issue_checklist_edit: text") if text is not None else None
    return client.checklist_edit(
        key,
        item_id,
        text=new_text,
        sort_order=_as_float(order, "sort_order") if order is not None else None,
    )


def _op_issue_checklist_waive(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_checklist_waive"))
    item_id = str(_require(args, "item_id", "issue_checklist_waive"))
    undo = args.get("undo")
    if undo is not None and not isinstance(undo, bool):
        raise ToolError("issue_checklist_waive: undo должен быть true или false")
    if undo:
        return client.checklist_waive(key, item_id, undo=True)
    # Причина обязательна: снятый без объяснения критерий неотличим от забытого. Проверяем до
    # похода в API, чтобы агент получил понятный отказ, а не 422.
    reason = _as_text(
        _require(args, "reason", "issue_checklist_waive"), "issue_checklist_waive: reason"
    )
    return client.checklist_waive(key, item_id, reason=reason)


def _op_issue_checklist_remove(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_checklist_remove"))
    item_id = str(_require(args, "item_id", "issue_checklist_remove"))
    reason = str(args.get("reason") or "").strip() or None
    client.checklist_remove(key, item_id, reason=reason)
    # Сервер отвечает 204 без тела: агенту нужен явный след, что удалилось, а не пустой объект.
    return {"removed": True, "issue_key": key, "item_id": item_id}


def _op_attention_request(client: Any, args: Mapping[str, Any]) -> Any:
    body = _as_text(
        _require(args, "body_md", "attention_request"), "attention_request: body_md"
    )
    kind = str(args.get("kind") or "question")
    issue_key = args.get("issue_key")
    options = _as_str_list(args.get("options"), "options")
    return client.create_inbox(
        kind=kind,
        issue=str(issue_key) if issue_key is not None else None,
        body_md=body,
        options=options or None,
    )


def _op_inbox_list(client: Any, args: Mapping[str, Any]) -> Any:
    status = args.get("status")
    addressee = str(args.get("addressee") or "").strip()
    # `me` клиент не разворачивает: имя актора по токену знает только сервер.
    extra = {"addressee": addressee} if addressee else {}
    return client.inbox(status=str(status) if status is not None else "open", **extra)


def _op_search(client: Any, args: Mapping[str, Any]) -> Any:
    query = _as_text(_require(args, "q", "search"), "search: q")
    limit = args.get("limit")
    return client.search(query, limit=_as_int(limit, "limit") if limit is not None else None)


def _op_metrics(client: Any, args: Mapping[str, Any]) -> Any:
    days = args.get("days")
    return client.metrics(days=_as_int(days, "days") if days is not None else 30)


def _op_docs_list(client: Any, args: Mapping[str, Any]) -> Any:
    compact = args.get("compact")
    if compact is not None and not isinstance(compact, bool):
        raise ToolError("docs_list: compact должен быть true или false")
    return client.docs(compact=bool(compact))


def _op_doc_get(client: Any, args: Mapping[str, Any]) -> Any:
    slug = str(_require(args, "slug", "doc_get")).strip()
    if not slug:
        # Пустой слаг превратил бы путь в `/docs` — это дерево документов, а не страница.
        raise ToolError("doc_get: слаг не может быть пустым")
    return client.doc(slug)


def _op_doc_create(client: Any, args: Mapping[str, Any]) -> Any:
    slug = str(_require(args, "slug", "doc_create")).strip()
    if not slug:
        raise ToolError("doc_create: слаг не может быть пустым")
    title = str(_require(args, "title", "doc_create")).strip()
    if not title:
        raise ToolError("doc_create: заголовок не может быть пустым")
    known = {"slug", "title", "body_md", "parent_slug", "sort_order", "source_path"}
    unknown = sorted(set(args) - known)
    if unknown:
        raise ToolError("doc_create: неизвестные аргументы: " + ", ".join(unknown))
    parent = args.get("parent_slug")
    source_path = args.get("source_path")
    order = args.get("sort_order")
    return client.create_doc(
        slug,
        title,
        body_md=str(args.get("body_md") or ""),
        parent_slug=str(parent) if parent else None,
        sort_order=_as_float(order, "sort_order") if order is not None else 0,
        source_path=str(source_path) if source_path else None,
    )


def _op_doc_update(client: Any, args: Mapping[str, Any]) -> Any:
    slug = str(_require(args, "slug", "doc_update")).strip()
    if not slug:
        raise ToolError("doc_update: слаг не может быть пустым")
    patch = args.get("patch")
    if not isinstance(patch, Mapping) or not patch:
        raise ToolError("doc_update: patch должен быть непустым объектом")
    return client.update_doc(slug, patch, version=args.get("version"))


def _op_milestones_list(client: Any, args: Mapping[str, Any]) -> Any:
    return client.milestones()


def _op_me(client: Any, args: Mapping[str, Any]) -> Any:
    return client.me()


HANDLERS: dict[str, Callable[[Any, Mapping[str, Any]], Any]] = {
    "issue_list": _op_issue_list,
    "issue_get": _op_issue_get,
    "issue_create": _op_issue_create,
    "issue_update": _op_issue_update,
    "issue_claim": _op_issue_claim,
    "issue_heartbeat": _op_issue_heartbeat,
    "issue_release": _op_issue_release,
    "issue_comment": _op_issue_comment,
    "issue_worklog": _op_issue_worklog,
    "issue_transition": _op_issue_transition,
    "issue_link": _op_issue_link,
    "issue_attach": _op_issue_attach,
    "issue_events": _op_issue_events,
    "issue_checklist_add": _op_issue_checklist_add,
    "issue_checklist_tick": _op_issue_checklist_tick,
    "issue_checklist_edit": _op_issue_checklist_edit,
    "issue_checklist_waive": _op_issue_checklist_waive,
    "issue_checklist_remove": _op_issue_checklist_remove,
    "attention_request": _op_attention_request,
    "inbox_list": _op_inbox_list,
    "search": _op_search,
    "metrics": _op_metrics,
    "docs_list": _op_docs_list,
    "doc_get": _op_doc_get,
    "doc_create": _op_doc_create,
    "doc_update": _op_doc_update,
    "milestones_list": _op_milestones_list,
    "me": _op_me,
}


# --- JSON-RPC ----------------------------------------------------------------


def _result(request_id: Any, payload: Any) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": request_id, "result": payload}


def jsonrpc_error(request_id: Any, code: int, message: str, data: Any = None) -> dict[str, Any]:
    """Ответ-ошибка JSON-RPC. `data` несёт детали, если они есть."""
    error: dict[str, Any] = {"code": code, "message": message}
    if data is not None:
        error["data"] = data
    return {"jsonrpc": "2.0", "id": request_id, "error": error}


def _text_content(text: str) -> dict[str, Any]:
    return {"content": [{"type": "text", "text": text}], "isError": False}


def _json_content(payload: Any) -> dict[str, Any]:
    """Успешный результат инструмента: JSON-текстом, чтобы агент читал поля, а не прозу."""
    if payload is None:
        payload = {}
    text = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False, indent=2, default=str)
    return _text_content(text)


def _tool_error(message: str, data: Any = None) -> dict[str, Any]:
    """Отказ инструмента: текст для агента. `data` — детали отказа, если сервер их прислал."""
    text = message
    if data:
        text += "\nдетали: " + json.dumps(data, ensure_ascii=False, default=str)
    return {"content": [{"type": "text", "text": text}], "isError": True}


def _api_error_text(exc: BaseException) -> str | None:
    """Текст отказа API, если исключение похоже на отказ клиента.

    `None` означает «это не отказ API»: ядро инструментов не знает класса ошибки клиента —
    в пакете это `AjiraApiError`, в автономной сборке свой `ApiError`, — поэтому смотрит на
    поля `message`, `status`, `code` и `details`, которые есть у обоих.
    """
    message = getattr(exc, "message", None)
    if not isinstance(message, str):
        return None
    parts = [f"Ошибка API: {message}"]
    status = getattr(exc, "status", None)
    if status is not None:
        parts.append(f"статус {status}")
    code = getattr(exc, "code", None)
    if code and code != "http_error":
        parts.append(f"код {code}")
    details = getattr(exc, "details", None)
    if details:
        parts.append("детали: " + json.dumps(details, ensure_ascii=False, default=str))
    return ", ".join(parts)


#: Порядок работы агента. Это первое, что читает подключившийся клиент, поэтому здесь нет
#: ничего про конкретный проект: ключ проекта трекера дописывается ниже, из ответа API.
BASE_INSTRUCTIONS = (
    "Трекер задач AgentJira. Порядок работы: issue_list (status=ready, label=agent-ready, "
    "compact=true) → issue_claim с ttl_seconds (для долгой работы — больше, до 86400) → "
    "issue_heartbeat каждые ~1/3 TTL (иначе лиз истечёт и задача вернётся в ready) → прогресс в "
    "issue_comment и расход в issue_worklog (от него зависят суточные бюджеты токенов и денег) → "
    "issue_transition в review → done. Перед review нужен след работы после старта (issue_comment, "
    "issue_worklog или вложение), иначе 409. После review переводи в done сам, если у задачи нет "
    "метки needs-human: с ней закрывает человек, и снять её агент не может. Критерии приёмки видны "
    "в issue_get — отмечай их issue_checklist_tick по мере выполнения. Вопрос, утверждение или "
    "блокер — attention_request (Inbox), ответы человека читай через inbox_list; свои упоминания "
    "(@имя) — inbox_list с addressee=me. Блокер — ещё и "
    "issue_transition в blocked с причиной в note. Любая проблема или затруднение при работе с "
    "самим трекером (ошибка, непонятный отказ, неудобный или недостающий инструмент, неясная "
    "инструкция), нужный фикс или улучшение — задача через issue_create в проекте трекера: не "
    "обходи молча. Для вопросов человеку задачу не заводи."
)

#: Сколько страниц плана называть в инструкции: агенту нужен вход, а не оглавление.
INSTRUCTION_DOC_LIMIT = 5


def _count_of(payload: Any) -> int | None:
    """Число записей из ответа-списка: `count`, иначе длина `items`."""
    if not isinstance(payload, Mapping):
        return None
    count = payload.get("count")
    if isinstance(count, int):
        return count
    items = payload.get("items")
    return len(items) if isinstance(items, Sequence) else None


def _connection_context(client: Any) -> str:
    """Что агент должен знать о том, куда он подключился: проект, очередь, страницы плана.

    Все данные берутся из API, и любая неудача здесь не ломает `initialize`: без контекста
    остаётся статичная часть инструкции.
    """
    meta = _safe_call(client.meta)
    if not isinstance(meta, Mapping):
        return ""

    lines: list[str] = []
    project = meta.get("project") if isinstance(meta.get("project"), Mapping) else {}
    key = project.get("key")
    name = project.get("name")
    if key:
        lines.append(
            f"Проект: {key} — {name}." if name else f"Проект: {key}."
        )
        lines.append(
            f"Проект запроса задаётся заголовком X-Ajira-Project или аргументом --project; "
            f"сейчас это {key}."
        )

    # Правило про проект трекера — часть протокола, а не пожелание: агент видит инструкцию при
    # подключении, а не README репозитория, поэтому она должна быть здесь. Ключ называет API
    # (`tracker_project`), а не строка в коде: в чужом трекере он другой. Проекта с этим ключом
    # в базе может не быть — тогда `null` и правило идёт без конкретного ключа.
    tracker = meta.get("tracker_project")
    tracker_key = tracker.get("key") if isinstance(tracker, Mapping) else None
    tracker_name = tracker.get("name") if isinstance(tracker, Mapping) else None
    if tracker_key and tracker_name:
        where = f"проекте {tracker_key} ({tracker_name})"
    elif tracker_key:
        where = f"проекте {tracker_key}"
    else:
        where = "проекте самого трекера"
    lines.append(
        f"Если при работе с трекером возникла проблема или затруднение — ошибка, непонятный "
        f"отказ, неудобный или недостающий инструмент, неясная инструкция — или нужен фикс или "
        f"улучшение, заведи задачу через issue_create в {where}: что делал, что получил, что "
        f"ожидал. Этот проект про сам AgentJira. Вопрос человеку задаётся не задачей, а "
        f"attention_request; рабочие задачи проекта заводятся в проекте проекта."
    )

    ready = _count_of(_safe_call(client.list_issues, status=["ready"], label=["agent-ready"], limit=1))
    if ready is not None:
        lines.append(f"Готовы к работе (ready + agent-ready): {ready}.")
    inbox = _count_of(_safe_call(client.inbox, "open"))
    if inbox is not None:
        lines.append(f"Записей в Inbox, ожидающих человека: {inbox}.")

    pages = _plan_pages(client)
    if pages:
        lines.append("Страницы плана (doc_get по слагу): " + ", ".join(pages) + ".")

    guardrails = meta.get("guardrails") if isinstance(meta.get("guardrails"), Mapping) else {}
    if guardrails.get("require_checklist_done_for_done"):
        lines.append(
            "В проекте включён гвард критериев: в done задача уходит, только когда каждый "
            "критерий приёмки отмечен выполненным или «не требуется»."
        )
    if guardrails.get("require_human_for_done"):
        lines.append("В проекте включён гвард закрытия: в done задачу переводит только человек.")
    return " ".join(lines)


def _safe_call(function: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
    """Вызов API, который не должен мешать подключению: нет ответа — нет строки."""
    try:
        return function(*args, **kwargs)
    except Exception:  # noqa: BLE001 — контекст подключения не важнее самого подключения
        return None


def _plan_pages(client: Any) -> list[str]:
    """Слаги страниц плана для инструкции: только имена, без текстов."""
    payload = _safe_call(client.docs, compact=True)
    items = payload.get("items") if isinstance(payload, Mapping) else None
    if not isinstance(items, Sequence):
        return []
    names: list[str] = []
    for item in items:
        if isinstance(item, Mapping) and item.get("slug"):
            title = item.get("title")
            names.append(f"{item['slug']} ({title})" if title else str(item["slug"]))
    if len(names) > INSTRUCTION_DOC_LIMIT:
        extra = len(names) - INSTRUCTION_DOC_LIMIT
        names = names[:INSTRUCTION_DOC_LIMIT] + [f"и ещё {extra} — docs_list"]
    return names


def build_instructions(client: Any) -> str:
    """Инструкция из `initialize`: порядок работы плюс то, куда агент подключился."""
    context = _connection_context(client)
    return f"{BASE_INSTRUCTIONS}\n\n{context}" if context else BASE_INSTRUCTIONS


def call_tool(client: Any, name: str, arguments: Mapping[str, Any] | None = None) -> dict[str, Any]:
    """Вызов инструмента. Возвращает готовый блок `tools/call`: ошибка — тоже результат."""
    if name not in TOOLS_BY_NAME:
        return _tool_error(
            f"Неизвестный инструмент: {name}. Доступны: "
            + ", ".join(sorted(TOOLS_BY_NAME))
        )
    handler = HANDLERS[name]
    args: Mapping[str, Any] = arguments or {}
    try:
        payload = handler(client, args)
    except ToolError as exc:
        return _tool_error(str(exc))
    except Exception as exc:  # noqa: BLE001 — процесс не должен падать от одного вызова
        described = _api_error_text(exc)
        if described is None:
            return _tool_error(f"{name}: внутренняя ошибка: {exc!r}")
        return _tool_error(described)
    except (OSError, ValueError) as exc:
        return _tool_error(f"{name}: {exc}")
    return _json_content(payload)


def handle_request(client: Any, payload: Any) -> dict[str, Any] | None:
    """Обрабатывает один JSON-RPC запрос. `None` — это уведомление, отвечать не нужно.

    Чистая функция: без чтения stdin, поэтому её проверяют тестом напрямую.
    """
    if not isinstance(payload, Mapping):
        return jsonrpc_error(None, INVALID_REQUEST, "запрос должен быть JSON-объектом")

    method = payload.get("method")
    request_id = payload.get("id")
    is_notification = "id" not in payload
    params = payload.get("params")
    if params is None:
        params = {}
    if not isinstance(params, Mapping):
        return jsonrpc_error(request_id, INVALID_PARAMS, "params должен быть объектом")
    if not isinstance(method, str):
        if is_notification:
            return None
        return jsonrpc_error(request_id, INVALID_REQUEST, "поле method обязательно")

    # Уведомления MCP: ответа не требуют по протоколу.
    if is_notification or method.startswith("notifications/"):
        return None

    if method == "initialize":
        return _result(
            request_id,
            {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                "instructions": build_instructions(client),
            },
        )

    if method == "ping":
        return _result(request_id, {})

    if method == "tools/list":
        return _result(request_id, {"tools": [dict(tool) for tool in TOOLS]})

    if method == "tools/call":
        name = params.get("name")
        if not isinstance(name, str) or not name:
            return jsonrpc_error(request_id, INVALID_PARAMS, "tools/call требует name")
        arguments = params.get("arguments") or {}
        if not isinstance(arguments, Mapping):
            return jsonrpc_error(request_id, INVALID_PARAMS, "arguments должен быть объектом")
        return _result(request_id, call_tool(client, name, arguments))

    return jsonrpc_error(request_id, METHOD_NOT_FOUND, f"метод не поддерживается: {method}")


def serve(client: Any, stdin: Any = None, stdout: Any = None) -> int:
    """Цикл stdio: строка запроса на входе — строка ответа на выходе."""
    source = stdin if stdin is not None else sys.stdin
    sink = stdout if stdout is not None else sys.stdout
    for line in source:
        if not line.strip():
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError as exc:
            response: dict[str, Any] | None = jsonrpc_error(
                None, PARSE_ERROR, f"не разобрал JSON: {exc.msg}"
            )
        else:
            try:
                response = handle_request(client, payload)
            except Exception as exc:  # noqa: BLE001 — процесс не должен падать от одного запроса
                print(f"ajira-mcp: внутренняя ошибка: {exc!r}", file=sys.stderr)
                request_id = payload.get("id") if isinstance(payload, Mapping) else None
                response = jsonrpc_error(request_id, INTERNAL_ERROR, f"внутренняя ошибка: {exc}")
        if response is None:
            continue
        sink.write(json.dumps(response, ensure_ascii=False, default=str) + "\n")
        sink.flush()
    return 0

#: Адрес трекера по умолчанию. Переопределяется `AJIRA_URL` или аргументом `--url`.
DEFAULT_BASE_URL = "http://127.0.0.1:8787"
URL_ENV = "AJIRA_URL"
TOKEN_ENV = "AJIRA_TOKEN"
TOKEN_FILE_ENV = "AJIRA_TOKEN_FILE"
PROJECT_ENV = "AJIRA_PROJECT"
TIMEOUT_ENV = "AJIRA_TIMEOUT"
DEFAULT_TIMEOUT = 30.0

API_PREFIX = "/api/v1"

#: Кто действует с точки зрения сервера: значение заголовка `X-Ajira-Source`.
SOURCE = "mcp"

#: Пояснения к статусам, когда сервер не прислал свой `message`.
STATUS_HINTS: dict[int, str] = {
    400: "некорректный запрос",
    401: "нужен токен агента",
    403: "действие запрещено",
    404: "не найдено",
    405: "эндпоинт не поддерживает этот метод",
    409: "конфликт состояния задачи",
    413: "файл больше допустимого размера",
    429: "лимит исчерпан",
}

#: Вид запроса к человеку по умолчанию: чаще всего агент именно спрашивает.
DEFAULT_INBOX_KIND = "question"


class ApiError(Exception):
    """Отказ API или недоступность сервера.

    `status is None` означает, что сервер не ответил вовсе: не поднят, не тот адрес, таймаут.
    В этом случае заполнены `kind` (`unreachable`/`timeout`) и `message` — без трассировки.
    """

    def __init__(
        self,
        message: str,
        *,
        status: int | None = None,
        code: str = "http_error",
        details: Mapping[str, Any] | None = None,
        kind: str = "api",
    ) -> None:
        super().__init__(message)
        self.message = message
        self.status = status
        self.code = code
        self.details: dict[str, Any] = dict(details or {})
        self.kind = kind

    def __str__(self) -> str:
        return self.message


# --- мелкие помощники --------------------------------------------------------


def _clean(params: Mapping[str, Any]) -> dict[str, Any]:
    """Убирает пустые фильтры: `None` не должен превращаться в `param=None`."""
    return {key: value for key, value in params.items() if value is not None}


def _quote(value: Any) -> str:
    """Значение для пути URL: ключ задачи или slug не должны ничего сломать в адресе."""
    return urlparse.quote(str(value), safe="")


def _fmt_seconds(value: float) -> str:
    """Секунды без хвоста `.0`: текст читает человек."""
    return f"{value:g}"


def _resolve_timeout(value: float | None) -> float:
    """Таймаут запроса: аргумент, иначе `AJIRA_TIMEOUT`, иначе 30 секунд."""
    if value is not None:
        return float(value)
    raw = os.environ.get(TIMEOUT_ENV)
    if raw is None or not raw.strip():
        return DEFAULT_TIMEOUT
    try:
        seconds = float(raw)
    except ValueError:
        print(
            f"ajira-mcp: {TIMEOUT_ENV}={raw!r} не число, беру {DEFAULT_TIMEOUT:g} с",
            file=sys.stderr,
        )
        return DEFAULT_TIMEOUT
    if seconds <= 0:
        print(
            f"ajira-mcp: {TIMEOUT_ENV}={raw!r} не положительное число, беру {DEFAULT_TIMEOUT:g} с",
            file=sys.stderr,
        )
        return DEFAULT_TIMEOUT
    return seconds


def read_token_file(path: str | Path) -> str:
    """Читает токен из файла: секрет остаётся вне конфига агента, который обычно коммитят."""
    file_path = Path(path).expanduser()
    if not file_path.is_file():
        raise ApiError(f"файл с токеном не найден: {file_path}", code="no_such_file", kind="local")
    try:
        token = file_path.read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise ApiError(
            f"файл с токеном не прочитан: {file_path}: {exc}", code="token_file", kind="local"
        ) from exc
    if not token:
        raise ApiError(
            f"файл с токеном пуст: {file_path}", code="empty_token_file", kind="local"
        )
    return token


def resolve_token(token_file: str | Path | None = None) -> str | None:
    """Токен из файла (аргумент или `AJIRA_TOKEN_FILE`), иначе из `AJIRA_TOKEN`."""
    path = token_file if token_file is not None else os.environ.get(TOKEN_FILE_ENV)
    if path:
        return read_token_file(path)
    return os.environ.get(TOKEN_ENV) or None


def _parse_body(raw: bytes) -> Any:
    """Тело ответа как JSON, иначе как текст. Пустое тело — `None`."""
    if not raw.strip():
        return None
    text = raw.decode("utf-8", "replace")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def _server_message(payload: Any) -> str | None:
    """Текст отказа из тела ответа. Сообщение сервера не переписываем — отдаём как есть."""
    if isinstance(payload, Mapping):
        message = payload.get("message")
        if isinstance(message, str) and message.strip():
            return message.strip()
    if isinstance(payload, str) and payload.strip():
        return payload.strip()
    return None


def _api_error(status: int, payload: Any) -> ApiError:
    """Собирает `ApiError` из статуса и тела: код и детали берутся из ответа сервера."""
    fallback = STATUS_HINTS.get(status, f"сервер ответил {status}")
    code = "http_error"
    details: dict[str, Any] = {}
    if isinstance(payload, Mapping):
        raw_code = payload.get("error")
        if isinstance(raw_code, str) and raw_code:
            code = raw_code
        raw_details = payload.get("details")
        if isinstance(raw_details, Mapping):
            details = dict(raw_details)
    return ApiError(
        _server_message(payload) or fallback, status=status, code=code, details=details
    )


def _multipart_body(field: str, filename: str, content: bytes, mime: str) -> tuple[bytes, str]:
    """Собирает тело `multipart/form-data` с одним файловым полем.

    Возвращает тело и значение заголовка `Content-Type` с тем же boundary. `urllib` такого не
    умеет, поэтому форма собирается вручную; имя файла экранируется, чтобы не сломать заголовок.
    """
    boundary = "----ajira" + uuid.uuid4().hex
    safe_name = filename.replace("\\", "\\\\").replace('"', '\\"')
    safe_name = safe_name.replace("\r", " ").replace("\n", " ")
    head = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{field}"; filename="{safe_name}"\r\n'
        f"Content-Type: {mime}\r\n"
        "\r\n"
    ).encode("utf-8")
    tail = f"\r\n--{boundary}--\r\n".encode("utf-8")
    return head + content + tail, f"multipart/form-data; boundary={boundary}"


def _decode_response(status: int, raw: bytes) -> Any:
    """Разбирает ответ: статус 4xx/5xx превращается в `ApiError`, остальное — в данные."""
    payload = _parse_body(raw)
    if status >= 400:
        raise _api_error(status, payload)
    return payload


def _idempotency(key: str | None) -> dict[str, str] | None:
    """Заголовок `Idempotency-Key`, если ключ задан. Транспорт запросы не повторяет и ключ
    не придумывает: защитить повтор может только ключ того, кто повторяет."""
    return {"Idempotency-Key": key} if key else None


# --- транспорт ---------------------------------------------------------------


class Transport:
    """HTTP-транспорт на `urllib`: адрес, токен, проект и таймаут.

    Ввод-вывод вынесен в `send` — единственное место, которое ходит в сеть. Тесты подменяют
    именно его и проверяют ушедший запрос целиком: метод, адрес, заголовки и тело.

    Методы повторяют `AjiraClient` из пакета: ядро инструментов зовёт клиента одинаково в обеих
    сборках, поэтому имена и умолчания здесь не произвольны.
    """

    def __init__(
        self,
        base_url: str | None = None,
        token: str | None = None,
        timeout: float | None = None,
        *,
        token_file: str | Path | None = None,
        project: str | None = None,
    ) -> None:
        resolved = base_url or os.environ.get(URL_ENV) or DEFAULT_BASE_URL
        self.base_url = resolved.rstrip("/")
        self.token = token if token is not None else resolve_token(token_file)
        self.project = project if project is not None else (os.environ.get(PROJECT_ENV) or None)
        self.timeout = _resolve_timeout(timeout)

    def close(self) -> None:
        """Совместимость с клиентом пакета: у `urllib` закрывать нечего."""

    def __enter__(self) -> "Transport":
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    # --- низкий уровень -----------------------------------------------------

    def send(
        self,
        method: str,
        url: str,
        headers: Mapping[str, str],
        body: bytes | None,
    ) -> tuple[int, bytes]:
        """Единственный выход в сеть. Ответ с любым статусом отдаётся как есть."""
        # Так выглядит самая частая ошибка в настройках: `127.0.0.1:8787` без схемы. Своё
        # объяснение полезнее, чем `unknown url type` от `urllib`.
        if urlparse.urlsplit(url).scheme not in ("http", "https"):
            raise ApiError(
                f"адрес трекера не разобран: {url} (нужна схема http:// или https://)",
                code="bad_url",
                kind="local",
            )
        try:
            request = urlrequest.Request(url, data=body, headers=dict(headers), method=method)
            with urlrequest.urlopen(request, timeout=self.timeout) as response:
                return int(response.getcode() or 0), response.read()
        except urlerror.HTTPError as exc:
            # Тело отказа несёт `message`, `error` и `details`: читаем его, а не гадаем по коду.
            return int(exc.code), exc.read()
        except socket.timeout as exc:
            raise ApiError(
                f"сервер {self.base_url} не ответил за {_fmt_seconds(self.timeout)} с",
                code="timeout",
                kind="timeout",
            ) from exc
        except urlerror.URLError as exc:
            if isinstance(exc.reason, socket.timeout):
                raise ApiError(
                    f"сервер {self.base_url} не ответил за {_fmt_seconds(self.timeout)} с",
                    code="timeout",
                    kind="timeout",
                ) from exc
            raise ApiError(
                f"сервер {self.base_url} недоступен: {exc.reason}",
                code="unreachable",
                kind="unreachable",
            ) from exc
        except (ValueError, OSError) as exc:
            raise ApiError(
                f"запрос к {url} не отправлен: {exc}", code="bad_request", kind="local"
            ) from exc

    def _headers(
        self,
        *,
        content_type: str | None = None,
        extra: Mapping[str, str] | None = None,
    ) -> dict[str, str]:
        """Заголовки запроса: источник, токен агента и проект, о котором спрашивают."""
        headers = {"Accept": "application/json", "X-Ajira-Source": SOURCE}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        if self.project:
            headers["X-Ajira-Project"] = self.project
        if content_type:
            headers["Content-Type"] = content_type
        if extra:
            headers.update(extra)
        return headers

    def request(
        self,
        method: str,
        path: str,
        *,
        params: Mapping[str, Any] | None = None,
        json_body: Any = None,
        body: bytes | None = None,
        content_type: str | None = None,
        extra_headers: Mapping[str, str] | None = None,
    ) -> Any:
        """Собирает запрос и отдаёт разобранный ответ. Отказ API — `ApiError`."""
        url = self.base_url + path
        if params:
            query = urlparse.urlencode(_clean(params), doseq=True)
            if query:
                url = f"{url}?{query}"
        headers = self._headers(content_type=content_type, extra=extra_headers)
        data = body
        if json_body is not None:
            data = json.dumps(json_body, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json; charset=utf-8"
        status, raw = self.send(method, url, headers, data)
        return _decode_response(status, raw)

    # --- эндпоинты ----------------------------------------------------------

    def list_issues(self, **filters: Any) -> Any:
        """Список задач. Списки фильтров уходят повторением параметра: `?status=ready&status=blocked`."""
        return self.request("GET", f"{API_PREFIX}/issues", params=filters)

    def create_issue(self, **fields: Any) -> Any:
        """Создать задачу. `idempotency_key` уходит заголовком, как у клиента пакета."""
        headers = _idempotency(fields.pop("idempotency_key", None))
        return self.request(
            "POST", f"{API_PREFIX}/issues", json_body=dict(fields), extra_headers=headers
        )

    def get_issue(self, key: str) -> Any:
        """Карточка задачи: поля, лиз, связи, вложения, комментарии, доступные переходы."""
        return self.request("GET", f"{API_PREFIX}/issues/{_quote(key)}")

    def update_issue(self, key: str, patch: Mapping[str, Any], version: Any = None) -> Any:
        """Правка полей. `version` уходит в `If-Match` — оптимистическая блокировка."""
        headers = {"If-Match": str(version)} if version is not None else None
        return self.request(
            "PATCH",
            f"{API_PREFIX}/issues/{_quote(key)}",
            json_body=dict(patch),
            extra_headers=headers,
        )

    def claim(self, key: str, ttl_seconds: int | None = None) -> Any:
        """Взять задачу в работу: создаёт лиз."""
        body: dict[str, Any] = {}
        if ttl_seconds is not None:
            body["ttl_seconds"] = ttl_seconds
        return self.request("POST", f"{API_PREFIX}/issues/{_quote(key)}/claim", json_body=body)

    def heartbeat(self, key: str, ttl_seconds: int | None = None) -> Any:
        """Продлить лиз."""
        body: dict[str, Any] = {}
        if ttl_seconds is not None:
            body["ttl_seconds"] = ttl_seconds
        return self.request("POST", f"{API_PREFIX}/issues/{_quote(key)}/heartbeat", json_body=body)

    def release(self, key: str, reason: str | None = None) -> Any:
        """Освободить задачу, не дожидаясь TTL."""
        body: dict[str, Any] = {}
        if reason is not None:
            body["reason"] = reason
        return self.request("POST", f"{API_PREFIX}/issues/{_quote(key)}/release", json_body=body)

    def transition(self, key: str, to: str, note: str | None = None) -> Any:
        """Сменить статус: гварды сервера решают, хватает ли доказательств работы."""
        body: dict[str, Any] = {"to": to}
        if note is not None:
            body["note"] = note
        return self.request(
            "POST", f"{API_PREFIX}/issues/{_quote(key)}/transitions", json_body=body
        )

    def comment(
        self,
        key: str,
        body_md: str,
        attachments: Sequence[str] | None = None,
        *,
        idempotency_key: str | None = None,
    ) -> Any:
        """Комментарий в markdown. `attachments` — id уже загруженных вложений."""
        body: dict[str, Any] = {"body_md": body_md}
        if attachments is not None:
            body["attachments"] = list(attachments)
        return self.request(
            "POST",
            f"{API_PREFIX}/issues/{_quote(key)}/comments",
            json_body=body,
            extra_headers=_idempotency(idempotency_key),
        )

    def worklog(
        self,
        key: str,
        note: str = "",
        *,
        seconds: int | None = None,
        tokens_in: int = 0,
        tokens_out: int = 0,
        cost_usd: float = 0.0,
        idempotency_key: str | None = None,
    ) -> Any:
        """Запись о проделанной работе: время, токены и деньги — основа метрик и бюджетов."""
        body: dict[str, Any] = {
            "note": note,
            "tokens_in": tokens_in,
            "tokens_out": tokens_out,
            "cost_usd": cost_usd,
        }
        if seconds is not None:
            body["seconds"] = seconds
        return self.request(
            "POST",
            f"{API_PREFIX}/issues/{_quote(key)}/worklogs",
            json_body=body,
            extra_headers=_idempotency(idempotency_key),
        )

    def link(
        self,
        key: str,
        to: str,
        type: str,  # noqa: A002 — имя как у клиента пакета
        *,
        idempotency_key: str | None = None,
    ) -> Any:
        """Связать две задачи. Обратный тип связи сервер выводит сам."""
        return self.request(
            "POST",
            f"{API_PREFIX}/issues/{_quote(key)}/links",
            json_body={"to": to, "type": type},
            extra_headers=_idempotency(idempotency_key),
        )

    def attach(self, key: str, path: Path, *, idempotency_key: str | None = None) -> Any:
        """Загрузить файл-артефакт: тело `multipart/form-data` собирается вручную."""
        content = path.read_bytes()
        mime = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        body, content_type = _multipart_body("file", path.name, content, mime)
        return self.request(
            "POST",
            f"{API_PREFIX}/issues/{_quote(key)}/attachments",
            body=body,
            content_type=content_type,
            extra_headers=_idempotency(idempotency_key),
        )

    def issue_events(self, key: str, limit: int | None = None) -> Any:
        """История конкретной задачи."""
        return self.request(
            "GET", f"{API_PREFIX}/issues/{_quote(key)}/events", params={"limit": limit}
        )

    def checklist_add(self, key: str, text: str, sort_order: float | None = None) -> Any:
        """Добавить критерий приёмки."""
        body: dict[str, Any] = {"text": text}
        if sort_order is not None:
            body["sort_order"] = sort_order
        return self.request(
            "POST", f"{API_PREFIX}/issues/{_quote(key)}/checklist", json_body=body
        )

    def checklist_tick(self, key: str, item_id: str, done: bool = True) -> Any:
        """Отметить критерий приёмки выполненным или снять отметку."""
        return self.request(
            "PATCH",
            f"{API_PREFIX}/issues/{_quote(key)}/checklist/{_quote(item_id)}",
            json_body={"done": done},
        )

    def checklist_edit(
        self,
        key: str,
        item_id: str,
        *,
        text: str | None = None,
        sort_order: float | None = None,
    ) -> Any:
        """Переименовать пункт чеклиста или поменять его позицию."""
        body: dict[str, Any] = {}
        if text is not None:
            body["text"] = text
        if sort_order is not None:
            body["sort_order"] = sort_order
        return self.request(
            "PATCH",
            f"{API_PREFIX}/issues/{_quote(key)}/checklist/{_quote(item_id)}",
            json_body=body,
        )

    def checklist_waive(
        self, key: str, item_id: str, *, reason: str | None = None, undo: bool = False
    ) -> Any:
        """«Не требуется»: закрыть пункт без выполнения. `undo=True` снимает отметку."""
        body: dict[str, Any] = {"waived": not undo}
        if not undo:
            body["reason"] = reason
        return self.request(
            "PATCH",
            f"{API_PREFIX}/issues/{_quote(key)}/checklist/{_quote(item_id)}",
            json_body=body,
        )

    def checklist_remove(self, key: str, item_id: str, reason: str | None = None) -> Any:
        """Удалить пункт физически; причина уходит query-параметром."""
        return self.request(
            "DELETE",
            f"{API_PREFIX}/issues/{_quote(key)}/checklist/{_quote(item_id)}",
            params={"reason": reason},
        )

    def create_inbox(
        self,
        *,
        kind: str = DEFAULT_INBOX_KIND,
        issue: str | None = None,
        body_md: str = "",
        options: Sequence[str] | None = None,
    ) -> Any:
        """Запрос к человеку: вопрос, утверждение, правки или блокер попадают в Inbox."""
        body: dict[str, Any] = {"kind": kind, "body_md": body_md}
        if issue is not None:
            body["issue"] = issue
        if options is not None:
            body["options"] = list(options)
        return self.request("POST", f"{API_PREFIX}/inbox", json_body=body)

    def inbox(self, status: str = "open", addressee: str | None = None) -> Any:
        """Очередь «нужен человек»; с `addressee` — упоминания актора (`me` — свои, по токену)."""
        return self.request(
            "GET", f"{API_PREFIX}/inbox", params={"status": status, "addressee": addressee}
        )

    def search(self, q: str, limit: int | None = None) -> Any:
        """Полнотекстовый поиск по задачам."""
        return self.request("GET", f"{API_PREFIX}/search", params={"q": q, "limit": limit})

    def metrics(self, days: int = 30) -> Any:
        """Метрики за период: цикл, простой, расход агентов и остаток бюджетов."""
        return self.request("GET", f"{API_PREFIX}/metrics", params={"days": days})

    def meta(self) -> Any:
        """Словари домена, граф переходов, гварды и проект запроса."""
        return self.request("GET", f"{API_PREFIX}/meta")

    def docs(self, compact: bool = False) -> Any:
        """Дерево страниц «Планы». `compact=True` — без текстов."""
        params = {"compact": "true"} if compact else {}
        return self.request("GET", f"{API_PREFIX}/docs", params=params or None)

    def doc(self, slug: str) -> Any:
        """Страница «Планы» по slug."""
        return self.request("GET", f"{API_PREFIX}/docs/{_quote(slug)}")

    def create_doc(
        self,
        slug: str,
        title: str,
        *,
        body_md: str = "",
        parent_slug: str | None = None,
        sort_order: float = 0,
        source_path: str | None = None,
    ) -> Any:
        """Создать страницу «Планы»: текст в базе или указатель на файл репозитория."""
        body = _clean(
            {
                "slug": slug,
                "title": title,
                "body_md": body_md,
                "parent_slug": parent_slug,
                "sort_order": sort_order,
                "source_path": source_path,
            }
        )
        return self.request("POST", f"{API_PREFIX}/docs", json_body=body)

    def update_doc(self, slug: str, patch: Mapping[str, Any], version: Any = None) -> Any:
        """Правка страницы «Планы». `version` уходит в `If-Match`."""
        headers = {"If-Match": str(version)} if version is not None else None
        return self.request(
            "PATCH",
            f"{API_PREFIX}/docs/{_quote(slug)}",
            json_body=dict(patch),
            extra_headers=headers,
        )

    def milestones(self) -> Any:
        """Вехи роадмапа с прогрессом по задачам."""
        return self.request("GET", f"{API_PREFIX}/milestones")

    def me(self) -> Any:
        """Кто я: лимиты, остаток бюджета, активные лизы."""
        return self.request("GET", f"{API_PREFIX}/me")


# --- точка входа -------------------------------------------------------------


def _force_utf8_stdio() -> None:
    """Переводит stdio в UTF-8: так требует MCP, а консоль Windows по умолчанию не в UTF-8.

    Без этого русский текст ответа доезжает до агента кракозябрами: `python ajira_mcp.py`
    в cmd или PowerShell декодирует поток кодовой страницей системы.
    """
    for stream in (sys.stdin, sys.stdout):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")


def run_stdio(
    *,
    url: str | None = None,
    token: str | None = None,
    token_file: str | Path | None = None,
    project: str | None = None,
    timeout: float | None = None,
    stdin: Any = None,
    stdout: Any = None,
) -> int:
    """Запускает сервер на готовом транспорте: так его запускают и CLI, и тесты."""
    transport = Transport(
        base_url=url,
        token=token,
        timeout=timeout,
        token_file=token_file,
        project=project,
    )
    return serve(transport, stdin=stdin, stdout=stdout)


def main(
    argv: Sequence[str] | None = None, stdin: Any = None, stdout: Any = None
) -> int:
    """Точка входа: разбирает свои аргументы и запускает цикл stdio."""
    parser = argparse.ArgumentParser(
        prog="ajira_mcp.py",
        description="Автономный MCP-сервер AgentJira по stdio (JSON-RPC 2.0)",
    )
    parser.add_argument("--url", default=None, help="адрес трекера (иначе AJIRA_URL)")
    parser.add_argument("--token", default=None, help="токен агента (иначе AJIRA_TOKEN)")
    parser.add_argument(
        "--token-file",
        dest="token_file",
        default=None,
        help="файл с токеном агента (иначе AJIRA_TOKEN_FILE): секрет остаётся вне конфига агента",
    )
    parser.add_argument(
        "--project",
        default=None,
        help="ключ проекта (иначе AJIRA_PROJECT); без него — проект по умолчанию трекера",
    )
    parser.add_argument(
        "--timeout", type=float, default=None, help="таймаут запроса в секундах (иначе AJIRA_TIMEOUT)"
    )
    args = parser.parse_args(list(argv) if argv is not None else None)
    _force_utf8_stdio()
    return run_stdio(
        url=args.url,
        token=args.token,
        token_file=args.token_file,
        project=args.project,
        timeout=args.timeout,
        stdin=stdin,
        stdout=stdout,
    )

if __name__ == "__main__":
    sys.exit(main())
