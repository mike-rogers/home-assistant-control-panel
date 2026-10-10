"""Config flow: one entry per panel, keyed by its device id.

M1 adds panels by hand; zeroconf discovery and adoption (as in taskpad)
arrive with the firmware platform in M2.
"""

from __future__ import annotations

from typing import Any

import probatio as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult

from .const import CONF_DEVICE_ID, DOMAIN

DEVICE_ID = vol.Match(r"^[a-z0-9][a-z0-9-]{0,31}$")


class KitchenPanelConfigFlow(ConfigFlow, domain=DOMAIN):
    """Add a panel."""

    VERSION = 1

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                device_id = DEVICE_ID(user_input[CONF_DEVICE_ID].strip().lower())
            except vol.Invalid:
                errors[CONF_DEVICE_ID] = "invalid_device_id"
            else:
                await self.async_set_unique_id(device_id)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(
                    title=user_input["name"], data={CONF_DEVICE_ID: device_id}
                )

        schema = vol.Schema(
            {
                vol.Required("name", default="Kitchen Panel"): str,
                vol.Required(CONF_DEVICE_ID, default="kitchenpanel"): str,
            }
        )
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)
