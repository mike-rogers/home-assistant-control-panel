import asyncio
import itertools
import json

import pytest

from kitchen_panel.dispatcher import Dispatcher, Hooks
from kitchen_panel.menu_model import validate_menu

MENU = validate_menu(
    {
        "title": "Kitchen",
        "items": [
            {"id": "goodnight", "label": "Good night", "type": "action",
             "target": "script.good_night", "undo": True},
            {"id": "brighten", "label": "Brighten", "type": "action",
             "target": "scene.bright", "undo": True,
             "snapshot": ["light.living"]},
            {"id": "kids", "label": "Summon the kids", "type": "action",
             "target": "script.summon_kids"},
            {"id": "noop", "label": "Nothing restorable", "type": "action",
             "target": "script.notify_only", "undo": True},
            {"id": "wake_time", "label": "Wake-up", "type": "time",
             "entity": "input_datetime.house_wakeup_time"},
            {"id": "temp", "label": "Temp", "type": "info",
             "entity": "sensor.t"},
        ],
    }
)

DERIVED = {
    "script.good_night": ["light.kitchen", "lock.front", "light.kitchen"],
    "script.notify_only": [],
}


class FakeHA:
    def __init__(self):
        self.now = 1000.0
        self.calls = []
        self.successes = []
        self.states = {
            "light.kitchen": "on",
            "light.living": "off",
            "lock.front": "unlocked",
            "input_datetime.house_wakeup_time": "06:45:00",
        }
        self.fail_target = None

    async def run_target(self, target):
        self.calls.append(("run", target))
        if target == self.fail_target:
            raise RuntimeError("script.good_night failed partway")
        if target == "script.good_night":
            self.states.update({"light.kitchen": "off", "lock.front": "locked"})
        if target == "scene.bright":
            self.states["light.living"] = "on"

    async def set_time(self, entity, value):
        self.calls.append(("set", entity, value))
        self.states[entity] = value

    def capture(self, entities):
        self.calls.append(("capture", tuple(entities)))
        return {e: self.states[e] for e in entities}

    async def restore(self, states):
        self.calls.append(("restore", tuple(sorted(states))))
        self.states.update(states)

    def hooks(self):
        return Hooks(
            get_menu=lambda: MENU,
            run_target=self.run_target,
            set_time=self.set_time,
            derive_snapshot=lambda target: DERIVED.get(target, []),
            capture=self.capture,
            restore=self.restore,
            on_success=lambda item, op: self.successes.append((item["id"], op)),
            clock=lambda: self.now,
        )


@pytest.fixture
def ha():
    return FakeHA()


@pytest.fixture
def dispatcher(ha):
    return Dispatcher(ha.hooks())


_ids = itertools.count(1)


def send(dispatcher, item_id, op="run", value=None, request_id=None):
    # Unique per call, like the firmware's boot id + counter.
    request_id = request_id or f"auto-{next(_ids)}"
    payload = json.dumps({"request_id": request_id, "item_id": item_id,
                          "op": op, "value": value})
    return asyncio.run(dispatcher.async_handle(payload))


def test_run_with_derived_snapshot_then_undo(ha, dispatcher):
    result = send(dispatcher, "goodnight", request_id="r1")
    assert result == {"request_id": "r1", "status": "ok", "message": "",
                      "undo_seconds": 30}
    # Derived entities are de-duplicated and captured BEFORE the run.
    assert ha.calls[:2] == [("capture", ("light.kitchen", "lock.front")),
                            ("run", "script.good_night")]
    assert ha.states["lock.front"] == "locked"
    assert ha.successes == [("goodnight", "run")]

    ha.now += 29
    result = send(dispatcher, "goodnight", op="undo", request_id="r2")
    assert result["status"] == "ok"
    assert result["undo_seconds"] is None
    assert ha.states["light.kitchen"] == "on"
    assert ha.states["lock.front"] == "unlocked"

    # Single use.
    assert send(dispatcher, "goodnight", op="undo")["message"] == "Nothing to undo"


