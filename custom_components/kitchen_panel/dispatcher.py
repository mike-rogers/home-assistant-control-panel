"""Request handling for panel/<id>/request (spec §4, §7).

Pure Python: everything that touches Home Assistant is injected, so the
run / set / undo rules are unit-testable without an HA install. The manager
wires the real implementations in.
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable, Iterable
from dataclasses import dataclass
import json
import logging
import re
from typing import Any

from .menu_model import find_item

_LOGGER = logging.getLogger(__name__)

DEFAULT_UNDO_SECONDS = 30
MESSAGE_MAX = 63  # the firmware's result message buffer
TIME_VALUE = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")


@dataclass
class Hooks:
    """The Home Assistant operations the dispatcher needs."""

    get_menu: Callable[[], dict[str, Any]]
    # Start an action target (script/automation/scene/button).
    run_target: Callable[[str], Awaitable[None]]
    # Set a time entity to "HH:MM:SS".
    set_time: Callable[[str, str], Awaitable[None]]
    # Entities an action target affects, for the undo snapshot.
    derive_snapshot: Callable[[str], Iterable[str]]
    # Capture current states (opaque to the dispatcher); restore them later.
    capture: Callable[[list[str]], Any]
    restore: Callable[[Any], Awaitable[None]]
    # Called after each successful run/set (last-action sensor).
    on_success: Callable[[dict[str, Any], str], None]
    clock: Callable[[], float]


@dataclass
class _UndoSlot:
    item_id: str
    states: Any
    deadline: float


class RequestError(Exception):
    """A request HA refuses; the message is shown on the panel."""


def _message(err: BaseException) -> str:
    text = str(err) or type(err).__name__
    return text if len(text) <= MESSAGE_MAX else text[: MESSAGE_MAX - 3] + "..."


class Dispatcher:
    """Turns requests into HA calls; keeps the single undo slot."""

    def __init__(self, hooks: Hooks, undo_seconds: int = DEFAULT_UNDO_SECONDS):
        self._hooks = hooks
        self._undo_seconds = undo_seconds
        self._undo: _UndoSlot | None = None
        # One request at a time, so snapshots never interleave.
        self._lock = asyncio.Lock()

    def undo_item(self) -> str | None:
        """The item that can currently be undone, if any."""
        if self._undo and self._hooks.clock() < self._undo.deadline:
            return self._undo.item_id
        return None

    async def async_handle(self, payload: str | bytes) -> dict[str, Any] | None:
        """Handle one request payload; returns the result to publish.

        None when the payload is too broken to answer (no request_id): the
        device times out and says HA didn't respond, which is accurate.
        """
        try:
            request = json.loads(payload)
            request_id = request["request_id"]
            if not isinstance(request_id, str):
                raise TypeError("request_id must be a string")
        except (ValueError, KeyError, TypeError) as err:
            _LOGGER.warning("Unusable panel request %r: %s", payload, err)
            return None

        async with self._lock:
            undo_seconds = None
            try:
                undo_seconds = await self._dispatch(request)
            except RequestError as err:
                return self._result(request_id, False, str(err), None)
            except Exception as err:  # noqa: BLE001 - reported to the panel
                _LOGGER.exception("Panel request %s failed", request)
                # A run that failed partway may still be undoable.
                return self._result(
                    request_id, False, _message(err), self._live_window(request)
                )
            return self._result(request_id, True, "", undo_seconds)

    def _live_window(self, request: dict[str, Any]) -> int | None:
        if request.get("op") != "undo" and self.undo_item() == request.get(
            "item_id"
        ):
            return self._undo_seconds
        return None

    @staticmethod
    def _result(
        request_id: str, ok: bool, message: str, undo_seconds: int | None
    ) -> dict[str, Any]:
        return {
            "request_id": request_id,
            "status": "ok" if ok else "error",
            "message": message,
            "undo_seconds": undo_seconds,
        }

    async def _dispatch(self, request: dict[str, Any]) -> int | None:
        op = request.get("op")
        item_id = request.get("item_id")
        if not isinstance(item_id, str):
            raise RequestError("Request without an item")

        if op == "undo":
            return await self._undo_op(item_id)

        item = find_item(self._hooks.get_menu(), item_id)
        if item is None:
            raise RequestError("Menu out of date: unknown item")

        if op == "run":
            if item["type"] != "action":
                raise RequestError(f"{item['label']} can't be run")
            return await self._run(item)
        if op == "set":
            if item["type"] != "time":
                raise RequestError(f"{item['label']} can't be set")
            return await self._set_time(item, request.get("value"))
        raise RequestError(f"Unknown operation {op!r}")

    def _take_snapshot(self, item: dict[str, Any], entities: list[str]) -> bool:
        """Replace the undo slot: HA keeps only the latest operation's."""
        self._undo = None
        if not entities:
            return False
        self._undo = _UndoSlot(
            item_id=item["id"],
            states=self._hooks.capture(entities),
            deadline=self._hooks.clock() + self._undo_seconds,
        )
        return True

    async def _run(self, item: dict[str, Any]) -> int | None:
        undoable = False
        if item["undo"]:
            entities = item["snapshot"]
            if entities is None:
                entities = sorted(set(self._hooks.derive_snapshot(item["target"])))
            undoable = self._take_snapshot(item, entities)
            if not undoable:
                _LOGGER.warning(
                    "%s is marked undoable but affects no restorable entities; "
                    "set its snapshot list in the panel card",
                    item["target"],
                )
        else:
            self._undo = None
        await self._hooks.run_target(item["target"])
        self._hooks.on_success(item, "run")
        return self._undo_seconds if undoable else None

    async def _set_time(self, item: dict[str, Any], value: Any) -> int | None:
        match = TIME_VALUE.match(value) if isinstance(value, str) else None
        if match is None:
            raise RequestError("Time must be HH:MM")
        undoable = (
            self._take_snapshot(item, [item["entity"]]) if item["undo"] else False
        )
        if not item["undo"]:
            self._undo = None
        await self._hooks.set_time(item["entity"], f"{value}:00")
        self._hooks.on_success(item, "set")
        return self._undo_seconds if undoable else None

    async def _undo_op(self, item_id: str) -> None:
        if self.undo_item() != item_id:
            raise RequestError("Nothing to undo")
        slot, self._undo = self._undo, None
        await self._hooks.restore(slot.states)
        return None
