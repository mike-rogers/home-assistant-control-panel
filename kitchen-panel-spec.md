# Kitchen Panel — ESP32 Home Assistant Controller

Handoff spec for development. Hardware details marked **OPEN** are not decided; ask the owner before writing code that depends on them. Decisions made after the original handoff are recorded in §12.

## 1. Context

The kitchen has a wall display (HDMI LCD + Raspberry Pi kiosk) that will show a passive Home Assistant dashboard: the family Google Calendar, weather, and house state. The display has no input and will not get any.

This project is the **input side**: a small, mains-powered ESP32 device with its own LCD and a rotary/directional control. It triggers about a dozen Home Assistant actions from a shallow menu. It talks to HA only over MQTT and is independent of the display. The device must not require the display to work, and vice versa.

The primary user is the owner, but anyone in the family should be able to use it with no explanation. Avoid hidden chords, long-press-only features, and modes that are invisible on screen.

## 2. Environment and constraints

- **Firmware:** ESP-IDF **v6.0** (C). **Do not use ESPHome or Arduino.** Use ESP-IDF's own components wherever possible: `esp_wifi`, `esp-mqtt`, `nvs_flash`, `cJSON`, `driver/i2c_master`, and the IDF component manager for anything else (LVGL via `esp_lvgl_port`).
- **Home Assistant:** current Home Assistant OS, with the Mosquitto broker app installed and the MQTT integration configured.
- **Network:** 2.4 GHz Wi-Fi on the same LAN as HA.
- **Latency:** the device's own screen must respond to input immediately (target under 50 ms, with no network in the loop). Action acknowledgement from HA should normally arrive well under a second.
- **Power:** mains USB. A battery backup is a possible later addition, so keep power management behind an abstraction. Do not design around deep sleep in v1.
- **Sibling project:** `../taskpad` is the reference implementation for Wi-Fi provisioning (Improv over BLE), adoption (mDNS + local HTTP + HA zeroconf config flow), OTA (HA `update` entity + `esp_https_ota` with rollback), and the ANO seesaw driver. Copy and adapt those modules; extracting a shared component is deferred until a third device exists.

### Hardware

- ESP32 board: **SparkFun Pro Micro ESP32-C3** (4 MB flash, ~400 KB RAM, no PSRAM): the same board as taskpad, so its pin map and RAM tuning (BLE + Wi-Fi + LVGL) carry over.
- LCD: **Lonely Binary 2" or 2.4" TFT**, 240×320, ST7789, SPI (both sizes run the same code). Used in **landscape** (320×240), configurable in menuconfig.
- Input: **Adafruit ANO** directional navigation + scroll wheel on its I2C seesaw adapter (product 5740, STEMMA QT, address 0x49).
- No dedicated Oops button. Undo is an on-screen menu item (§6).

## 3. Architecture

```
[Kitchen Panel] ⇄ MQTT (Mosquitto) ⇄ [HA: kitchen_panel custom integration]
       ▲                                        │
       └── retained menu JSON + entity states ──┘
```

Design principles:

1. **The menu is defined in HA, not in the firmware.** The owner composes it in a dashboard card; the integration publishes it as retained JSON, and the device renders whatever it receives. Adding or reordering actions never requires reflashing.
2. **The firmware is a generic engine.** It renders the tree, edits values, publishes requests, and shows results. It knows nothing about lights, kids, or bedtime.
3. **Input is abstracted.** Hardware drivers emit logical input events. The UI consumes only logical events, so new input devices can be added later without touching UI logic.
4. **Undo is owned by HA.** HA owns the state and has the tools for it (scene snapshots).
5. **The device is a reusable pattern** for future panels (for example a bedside "Good night" panel). The integration supports one config entry per panel; every topic is namespaced by the panel's device ID.

## 4. MQTT contract

`<id>` is the device ID, derived from the MAC (`panel-xxxxxx`) and reported during adoption. All payloads are UTF-8 JSON unless noted.

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `panel/<id>/availability` | device → | yes | `online` / `offline` (plain text; `offline` set as LWT) |
| `panel/<id>/status` | device → | yes | `{ "version": "x.y.z", "rssi": -61 }` (re-published periodically) |
| `panel/<id>/menu` | HA → device | yes | Menu tree (§5) |
| `panel/<id>/request` | device → HA | no | `{ "request_id": "...", "item_id": "...", "op": "run" \| "set" \| "undo", "value": ... }` |
| `panel/<id>/result` | HA → device | no | `{ "request_id": "...", "status": "ok" \| "error", "message": "...", "undo_seconds": <int or null> }` |
| `panel/<id>/state/<entity_id>` | HA → device | yes | `{ "state": "...", "attributes": { ... } }` for entities referenced by the menu |
| `panel/<id>/ota` | HA → device | no | `{ "url": "http://..." }` |

Notes:

