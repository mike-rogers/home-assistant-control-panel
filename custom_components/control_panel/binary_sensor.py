"""Panel connectivity, driven by the MQTT availability topic (LWT)."""

from __future__ import annotations

from homeassistant.components import mqtt
from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
)
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import PanelConfigEntry
from .entity import PanelEntity


async def async_setup_entry(
    hass: HomeAssistant,
    entry: PanelConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    async_add_entities([PanelConnectivity(entry.runtime_data)])


class PanelConnectivity(PanelEntity, BinarySensorEntity):
    """on = the panel holds its MQTT session."""

    _attr_name = "Connectivity"
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, manager) -> None:
        super().__init__(manager, "connectivity")
        self._attr_is_on = False

    async def async_added_to_hass(self) -> None:
        @callback
        def received(msg: mqtt.ReceiveMessage) -> None:
            self._attr_is_on = msg.payload == "online"
            self.async_write_ha_state()

        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass, f"{self._manager.topic}/availability", received
            )
        )
