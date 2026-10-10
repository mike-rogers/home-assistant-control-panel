"""Websocket commands behind the menu editor card."""

from __future__ import annotations

from typing import Any

import probatio as vol

from homeassistant.components import websocket_api
from homeassistant.config_entries import ConfigEntryState
from homeassistant.core import HomeAssistant, callback

from .const import DOMAIN
from .manager import PanelManager
from .menu_model import MenuError
from .snapshot import derive_snapshot


@callback
def async_register(hass: HomeAssistant) -> None:
    websocket_api.async_register_command(hass, ws_panels)
    websocket_api.async_register_command(hass, ws_get_menu)
    websocket_api.async_register_command(hass, ws_set_menu)
    websocket_api.async_register_command(hass, ws_derive_snapshot)


def _manager(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
) -> PanelManager | None:
    entry = hass.config_entries.async_get_entry(msg["entry_id"])
    if (
        entry is None
        or entry.domain != DOMAIN
        or entry.state is not ConfigEntryState.LOADED
    ):
        connection.send_error(msg["id"], "not_found", "Panel not found or not loaded")
        return None
    return entry.runtime_data


@websocket_api.websocket_command({vol.Required("type"): "kitchen_panel/panels"})
@callback
def ws_panels(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
) -> None:
    """Loaded panels, for the card's panel chooser."""
    connection.send_result(
        msg["id"],
        [
            {
                "entry_id": entry.entry_id,
                "title": entry.title,
                "device_id": entry.runtime_data.device_id,
            }
            for entry in hass.config_entries.async_loaded_entries(DOMAIN)
        ],
    )


@websocket_api.websocket_command(
    {vol.Required("type"): "kitchen_panel/menu/get", vol.Required("entry_id"): str}
)
@callback
def ws_get_menu(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
) -> None:
    if manager := _manager(hass, connection, msg):
        connection.send_result(msg["id"], {"menu": manager.menu})


@websocket_api.websocket_command(
    {
        vol.Required("type"): "kitchen_panel/menu/set",
        vol.Required("entry_id"): str,
        vol.Required("menu"): dict,
    }
)
@websocket_api.require_admin
@websocket_api.async_response
async def ws_set_menu(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
) -> None:
    """Validate, store and publish; the panel updates immediately."""
    if not (manager := _manager(hass, connection, msg)):
        return
    try:
        await manager.async_set_menu(msg["menu"])
    except MenuError as err:
        connection.send_error(msg["id"], "invalid_menu", str(err))
        return
    connection.send_result(msg["id"], {"menu": manager.menu})


@websocket_api.websocket_command(
    {vol.Required("type"): "kitchen_panel/snapshot/derive", vol.Required("target"): str}
)
@callback
def ws_derive_snapshot(
    hass: HomeAssistant, connection: websocket_api.ActiveConnection, msg: dict
) -> None:
    """What Oops would restore for this target, as derived automatically."""
    entities: list[dict[str, Any]] = []
    for entity_id in sorted(derive_snapshot(hass, msg["target"])):
        state = hass.states.get(entity_id)
        name = state.attributes.get("friendly_name", entity_id) if state else entity_id
        entities.append({"entity_id": entity_id, "name": name})
    connection.send_result(msg["id"], {"entities": entities})
