"""Panel sensors: Wi-Fi signal (from the status topic) and last action."""

from __future__ import annotations

import json
import logging

from homeassistant.components import mqtt
from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.const import SIGNAL_STRENGTH_DECIBELS_MILLIWATT, EntityCategory
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from . import PanelConfigEntry
from .const import SIGNAL_LAST_ACTION
from .entity import PanelEntity

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: PanelConfigEntry,
    async_add_entities: AddConfigEntryEntitiesCallback,
) -> None:
    manager = entry.runtime_data
    async_add_entities([PanelRssi(manager), PanelLastAction(manager)])


class PanelRssi(PanelEntity, SensorEntity):
    """Wi-Fi RSSI as reported in the retained status message."""

    _attr_name = "Wi-Fi signal"
    _attr_device_class = SensorDeviceClass.SIGNAL_STRENGTH
    _attr_native_unit_of_measurement = SIGNAL_STRENGTH_DECIBELS_MILLIWATT
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, manager) -> None:
        super().__init__(manager, "rssi")

    async def async_added_to_hass(self) -> None:
        @callback
        def received(msg: mqtt.ReceiveMessage) -> None:
            try:
                rssi = json.loads(msg.payload).get("rssi")
            except (ValueError, AttributeError):
                _LOGGER.warning("Bad panel status payload: %r", msg.payload)
                return
            if isinstance(rssi, int):
                self._attr_native_value = rssi
                self.async_write_ha_state()

        self.async_on_remove(
            await mqtt.async_subscribe(
                self.hass, f"{self._manager.topic}/status", received
            )
        )


class PanelLastAction(PanelEntity, SensorEntity):
    """Label of the last item run or set from the panel."""

    _attr_name = "Last action"
    _attr_icon = "mdi:gesture-tap-button"

    def __init__(self, manager) -> None:
        super().__init__(manager, "last_action")

    @property
    def native_value(self) -> str | None:
        last = self._manager.last_action
        return last["label"] if last else None

    @property
    def extra_state_attributes(self) -> dict[str, str] | None:
        last = self._manager.last_action
        if not last:
            return None
        return {
            "item_id": last["item_id"],
            "op": last["op"],
            "at": last["at"].isoformat(),
        }

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(
            async_dispatcher_connect(
                self.hass,
                SIGNAL_LAST_ACTION.format(self._manager.entry.entry_id),
                self.async_write_ha_state,
            )
        )
