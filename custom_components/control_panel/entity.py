"""Shared base for the panel's entities."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import Entity

from .const import DOMAIN
from .manager import PanelManager


class PanelEntity(Entity):
    """An entity on the panel's device."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, manager: PanelManager, key: str) -> None:
        self._manager = manager
        self._attr_unique_id = f"{manager.device_id}_{key}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, manager.device_id)},
            name=manager.entry.title,
            manufacturer="DIY",
            model="ESP32 menu panel",
        )
