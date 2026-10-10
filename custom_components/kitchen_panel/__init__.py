"""The Kitchen Panel integration: HA side of an MQTT menu panel (spec §7)."""

from __future__ import annotations

import logging
from typing import Any

import probatio as vol

from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry, ConfigEntryState
from homeassistant.const import Platform
from homeassistant.core import (
    HomeAssistant,
    ServiceCall,
    ServiceResponse,
    SupportsResponse,
)
from homeassistant.exceptions import ConfigEntryNotReady, ServiceValidationError
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.storage import Store
from homeassistant.helpers.typing import ConfigType

from .const import (
    ATTR_CONFIG_ENTRY_ID,
    ATTR_MENU,
    DOMAIN,
    SERVICE_GET_MENU,
    SERVICE_SET_MENU,
    STORAGE_VERSION,
)
from .manager import PanelManager
from .menu_model import MenuError

_LOGGER = logging.getLogger(__name__)

PLATFORMS = [Platform.BINARY_SENSOR, Platform.SENSOR]
CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

type PanelConfigEntry = ConfigEntry[PanelManager]

ENTRY_SCHEMA = vol.Schema({vol.Required(ATTR_CONFIG_ENTRY_ID): cv.string})
SET_MENU_SCHEMA = ENTRY_SCHEMA.extend({vol.Required(ATTR_MENU): dict})


def _manager(hass: HomeAssistant, call: ServiceCall) -> PanelManager:
    entry = hass.config_entries.async_get_entry(call.data[ATTR_CONFIG_ENTRY_ID])
    if entry is None or entry.domain != DOMAIN:
        raise ServiceValidationError("Not a Kitchen Panel config entry")
    if entry.state is not ConfigEntryState.LOADED:
        raise ServiceValidationError(f"{entry.title} is not loaded")
    return entry.runtime_data


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    """Register the menu services (shared by every panel)."""

    async def handle_set_menu(call: ServiceCall) -> None:
        try:
            await _manager(hass, call).async_set_menu(call.data[ATTR_MENU])
        except MenuError as err:
            raise ServiceValidationError(f"Invalid menu: {err}") from err

    async def handle_get_menu(call: ServiceCall) -> ServiceResponse:
        menu: dict[str, Any] = _manager(hass, call).menu
        return {ATTR_MENU: menu}

    hass.services.async_register(
        DOMAIN, SERVICE_SET_MENU, handle_set_menu, schema=SET_MENU_SCHEMA
    )
    hass.services.async_register(
        DOMAIN,
        SERVICE_GET_MENU,
        handle_get_menu,
        schema=ENTRY_SCHEMA,
        supports_response=SupportsResponse.ONLY,
    )
    return True


async def async_setup_entry(hass: HomeAssistant, entry: PanelConfigEntry) -> bool:
    """Set up one panel."""
    if not await mqtt.async_wait_for_mqtt_client(hass):
        raise ConfigEntryNotReady("The MQTT integration is not available")

    manager = PanelManager(hass, entry)
    await manager.async_load()
    entry.runtime_data = manager

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    await manager.async_start()
    return True


async def async_unload_entry(hass: HomeAssistant, entry: PanelConfigEntry) -> bool:
    """Unload a panel."""
    await entry.runtime_data.async_stop()
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)


async def async_remove_entry(hass: HomeAssistant, entry: PanelConfigEntry) -> None:
    """Removal: drop storage and the retained topics HA published."""
    manager = PanelManager(hass, entry)
    await manager.async_load()
    try:
        await manager.async_clear_retained()
    except Exception:  # noqa: BLE001 - MQTT may already be gone
        _LOGGER.debug("Could not clear retained topics for %s", entry.title)
    await Store(hass, STORAGE_VERSION, f"{DOMAIN}.{entry.entry_id}").async_remove()
