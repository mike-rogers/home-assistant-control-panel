"""Pushing broker configuration to a panel over its local API."""

from __future__ import annotations

import asyncio

from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession


async def async_push_config(
    hass: HomeAssistant,
    host: str,
    mqtt_uri: str,
    mqtt_username: str,
    mqtt_password: str,
    port: int = 80,
) -> None:
    """POST broker credentials to the panel; it persists them and reboots.

    Raises on any failure so config flows can surface cannot_connect.
    """
    session = async_get_clientsession(hass)
    payload = {
        "mqtt_uri": mqtt_uri,
        "mqtt_username": mqtt_username,
        "mqtt_password": mqtt_password,
    }
    async with asyncio.timeout(10):
        response = await session.post(
            f"http://{host}:{port}/api/config", json=payload
        )
        response.raise_for_status()
