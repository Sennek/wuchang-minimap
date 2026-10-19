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
* `AJIRA_TIMEOUT` — таймаут одного запроса в секундах, по умолчанию 30;
* `AJIRA_AUTO_UPDATE` — `1`: при старте обновить мост с трекера (`--auto-update`).

Мост обновляет себя сам: инструмент `bridge_update` скачивает с трекера актуальную папку моста
(`GET /api/v1/mcp/bridge`), проверяет её и заменяет файлы, оставляя прежние в `.ajira-backup`.

Каждый запрос уходит с заголовком `X-Ajira-Source: mcp`: так событие в истории трекера показывает,
что действие сделано через MCP. Условная правка задачи (`issue_update` с `version`) отправляет
`If-Match` — на этом стоит оптимистическая блокировка сервера.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import types
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Mapping, Sequence
import argparse
import mimetypes
import socket
import uuid
from pathlib import Path
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

#: Размер строки stdin в байтах (без перевода строки), который сервер согласен принять: защита
#: памяти от мусора в потоке. Строка длиннее — ответ -32700, хвост её пропускается.
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
_VERBOSE = {
    "type": "boolean",
    "description": "true — полная карточка в ответе, как у issue_get. По умолчанию краткий ответ",
}
_VERBOSE_ITEM = {
    "type": "boolean",
    "description": "true — пункт чеклиста целиком: кто и когда отметил, причина снятия. "
    "По умолчанию краткий ответ",
}
_ITEM_IDS = {
    "type": "array",
    "items": {"type": "string"},
    "description": "несколько пунктов за один вызов; вместо item_id. Ответ — списком items",
}
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
#: Статусы вехи — те же, что `MILESTONE_STATUSES` в `ajira/enums.py` (ядро пакет не импортирует).
_MILESTONE_STATUSES = ("planned", "active", "done")
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
        "Карточка задачи: поля, лиза, доступные переходы, вложения и комментарии. Описание "
        "уже прочитано — перечитывай с compact=true или fields. Из переписки обычно нужен "
        "последний комментарий (хендофф) — comments_last=1.",
        {
            "issue_key": _ISSUE_KEY,
            "compact": {
                "type": "boolean",
                "description": (
                    "карточка без описания, комментариев и вложений: статус, лиз, переходы, "
                    "чеклист с item_id, связанные задачи как {key, title, status}. С "
                    "comments_last комментарии остаются"
                ),
            },
            "comments_last": {
                "type": "integer",
                "minimum": 0,
                "description": (
                    "только последние N комментариев; сколько более ранних не вошло — в "
                    "comments_omitted, всего — в counts.comments. По умолчанию все"
                ),
            },
            "fields": {
                **_STRING_LIST,
                "description": (
                    "какие поля карточки вернуть, точно: например [\"status\",\"checklist\"]. "
                    "Сильнее compact"
                ),
            },
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_create",
        "Создать задачу или подзадачу. Метка agent-ready разрешает агентам её взять. Отвечает "
        "id, key, status и version; карточка — в issue_get.",
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
            "project": {
                "type": "string",
                "description": "ключ проекта, если не тот, к которому подключён сервер: "
                "например задача про сам трекер",
            },
            "idempotency_key": _IDEMPOTENCY_KEY,
            "verbose": _VERBOSE,
        },
        required=("title",),
    ),
    _tool(
        "issue_update",
        "Правка полей задачи. version уходит в If-Match: при рассинхроне сервер ответит 409 "
        "version_conflict с current_version — повтори с ней. Отвечает id, key, status и новой "
        "version.",
        {
            "issue_key": _ISSUE_KEY,
            "patch": {
                "type": "object",
                "description": "изменяемые поля: title, description_md, priority, labels, meta, ...",
            },
            "version": {
                "type": "integer",
                "description": "версия из issue_get или из ответа последней мутации; claim и "
                "переходы её поднимают",
            },
            "verbose": _VERBOSE,
        },
        required=("issue_key", "patch"),
    ),
    _tool(
        "issue_claim",
        "Взять задачу в работу: создаёт лиз и переводит её в in_progress — из ready, blocked и "
        "failed. Вернуть задачу в работу после failed или снятого лиза можно только так: "
        "issue_transition в in_progress без лиза получит отказ. 409 — задача занята, 429 — "
        "исчерпан лимит агента. "
        "claim поднимает version задачи: version, прочитанная до него, устарела — для "
        "issue_update бери issue.version из ответа.",
        {
            "issue_key": _ISSUE_KEY,
            "ttl_seconds": {"type": "integer", "description": "срок лиза, по умолчанию серверный"},
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_heartbeat",
        "Продлить лиз. Пропущенный heartbeat — лиз истекает, задача из in_progress "
        "возвращается в ready.",
        {
            "issue_key": _ISSUE_KEY,
            "ttl_seconds": {"type": "integer", "description": "новый срок лиза"},
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_release",
        "Освободить задачу явно, не дожидаясь TTL. Из in_progress задача возвращается в "
        "ready; blocked и review сохраняют статус — заблокированную задачу отпускай, чтобы "
        "не занимать слот, блокер с причиной останется.",
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
        "Сменить статус. Гварды проверяют доказательства: для review нужен след работы. "
        "Переход в done, cancelled, failed или ready сам снимает лиз — issue_release после "
        "него не нужен. В in_progress пускает только действующий лиз: из ready, blocked и "
        "failed без него — issue_claim. Вернуть сданную на доработку — в ready с note (лиз "
        "снимется, задачу возьмут issue_claim; note из review или done в ready станет "
        "комментарием в карточке); из review в in_progress — только держатель "
        "лиза. Задачу в in_progress без действующего лиза (ничью) в ready возвращает любой. "
        "Решили не делать — в cancelled из любого незакрытого статуса с причиной в note. Уже "
        "сделано в другом месте или дубликат — из backlog, ready, blocked или failed сразу в "
        "done без лиза, в note — где сделано («сделано в X», «дубликат Y»); эпик, все подзадачи "
        "которого закрыты, — так же, note не нужен. Обе отмены и закрытие мимо review агенту "
        "запрещает метка needs-human; человек увидит их в Inbox. "
        "Отвечает key, status, version и available_transitions; карточка — в issue_get.",
        {
            "issue_key": _ISSUE_KEY,
            "to": _STATUS,
            "note": {"type": "string", "description": "объяснение перехода"},
            "verbose": _VERBOSE,
        },
        required=("issue_key", "to"),
    ),
    _tool(
        "issue_link",
        "Связать две задачи. Обратный тип связи сервер выводит сам. Отвечает созданной связью "
        "и списком связей задачи, связанные задачи — как {key, title, status}.",
        {
            "issue_key": _ISSUE_KEY,
            "to": {"type": "string", "description": "ключ второй задачи"},
            "type": _LINK_TYPE,
            "idempotency_key": _IDEMPOTENCY_KEY,
            "verbose": _VERBOSE,
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
        "пройдёт гвард backlog → ready. Отвечает issue_key и item_id нового пункта.",
        {
            "issue_key": _ISSUE_KEY,
            "text": {"type": "string", "description": "формулировка критерия"},
            "sort_order": {"type": "number", "description": "позиция в списке, иначе в конец"},
            "verbose": _VERBOSE_ITEM,
        },
        required=("issue_key", "text"),
    ),
    _tool(
        "issue_checklist_tick",
        "Отметить критерий приёмки выполненным или снять отметку. item_id (или item_ids — сразу "
        "несколько) берётся из карточки issue_get.",
        {
            "issue_key": _ISSUE_KEY,
            "item_id": {"type": "string", "description": "id пункта чеклиста"},
            "item_ids": _ITEM_IDS,
            "done": {
                "type": "boolean",
                "description": "true отмечает выполнение, false снимает; по умолчанию true",
            },
            "verbose": _VERBOSE_ITEM,
        },
        required=("issue_key",),
    ),
    _tool(
        "issue_checklist_edit",
        "Переименовать критерий приёмки или поменять его позицию в списке.",
        {
            "issue_key": _ISSUE_KEY,
            "item_id": {"type": "string", "description": "id пункта чеклиста"},
            "text": {"type": "string", "description": "новая формулировка"},
            "sort_order": {"type": "number", "description": "новая позиция в списке"},
            "verbose": _VERBOSE_ITEM,
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
            "item_ids": _ITEM_IDS,
            "reason": {"type": "string", "description": "почему критерий не требуется"},
            "undo": {"type": "boolean", "description": "true снимает отметку «не требуется»"},
            "verbose": _VERBOSE_ITEM,
        },
        required=("issue_key",),
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
        "inbox_resolve",
        "Снять свой запрос из Inbox, когда ответ пришёл другим каналом (чат сессии, комментарий) "
        "или вопрос отпал; так же убирается своё упоминание. Чужие записи снимает только человек. "
        "В note — где получен ответ. Задачу из blocked это не возвращает: переведи её сам.",
        {
            "item_id": {"type": "string", "description": "id записи из inbox_list"},
            "note": {
                "type": "string",
                "description": (
                    "где получен ответ и что решено, например «отвечено в чате, решение — "
                    "design/systems/10-bestiary.md»"
                ),
            },
            "status": {
                "type": "string",
                "enum": ["resolved", "dismissed"],
                "description": (
                    "resolved — ответ получен (по умолчанию), dismissed — вопрос отпал, запрос отозван"
                ),
            },
        },
        required=("item_id", "note"),
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
        "Правка страницы плана. version уходит в If-Match: при рассинхроне сервер ответит 409 "
        "version_conflict с current_version — повтори с ней. "
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
        "вехе относится задача и что ещё в ней не закрыто. Описания вех длинные — для выбора "
        "бери compact=true или fields, открытые вехи — status=[\"planned\",\"active\"].",
        {
            "status": {
                "type": "array",
                "items": {"type": "string", "enum": list(_MILESTONE_STATUSES)},
                "description": "только вехи с этими статусами: planned, active, done",
            },
            "compact": {
                "type": "boolean",
                "description": (
                    "без описаний: id, name, status, target_date, прогресс (total, done, closed, "
                    "progress) и version для milestone_update"
                ),
            },
            "fields": {
                **_STRING_LIST,
                "description": (
                    "какие поля вехи вернуть, точно: например [\"name\",\"status\",\"progress\"]. "
                    "Сильнее compact"
                ),
            },
        },
    ),
    _tool(
        "milestone_create",
        "Создать веху роадмапа. Задачи привязываются к ней полем milestone по имени, поэтому "
        "имя в проекте уникально: занятое даёт 409 milestone_exists, а в details.milestone — "
        "справка о существующей вехе (id, name, status, target_date, version, без описания), "
        "её и бери.",
        {
            "name": {"type": "string", "description": "имя вехи, например «Культивация»"},
            "description_md": {"type": "string", "description": "что входит в веху, markdown"},
            "target_date": {
                "type": "string",
                "description": "срок, дата ISO 8601, например 2026-12-01",
            },
            "status": {
                "type": "string",
                "enum": list(_MILESTONE_STATUSES),
                "description": "planned (по умолчанию), active или done",
            },
            "sort_order": {"type": "number", "description": "позиция на роадмапе"},
        },
        required=("name",),
    ),
    _tool(
        "milestone_update",
        "Правка вехи: имя, описание, срок, статус, порядок. Закрыть веху — status=done. "
        "version уходит в If-Match: при рассинхроне сервер ответит 409 version_conflict с "
        "current_version — повтори с ней.",
        {
            "milestone": {"type": "string", "description": "имя вехи или её id"},
            "patch": {
                "type": "object",
                "description": (
                    "изменяемые поля: name, description_md, target_date (null — снять срок), "
                    "status, sort_order"
                ),
            },
            "version": {"type": "integer", "description": "версия из milestones_list"},
        },
        required=("milestone", "patch"),
    ),
    _tool(
        "me",
        "Кто я: остаток суточного бюджета и мои активные лизы.",
        {},
    ),
    _tool(
        "bridge_update",
        "Обновить этот MCP-мост с трекера: скачивает актуальную папку моста (ajira_mcp.py, "
        "README.md, mcp.json.example, skills/agentjira/SKILL.md), сверяет sha256 и отпечаток "
        "каталога, заменяет изменившиеся файлы и кладёт прежние в .ajira-backup рядом. Зови, "
        "когда инструкция на initialize предупреждает, что мост разошёлся с сервером. Новые "
        "инструменты появятся только в новой сессии агента: список инструментов клиент "
        "запоминает при подключении. Мост из пакета ajira (ajira mcp) обновляется вместе с "
        "трекером — ему заменять нечего.",
        {
            "check_only": {
                "type": "boolean",
                "description": "только сравнить версии и файлы, ничего не записывая",
            },
        },
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
    payload = client.list_issues(**filters)
    compact = bool(filters.get("compact"))
    fields = filters.get("fields") or []
    if not (compact or fields):
        return payload
    # Сужаем и здесь: трекер до compact/fields молча отдал бы полный список с описаниями.
    return _narrow_list(payload, "issue_list", _ISSUE_COMPACT_FIELDS, compact=compact, fields=fields)


#: Поля compact-списка задач — те же, что `COMPACT_FIELDS` в `ajira/service/issues.py`: ядро
#: пакет не импортирует, совпадение сверяет тест.
_ISSUE_COMPACT_FIELDS = (
    "key",
    "title",
    "type",
    "status",
    "priority",
    "assignee",
    "parent",
    "milestone",
    "labels",
    "checklist_done",
    "checklist_total",
    "updated_at",
    "version",
)


def _op_issue_get(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_get"))
    fields = _as_str_list(args.get("fields"), "fields")
    compact = args.get("compact")
    if compact is not None and not isinstance(compact, bool):
        raise ToolError("issue_get: compact должен быть true или false")
    last = args.get("comments_last")
    if last is not None:
        last = _as_int(last, "comments_last")
        if last < 0:
            raise ToolError("issue_get: comments_last не может быть отрицательным")
        card = client.get_issue(key, comments_last=last)
    else:
        card = client.get_issue(key)
    # Сужаем здесь, а не на сервере: так флаги работают и со старым трекером, а экономится
    # именно контекст агента — карточка, перечитанная пять раз, оплачивается на каждом шаге.
    if not isinstance(card, Mapping):
        return card
    if last is not None:
        card = _last_comments(card, last)
    if fields:
        unknown = [name for name in fields if name not in card]
        if unknown:
            raise ToolError(
                "issue_get: неизвестные поля в fields: "
                + ", ".join(unknown)
                + ". Есть: "
                + ", ".join(sorted(card))
            )
        return {name: card[name] for name in fields}
    if not compact:
        return card
    # Явно попрошенные последние комментарии compact не выбрасывает: «статус и хендофф» —
    # ровно то, с чего агент начинает сессию.
    dropped = _GET_COMPACT_DROP if last is None else ("description_md", "attachments")
    brief = {name: value for name, value in card.items() if name not in dropped}
    if isinstance(card.get("checklist"), list):
        brief["checklist"] = [
            {name: item.get(name) for name in ("id", "text", "done", "waived")}
            if isinstance(item, Mapping)
            else item
            for item in card["checklist"]
        ]
    if isinstance(card.get("links"), list):
        brief["links"] = [
            {**item, "issue": _issue_brief(item.get("issue"))} if isinstance(item, Mapping) else item
            for item in card["links"]
        ]
    return brief


#: Что compact-карточка оставляет за бортом: длинные тексты, которые агент уже прочитал.
#: Их число остаётся в `counts`.
_GET_COMPACT_DROP = ("description_md", "comments", "attachments")


def _last_comments(card: Mapping[str, Any], last: int) -> dict[str, Any]:
    """Последние `last` комментариев карточки и `comments_omitted` — сколько не вошло.

    Новый трекер режет сам, и тогда срез здесь ничего не меняет; старый параметр не знает и
    отдаёт всю переписку — режем на месте, чтобы обещание инструмента держалось с любым.
    """
    result = dict(card)
    comments = card.get("comments")
    omitted = card.get("comments_omitted")
    omitted = omitted if isinstance(omitted, int) and not isinstance(omitted, bool) else 0
    if isinstance(comments, list) and len(comments) > last:
        omitted += len(comments) - last
        # Срез `[-0:]` вернул бы весь список, поэтому ноль — отдельный случай.
        result["comments"] = comments[-last:] if last else []
    result["comments_omitted"] = omitted
    return result


def _card_brief(card: Any) -> Any:
    """Ответ мутации задачи: где она теперь. Карточку целиком агент перечитает, если нужно."""
    if not isinstance(card, Mapping):
        return card
    return {name: card.get(name) for name in ("id", "key", "status", "version")}


def _item_brief(key: str, item: Any, *names: str) -> Any:
    """Ответ мутации пункта чеклиста: item_id для следующих вызовов и то, что поменялось."""
    if not isinstance(item, Mapping):
        return item
    return {"issue_key": key, "item_id": item.get("id"), **{name: item.get(name) for name in names}}


def _item_ids_of(args: Mapping[str, Any], tool: str) -> list[str]:
    """item_ids (пакет) или прежний одиночный item_id; пакет проверяется до первого вызова API."""
    if "item_ids" in args:
        ids = args["item_ids"]
        if not isinstance(ids, list) or not ids or not all(isinstance(i, str) and i for i in ids):
            raise ToolError(f"{tool}: item_ids должен быть непустым списком id пунктов")
        return list(ids)
    return [str(_require(args, "item_id", tool))]


def _batch_items(key: str, items: list[Any], verbose: bool, *names: str, batch: bool) -> Any:
    """Ответ одиночного вызова прежний; пакет отвечает одним списком."""
    if not batch:
        return items[0] if verbose else _item_brief(key, items[0], *names)
    if verbose:
        return {"issue_key": key, "items": items}
    return {
        "issue_key": key,
        "items": [
            {k: v for k, v in _item_brief(key, item, *names).items() if k != "issue_key"}
            for item in items
        ],
    }


def _op_issue_create(client: Any, args: Mapping[str, Any]) -> Any:
    fields = dict(args)
    fields.pop("issue_key", None)
    # Ключ — не поле задачи: уходит заголовком, а пустой не уходит вовсе.
    fields.pop("idempotency_key", None)
    verbose = _verbose_of(fields, "issue_create")
    fields.pop("verbose", None)
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
        "project",
    }
    if "project" in fields:
        fields["project"] = _as_text(fields["project"], "issue_create: project")
    unknown = sorted(set(fields) - known)
    if unknown:
        raise ToolError("issue_create: неизвестные аргументы: " + ", ".join(unknown))
    card = client.create_issue(**fields, **_idempotency_of(args))
    return card if verbose else _card_brief(card)


def _op_issue_update(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_update"))
    patch = args.get("patch")
    if not isinstance(patch, Mapping) or not patch:
        raise ToolError("issue_update: patch должен быть непустым объектом")
    version = args.get("version")
    verbose = _verbose_of(args, "issue_update")
    card = client.update_issue(key, patch, version=version)
    return card if verbose else _card_brief(card)


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


def _verbose_of(args: Mapping[str, Any], tool: str) -> bool:
    verbose = args.get("verbose")
    if verbose is not None and not isinstance(verbose, bool):
        raise ToolError(f"{tool}: verbose должен быть true или false")
    return bool(verbose)


def _issue_brief(issue: Any) -> Any:
    """Связанная задача в ответе мутации: без описания, иначе одна связь весит килобайты."""
    if not isinstance(issue, Mapping):
        return issue
    return {name: issue.get(name) for name in ("key", "title", "status")}


def _op_issue_transition(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_transition"))
    to = str(_require(args, "to", "issue_transition"))
    note = args.get("note")
    verbose = _verbose_of(args, "issue_transition")
    card = client.transition(key, to, note=str(note) if note is not None else None)
    # Сервер отвечает полной карточкой со связанными задачами целиком — до десятков КБ на переход.
    # Агенту после перехода нужно только, где задача теперь и куда из неё можно.
    if verbose or not isinstance(card, Mapping):
        return card
    # Отказ приходит ToolError-ом вместе с объяснением, поэтому в успешном ответе доступные
    # переходы с длинными reason — только по verbose.
    return {name: card.get(name) for name in ("key", "status", "version")}


def _op_issue_link(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_link"))
    to = str(_require(args, "to", "issue_link"))
    link_type = str(_require(args, "type", "issue_link"))
    verbose = _verbose_of(args, "issue_link")
    payload = client.link(key, to, link_type, **_idempotency_of(args))
    if verbose or not isinstance(payload, Mapping):
        return payload
    links = payload.get("links")
    return {
        "key": payload.get("key", key),
        "link": {"to": to, "type": link_type},
        "links": [
            {**item, "issue": _issue_brief(item.get("issue"))} if isinstance(item, Mapping) else item
            for item in (links if isinstance(links, list) else [])
        ],
    }


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
    verbose = _verbose_of(args, "issue_checklist_add")
    item = client.checklist_add(
        key, text, sort_order=_as_float(order, "sort_order") if order is not None else None
    )
    return item if verbose else _item_brief(key, item)


def _op_issue_checklist_tick(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_checklist_tick"))
    item_ids = _item_ids_of(args, "issue_checklist_tick")
    done = args.get("done")
    if done is None:
        done = True
    if not isinstance(done, bool):
        raise ToolError("issue_checklist_tick: done должен быть true или false")
    verbose = _verbose_of(args, "issue_checklist_tick")
    items = [client.checklist_tick(key, item_id, done=done) for item_id in item_ids]
    return _batch_items(key, items, verbose, "done", batch="item_ids" in args)


def _op_issue_checklist_edit(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_checklist_edit"))
    item_id = str(_require(args, "item_id", "issue_checklist_edit"))
    text = args.get("text")
    order = args.get("sort_order")
    if text is None and order is None:
        raise ToolError("issue_checklist_edit: нужен text или sort_order")
    new_text = _as_text(text, "issue_checklist_edit: text") if text is not None else None
    verbose = _verbose_of(args, "issue_checklist_edit")
    item = client.checklist_edit(
        key,
        item_id,
        text=new_text,
        sort_order=_as_float(order, "sort_order") if order is not None else None,
    )
    return item if verbose else _item_brief(key, item, "text", "sort_order")


def _op_issue_checklist_waive(client: Any, args: Mapping[str, Any]) -> Any:
    key = str(_require(args, "issue_key", "issue_checklist_waive"))
    item_ids = _item_ids_of(args, "issue_checklist_waive")
    undo = args.get("undo")
    if undo is not None and not isinstance(undo, bool):
        raise ToolError("issue_checklist_waive: undo должен быть true или false")
    verbose = _verbose_of(args, "issue_checklist_waive")
    if undo:
        items = [client.checklist_waive(key, item_id, undo=True) for item_id in item_ids]
    else:
        # Причина обязательна: снятый без объяснения критерий неотличим от забытого. Проверяем до
        # похода в API, чтобы агент получил понятный отказ, а не 422.
        reason = _as_text(
            _require(args, "reason", "issue_checklist_waive"), "issue_checklist_waive: reason"
        )
        items = [client.checklist_waive(key, item_id, reason=reason) for item_id in item_ids]
    return _batch_items(key, items, verbose, "waived", batch="item_ids" in args)


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


def _op_inbox_resolve(client: Any, args: Mapping[str, Any]) -> Any:
    item_id = str(_require(args, "item_id", "inbox_resolve")).strip()
    if not item_id:
        raise ToolError("inbox_resolve: item_id не может быть пустым")
    note = _as_text(_require(args, "note", "inbox_resolve"), "inbox_resolve: note")
    status = str(args.get("status") or "resolved")
    if status not in ("resolved", "dismissed"):
        raise ToolError("inbox_resolve: status — resolved или dismissed")
    return client.resolve_inbox(item_id, status=status, note=note)


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
    doc = client.doc(slug)
    # Сервер кладёт текст файла и в body_md, и в source.body_md: агент платил бы за страницу
    # дважды. Оставляем верхний body_md — его же правит doc_update.
    source = doc.get("source") if isinstance(doc, Mapping) else None
    if isinstance(source, Mapping) and source.get("body_md") == doc.get("body_md"):
        doc = {**doc, "source": {k: v for k, v in source.items() if k != "body_md"}}
    return doc


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
    status = _as_str_list(args.get("status"), "status")
    unknown = [value for value in status if value not in _MILESTONE_STATUSES]
    if unknown:
        raise ToolError(
            "milestones_list: неизвестный статус вехи: "
            + ", ".join(unknown)
            + ". Есть: "
            + ", ".join(_MILESTONE_STATUSES)
        )
    fields = _as_str_list(args.get("fields"), "fields")
    compact = args.get("compact")
    if compact is not None and not isinstance(compact, bool):
        raise ToolError("milestones_list: compact должен быть true или false")
    narrowing: dict[str, Any] = {}
    if status:
        narrowing["status"] = status
    if compact:
        narrowing["compact"] = True
    if fields:
        narrowing["fields"] = fields
    payload = client.milestones(**narrowing)
    return _narrow_list(
        payload,
        "milestones_list",
        _MILESTONE_COMPACT_FIELDS,
        compact=bool(compact),
        fields=fields,
        # fields без status: сервер фильтр уже применил, а у проекции статуса нет (AJ-76).
        keep=(lambda item: "status" not in item or item["status"] in status) if status else None,
    )


#: Поля compact-списка вех — те же, что `COMPACT_FIELDS` в `ajira/service/milestones.py`.
_MILESTONE_COMPACT_FIELDS = (
    "id",
    "name",
    "status",
    "target_date",
    "total",
    "done",
    "closed",
    "progress",
    "version",
)


def _narrow_list(
    payload: Any,
    tool: str,
    compact_fields: Sequence[str],
    *,
    compact: bool,
    fields: Sequence[str],
    keep: Callable[[Mapping[str, Any]], bool] | None = None,
) -> Any:
    """Фильтр и сужение списка `{"items": [...]}` на стороне моста.

    Новый трекер делает это сам, и тогда здесь ничего не меняется: уже суженные элементы
    проецируются в те же поля. Старый параметров не знает и отдаёт полные элементы с описаниями —
    без этого шага агент молча получил бы прежние тысячи токенов. `keep` — фильтр, который
    старый трекер тоже мог проигнорировать.
    """
    if not isinstance(payload, Mapping) or not isinstance(payload.get("items"), list):
        return payload
    items = [item for item in payload["items"] if isinstance(item, Mapping)]
    if keep is not None:
        items = [item for item in items if keep(item)]
    if fields:
        unknown = sorted({name for item in items for name in fields if name not in item})
        if unknown:
            raise ToolError(
                f"{tool}: неизвестные поля в fields: "
                + ", ".join(unknown)
                + ". Есть: "
                + ", ".join(sorted({name for item in items for name in item}))
            )
        items = [{name: item[name] for name in fields} for item in items]
    elif compact:
        items = [{name: item[name] for name in compact_fields if name in item} for item in items]
    return {**payload, "items": items, "count": len(items)}


def _op_milestone_create(client: Any, args: Mapping[str, Any]) -> Any:
    name = _as_text(_require(args, "name", "milestone_create"), "milestone_create: имя")
    known = {"name", "description_md", "target_date", "status", "sort_order"}
    unknown = sorted(set(args) - known)
    if unknown:
        raise ToolError("milestone_create: неизвестные аргументы: " + ", ".join(unknown))
    order = args.get("sort_order")
    target = args.get("target_date")
    status = args.get("status")
    return client.create_milestone(
        name,
        description_md=str(args.get("description_md") or ""),
        target_date=str(target) if target else None,
        status=str(status) if status else None,
        sort_order=_as_float(order, "sort_order") if order is not None else None,
    )


def _op_milestone_update(client: Any, args: Mapping[str, Any]) -> Any:
    milestone = str(_require(args, "milestone", "milestone_update")).strip()
    if not milestone:
        raise ToolError("milestone_update: имя или id вехи не может быть пустым")
    patch = args.get("patch")
    if not isinstance(patch, Mapping) or not patch:
        raise ToolError("milestone_update: patch должен быть непустым объектом")
    return client.update_milestone(milestone, patch, version=args.get("version"))


def _op_me(client: Any, args: Mapping[str, Any]) -> Any:
    return client.me()


# --- самообновление моста ----------------------------------------------------

#: Папка моста, как её раздаёт сервер (`GET /api/v1/mcp/bridge`) и собирает релиз
#: (`dist/ajira-mcp`). Первый файл — сам мост: у копии в проекте он может называться иначе.
BRIDGE_FILES: tuple[str, ...] = (
    "ajira_mcp.py",
    "README.md",
    "mcp.json.example",
    "skills/agentjira/SKILL.md",
)
BRIDGE_ENTRY = BRIDGE_FILES[0]
BRIDGE_SKILL = "skills/agentjira/SKILL.md"

#: Прежние версии заменённых файлов: одна копия — то, что было до последнего обновления.
BRIDGE_BACKUP_DIR = ".ajira-backup"

#: Метка версии рядом с мостом: её кладёт релиз (`dist/ajira-mcp/RELEASE.txt`) и `bridge_update`.
BRIDGE_RELEASE_FILE = "RELEASE.txt"
_RELEASE_REVISION = "ревизия git:"

#: Где Claude Code ищет скилл проекта. Туда его копирует человек, а мост только сверяет копию.
PROJECT_SKILL_PARTS = (".claude", "skills", "agentjira", "SKILL.md")

NEW_SESSION_NOTE = (
    "Новые инструменты и инструкция появятся только в новой сессии агента: список инструментов "
    "клиент запоминает при подключении, перезапуск процесса моста его не обновляет. Скажи "
    "человеку, что сессию нужно начать заново."
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _file_sha256(path: Path) -> str | None:
    try:
        return _sha256(path.read_bytes())
    except OSError:
        return None


def bridge_path_of(client: Any) -> Path | None:
    """Файл работающего моста. У пакетного клиента его нет: там мост — сам пакет `ajira`."""
    value = getattr(client, "bridge_path", None)
    return Path(value) if value else None


def bridge_revision(bridge_path: Path | None) -> str | None:
    """Ревизия трекера, из которой взята копия моста: строка `ревизия git:` из RELEASE.txt рядом.

    Сам мост ревизии не знает: зашитая в файл, она устаревала бы с каждым коммитом. Метку кладёт
    рядом релиз и `bridge_update`; скопированного без неё моста хватает и отпечатка.
    """
    if bridge_path is None:
        return None
    try:
        text = (bridge_path.parent / BRIDGE_RELEASE_FILE).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None
    for line in text.splitlines():
        if line.startswith(_RELEASE_REVISION):
            words = line[len(_RELEASE_REVISION):].split()
            return words[0] if words else None
    return None


def load_bridge(source: str, origin: Path, name: str = "ajira_mcp_candidate") -> Any:
    """Исполняет текст моста как модуль, не трогая `sys.modules` и не оставляя `__pycache__`."""
    module = types.ModuleType(name)
    module.__file__ = str(origin)
    exec(compile(source, str(origin), "exec"), module.__dict__)  # noqa: S102 — свой файл с трекера
    return module


def _candidate_digest(source: str, origin: Path) -> str:
    """Отпечаток скачанного моста. Файл, который не загружается, сюда не дойдёт до замены."""
    try:
        module = load_bridge(source, origin)
    except SyntaxError as exc:
        raise ToolError(f"bridge_update: скачанный мост не разбирается как Python: {exc}") from exc
    except Exception as exc:  # noqa: BLE001 — любой сбой загрузки отменяет обновление
        raise ToolError(f"bridge_update: скачанный мост не загрузился: {exc!r}") from exc
    digest_of = getattr(module, "catalog_digest", None)
    if not callable(digest_of) or not callable(getattr(module, "main", None)):
        raise ToolError("bridge_update: в скачанном файле нет моста (catalog_digest и main)")
    return str(digest_of())


def _is_bridge_folder(directory: Path) -> bool:
    """Папка отдана мосту целиком — тогда README, пример конфига и скилл рядом с ним его.

    Мост могли положить и прямо в корень проекта, где README.md — README проекта: затирать его
    нельзя. Своей папку считаем по меткам, которые кладут только релиз моста и `bridge_update`.
    """
    try:
        example = directory / "mcp.json.example"
        if example.is_file() and "ajira_mcp" in example.read_text(encoding="utf-8"):
            return True
        release = directory / BRIDGE_RELEASE_FILE
        return release.is_file() and release.read_text(encoding="utf-8").startswith("AgentJira")
    except (OSError, UnicodeDecodeError):
        return False


def _safe_relative(path: Any) -> str | None:
    """Путь из манифеста, если он остаётся внутри папки моста: без `..`, диска и корня."""
    if not isinstance(path, str) or not path or ":" in path or "\\" in path:
        return None
    pure = PurePosixPath(path)
    if pure.is_absolute() or ".." in pure.parts:
        return None
    return pure.as_posix()


def _manifest_files(manifest: Mapping[str, Any]) -> list[dict[str, Any]]:
    files = manifest.get("files")
    if not isinstance(files, Sequence) or isinstance(files, str):
        raise ToolError("bridge_update: сервер не прислал список файлов моста")
    result: list[dict[str, Any]] = []
    for item in files:
        relative = _safe_relative(item.get("path")) if isinstance(item, Mapping) else None
        sha = item.get("sha256") if isinstance(item, Mapping) else None
        if relative is None or not isinstance(sha, str):
            raise ToolError(f"bridge_update: в манифесте сервера негодная запись файла: {item!r}")
        result.append({"path": relative, "sha256": sha, "content": item.get("content")})
    return result


def _bridge_plan(
    bridge_path: Path, files: Sequence[Mapping[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, str]], bool]:
    """Что заменить или создать, что пропустить и отдана ли мосту вся папка."""
    directory = bridge_path.parent
    whole_folder = _is_bridge_folder(directory)
    changes: list[dict[str, Any]] = []
    skipped: list[dict[str, str]] = []
    for item in files:
        is_entry = item["path"] == BRIDGE_ENTRY
        target = bridge_path if is_entry else directory / item["path"]
        if not is_entry and not whole_folder:
            skipped.append(
                {
                    "path": item["path"],
                    "reason": "мост лежит не в своей папке (нет mcp.json.example и RELEASE.txt "
                    "моста рядом): соседние файлы могут быть файлами проекта, их не трогаю",
                }
            )
            continue
        current = _file_sha256(target) if target.is_file() else None
        if current == item["sha256"]:
            continue
        changes.append({**item, "target": target, "exists": target.exists()})
    return changes, skipped, whole_folder


def _verify_download(changes: Sequence[dict[str, Any]], server_digest: Any) -> None:
    """Всё скачанное сходится с манифестом, а новый мост — с каталогом сервера. Иначе отказ."""
    for change in changes:
        content = change.get("content")
        if not isinstance(content, str):
            raise ToolError(f"bridge_update: сервер не прислал содержимое {change['path']}")
        data = content.encode("utf-8")
        if _sha256(data) != change["sha256"]:
            raise ToolError(
                f"bridge_update: sha256 файла {change['path']} не совпал с манифестом — "
                "файл испорчен по дороге. Файлы моста не тронуты."
            )
        change["data"] = data
        if change["path"] == BRIDGE_ENTRY:
            digest = _candidate_digest(content, change["target"])
            if digest != server_digest:
                raise ToolError(
                    f"bridge_update: сервер раздаёт мост с отпечатком {digest}, а сам работает с "
                    f"{server_digest}: его копия standalone/ajira_mcp.py устарела. Скажи человеку — "
                    "релиз трекера нужно пересобрать (scripts/build_release.py). Файлы моста не "
                    "тронуты."
                )


def _install(changes: Sequence[dict[str, Any]], directory: Path) -> Path:
    """Заменяет файлы через временный файл и `os.replace`: файл моста не бывает недописанным.

    Прежние версии ложатся в `.ajira-backup`. Сбой посередине возвращает уже заменённые файлы:
    половина нового моста рядом с половиной старого хуже любого из них.
    """
    backup_root = directory / BRIDGE_BACKUP_DIR
    if backup_root.is_dir():
        shutil.rmtree(backup_root)
    done: list[tuple[Path, Path | None]] = []
    current: Path = directory
    try:
        for change in changes:
            current = change["target"]
            current.parent.mkdir(parents=True, exist_ok=True)
            backup: Path | None = None
            if change["exists"]:
                backup = backup_root / current.relative_to(directory)
                backup.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(current, backup)
            fresh = current.with_name(current.name + ".new")
            fresh.write_bytes(change["data"])
            os.replace(fresh, current)
            done.append((current, backup))
    except OSError as exc:
        for target, backup in reversed(done):
            try:
                if backup is not None:
                    shutil.copy2(backup, target)
                else:
                    target.unlink()
            except OSError:
                pass
        leftover = current.with_name(current.name + ".new")
        if leftover.exists():
            try:
                leftover.unlink()
            except OSError:
                pass
        raise ToolError(
            f"bridge_update: не удалось записать {current}: {exc}. Уже заменённые файлы "
            "возвращены, мост остался прежним."
        ) from exc
    return backup_root


def _write_release(directory: Path, manifest: Mapping[str, Any], base_url: Any) -> None:
    """Метка версии рядом с мостом: из неё предупреждение называет ревизию моста."""
    build = manifest.get("build") if isinstance(manifest.get("build"), Mapping) else {}
    revision = build.get("revision") or "неизвестна"
    now = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    lines = [
        "AgentJira — MCP-мост, обновлён инструментом bridge_update",
        f"{_RELEASE_REVISION} {revision}" + ("  (есть несохранённые правки)" if build.get("dirty") else ""),
        f"собрано: {build.get('built_at') or 'неизвестно'}",
        f"отпечаток каталога MCP: {manifest.get('catalog_digest')}",
        f"обновлено: {now} с {base_url or 'трекера'}",
        "",
    ]
    # Байтами, а не write_text(newline=...): мосту хватает Python 3.9, где такого аргумента нет.
    (directory / BRIDGE_RELEASE_FILE).write_bytes("\n".join(lines).encode("utf-8"))


def _skill_copies(bridge_path: Path, skill_sha: str | None) -> list[dict[str, Any]]:
    """Копии скилла в `.claude/skills/agentjira/`: в текущем каталоге и над папкой моста.

    Мост их не переписывает: `.claude/` — настройки проекта, их правит человек или агент на
    виду у человека, а Claude Code всё равно перечитает скилл только в новой сессии.
    """
    roots = [Path.cwd(), bridge_path.parent, *bridge_path.parent.parents]
    seen: set[str] = set()
    found: list[dict[str, Any]] = []
    for root in roots:
        candidate = root.joinpath(*PROJECT_SKILL_PARTS)
        key = str(candidate.resolve()) if candidate.exists() else ""
        if not key or key in seen or not candidate.is_file():
            continue
        seen.add(key)
        found.append({"path": str(candidate), "current": _file_sha256(candidate) == skill_sha})
    return found


def update_bridge(client: Any, bridge_path: Path | None, *, check_only: bool = False) -> dict[str, Any]:
    """Сверяет папку моста с той, что раздаёт сервер, и, если надо, заменяет файлы.

    Возвращает отчёт для агента: версии моста и сервера, что заменено, где резервная копия и что
    делать дальше. Отказ (битый файл, мост не той версии) — `ToolError`, файлы при этом не тронуты.
    """
    standalone = bridge_path is not None
    manifest = client.bridge_manifest(content=standalone and not check_only)
    if not isinstance(manifest, Mapping):
        raise ToolError("bridge_update: сервер не прислал манифест моста")
    server_digest = manifest.get("catalog_digest")
    local_digest = catalog_digest()
    build = manifest.get("build") if isinstance(manifest.get("build"), Mapping) else {}
    report: dict[str, Any] = {
        "bridge": str(bridge_path) if bridge_path is not None else None,
        "bridge_version": {"catalog_digest": local_digest, "revision": bridge_revision(bridge_path)},
        "server_version": {
            "catalog_digest": server_digest,
            "revision": build.get("revision"),
            "built_at": build.get("built_at"),
        },
    }
    if bridge_path is None:
        report["status"] = "packaged"
        report["message"] = (
            "MCP запущен из пакета ajira (ajira mcp), а не из автономного ajira_mcp.py: он "
            "обновляется вместе с трекером, заменять нечего."
        )
        return report

    files = _manifest_files(manifest)
    changes, skipped, whole_folder = _bridge_plan(bridge_path, files)
    skill_sha = next((item["sha256"] for item in files if item["path"] == BRIDGE_SKILL), None)
    if skipped:
        report["skipped"] = skipped
    entry_changes = any(change["path"] == BRIDGE_ENTRY for change in changes)

    if check_only or not changes:
        report["status"] = "outdated" if changes else "up_to_date"
        if changes:
            report["changed_files"] = [change["path"] for change in changes]
            report["next_step"] = "вызови bridge_update без check_only — он заменит эти файлы"
        elif server_digest and local_digest != server_digest:
            # Файл на диске уже как у сервера, а процесс работает на старом коде: обновление
            # прошло в этой же сессии. Если и файл не той версии — сервер раздаёт чужой мост.
            on_disk = _candidate_digest(bridge_path.read_text(encoding="utf-8"), bridge_path)
            if on_disk == server_digest:
                report["next_step"] = NEW_SESSION_NOTE
            else:
                report["warning"] = (
                    f"сервер раздаёт мост с отпечатком {on_disk}, а сам работает с "
                    f"{server_digest}: скажи человеку, что релиз трекера нужно пересобрать"
                )
        _report_skills(report, bridge_path, skill_sha, whole_folder)
        return report

    _verify_download(changes, server_digest)
    backup = _install(changes, bridge_path.parent)
    if whole_folder:
        _write_release(bridge_path.parent, manifest, getattr(client, "base_url", None))
    report["status"] = "updated"
    report["updated"] = [change["path"] for change in changes if change["exists"]]
    report["created"] = [change["path"] for change in changes if not change["exists"]]
    report["entry_replaced"] = entry_changes
    report["backup"] = str(backup)
    report["next_step"] = NEW_SESSION_NOTE
    _report_skills(report, bridge_path, skill_sha, whole_folder)
    return report


def _report_skills(
    report: dict[str, Any], bridge_path: Path, skill_sha: str | None, whole_folder: bool
) -> None:
    skills = _skill_copies(bridge_path, skill_sha)
    report["skills"] = skills
    stale = [copy["path"] for copy in skills if not copy["current"]]
    if not stale:
        return
    if whole_folder:
        source = f"файл {bridge_path.parent / BRIDGE_SKILL} (после обновления моста)"
    else:
        source = f"текст из GET /api/v1/mcp/bridge/files/{BRIDGE_SKILL}"
    report["skills_hint"] = (
        f"копия скилла отличается от раздаваемой сервером: {', '.join(stale)}. Скопируй туда "
        f"{source}, если человек не против: мост сам настройки проекта (.claude/) не правит"
    )


def _op_bridge_update(client: Any, args: Mapping[str, Any]) -> Any:
    check_only = args.get("check_only", False)
    if not isinstance(check_only, bool):
        raise ToolError("bridge_update: check_only должен быть true или false")
    return update_bridge(client, bridge_path_of(client), check_only=check_only)


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
    "inbox_resolve": _op_inbox_resolve,
    "search": _op_search,
    "metrics": _op_metrics,
    "docs_list": _op_docs_list,
    "doc_get": _op_doc_get,
    "doc_create": _op_doc_create,
    "doc_update": _op_doc_update,
    "milestones_list": _op_milestones_list,
    "milestone_create": _op_milestone_create,
    "milestone_update": _op_milestone_update,
    "me": _op_me,
    "bridge_update": _op_bridge_update,
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
    text = payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False, separators=(",", ":"), default=str)
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
    "(@имя) — inbox_list с addressee=me. Ответ пришёл другим каналом (чат, комментарий) или "
    "вопрос отпал — сними свой запрос inbox_resolve, в note — где получен ответ. Блокер — ещё и "
    "issue_transition в blocked с причиной в note; вернуть в работу из blocked или failed без "
    "лиза — снова issue_claim. Задача отпала — issue_transition в cancelled с причиной; уже "
    "сделана в другом месте или эпик с закрытыми подзадачами — сразу в done (с note, где "
    "сделано), без claim и review. Любая проблема или затруднение при работе с "
    "самим трекером (ошибка, непонятный отказ, неудобный или недостающий инструмент, неясная "
    "инструкция), нужный фикс или улучшение — задача через issue_create в проекте трекера: не "
    "обходи молча. Для вопросов человеку задачу не заводи. Если ниже сказано, что мост "
    "разошёлся с сервером, обнови его инструментом bridge_update и скажи человеку начать новую "
    "сессию."
)

#: Сколько страниц плана называть в инструкции: агенту нужен вход, а не оглавление.
INSTRUCTION_DOC_LIMIT = 5


def catalog_digest() -> str:
    """Отпечаток того, что мост обещает агенту: каталог инструментов и порядок работы.

    Копию автономного моста кладут в чужой проект и забывают, а сервер тем временем уходит
    вперёд: агент проекта DF жаловался на отсутствие `project` у `issue_create`, который уже был
    в трекере, но не в его копии (AJ-44). Сервер отдаёт свой отпечаток в `/meta`, мост сверяет
    его со своим и предупреждает агента. Отпечаток считается из данных, а не из ревизии git:
    иначе собранный `standalone/ajira_mcp.py` устаревал бы с каждым коммитом.
    """
    payload = json.dumps(
        {"tools": TOOLS, "instructions": BASE_INSTRUCTIONS}, ensure_ascii=False, sort_keys=True
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()[:12]


def _version_label(digest: Any, revision: Any = None, built_at: Any = None) -> str:
    """Версия для человека: отпечаток каталога и, если известны, ревизия git и дата сборки."""
    extra = []
    if revision:
        extra.append(f"ревизия {revision}")
    if built_at:
        extra.append(f"собран {built_at}")
    return f"{digest} ({', '.join(extra)})" if extra else str(digest)


def _version_warning(meta: Mapping[str, Any], bridge_path: Path | None = None) -> str | None:
    """Предупреждение о том, что мост и сервер собраны из разных версий трекера.

    Старый сервер отпечатка не отдаёт — тогда молчим: сравнивать не с чем. Совет зависит от
    того, кто запущен: автономный мост обновляется сам (`bridge_update`), если сервер его
    раздаёт; пакетный — только вместе с пакетом, это делает человек.
    """
    mcp = meta.get("mcp") if isinstance(meta.get("mcp"), Mapping) else {}
    server = mcp.get("catalog_digest")
    local = catalog_digest()
    if not isinstance(server, str) or not server or server == local:
        return None
    mine = _version_label(local, bridge_revision(bridge_path))
    theirs = _version_label(server, mcp.get("revision"), mcp.get("built_at"))
    text = (
        f"ВНИМАНИЕ: этот MCP-мост собран из другой версии трекера, чем запущенный сервер: "
        f"версия моста {mine}, версия сервера {theirs}. Инструментов, аргументов или правил "
        f"может не хватать, или они ведут себя иначе, чем описано у сервера. "
    )
    if bridge_path is not None and mcp.get("bridge"):
        return text + (
            "Обнови мост сам: вызови инструмент bridge_update — он скачает с трекера актуальную "
            "папку моста и заменит файлы. Новые инструменты появятся только в новой сессии "
            "агента: после обновления скажи человеку, что её нужно начать."
        )
    if bridge_path is not None:
        return text + (
            "Сервер мост не раздаёт, обновить его сам ты не сможешь. Скажи человеку: копию моста "
            "в проекте обновляют из папки ajira-mcp релиза трекера."
        )
    return text + (
        "Скажи человеку: MCP запущен из пакета ajira другой сборки, чем трекер, — пакет обновляют "
        "вместе с трекером, а копию моста в проекте — из папки ajira-mcp релиза."
    )


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
    # Первой строкой: всё, что ниже, сервер говорит о своей версии, а не о версии моста.
    warning = _version_warning(meta, bridge_path_of(client))
    if warning:
        lines.append(warning)
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
    # Агент чужого проекта иначе завёл бы задачу про трекер у себя: проект запроса задан
    # подключением, а не инструментом (AJ-38).
    if tracker_key and tracker_key != key:
        where += f" (аргумент project={tracker_key})"
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
                # Отпечаток в версии виден в клиенте рядом с именем сервера: по нему
                # человек сверяет копию моста с трекером, не открывая инструкцию.
                "serverInfo": {"name": SERVER_NAME, "version": f"{SERVER_VERSION}+{catalog_digest()}"},
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


def force_utf8_stdio() -> None:
    """Переводит stdin, stdout и stderr процесса в UTF-8.

    MCP по stdio объявлен в UTF-8, а Windows по умолчанию декодирует потоки кодовой страницей
    системы: русский текст запроса и ответа ломается, а сообщения в stderr (таймауты,
    автообновление, внутренние ошибки) в консоли и логах MCP-клиента выходят кракозябрами.
    stderr — только диагностика, поэтому непечатаемое заменяется, а не роняет процесс.
    """
    for stream, errors in ((sys.stdin, "strict"), (sys.stdout, "strict"), (sys.stderr, "replace")):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue  # подменённый поток (StringIO, обёртка клиента) — кодировку выбирал не мост
        try:
            reconfigure(encoding="utf-8", errors=errors)
        except (ValueError, OSError):
            # Поток, из которого уже читали, перекодировать нельзя; работать дальше лучше,
            # чем упасть на старте.
            pass


#: Кусок, которым пропускается хвост слишком длинной строки: память не растёт с длиной мусора.
_SKIP_CHUNK = 64 * 1024


def _input_lines(source: Any, limit: int) -> Any:
    """Строки запросов, по возможности — сырыми байтами; вместо слишком длинной — `None`.

    Текстовый stdin декодирует поток сам и на первом же не-UTF-8 байте бросает
    `UnicodeDecodeError` прямо из итерации: цикл обрывается, мост умирает, и агент теряет трекер
    до конца сессии. К тому же после такой ошибки обёртка теряет уже прочитанный хвост блока, так
    что продолжить чтение с неё нельзя. Поэтому у настоящего потока берётся `buffer`, а каждая
    строка декодируется отдельно. Поток без `buffer` (StringIO, обёртка клиента) читается как есть.

    Строка читается с пределом: итерация по потоку копит строку без перевода строки целиком, и
    мусор в stdin съел бы память процесса. Хвост длинной строки пропускается кусками до перевода
    строки, поэтому следующая строка читается с начала. Предел — в байтах UTF-8 без перевода
    строки; у текстового потока `readline` считает символы, и байты досчитываются после чтения.
    """
    buffer = getattr(source, "buffer", None)
    stream = buffer if buffer is not None else source
    while True:
        line = stream.readline(limit + 1)
        if not line:
            return
        newline = b"\n" if isinstance(line, (bytes, bytearray)) else "\n"
        complete = line.endswith(newline)
        if not complete and len(line) > limit:
            _skip_rest(stream, newline)
            yield None
            continue
        if isinstance(line, str) and _text_size(line) - int(complete) > limit:
            yield None
            continue
        yield line


def _skip_rest(stream: Any, newline: Any) -> None:
    """Дочитывает и выбрасывает хвост строки до перевода строки или конца потока."""
    while True:
        chunk = stream.readline(_SKIP_CHUNK)
        if not chunk or chunk.endswith(newline):
            return


def _text_size(line: str) -> int:
    """Размер текстовой строки в байтах UTF-8; одиночный суррогат считается как в `surrogatepass`."""
    return len(line.encode("utf-8", "surrogatepass"))


def _too_long_message(limit: int) -> str:
    """Текст ошибки разбора для строки длиннее предела — без её содержимого."""
    return f"строка запроса длиннее {limit} байт: запрос отброшен"


def _decode_line(line: Any) -> str:
    """Строка запроса как текст. Не-UTF-8 — `UnicodeError`, его разбирает `serve`.

    Текстовый источник проверяется тоже: в нём битые байты могут прийти одиночными суррогатами
    (`surrogateescape`), и такой запрос нельзя ни передать трекеру, ни повторить в ответе.
    """
    if isinstance(line, (bytes, bytearray)):
        return bytes(line).decode("utf-8")
    line.encode("utf-8")
    return line


def _bad_encoding_message(exc: UnicodeError) -> str:
    """Текст ошибки разбора для строки не в UTF-8: где сломалось, без самих байтов."""
    if isinstance(exc, UnicodeDecodeError):
        bad = exc.object[exc.start : exc.start + 1]
        return f"строка запроса не в UTF-8: байт 0x{bad.hex()} в позиции {exc.start}"
    return "строка запроса не в UTF-8"


def _dump_response(response: Mapping[str, Any]) -> str:
    """Ответ одной строкой JSON, которую гарантированно примет stdout в UTF-8.

    Корректный JSON может нести одиночный суррогат экранированием `\\ud800` — в id запроса или в
    тексте, который ответ повторяет. Записать его в строгий UTF-8 нельзя, и мост упал бы уже на
    ответе. Тогда весь ответ уходит в ASCII-экранировании: тот же JSON, читаемый любым клиентом.
    """
    text = json.dumps(response, ensure_ascii=False, default=str)
    try:
        text.encode("utf-8")
    except UnicodeEncodeError:
        text = json.dumps(response, ensure_ascii=True, default=str)
    return text


def serve(client: Any, stdin: Any = None, stdout: Any = None) -> int:
    """Цикл stdio: строка запроса на входе — строка ответа на выходе.

    Ни одна строка входа не роняет цикл: битая кодировка, битый JSON и строка длиннее
    `MAX_LINE_BYTES` — ответ `-32700`, исключение обработчика — `-32603`, и мост обслуживает
    следующую строку.
    """
    source = stdin if stdin is not None else sys.stdin
    sink = stdout if stdout is not None else sys.stdout
    # Предел берётся при вызове, а не при импорте: тест подменяет его, не гоняя мегабайты.
    limit = MAX_LINE_BYTES
    for raw in _input_lines(source, limit):
        response: dict[str, Any] | None
        if raw is None:
            response = jsonrpc_error(None, PARSE_ERROR, _too_long_message(limit))
        else:
            try:
                line = _decode_line(raw)
            except UnicodeError as exc:
                response = jsonrpc_error(None, PARSE_ERROR, _bad_encoding_message(exc))
            else:
                if not line.strip():
                    continue
                response = _handle_line(client, line)
        if response is None:
            continue
        sink.write(_dump_response(response) + "\n")
        sink.flush()
    return 0


def _handle_line(client: Any, line: str) -> dict[str, Any] | None:
    """Ответ на одну непустую строку входа: разбор JSON и вызов `handle_request`."""
    try:
        payload = json.loads(line)
    except json.JSONDecodeError as exc:
        return jsonrpc_error(None, PARSE_ERROR, f"не разобрал JSON: {exc.msg}")
    try:
        return handle_request(client, payload)
    except Exception as exc:  # noqa: BLE001 — процесс не должен падать от одного запроса
        print(f"ajira-mcp: внутренняя ошибка: {exc!r}", file=sys.stderr)
        request_id = payload.get("id") if isinstance(payload, Mapping) else None
        return jsonrpc_error(request_id, INTERNAL_ERROR, f"внутренняя ошибка: {exc}")

#: Адрес трекера по умолчанию. Переопределяется `AJIRA_URL` или аргументом `--url`.
DEFAULT_BASE_URL = "http://127.0.0.1:8787"
URL_ENV = "AJIRA_URL"
TOKEN_ENV = "AJIRA_TOKEN"
TOKEN_FILE_ENV = "AJIRA_TOKEN_FILE"
PROJECT_ENV = "AJIRA_PROJECT"
TIMEOUT_ENV = "AJIRA_TIMEOUT"
DEFAULT_TIMEOUT = 30.0

#: Включает обновление моста с трекера при старте процесса, до подключения клиента.
AUTO_UPDATE_ENV = "AJIRA_AUTO_UPDATE"
#: Ставится перед запуском только что обновлённого моста в том же процессе: проверять второй
#: раз нечего, а без метки сбой сервера мог бы зациклить перезапуск.
RELOADED_ENV = "AJIRA_BRIDGE_RELOADED"
#: Проверка при старте не должна держать клиента: недоступный трекер — работаем как есть.
AUTO_UPDATE_TIMEOUT = 5.0

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


def _query_value(value: Any) -> Any:
    """Значение параметра запроса так, как его пишет httpx пакетного клиента.

    `urlencode` превращает bool в `True`/`False`, а httpx — в `true`/`false`: без перевода одна и
    та же команда агента уходила бы из двух сборок разными запросами, и сервер, строже
    разбирающий bool, понял бы их по-разному.
    """
    if isinstance(value, bool):
        return "true" if value else "false"
    return value


def _encode_query(params: Mapping[str, Any]) -> str:
    """Строка запроса: пустые фильтры выброшены, списки — повторением параметра."""
    pairs: dict[str, Any] = {}
    for key, value in _clean(params).items():
        if isinstance(value, (list, tuple)):
            pairs[key] = [_query_value(item) for item in value]
        else:
            pairs[key] = _query_value(value)
    return urlparse.urlencode(pairs, doseq=True)


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
        bridge_path: str | Path | None = None,
    ) -> None:
        resolved = base_url or os.environ.get(URL_ENV) or DEFAULT_BASE_URL
        self.base_url = resolved.rstrip("/")
        self.token = token if token is not None else resolve_token(token_file)
        self.project = project if project is not None else (os.environ.get(PROJECT_ENV) or None)
        self.timeout = _resolve_timeout(timeout)
        # Файл работающего моста: его и соседей заменяет `bridge_update`. Без него инструмент
        # только сверяет версии.
        self.bridge_path = Path(bridge_path) if bridge_path is not None else None

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
            query = _encode_query(params)
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
        """Создать задачу. `idempotency_key` и `project` уходят заголовками, как у клиента пакета.

        `None` не уходит: при создании null не сбрасывает, а у type и priority сервер его не
        принимает (AJ-62) — сервер подставляет своё умолчание.
        """
        headers = _idempotency(fields.pop("idempotency_key", None)) or {}
        project = fields.pop("project", None)
        if project:
            headers["X-Ajira-Project"] = str(project)
        return self.request(
            "POST", f"{API_PREFIX}/issues", json_body=_clean(fields), extra_headers=headers
        )

    def get_issue(self, key: str, *, comments_last: int | None = None) -> Any:
        """Карточка задачи: поля, лиз, связи, вложения, комментарии, доступные переходы.
        `comments_last` — только последние N комментариев."""
        return self.request(
            "GET", f"{API_PREFIX}/issues/{_quote(key)}", params={"comments_last": comments_last}
        )

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

    def resolve_inbox(
        self,
        item_id: str,
        *,
        status: str = "resolved",
        note: str | None = None,
        answer: str | None = None,
    ) -> Any:
        """Закрыть запись Inbox. Агенту можно только свой запрос или своё упоминание."""
        body = {
            name: value
            for name, value in (("status", status), ("note", note), ("answer", answer))
            if value is not None
        }
        return self.request(
            "POST", f"{API_PREFIX}/inbox/{_quote(item_id)}/resolve", json_body=body
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

    def milestones(
        self,
        *,
        status: Sequence[str] | None = None,
        compact: bool = False,
        fields: Sequence[str] | None = None,
    ) -> Any:
        """Вехи роадмапа с прогрессом по задачам. `status` — фильтр, `compact=True` — без
        описаний, `fields` — точный список полей."""
        params = {
            "status": list(status) if status else None,
            "compact": "true" if compact else None,
            "fields": list(fields) if fields else None,
        }
        return self.request("GET", f"{API_PREFIX}/milestones", params=params)

    def create_milestone(
        self,
        name: str,
        *,
        description_md: str = "",
        target_date: str | None = None,
        status: str | None = None,
        sort_order: float | None = None,
    ) -> Any:
        """Создать веху роадмапа. Не заданные поля сервер заполняет своими умолчаниями."""
        body = _clean(
            {
                "name": name,
                "description_md": description_md,
                "target_date": target_date,
                "status": status,
                "sort_order": sort_order,
            }
        )
        return self.request("POST", f"{API_PREFIX}/milestones", json_body=body)

    def update_milestone(self, milestone: str, patch: Mapping[str, Any], version: Any = None) -> Any:
        """Правка вехи по id или имени. `version` уходит в `If-Match`."""
        headers = {"If-Match": str(version)} if version is not None else None
        return self.request(
            "PATCH",
            f"{API_PREFIX}/milestones/{_quote(milestone)}",
            json_body=dict(patch),
            extra_headers=headers,
        )

    def me(self) -> Any:
        """Кто я: лимиты, остаток бюджета, активные лизы."""
        return self.request("GET", f"{API_PREFIX}/me")

    def bridge_manifest(self, content: bool = False) -> Any:
        """Манифест папки моста на сервере: отпечаток, ревизия, sha256 файлов, с `content` — тексты."""
        params = {"content": "true"} if content else None
        return self.request("GET", f"{API_PREFIX}/mcp/bridge", params=params)


# --- точка входа -------------------------------------------------------------


def run_stdio(
    *,
    url: str | None = None,
    token: str | None = None,
    token_file: str | Path | None = None,
    project: str | None = None,
    timeout: float | None = None,
    stdin: Any = None,
    stdout: Any = None,
    bridge_path: str | Path | None = None,
) -> int:
    """Запускает сервер на готовом транспорте: так его запускают и CLI, и тесты."""
    transport = Transport(
        base_url=url,
        token=token,
        timeout=timeout,
        token_file=token_file,
        project=project,
        bridge_path=bridge_path if bridge_path is not None else Path(__file__).resolve(),
    )
    return serve(transport, stdin=stdin, stdout=stdout)


def _env_flag(name: str) -> bool:
    return (os.environ.get(name) or "").strip().lower() in ("1", "true", "yes", "on")


def auto_update(
    bridge_path: Path,
    *,
    url: str | None = None,
    token: str | None = None,
    token_file: str | Path | None = None,
    project: str | None = None,
    timeout: float | None = None,
) -> bool:
    """Обновляет мост до подключения клиента. True — файл моста заменён, его надо загрузить.

    Так новая сессия агента сразу получает инструменты сервера, а не узнаёт из предупреждения,
    что их не хватает. Любой сбой (трекер не поднят, старый сервер без раздачи моста, битый файл)
    только пишется в stderr: работать на текущей версии лучше, чем не подняться совсем.
    """
    limit = AUTO_UPDATE_TIMEOUT if timeout is None else min(timeout, AUTO_UPDATE_TIMEOUT)
    try:
        transport = Transport(
            base_url=url,
            token=token,
            timeout=limit,
            token_file=token_file,
            project=project,
            bridge_path=bridge_path,
        )
        report = update_bridge(transport, bridge_path)
    except Exception as exc:  # noqa: BLE001 — сбой обновления не мешает работе текущей версии
        print(f"ajira-mcp: автообновление не удалось, работаю на текущей версии: {exc}", file=sys.stderr)
        return False
    if report.get("status") != "updated":
        return False
    changed = list(report.get("updated") or []) + list(report.get("created") or [])
    print(
        f"ajira-mcp: мост обновлён с трекера ({', '.join(changed)}), резервная копия в "
        f"{report.get('backup')}",
        file=sys.stderr,
    )
    return bool(report.get("entry_replaced"))


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
    parser.add_argument(
        "--auto-update",
        dest="auto_update",
        action="store_true",
        help=f"при старте обновить мост с трекера (иначе {AUTO_UPDATE_ENV}=1)",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)
    # До автообновления: оно первым пишет в stderr по-русски.
    force_utf8_stdio()
    bridge_path = Path(__file__).resolve()
    connection = {
        "url": args.url,
        "token": args.token,
        "token_file": args.token_file,
        "project": args.project,
        "timeout": args.timeout,
    }
    if (args.auto_update or _env_flag(AUTO_UPDATE_ENV)) and not _env_flag(RELOADED_ENV):
        if auto_update(bridge_path, **connection):
            # Процесс уже исполняет старый код, а клиент ещё не подключился: загружаем новый мост
            # и отдаём stdio ему — новые инструменты будут в этой же сессии.
            os.environ[RELOADED_ENV] = "1"
            fresh = load_bridge(bridge_path.read_text(encoding="utf-8"), bridge_path, "ajira_mcp")
            return int(fresh.main(argv, stdin=stdin, stdout=stdout))
    return run_stdio(stdin=stdin, stdout=stdout, bridge_path=bridge_path, **connection)

if __name__ == "__main__":
    sys.exit(main())