- `request_id` is unique per request (a monotonic counter plus a boot ID). The device matches results to requests and times out after a few seconds, showing "No response from Home Assistant".
- **Never queue requests while offline.** A "Good night" that fires minutes late is worse than one that fails visibly. If MQTT is disconnected, show the error and drop the request.
- `undo_seconds` is relative, so the device needs no wall clock for undo. The device starts its countdown when the result arrives.
- The device does **not** publish MQTT discovery. The integration owns the HA device and its entities (connectivity, Wi-Fi RSSI, firmware update, last action), as in taskpad.

## 5. Menu model

The integration publishes the menu retained. The schema is versioned so the firmware can reject a menu it doesn't understand.

```json
{
  "schema": 1,
  "title": "Kitchen",
  "items": [
    {
      "id": "wake",
      "label": "Wake-up",
      "type": "submenu",
      "items": [
        {
          "id": "wake_time",
          "label": "Tomorrow's wake-up",
          "type": "time",
          "entity": "input_datetime.house_wakeup_time",
          "step_minutes": 15
        },
        { "id": "wake_snow_day", "label": "Snow day (sleep in)", "type": "action", "undo": true }
      ]
    },
    { "id": "goodnight", "label": "Good night", "type": "action", "undo": true },
    {
      "id": "automations",
      "label": "Automations",
      "type": "submenu",
      "items": [
        { "id": "brighten_living", "label": "Brighten living room", "type": "action", "undo": true },
        { "id": "summon_kids", "label": "Summon the kids", "type": "action", "undo": false, "confirm": true }
      ]
    }
  ]
}
```

The device-facing JSON is deliberately minimal: what the action *does* (which script, which entities to snapshot) lives only in the integration's storage and never reaches the device.

Item types for v1:

| `type` | Behavior |
|---|---|
| `submenu` | Opens child items. |
| `action` | Sends `op: "run"`. If `confirm` is true, show a confirm screen first. |
| `time` | Shows the entity's current value (from `state/<entity>`). Selecting it enters edit mode: wheel changes the value by `step_minutes`, select commits (`op: "set"`, value `"HH:MM"`), back cancels. |
| `info` | Read-only display of an entity's state (for example "Wake-up: 6:45"). |

Rules:

- `confirm` defaults to `true` whenever `undo` is `false`, so anything that can't be undone gets a confirmation step.
- An unknown `type` is shown greyed out, not a crash. An unknown `schema` version shows an error screen and keeps the last good menu.
- Persist the last valid menu in NVS so the device is usable immediately after boot, before MQTT connects.
- Validate depth (max 3) and item count. A malformed menu must not crash the device.

## 6. Interaction design

### Logical input events

`NAV_NEXT`, `NAV_PREV`, `SELECT`, `BACK`, `HOME`, `UNDO`.

### Default mapping for the ANO wheel

| Hardware | Event |
|---|---|
| Ring clockwise / counter-clockwise | `NAV_NEXT` / `NAV_PREV` (or value up/down in edit mode) |
| Down / Up buttons | `NAV_NEXT` / `NAV_PREV` |
| Center | `SELECT` |
| Left | `BACK` |
| Right | unassigned |

`UNDO` exists as a logical event for a possible future button; no hardware is mapped to it in v1.

### Screens

- **Menu list:** title or breadcrumb, highlighted item, and a value preview where relevant (for example "Tomorrow's wake-up 6:45").
- **Edit (time):** a large value, and a hint line such as "Turn to change · Press to save · ◀ cancel".
- **Confirm:** "Summon the kids?" with the selection on **No** by default, so a stray press does nothing.
- **Result toast:** "Done", or the error message, for about 2 s.
- **Oops item:** after an undoable operation succeeds, a row "↶ Oops: undo <label> (Ns)" is pinned to the bottom of whichever menu list is shown, counting down `undo_seconds`. Selecting it sends `op: "undo"` with the original `item_id`. It disappears when the countdown ends. The cursor never jumps to it automatically. Only the most recent operation is undoable.
- **Status:** a small indicator for Wi-Fi/MQTT/HA connectivity, always visible.

### Behavior

- Return to the top menu after about 30 s of inactivity (the Oops item, if live, stays visible there). Dim the backlight after about 60 s, and wake on any input without acting on that input.
- No feature may be reachable only by long-press or by combinations of buttons.

## 7. Home Assistant side: `custom_components/kitchen_panel`

A custom integration modeled on taskpad's, with these differences: multiple config entries (one per panel), and the menu editor described below.

