"""The Kitchen Panel integration: HA side of an MQTT menu panel (spec §7)."""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

import probatio as vol

from homeassistant.components import mqtt
from homeassistant.components.http import StaticPathConfig
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
    CARD_URL,
    DOMAIN,
    FIRMWARE_FILE,
    FIRMWARE_URL,
    SERVICE_GET_MENU,
    SERVICE_SET_MENU,
    STORAGE_VERSION,
    VERSION,
)
from . import websocket
from .manager import PanelManager
from .menu_model import MenuError

_LOGGER = logging.getLogger(__name__)

PLATFORMS = [Platform.BINARY_SENSOR, Platform.SENSOR, Platform.UPDATE]
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

    websocket.async_register(hass)
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

    # One firmware image for every panel; each panel's update entity points
    # its device here.
    await hass.http.async_register_static_paths(
        [
            StaticPathConfig(
                FIRMWARE_URL,
                str(Path(__file__).parent / "firmware" / FIRMWARE_FILE),
                cache_headers=False,
            ),
            StaticPathConfig(
                CARD_URL,
                str(Path(__file__).parent / "frontend" / "kitchen-panel-card.js"),
                cache_headers=False,
            ),
        ]
    )
    await _async_ensure_lovelace_resource(hass)
    return True


async def _async_ensure_lovelace_resource(hass: HomeAssistant) -> None:
    """Register the card in the Lovelace resource registry (as taskpad).

    Resources are awaited by the frontend before cards render; a script
    injected with add_extra_js_url is not, and a custom card element that
    registers after first render never recovers.
    """
    versioned_url = f"{CARD_URL}?v={VERSION}"
    try:
        resources = getattr(hass.data.get("lovelace"), "resources", None)
        if resources is None:
            _LOGGER.warning(
                "Lovelace resources unavailable (YAML-mode dashboards?); "
                "add %s as a module resource manually",
                versioned_url,
            )
            return
        if hasattr(resources, "loaded") and not resources.loaded:
            await resources.async_load()
        existing = next(
            (
                item
                for item in resources.async_items()
                if str(item.get("url", "")).split("?")[0] == CARD_URL
            ),
            None,
        )
        if existing is None:
            await resources.async_create_item(
                {"res_type": "module", "url": versioned_url}
            )
        elif existing["url"] != versioned_url:
            await resources.async_update_item(
                existing["id"], {"res_type": "module", "url": versioned_url}
            )
    except Exception:  # noqa: BLE001 - card plumbing must never block setup
        _LOGGER.exception(
            "Could not register the Lovelace resource; add %s as a module "
            "resource manually (Settings > Dashboards > Resources)",
            versioned_url,
        )


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
