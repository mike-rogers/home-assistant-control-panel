"""The integration end to end inside HA core, with MQTT mocked."""

import itertools
import json
from unittest.mock import patch

import pytest
from pytest_homeassistant_custom_component.common import (
    MockConfigEntry,
    async_capture_events,
    async_fire_mqtt_message,
)

from homeassistant import config_entries
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.exceptions import ServiceValidationError
from homeassistant.setup import async_setup_component

from custom_components.kitchen_panel.const import DOMAIN

TOPIC = "panel/kitchenpanel"
WAKE = "input_datetime.house_wakeup_time"

MENU = {
    "title": "Kitchen",
    "items": [
        {
            "id": "wake",
            "label": "Wake-up",
            "type": "submenu",
            "items": [
                {"id": "wake_time", "label": "Tomorrow's wake-up",
                 "type": "time", "entity": WAKE},
            ],
        },
        {"id": "goodnight", "label": "Good night", "type": "action",
         "target": "script.good_night", "undo": True},
        {"id": "kids", "label": "Summon the kids", "type": "action",
         "target": "script.summon_kids"},
    ],
}


@pytest.fixture
async def house(hass: HomeAssistant):
    """A tiny house: two 'lights' (input_booleans), a wake-up time, and
    scripts, one of which calls another (snapshot derivation recurses)."""
    assert await async_setup_component(
        hass,
        "input_boolean",
        {"input_boolean": {"kitchen_light": {"initial": True},
                           "porch_light": {"initial": True}}},
    )
    assert await async_setup_component(
        hass,
        "input_datetime",
        {"input_datetime": {"house_wakeup_time": {
            "has_date": False, "has_time": True, "initial": "06:45"}}},
    )
    assert await async_setup_component(
        hass,
        "script",
        {"script": {
            "good_night": {"sequence": [
                {"action": "input_boolean.turn_off",
                 "target": {"entity_id": "input_boolean.kitchen_light"}},
                {"action": "script.porch_off"},
            ]},
            "porch_off": {"sequence": [
                {"action": "input_boolean.turn_off",
                 "target": {"entity_id": "input_boolean.porch_light"}},
            ]},
            "summon_kids": {"sequence": []},
        }},
    )
    await hass.async_block_till_done()


@pytest.fixture
async def panel(hass: HomeAssistant, mqtt_mock, house):
    entry = MockConfigEntry(
        domain=DOMAIN,
        title="Kitchen Panel",
        unique_id="kitchenpanel",
        data={"device_id": "kitchenpanel"},
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry


def published(mqtt_mock, topic):
    """Payloads published to a topic, oldest first, as (payload, retain)."""
    return [
        (call.args[1], call.args[3])
        for call in mqtt_mock.async_publish.call_args_list
        if call.args[0] == topic
    ]


async def set_menu(hass, entry, menu=MENU):
    await hass.services.async_call(
        DOMAIN, "set_menu",
        {"config_entry_id": entry.entry_id, "menu": menu},
        blocking=True,
    )
    await hass.async_block_till_done()


_ids = itertools.count(1)


async def request(hass, mqtt_mock, item_id, op="run", value=None, rid=None):
    rid = rid or f"auto-{next(_ids)}"  # unique, like the firmware's
    mqtt_mock.async_publish.reset_mock()
    async_fire_mqtt_message(
        hass, f"{TOPIC}/request",
        json.dumps({"request_id": rid, "item_id": item_id, "op": op,
                    "value": value}),
    )
    await hass.async_block_till_done()
    results = published(mqtt_mock, f"{TOPIC}/result")
    assert len(results) == 1, results
    payload, retain = results[0]
    assert retain is False
    return json.loads(payload)


# ---- Config flow ----

async def test_config_flow(hass: HomeAssistant, mqtt_mock):
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_USER}
    )
    assert result["type"] is FlowResultType.FORM

    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {"name": "Kitchen", "device_id": "Bad Id!"}
    )
    assert result["errors"] == {"device_id": "invalid_device_id"}

    with patch("custom_components.kitchen_panel.async_setup_entry",
               return_value=True):
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], {"name": "Kitchen", "device_id": "panel-a1b2c3"}
        )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"] == {"device_id": "panel-a1b2c3"}

    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_USER}
    )
    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {"name": "Again", "device_id": "panel-a1b2c3"}
    )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"


# ---- Menu and state publishing ----

async def test_setup_publishes_empty_menu(hass, mqtt_mock, panel):
    [(payload, retain)] = published(mqtt_mock, f"{TOPIC}/menu")
    assert retain is True
    assert json.loads(payload) == {"schema": 1, "title": "Kitchen Panel",
                                   "items": []}


async def test_set_menu_publishes_device_form_and_states(hass, mqtt_mock, panel):
    mqtt_mock.async_publish.reset_mock()
    await set_menu(hass, panel)

    [(payload, retain)] = published(mqtt_mock, f"{TOPIC}/menu")
    assert retain is True
    device = json.loads(payload)
    assert device["items"][1] == {"id": "goodnight", "label": "Good night",
                                  "type": "action", "undo": True}

    [(payload, retain)] = published(mqtt_mock, f"{TOPIC}/state/{WAKE}")
    assert retain is True
    assert json.loads(payload)["state"] == "06:45:00"

    response = await hass.services.async_call(
        DOMAIN, "get_menu", {"config_entry_id": panel.entry_id},
        blocking=True, return_response=True,
    )
    assert response["menu"]["items"][1]["target"] == "script.good_night"