1. **Discovery and adoption.** The device advertises `_kpanel._tcp` over mDNS. The integration's zeroconf config flow pushes broker credentials to the device's local API, as in taskpad.
2. **Menu storage and editor.** The menu tree for each panel lives in the integration's `.storage`. A dashboard card (`custom:kitchen-panel-card`) edits it: add items using HA's entity pickers (scripts, automations, scenes, `input_datetime`, and any entity for `info`), group them into submenus (max depth 3), reorder them, and set the label, confirm, undo, and step size. Saving publishes the retained menu immediately. Matching services exist for automations and Developer Tools.
3. **State publisher.** The integration tracks every entity referenced by the menu and publishes `panel/<id>/state/<entity_id>` (retained) on change and on startup.
4. **Dispatcher.** Python, subscribed to `panel/<id>/request`:
   - `run`: calls the item's target (`script.turn_on`, `automation.trigger`, `scene.turn_on`) and publishes `result`.
   - For undoable items, before running: capture the current `State` of the affected entities. By default the integration derives that list from what the target references (entities, devices, areas, floors, labels), recursing through nested scripts, automations and scenes, whether they're targeted or called by name. The card can override it per item. Undo window: 30 s by default.
   - `set` on a `time` item: snapshot, then `input_datetime.set_datetime` (or `time.set_value`).
   - `undo`: if the window is open and `item_id` matches the most recent operation, restore the snapshot with `async_reproduce_state`, which is the mechanism `scene.create`/`scene.turn_on` use internally, without leaving `scene.*` entities behind. Otherwise return an error.
5. **Device entities.** Connectivity (binary sensor), Wi-Fi RSSI (diagnostic sensor), Last action (sensor), Firmware (update entity, served the same way as taskpad's).

## 8. Firmware structure

```
main/
  app_main.c        — init, task wiring
  net/              — wifi, improv_ble, adopt_server, mqtt client, ota
  menu/             — JSON parse/validate, NVS persistence, tree model
  ui/               — state machine (pure C, consumes logical events) + LVGL rendering
  input/            — input event queue + drivers (ano_seesaw.c)
  display/          — ST7789 driver + LVGL port
  power/            — backlight / future battery abstraction
test/host/          — IDF linux-target Unity tests for menu/ and the UI state machine
```

- A FreeRTOS queue sits between the input drivers and the UI task.
- The UI state machine (menu model + navigation, minus rendering) is pure C with no IDF dependencies, and is tested on the host: menu parsing, navigation, edit/commit/cancel, confirm defaults, and Oops window logic.
- Configuration follows taskpad: Wi-Fi via Improv BLE (menuconfig values remain an empty-by-default dev fallback), and broker settings pushed during adoption, both stored in NVS. `sdkconfig` is gitignored.
- Wi-Fi and MQTT reconnect with backoff. The UI keeps running and shows offline state.
- Two OTA app slots, with rollback until the first healthy MQTT connect.

## 9. Acceptance criteria

1. A factory-fresh device is provisioned over Improv BLE, discovered in HA, adopted, and renders the menu.
2. Editing the menu in the card updates the device's menu with no reflash and no HA restart.
3. Turning the wheel moves the selection with no perceptible lag, including while HA is unreachable.
4. "Good night" runs, shows "Done", and shows the Oops item for 30 s. Selecting it restores the snapshotted lights.
5. "Summon the kids" asks for confirmation, with No selected by default.
6. Tomorrow's wake-up time can be changed in 15-minute steps. HA reflects the new value, the dashboard shows it, and Oops restores the old value.
7. When Mosquitto is stopped, the device shows offline status, refuses actions with a visible message, and recovers on its own when the broker returns.
8. After a power cycle with HA down, the last known menu is shown from NVS.
9. A firmware update installs from the HA device page.

## 10. Out of scope for v1

- Driving the kitchen wall display (for example navigating its dashboard). This may be added later as a menu action type.
- Battery operation and deep sleep.
- Building more than one panel (the integration supports several, but only one is built).

## 11. Open questions

None at the moment.

## 12. Decision log (2026-10-10)

- Board: SparkFun Pro Micro ESP32-C3 (replacing the MicroMod ESP32). LCD: Lonely Binary 2"/2.4" ST7789 240×320, landscape.
- Input: Adafruit ANO seesaw. No dedicated Oops button; undo is a dynamic menu item at the bottom of the list.
- HA side: custom integration (taskpad pattern) instead of a YAML package. Device-side MQTT discovery is dropped.
- Menu composition: dashboard card editor with HA entity pickers. Undo snapshots are derived automatically from the target's references, with a per-item override.
- `undo_seconds` (relative) replaces `undo_until` (absolute).
- Single-level undo: only the most recent operation.
- Provisioning, adoption, and OTA follow taskpad. Taskpad modules are copied in, not shared, for now.
- Device id defaults to `panel-` + the MAC's last three bytes (menuconfig can override); discovery reports it.
- Requests use QoS 1, enqueued without blocking the UI; the MQTT outbox drops anything older than 4 s, and HA de-duplicates by `request_id`.
- ESP-IDF v6.0. The repo holds firmware, integration, and card together.
- Undo snapshots use `State` capture + `async_reproduce_state` rather than `scene.create` (same mechanism, no stray scene entities).
- The integration targets HA 2026.10+ and uses `probatio` (HA's replacement for `voluptuous` since 2026.9).
