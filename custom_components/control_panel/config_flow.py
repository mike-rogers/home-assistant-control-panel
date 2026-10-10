"""Config flow: one entry per panel, keyed by its device id.

A factory-fresh panel advertises _ctlpanel._tcp over mDNS; HA offers it as
discovered, and adopting it pushes the broker credentials to the panel
(which reboots onto the broker), as in taskpad. A panel can also be added
by hand (by device id) and adopted later from its options.
"""

from __future__ import annotations

import logging
from typing import Any
from urllib.parse import urlparse

import probatio as vol

from homeassistant.config_entries import (
    ConfigEntry,
    ConfigFlow,
    ConfigFlowResult,
    OptionsFlow,
)
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.network import get_url
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .adoption import async_push_config
from .const import (
    CONF_DEVICE_HOST,
    CONF_DEVICE_ID,
    CONF_MQTT_PASSWORD,
    CONF_MQTT_URI,
    CONF_MQTT_USERNAME,
    DOMAIN,
)

_LOGGER = logging.getLogger(__name__)

DEVICE_ID = vol.Match(r"^[a-z0-9][a-z0-9-]{0,31}$")
DEFAULT_NAME = "Control Panel"


def _guess_mqtt_uri(hass: HomeAssistant) -> str:
    """Best-effort default broker URI: HA's own address, port 1883.

    Prefer the raw IP: the panel cannot resolve .local mDNS names.
    """
    try:
        if ip := getattr(hass.config.api, "local_ip", None):
            return f"mqtt://{ip}:1883"
    except Exception:  # noqa: BLE001
        pass
    try:
        host = urlparse(get_url(hass, allow_external=False)).hostname
        if host and not host.endswith(".local"):
            return f"mqtt://{host}:1883"
    except Exception:  # noqa: BLE001 - a default is never worth failing over
        pass
    return "mqtt://192.168.1.100:1883"


def _broker_schema(hass: HomeAssistant, current: dict[str, Any]) -> vol.Schema:
    return vol.Schema(
        {
            vol.Required(
                CONF_MQTT_URI,
                default=current.get(CONF_MQTT_URI, _guess_mqtt_uri(hass)),
            ): str,
            vol.Required(
                CONF_MQTT_USERNAME, default=current.get(CONF_MQTT_USERNAME, "")
            ): str,
            vol.Required(
                CONF_MQTT_PASSWORD, default=current.get(CONF_MQTT_PASSWORD, "")
            ): str,
        }
    )


async def _async_try_push(
    hass: HomeAssistant, host: str, user_input: dict[str, Any]
) -> bool:
    try:
        await async_push_config(
            hass,
            host,
            user_input[CONF_MQTT_URI],
            user_input[CONF_MQTT_USERNAME],
            user_input[CONF_MQTT_PASSWORD],
        )
    except Exception:  # noqa: BLE001 - surfaced as a form error
        _LOGGER.exception("Adoption push to %s failed", host)
        return False
    return True


class ControlPanelConfigFlow(ConfigFlow, domain=DOMAIN):
    """Add a panel: discovered (adopt) or by hand."""

    VERSION = 1

    def __init__(self) -> None:
        self._host: str | None = None
        self._device_id: str | None = None

    async def async_step_zeroconf(
        self, discovery_info: ZeroconfServiceInfo
    ) -> ConfigFlowResult:
        host = str(discovery_info.ip_address)
        device_id = discovery_info.properties.get("id", "")
        try:
            device_id = DEVICE_ID(device_id)
        except vol.Invalid:
            return self.async_abort(reason="invalid_discovery")

        await self.async_set_unique_id(device_id)
        # Known panel: remember its current address (for re-adoption).
        self._abort_if_unique_id_configured(updates={CONF_DEVICE_HOST: host})

        self._host = host
        self._device_id = device_id
        self.context["title_placeholders"] = {
            "name": f"{DEFAULT_NAME} ({device_id})"
        }
        return await self.async_step_adopt()

    async def async_step_adopt(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Name the panel and push broker credentials to it."""
        errors: dict[str, str] = {}
        if user_input is not None:
            if await _async_try_push(self.hass, self._host, user_input):
                name = user_input.pop("name")
                return self.async_create_entry(
                    title=name,
                    data={
                        CONF_DEVICE_ID: self._device_id,
                        CONF_DEVICE_HOST: self._host,
                        **user_input,
                    },
                )
            errors["base"] = "cannot_connect"

        schema = vol.Schema(
            {vol.Required("name", default=DEFAULT_NAME): str}
        ).extend(_broker_schema(self.hass, user_input or {}).schema)
        return self.async_show_form(
            step_id="adopt",
            data_schema=schema,
            errors=errors,
            description_placeholders={
                "host": self._host or "?",
                "device_id": self._device_id or "?",
            },
        )

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        """Add by device id (the panel shows it while awaiting adoption)."""
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
                vol.Required("name", default=DEFAULT_NAME): str,
                vol.Required(CONF_DEVICE_ID): str,
            }
        )
        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> PanelOptionsFlow:
        return PanelOptionsFlow()


class PanelOptionsFlow(OptionsFlow):
    """(Re-)adopt: push broker credentials, e.g. after rotating them."""

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        current = {**self.config_entry.data, **self.config_entry.options}
        if user_input is not None:
            if await _async_try_push(
                self.hass, user_input[CONF_DEVICE_HOST], user_input
            ):
                return self.async_create_entry(
                    data={**self.config_entry.options, **user_input}
                )
            errors["base"] = "cannot_connect"

        source = user_input or current
        schema = _broker_schema(self.hass, source).extend(
            {
                vol.Required(
                    CONF_DEVICE_HOST, default=source.get(CONF_DEVICE_HOST, "")
                ): str,
            }
        )
        return self.async_show_form(step_id="init", data_schema=schema, errors=errors)
