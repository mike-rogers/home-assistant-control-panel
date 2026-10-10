"""Discovery/adoption config flow and the firmware update entity."""

from ipaddress import ip_address
import json
from unittest.mock import AsyncMock, Mock, patch

import pytest
from pytest_homeassistant_custom_component.common import (
    MockConfigEntry,
    async_fire_mqtt_message,
)

from homeassistant import config_entries
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from custom_components.control_panel import update as update_module
from custom_components.control_panel.const import DOMAIN

PUSH = "custom_components.control_panel.config_flow.async_push_config"
BROKER = {
    "mqtt_uri": "mqtt://10.0.0.2:1883",
    "mqtt_username": "panel",
    "mqtt_password": "secret",
}


def discovery(device_id="panel-a1b2c3", host="10.0.0.50"):
    return ZeroconfServiceInfo(
        ip_address=ip_address(host),
        ip_addresses=[ip_address(host)],
        port=80,
        hostname=f"{device_id}.local.",
        type="_ctlpanel._tcp.local.",
        name=f"Control Panel._ctlpanel._tcp.local.",
        properties={"id": device_id, "version": "0.1.0", "provisioned": "0"},
    )


async def start_discovery(hass, info):
    return await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=info
    )


async def test_discovered_panel_is_adopted(hass: HomeAssistant, mqtt_mock):
    result = await start_discovery(hass, discovery())
    assert result["type"] is FlowResultType.FORM
    assert result["step_id"] == "adopt"

    with (
        patch(PUSH, new=AsyncMock()) as push,
        patch("custom_components.control_panel.async_setup_entry",
              return_value=True),
    ):
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], {"name": "Kitchen", **BROKER}
        )
    push.assert_awaited_once_with(
        hass, "10.0.0.50", "mqtt://10.0.0.2:1883", "panel", "secret"
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["title"] == "Kitchen"
    assert result["data"] == {"device_id": "panel-a1b2c3",
                              "device_host": "10.0.0.50", **BROKER}
    assert result["result"].unique_id == "panel-a1b2c3"


async def test_adoption_push_failure_is_shown(hass: HomeAssistant, mqtt_mock):
    result = await start_discovery(hass, discovery())
    with patch(PUSH, new=AsyncMock(side_effect=OSError("refused"))):
        result = await hass.config_entries.flow.async_configure(
            result["flow_id"], {"name": "Kitchen", **BROKER}
        )
    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "cannot_connect"}


async def test_rediscovery_updates_host(hass: HomeAssistant, mqtt_mock):
    entry = MockConfigEntry(
        domain=DOMAIN, unique_id="panel-a1b2c3",
        data={"device_id": "panel-a1b2c3", "device_host": "10.0.0.50"},
    )
    entry.add_to_hass(hass)
    result = await start_discovery(hass, discovery(host="10.0.0.77"))
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"
    assert entry.data["device_host"] == "10.0.0.77"


async def test_discovery_without_valid_id(hass: HomeAssistant, mqtt_mock):
    result = await start_discovery(hass, discovery(device_id="Not Valid!"))
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "invalid_discovery"


async def test_options_flow_readopts(hass: HomeAssistant, mqtt_mock):
    entry = MockConfigEntry(
        domain=DOMAIN, unique_id="panel-a1b2c3",
        data={"device_id": "panel-a1b2c3", "device_host": "10.0.0.50"},
    )
    entry.add_to_hass(hass)
    result = await hass.config_entries.options.async_init(entry.entry_id)
    assert result["type"] is FlowResultType.FORM
    with patch(PUSH, new=AsyncMock()) as push:
        result = await hass.config_entries.options.async_configure(
            result["flow_id"], {"device_host": "10.0.0.51", **BROKER}
        )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert push.await_args.args[1] == "10.0.0.51"


# ---- Firmware update entity ----

FIRMWARE = "update.control_panel_firmware"


@pytest.fixture
async def panel(hass: HomeAssistant, mqtt_mock, tmp_path):
    (tmp_path / "version.txt").write_text("0.2.0")
    (tmp_path / "control_panel.bin").write_bytes(b"\0" * 16)
    hass.config.api = Mock(local_ip="10.0.0.2", port=8123)
    entry = MockConfigEntry(
        domain=DOMAIN, title="Control Panel", unique_id="testpanel",
        data={"device_id": "testpanel"},
    )
    entry.add_to_hass(hass)
    with patch.object(update_module, "FIRMWARE_DIR", tmp_path):
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
        yield entry


async def test_update_offered_and_installed(hass, mqtt_mock, panel):
    async_fire_mqtt_message(hass, "panel/testpanel/status",
                            '{"version":"0.1.0","rssi":-50}')
    await hass.async_block_till_done()
    state = hass.states.get(FIRMWARE)
    assert state.attributes["installed_version"] == "0.1.0"
    assert state.attributes["latest_version"] == "0.2.0"
    assert state.state == "on"

    mqtt_mock.async_publish.reset_mock()
    await hass.services.async_call(
        "update", "install", {"entity_id": FIRMWARE}, blocking=True
    )
    ota = [c for c in mqtt_mock.async_publish.call_args_list
           if c.args[0] == "panel/testpanel/ota"]
    assert len(ota) == 1
    assert json.loads(ota[0].args[1]) == {
        "url": "http://10.0.0.2:8123/control_panel_files/firmware.bin"
    }


def test_no_update_without_staged_binary(tmp_path):
    (tmp_path / "version.txt").write_text("0.2.0")
    with patch.object(update_module, "FIRMWARE_DIR", tmp_path):
        assert update_module.read_latest_version() is None