async def test_menu_survives_reload(hass, mqtt_mock, panel):
    await set_menu(hass, panel)
    assert await hass.config_entries.async_reload(panel.entry_id)
    await hass.async_block_till_done()
    payload, _ = published(mqtt_mock, f"{TOPIC}/menu")[-1]
    assert json.loads(payload)["title"] == "Kitchen"


async def test_invalid_menu_is_refused(hass, mqtt_mock, panel):
    bad = {"items": [{"id": "x", "label": "X", "type": "action",
                      "target": "light.kitchen"}]}
    with pytest.raises(ServiceValidationError, match="must be one of"):
        await set_menu(hass, panel, bad)


async def test_state_changes_are_republished_and_stale_cleared(
    hass, mqtt_mock, panel
):
    await set_menu(hass, panel)
    mqtt_mock.async_publish.reset_mock()
    await hass.services.async_call(
        "input_datetime", "set_datetime",
        {"entity_id": WAKE, "time": "07:30:00"}, blocking=True,
    )
    await hass.async_block_till_done()
    payload, _ = published(mqtt_mock, f"{TOPIC}/state/{WAKE}")[-1]
    assert json.loads(payload)["state"] == "07:30:00"

    # Removing the time item clears its retained state topic.
    mqtt_mock.async_publish.reset_mock()
    await set_menu(hass, panel, {"title": "K", "items": MENU["items"][1:]})
    assert published(mqtt_mock, f"{TOPIC}/state/{WAKE}") == [("", True)]


# ---- Requests ----

async def test_goodnight_run_and_undo(hass, mqtt_mock, panel):
    await set_menu(hass, panel)
    events = async_capture_events(hass, "kitchen_panel_action")

    result = await request(hass, mqtt_mock, "goodnight", rid="r1")
    assert result == {"request_id": "r1", "status": "ok", "message": "",
                      "undo_seconds": 30}
    # The derived snapshot followed the nested script to the porch light.
    assert hass.states.get("input_boolean.kitchen_light").state == "off"
    assert hass.states.get("input_boolean.porch_light").state == "off"
    assert events[0].data["item_id"] == "goodnight"
    assert hass.states.get("sensor.kitchen_panel_last_action").state == (
        "Good night"
    )

    result = await request(hass, mqtt_mock, "goodnight", op="undo", rid="r2")
    assert result["status"] == "ok"
    assert hass.states.get("input_boolean.kitchen_light").state == "on"
    assert hass.states.get("input_boolean.porch_light").state == "on"


async def test_set_wake_time_and_undo(hass, mqtt_mock, panel):
    await set_menu(hass, panel)
    result = await request(hass, mqtt_mock, "wake_time", op="set",
                           value="07:15")
    assert result["status"] == "ok" and result["undo_seconds"] == 30
    assert hass.states.get(WAKE).state == "07:15:00"

    await request(hass, mqtt_mock, "wake_time", op="undo")
    assert hass.states.get(WAKE).state == "06:45:00"


async def test_missing_target_is_reported(hass, mqtt_mock, panel):
    menu = {"items": [{"id": "gone", "label": "Gone", "type": "action",
                       "target": "script.does_not_exist"}]}
    await set_menu(hass, panel, menu)
    result = await request(hass, mqtt_mock, "gone")
    assert result["status"] == "error"
    assert result["message"] == "script.does_not_exist not found"


# ---- Device entities ----

async def test_connectivity_and_rssi(hass, mqtt_mock, panel):
    connectivity = "binary_sensor.kitchen_panel_connectivity"
    rssi = "sensor.kitchen_panel_wi_fi_signal"
    assert hass.states.get(connectivity).state == "off"

    async_fire_mqtt_message(hass, f"{TOPIC}/availability", "online")
    async_fire_mqtt_message(hass, f"{TOPIC}/status",
                            '{"version":"0.1.0","rssi":-61}')
    await hass.async_block_till_done()
    assert hass.states.get(connectivity).state == "on"
    assert hass.states.get(rssi).state == "-61"

    async_fire_mqtt_message(hass, f"{TOPIC}/availability", "offline")
    await hass.async_block_till_done()
    assert hass.states.get(connectivity).state == "off"


async def test_remove_clears_retained_topics(hass, mqtt_mock, panel):
    await set_menu(hass, panel)
    mqtt_mock.async_publish.reset_mock()
    assert await hass.config_entries.async_remove(panel.entry_id)
    await hass.async_block_till_done()
    assert published(mqtt_mock, f"{TOPIC}/menu") == [("", True)]
    assert published(mqtt_mock, f"{TOPIC}/state/{WAKE}") == [("", True)]


async def test_automation_target_snapshot_via_script_turn_on(
    hass, mqtt_mock, panel
):
    assert await async_setup_component(
        hass,
        "automation",
        {"automation": {
            "id": "porch_auto",
            "alias": "Porch auto",
            "triggers": [],
            "actions": [
                {"action": "script.turn_on",
                 "target": {"entity_id": "script.porch_off"}},
            ],
        }},
    )
    await hass.async_block_till_done()
    await set_menu(hass, panel, {"items": [
        {"id": "porch", "label": "Porch", "type": "action",
         "target": "automation.porch_auto", "undo": True},
    ]})

    result = await request(hass, mqtt_mock, "porch")
    assert result["undo_seconds"] == 30
    await hass.async_block_till_done()
    assert hass.states.get("input_boolean.porch_light").state == "off"

    await request(hass, mqtt_mock, "porch", op="undo")
    assert hass.states.get("input_boolean.porch_light").state == "on"
