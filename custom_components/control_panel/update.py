"""Panel firmware update entity.

installed_version comes from the panel's retained status message;
latest_version from firmware/version.txt next to the served binary (staged
by scripts/release_firmware.sh). Install publishes the download URL to
panel/<id>/ota; the panel pulls it, flashes its idle OTA slot and reboots,
with bootloader rollback if the new image never reaches MQTT.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path

from homeassistant.components import mqtt
from homeassistant.components.update import (
    UpdateDeviceClass,
    UpdateEntity,
    UpdateEntityFeature,
)
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import PanelConfigEntry
from .const import FIRMWARE_FILE, FIRMWARE_URL
from .entity import PanelEntity

_LOGGER = logging.getLogger(__name__)

FIRMWARE_DIR = Path(__file__).parent / "firmware"


def read_latest_version() -> str | None:
    """The staged firmware's version, if a binary is actually staged.

    A git checkout tracks version.txt but not the binary; never advertise
    an update whose download would 404.
    """
    try:
        if not (FIRMWARE_DIR / FIRMWARE_FILE).is_file():
            return None
        version = (FIRMWARE_DIR / "version.txt").read_text(encoding="utf-8")
        return version.strip() or None
    except OSError:
        return None


async def async_setup_entry(
    hass: HomeAssistant,
    entry: PanelConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    async_add_entities([PanelFirmware(entry.runtime_data)], update_before_add=True)


class PanelFirmware(PanelEntity, UpdateEntity):
    _attr_name = "Firmware"
    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_supported_features = UpdateEntityFeature.INSTALL
    _attr_should_poll = True  # re-reads version.txt after a new staging

    def __init__(self, manager) -> None:
        super().__init__(manager, "firmware")
        self._attr_installed_version = None
        self._attr_latest_version = None

    async def async_added_to_hass(self) -> None:
        @callback
        def status_received(msg: mqtt.ReceiveMessage) -> None:
            try:
                version = json.loads(msg.payload).get("version")
            except (ValueError, AttributeError):
                return
            if isinstance(version, str):
                self._attr_installed_version = version
                self.async_write_ha_state()

        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass, f"{self._manager.topic}/status", status_received
            )
        )

    async def async_update(self) -> None:
        self._attr_latest_version = await self.hass.async_add_executor_job(
            read_latest_version
        )

    async def async_install(
        self, version: str | None, backup: bool, **kwargs
    ) -> None:
        host = getattr(self.hass.config.api, "local_ip", None)
        port = getattr(self.hass.config.api, "port", 8123)
        if not host:
            _LOGGER.error("Cannot determine HA's local IP for the OTA URL")
            return
        url = f"http://{host}:{port}{FIRMWARE_URL}"
        _LOGGER.info("Requesting %s OTA from %s", self._manager.device_id, url)
        await mqtt.async_publish(
            self.hass,
            f"{self._manager.topic}/ota",
            json.dumps({"url": url}),
            qos=1,
            retain=False,
        )
