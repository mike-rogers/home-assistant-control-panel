"""One panel: its stored menu, MQTT topics, state publisher and dispatcher."""

from __future__ import annotations

from collections.abc import Callable
from datetime import datetime
from functools import partial
import json
import logging
import time
from typing import Any

from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import (
    Context,
    Event,
    EventStateChangedData,
    HomeAssistant,
    State,
    callback,
)
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.event import async_track_state_change_event
from homeassistant.helpers.state import async_reproduce_state
from homeassistant.helpers.storage import Store
from homeassistant.util import dt as dt_util

from .const import (
    CONF_DEVICE_ID,
    DOMAIN,
    EVENT_ACTION,
    SIGNAL_LAST_ACTION,
    STORAGE_VERSION,
    TOPIC_ROOT,
)
from .dispatcher import Dispatcher, Hooks, RequestError
from .menu_model import MenuError, state_entities, to_device, validate_menu
from .snapshot import derive_snapshot

_LOGGER = logging.getLogger(__name__)

# How each action target domain is started.
_RUN_SERVICES: dict[str, tuple[str, dict[str, Any]]] = {
    "script": ("turn_on", {}),
    "automation": ("trigger", {"skip_condition": True}),
    "scene": ("turn_on", {}),
    "button": ("press", {}),
}
# How each time entity domain is set.
_TIME_SERVICES = {
    "input_datetime": "set_datetime",
    "time": "set_value",
}
# Attributes worth sending to the device (it shows the unit).
_STATE_ATTRIBUTES = ("unit_of_measurement", "friendly_name")