def test_explicit_snapshot_overrides_derivation(ha, dispatcher):
    send(dispatcher, "brighten")
    assert ("capture", ("light.living",)) in ha.calls


def test_undo_window_expires(ha, dispatcher):
    send(dispatcher, "goodnight")
    ha.now += 30
    result = send(dispatcher, "goodnight", op="undo")
    assert result["status"] == "error"
    assert result["message"] == "Nothing to undo"
    assert ha.states["lock.front"] == "locked"


def test_undo_must_match_latest_operation(ha, dispatcher):
    send(dispatcher, "goodnight")
    send(dispatcher, "brighten")
    assert send(dispatcher, "goodnight", op="undo")["status"] == "error"
    assert send(dispatcher, "brighten", op="undo")["status"] == "ok"


def test_non_undoable_run_clears_slot(ha, dispatcher):
    send(dispatcher, "goodnight")
    result = send(dispatcher, "kids")
    assert result["status"] == "ok" and result["undo_seconds"] is None
    assert send(dispatcher, "goodnight", op="undo")["status"] == "error"


def test_undoable_item_with_nothing_to_restore(ha, dispatcher):
    result = send(dispatcher, "noop")
    assert result["status"] == "ok"
    assert result["undo_seconds"] is None


def test_failed_run_reports_error_but_stays_undoable(ha, dispatcher):
    ha.fail_target = "script.good_night"
    result = send(dispatcher, "goodnight")
    assert result["status"] == "error"
    assert "failed partway" in result["message"]
    assert result["undo_seconds"] == 30
    assert ha.successes == []


def test_long_error_messages_fit_the_device(ha, dispatcher):
    ha.fail_target = "script.good_night"

    async def boom(target):
        raise RuntimeError("x" * 200)

    dispatcher._hooks.run_target = boom
    message = send(dispatcher, "goodnight")["message"]
    assert len(message) == 63 and message.endswith("...")


def test_set_time_and_undo(ha, dispatcher):
    result = send(dispatcher, "wake_time", op="set", value="07:15")
    assert result["status"] == "ok" and result["undo_seconds"] == 30
    assert ("set", "input_datetime.house_wakeup_time", "07:15:00") in ha.calls
    assert ha.successes == [("wake_time", "set")]
    send(dispatcher, "wake_time", op="undo")
    assert ha.states["input_datetime.house_wakeup_time"] == "06:45:00"


@pytest.mark.parametrize("value", [None, "7:15", "24:00", "07:60", 715])
def test_set_time_rejects_bad_values(ha, dispatcher, value):
    result = send(dispatcher, "wake_time", op="set", value=value)
    assert result["status"] == "error"
    assert result["message"] == "Time must be HH:MM"
    assert not any(call[0] == "set" for call in ha.calls)


@pytest.mark.parametrize(
    ("item_id", "op", "message"),
    [
        ("missing", "run", "Menu out of date: unknown item"),
        ("temp", "run", "Temp can't be run"),
        ("goodnight", "set", "Good night can't be set"),
        ("goodnight", "explode", "Unknown operation 'explode'"),
    ],
)
def test_refusals(ha, dispatcher, item_id, op, message):
    result = send(dispatcher, item_id, op=op, request_id="r1")
    assert result == {"request_id": "r1", "status": "error",
                      "message": message, "undo_seconds": None}


@pytest.mark.parametrize(
    "payload", ["not json", "{}", '{"request_id": 5}', "[1]"]
)
def test_unanswerable_payloads(dispatcher, payload):
    assert asyncio.run(dispatcher.async_handle(payload)) is None


def test_duplicate_request_is_answered_not_rerun(ha, dispatcher):
    # QoS 1 is at-least-once: a retransmitted request must not run twice.
    first = send(dispatcher, "kids", request_id="dup")
    again = send(dispatcher, "kids", request_id="dup")
    assert again == first
    assert ha.calls.count(("run", "script.summon_kids")) == 1
    send(dispatcher, "kids", request_id="other")
    assert ha.calls.count(("run", "script.summon_kids")) == 2
