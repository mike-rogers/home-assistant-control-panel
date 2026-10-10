"""Which entities an action affects, for its undo snapshot.

Follows what HA itself knows a target references: a script's or
automation's entities, devices, areas, floors and labels (expanded to
entities), recursing through scripts/automations/scenes it calls, whether
targeted (script.turn_on) or called by name (action: script.porch_off). Only
entities whose state can meaningfully be restored are kept. Templated
targets are invisible here; the card's per-item snapshot list covers those.
"""

from __future__ import annotations

from collections.abc import Callable

from homeassistant.components.automation import (
    areas_in_automation,
    devices_in_automation,
    entities_in_automation,
    floors_in_automation,
    labels_in_automation,
)
from homeassistant.components.homeassistant.scene import entities_in_scene
from homeassistant.components.script import (
    areas_in_script,
    devices_in_script,
    entities_in_script,
    floors_in_script,
    labels_in_script,
)
from homeassistant.const import (
    ATTR_AREA_ID,
    ATTR_DEVICE_ID,
    ATTR_FLOOR_ID,
    ATTR_LABEL_ID,
)
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.target import (
    TargetSelection,
    async_extract_referenced_entity_ids,
)

# Domains whose state reproduces sensibly (what scene snapshots handle).
RESTORABLE_DOMAINS = frozenset(
    {
        "climate",
        "cover",
        "fan",
        "humidifier",
        "input_boolean",
        "input_datetime",
        "input_number",
        "input_select",
        "input_text",
        "light",
        "lock",
        "media_player",
        "number",
        "select",
        "siren",
        "switch",
        "valve",
        "water_heater",
    }
)

_CONTAINERS: dict[str, tuple[Callable[[HomeAssistant, str], list[str]], ...]] = {
    "script": (
        entities_in_script,
        devices_in_script,
        areas_in_script,
        floors_in_script,
        labels_in_script,
    ),
    "automation": (
        entities_in_automation,
        devices_in_automation,
        areas_in_automation,
        floors_in_automation,
        labels_in_automation,
    ),
}

MAX_NESTING = 8

# script.* services that act on other scripts rather than being one.
_SCRIPT_SERVICES = frozenset({"turn_on", "turn_off", "toggle", "reload"})
# Where each container keeps its actions in raw_config.
_ACTION_KEYS = {"script": ("sequence",), "automation": ("actions", "action")}


def _called_scripts(hass: HomeAssistant, entity_id: str) -> set[str]:
    """Scripts called by service name ("action: script.porch_off").

    HA's referenced_entities only sees entity targets, so a script run
    directly as a service would otherwise drop out of the snapshot.
    """
    domain = entity_id.split(".", 1)[0]
    component = hass.data.get(domain)
    entity = component.get_entity(entity_id) if component else None
    raw = getattr(entity, "raw_config", None) or {}
    found: set[str] = set()

    def walk(node: object) -> None:
        if isinstance(node, dict):
            for key in ("action", "service"):
                value = node.get(key)
                if isinstance(value, str) and value.startswith("script."):
                    name = value.split(".", 1)[1]
                    if name not in _SCRIPT_SERVICES:
                        found.add(value)
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    for key in _ACTION_KEYS[domain]:
        walk(raw.get(key))
    return found


@callback
def derive_snapshot(hass: HomeAssistant, target: str) -> set[str]:
    """Restorable entities the target may change."""
    found: set[str] = set()
    visited: set[str] = set()

    def visit(entity_id: str, depth: int) -> None:
        if entity_id in visited or depth > MAX_NESTING:
            return
        visited.add(entity_id)
        domain = entity_id.split(".", 1)[0]

        if domain == "scene":
            referenced = set(entities_in_scene(hass, entity_id))
        elif (helpers := _CONTAINERS.get(domain)) is not None:
            entities, devices, areas, floors, labels = (
                helper(hass, entity_id) for helper in helpers
            )
            selected = async_extract_referenced_entity_ids(
                hass,
                TargetSelection(
                    {
                        ATTR_DEVICE_ID: devices,
                        ATTR_AREA_ID: areas,
                        ATTR_FLOOR_ID: floors,
                        ATTR_LABEL_ID: labels,
                    }
                ),
            )
            referenced = (
                set(entities)
                | selected.indirectly_referenced
                | _called_scripts(hass, entity_id)
            )
        else:
            return  # e.g. a button: what it changes is unknowable

        for ref in referenced:
            if ref.split(".", 1)[0] in ("scene", "script", "automation"):
                visit(ref, depth + 1)
            elif ref.split(".", 1)[0] in RESTORABLE_DOMAINS:
                found.add(ref)

    visit(target, 0)
    return found
