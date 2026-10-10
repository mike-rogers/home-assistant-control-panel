"""Menu tree: the stored form the card edits, and the device form (spec §5).

Pure Python (probatio only, no homeassistant imports) so it is unit-testable
without a Home Assistant install. The limits mirror the firmware's
components/panel_core/include/menu.h; a menu that passes here always parses
on the device.
"""

from __future__ import annotations

from collections.abc import Iterator
from typing import Any

import probatio as vol

SCHEMA_VERSION = 1
MAX_DEPTH = 3  # list levels, root included
MAX_LIST = 20
MAX_ITEMS = 64
MAX_LABEL_BYTES = 47  # firmware truncates beyond this
MAX_ENTITY_BYTES = 63
DEFAULT_STEP_MINUTES = 15

ITEM_TYPES = ("submenu", "action", "time", "info")
ACTION_DOMAINS = ("script", "automation", "scene", "button")
TIME_DOMAINS = ("input_datetime", "time")

# Ids are wire keys (request item_id); firmware allows 31 bytes.
ITEM_ID = vol.Match(r"^[a-z0-9_]{1,31}$", msg="ids are 1-31 of a-z, 0-9, _")


class MenuError(ValueError):
    """The menu is invalid; the message says where and why."""


def _entity_id(domains: tuple[str, ...] | None = None):
    def validate(value: Any) -> str:
        if not isinstance(value, str) or "." not in value:
            raise vol.Invalid("must be an entity id")
        if len(value.encode()) > MAX_ENTITY_BYTES:
            raise vol.Invalid("entity id too long")
        if domains is not None and value.split(".", 1)[0] not in domains:
            raise vol.Invalid(f"must be one of: {', '.join(domains)}")
        return value

    return validate


def _label(value: Any) -> str:
    if not isinstance(value, str) or not value.strip():
        raise vol.Invalid("label must be non-empty text")
    if len(value.encode()) > MAX_LABEL_BYTES:
        raise vol.Invalid(f"label longer than {MAX_LABEL_BYTES} bytes")
    return value


_BASE = {
    vol.Required("id"): ITEM_ID,
    vol.Required("label"): _label,
    vol.Required("type"): vol.In(ITEM_TYPES),
}

_ITEM_SCHEMAS = {
    "submenu": vol.Schema({**_BASE, vol.Required("items"): list}),
    "action": vol.Schema(
        {
            **_BASE,
            vol.Required("target"): _entity_id(ACTION_DOMAINS),
            vol.Optional("undo", default=False): bool,
            # None = device default (confirm unless undoable).
            vol.Optional("confirm", default=None): vol.Any(None, bool),
            # None = derive from what the target references.
            vol.Optional("snapshot", default=None): vol.Any(
                None, [_entity_id()]
            ),
        }
    ),
    "time": vol.Schema(
        {
            **_BASE,
            vol.Required("entity"): _entity_id(TIME_DOMAINS),
            vol.Optional("step_minutes", default=DEFAULT_STEP_MINUTES): vol.All(
                int, vol.Range(min=1, max=720)
            ),
            vol.Optional("undo", default=True): bool,
        }
    ),
    "info": vol.Schema({**_BASE, vol.Required("entity"): _entity_id()}),
}


def validate_menu(data: Any) -> dict[str, Any]:
    """Validate and normalize a stored menu; raises MenuError."""
    if not isinstance(data, dict):
        raise MenuError("menu must be an object")
    title = data.get("title", "")
    if not isinstance(title, str) or len(title.encode()) > MAX_LABEL_BYTES:
        raise MenuError(f"title must be text of at most {MAX_LABEL_BYTES} bytes")

    seen: set[str] = set()

    def walk(items: Any, depth: int, where: str) -> list[dict[str, Any]]:
        if not isinstance(items, list):
            raise MenuError(f"{where}: items must be a list")
        if len(items) > MAX_LIST:
            raise MenuError(f"{where}: more than {MAX_LIST} items")
        out = []
        for index, raw in enumerate(items):
            here = f"{where}[{index}]"
            if not isinstance(raw, dict):
                raise MenuError(f"{here}: must be an object")
            schema = _ITEM_SCHEMAS.get(raw.get("type"))
            if schema is None:
                raise MenuError(
                    f"{here}: type must be one of {', '.join(ITEM_TYPES)}"
                )
            try:
                item = schema(raw)
            except vol.Invalid as err:
                raise MenuError(f"{here}: {err}") from err
            if item["id"] in seen:
                raise MenuError(f"{here}: duplicate id {item['id']}")
            seen.add(item["id"])
            if len(seen) > MAX_ITEMS:
                raise MenuError(f"more than {MAX_ITEMS} items in total")
            if item["type"] == "submenu":
                if depth >= MAX_DEPTH:
                    raise MenuError(
                        f"{here}: submenus nest at most {MAX_DEPTH} levels"
                    )
                item["items"] = walk(item["items"], depth + 1, f"{here}.items")
            out.append(item)
        return out

    return {"title": title, "items": walk(data.get("items", []), 1, "items")}


def iter_items(menu: dict[str, Any]) -> Iterator[dict[str, Any]]:
    """Every item, depth-first, submenus included."""

    def walk(items: list[dict[str, Any]]) -> Iterator[dict[str, Any]]:
        for item in items:
            yield item
            if item["type"] == "submenu":
                yield from walk(item["items"])

    return walk(menu["items"])


def find_item(menu: dict[str, Any], item_id: str) -> dict[str, Any] | None:
    return next((i for i in iter_items(menu) if i["id"] == item_id), None)


def state_entities(menu: dict[str, Any]) -> set[str]:
    """Entities whose state the device displays (time and info items)."""
    return {i["entity"] for i in iter_items(menu) if i["type"] in ("time", "info")}


def to_device(menu: dict[str, Any]) -> dict[str, Any]:
    """The device-facing JSON: only what the firmware needs to render.

    What an action does (its target, its snapshot list) stays in HA.
    """

    def convert(item: dict[str, Any]) -> dict[str, Any]:
        out: dict[str, Any] = {
            "id": item["id"],
            "label": item["label"],
            "type": item["type"],
        }
        if item["type"] == "submenu":
            out["items"] = [convert(child) for child in item["items"]]
        elif item["type"] == "action":
            out["undo"] = item["undo"]
            if item["confirm"] is not None:
                out["confirm"] = item["confirm"]
        elif item["type"] == "time":
            out["entity"] = item["entity"]
            out["step_minutes"] = item["step_minutes"]
            out["undo"] = item["undo"]
        else:
            out["entity"] = item["entity"]
        return out

    return {
        "schema": SCHEMA_VERSION,
        "title": menu["title"],
        "items": [convert(item) for item in menu["items"]],
    }