class PanelManager:
    """Everything one panel (one config entry) needs on the HA side."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        self.hass = hass
        self.entry = entry
        self.device_id: str = entry.data[CONF_DEVICE_ID]
        self.topic = f"{TOPIC_ROOT}/{self.device_id}"
        self.menu = validate_menu({"title": entry.title, "items": []})
        self.last_action: dict[str, Any] | None = None

        self._store: Store[dict[str, Any]] = Store(
            hass, STORAGE_VERSION, f"{DOMAIN}.{entry.entry_id}"
        )
        self._unsubs: list[Callable[[], None]] = []
        self._state_unsub: Callable[[], None] | None = None
        self._published_states: set[str] = set()
        self.dispatcher = Dispatcher(
            Hooks(
                get_menu=lambda: self.menu,
                run_target=self._async_run_target,
                set_time=self._async_set_time,
                derive_snapshot=partial(derive_snapshot, hass),
                capture=self._capture,
                restore=self._async_restore,
                on_success=self._on_success,
                clock=time.monotonic,
            )
        )

    # ---- Lifecycle ----

    async def async_load(self) -> None:
        data = await self._store.async_load()
        if not data:
            return
        try:
            self.menu = validate_menu(data.get("menu", {}))
        except MenuError as err:
            # Keep the empty default rather than failing setup; the card
            # can still load and fix it.
            _LOGGER.error("Stored menu for %s is invalid: %s", self.device_id, err)

    async def async_start(self) -> None:
        self._unsubs.append(
            await mqtt.async_subscribe(
                self.hass, f"{self.topic}/request", self._async_on_request, qos=1
            )
        )
        await self._async_publish_menu()
        await self._async_track_states()

    async def async_stop(self) -> None:
        for unsub in self._unsubs:
            unsub()
        self._unsubs.clear()
        if self._state_unsub:
            self._state_unsub()
            self._state_unsub = None

    async def async_clear_retained(self) -> None:
        """On removal: clear every retained topic HA published."""
        await mqtt.async_publish(
            self.hass, f"{self.topic}/menu", "", qos=1, retain=True
        )
        for entity_id in self._published_states | state_entities(self.menu):
            await mqtt.async_publish(
                self.hass, f"{self.topic}/state/{entity_id}", "", qos=1, retain=True
            )

    # ---- Menu ----

    async def async_set_menu(self, data: Any) -> None:
        """Validate, store and publish a new menu; raises MenuError."""
        self.menu = validate_menu(data)
        await self._store.async_save({"menu": self.menu})
        await self._async_publish_menu()
        await self._async_track_states()

    async def _async_publish_menu(self) -> None:
        await mqtt.async_publish(
            self.hass,
            f"{self.topic}/menu",
            json.dumps(to_device(self.menu), separators=(",", ":")),
            qos=1,
            retain=True,
        )

    # ---- Entity states for time/info items ----

    async def _async_track_states(self) -> None:
        wanted = state_entities(self.menu)
        if self._state_unsub:
            self._state_unsub()
            self._state_unsub = None
        if wanted:
            self._state_unsub = async_track_state_change_event(
                self.hass, wanted, self._async_on_state_event
            )
        # Retained topics outlive the menu: clear ones no longer referenced.
        for entity_id in self._published_states - wanted:
            await mqtt.async_publish(
                self.hass, f"{self.topic}/state/{entity_id}", "", qos=1, retain=True
            )
        for entity_id in wanted:
            await self._async_publish_state(
                entity_id, self.hass.states.get(entity_id)
            )
        self._published_states = wanted

    async def _async_on_state_event(
        self, event: Event[EventStateChangedData]
    ) -> None:
        await self._async_publish_state(
            event.data["entity_id"], event.data["new_state"]
        )

    async def _async_publish_state(self, entity_id: str, state: State | None) -> None:
        if state is None:
            payload = ""  # removed entity: the device forgets it
        else:
            payload = json.dumps(
                {
                    "state": state.state,
                    "attributes": {
                        key: state.attributes[key]
                        for key in _STATE_ATTRIBUTES
                        if key in state.attributes
                    },
                },
                separators=(",", ":"),
            )
        await mqtt.async_publish(
            self.hass, f"{self.topic}/state/{entity_id}", payload, qos=1, retain=True
        )

    # ---- Requests ----

    async def _async_on_request(self, msg: mqtt.ReceiveMessage) -> None:
        result = await self.dispatcher.async_handle(msg.payload)
        if result is not None:
            await mqtt.async_publish(
                self.hass,
                f"{self.topic}/result",
                json.dumps(result, separators=(",", ":")),
                qos=1,
                retain=False,
            )

    def _require_entity(self, entity_id: str) -> None:
        if self.hass.states.get(entity_id) is None:
            raise RequestError(f"{entity_id} not found")

    async def _async_run_target(self, target: str) -> None:
        self._require_entity(target)
        domain = target.split(".", 1)[0]
        service, extra = _RUN_SERVICES[domain]
        await self.hass.services.async_call(
            domain,
            service,
            {"entity_id": target, **extra},
            blocking=True,
            context=Context(),
        )

    async def _async_set_time(self, entity_id: str, value: str) -> None:
        self._require_entity(entity_id)
        domain = entity_id.split(".", 1)[0]
        await self.hass.services.async_call(
            domain,
            _TIME_SERVICES[domain],
            {"entity_id": entity_id, "time": value},
            blocking=True,
            context=Context(),
        )

    def _capture(self, entity_ids: list[str]) -> list[State]:
        states = [
            state
            for entity_id in entity_ids
            if (state := self.hass.states.get(entity_id)) is not None
        ]
        _LOGGER.debug(
            "Undo snapshot for %s: %s", self.device_id, [s.entity_id for s in states]
        )
        return states

    async def _async_restore(self, states: list[State]) -> None:
        await async_reproduce_state(self.hass, states, context=Context())

    @callback
    def _on_success(self, item: dict[str, Any], op: str) -> None:
        when: datetime = dt_util.utcnow()
        self.last_action = {
            "item_id": item["id"],
            "label": item["label"],
            "op": op,
            "at": when,
        }
        async_dispatcher_send(self.hass, SIGNAL_LAST_ACTION.format(self.entry.entry_id))
        self.hass.bus.async_fire(
            EVENT_ACTION,
            {
                "device_id": self.device_id,
                "item_id": item["id"],
                "label": item["label"],
                "op": op,
            },
        )
