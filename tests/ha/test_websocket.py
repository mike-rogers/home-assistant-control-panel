"""Websocket commands behind the menu editor card."""

from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.core import HomeAssistant
from homeassistant.setup import async_setup_component

from custom_components.control_panel.const import CARD_URL, DOMAIN

from .test_integration import MENU, house  # noqa: F401 - fixture


async def setup_panel(hass, mqtt_mock):
    entry = MockConfigEntry(
        domain=DOMAIN, title="Control Panel", unique_id="testpanel",
        data={"device_id": "testpanel"},
    )
    entry.add_to_hass(hass)
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry


async def test_menu_round_trip(hass: HomeAssistant, hass_ws_client, mqtt_mock, house):
    entry = await setup_panel(hass, mqtt_mock)
    ws = await hass_ws_client(hass)

    await ws.send_json_auto_id({"type": "control_panel/panels"})
    msg = await ws.receive_json()
    assert msg["result"] == [{"entry_id": entry.entry_id,
                              "title": "Control Panel",
                              "device_id": "testpanel"}]

    await ws.send_json_auto_id({"type": "control_panel/menu/set",
                                "entry_id": entry.entry_id, "menu": MENU})
    msg = await ws.receive_json()
    assert msg["success"], msg
    assert msg["result"]["menu"]["items"][1]["target"] == "script.good_night"

    await ws.send_json_auto_id({"type": "control_panel/menu/get",
                                "entry_id": entry.entry_id})
    msg = await ws.receive_json()
    assert msg["result"]["menu"]["title"] == "Kitchen"


async def test_invalid_menu_error(hass, hass_ws_client, mqtt_mock):
    entry = await setup_panel(hass, mqtt_mock)
    ws = await hass_ws_client(hass)
    await ws.send_json_auto_id({
        "type": "control_panel/menu/set", "entry_id": entry.entry_id,
        "menu": {"items": [{"id": "x", "label": "", "type": "action",
                            "target": "script.x"}]},
    })
    msg = await ws.receive_json()
    assert not msg["success"]
    assert msg["error"]["code"] == "invalid_menu"
    assert "label" in msg["error"]["message"]


async def test_set_menu_requires_admin(
    hass, hass_ws_client, hass_read_only_access_token, mqtt_mock
):
    entry = await setup_panel(hass, mqtt_mock)
    ws = await hass_ws_client(hass, hass_read_only_access_token)
    await ws.send_json_auto_id({"type": "control_panel/menu/set",
                                "entry_id": entry.entry_id, "menu": MENU})
    msg = await ws.receive_json()
    assert msg["error"]["code"] == "unauthorized"


async def test_unknown_entry(hass, hass_ws_client, mqtt_mock):
    await setup_panel(hass, mqtt_mock)
    ws = await hass_ws_client(hass)
    await ws.send_json_auto_id({"type": "control_panel/menu/get",
                                "entry_id": "nope"})
    msg = await ws.receive_json()
    assert msg["error"]["code"] == "not_found"


async def test_derive_snapshot_preview(hass, hass_ws_client, mqtt_mock, house):
    await setup_panel(hass, mqtt_mock)
    ws = await hass_ws_client(hass)
    await ws.send_json_auto_id({"type": "control_panel/snapshot/derive",
                                "target": "script.good_night"})
    msg = await ws.receive_json()
    assert [e["entity_id"] for e in msg["result"]["entities"]] == [
        "input_boolean.kitchen_light", "input_boolean.porch_light"]


async def test_card_is_served(hass, hass_client, mqtt_mock):
    await setup_panel(hass, mqtt_mock)
    client = await hass_client()
    resp = await client.get(CARD_URL)
    assert resp.status == 200
    assert "control-panel-card" in await resp.text()
