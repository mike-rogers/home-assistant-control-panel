# Kitchen Panel — ESP32 Home Assistant Controller

Handoff spec for development. The hardware details marked **OPEN** are not decided. Ask the owner about them before writing code that depends on them.

## 1. Context

The kitchen has a wall display (HDMI LCD + Raspberry Pi kiosk) that will show a passive Home Assistant dashboard: the family Google Calendar, weather, and house state. The display has no input and will not get any.

This project is the **input side**: a small, mains-powered ESP32 device with its own LCD and a rotary/directional control. It triggers about a dozen Home Assistant actions from a shallow menu. It talks to HA only over MQTT and is independent of the display. The device must not require the display to work, and vice versa.

The primary user is the owner, but anyone in the family should be able to use it with no explanation. Avoid hidden chords, long-press-only features, and modes that are invisible on screen.

## 2. Environment and constraints

- **Firmware:** ESP-IDF (C). The owner strongly prefers ESP-IDF. **Do not use ESPHome or Arduino.** Use ESP-IDF's own components wherever possible: `esp_wifi`, `esp-mqtt`, `nvs_flash`, `cJSON`, `driver/i2c`, and the IDF component manager for anything else (for example LVGL via `esp_lvgl_port`).
- **Home Assistant:** Home Assistant OS, with the Mosquitto broker add-on already installed. The MQTT integration is assumed to be configured.
- **Network:** 2.4 GHz Wi-Fi on the same LAN as HA.
- **Latency:** the device's own screen must respond to input immediately (target under 50 ms, with no network in the loop). Action acknowledgement from HA should normally arrive well under a second.
- **Power:** mains USB. A battery backup is a possible later addition, so keep power management behind an abstraction. Do not design around deep sleep in v1.

### Hardware — OPEN

- ESP32 variant/board: **OPEN.** The owner has many unprovisioned ESP32 boards. Ask which one. Prefer one with PSRAM if LVGL is used.
- LCD: **OPEN** (model, controller IC, resolution, interface).
- Input: an Adafruit ANO directional navigation + scroll wheel encoder on an I2C seesaw breakout (STEMMA QT / Qwiic). It has a rotary ring, a center button, and four directional buttons. A plain I2C rotary encoder is also on hand. Confirm which one is used and its I2C address.
- Dedicated "Oops" button: **OPEN.** Either a separate, clearly labeled physical button (preferred for discoverability) or one of the ANO direction buttons.

## 3. Architecture

```
[Kitchen Panel] ⇄ MQTT (Mosquitto) ⇄ [HA: dispatcher script + automations]
       ▲                                        │
       └── retained menu JSON + entity states ──┘
```

Design principles:

1. **The menu is defined in HA, not in the firmware.** HA publishes the menu tree as retained JSON, and the device renders whatever it receives. Adding or reordering actions should never require reflashing.
2. **The firmware is a generic engine.** It renders the tree, edits values, publishes requests, and shows results. It knows nothing about lights, kids, or bedtime.
3. **Input is abstracted.** Hardware drivers emit logical input events. The UI consumes only logical events, so new input devices (another encoder, buttons, a remote ESP32) can be added later without touching UI logic.
4. **Undo is owned by HA.** HA owns the state and has the tools for it, such as scene snapshots.
5. **The device should become a reusable pattern** for future panels (for example a bedside "Good night" panel). Make the device ID configurable and namespace all topics by it.

## 4. MQTT contract

`<id>` is the device ID, configurable, defaulting to `kitchenpanel`. All payloads are UTF-8 JSON unless noted.

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `panel/<id>/availability` | device → | yes | `online` / `offline` (plain text; `offline` set as LWT) |
| `panel/<id>/menu` | HA → device | yes | Menu tree (§5) |
| `panel/<id>/request` | device → HA | no | `{ "request_id": "...", "item_id": "...", "op": "run" \| "set" \| "undo", "value": ... }` |
| `panel/<id>/result` | HA → device | no | `{ "request_id": "...", "status": "ok" \| "error", "message": "...", "undo_until": <unix seconds or null> }` |
| `panel/<id>/state/<entity_id>` | HA → device | yes | `{ "state": "...", "attributes": { ... } }` for entities referenced by the menu |
| `homeassistant/status` | HA → device | — | HA birth message; on `online`, re-publish discovery |

Notes:

- `request_id` is unique per request (for example a monotonic counter plus boot ID). The device matches results to requests and times out after a few seconds, showing "No response from Home Assistant".
- **Never queue requests while offline.** A "Good night" that fires minutes late is worse than one that fails visibly. If MQTT is disconnected, show the error and drop the request.
- The device subscribes to `panel/<id>/state/#`. HA publishes states for the menu's referenced entities (§7). This is how the device shows the current wakeup time, for example.

### Home Assistant MQTT discovery

The device appears in HA as a proper device with no YAML required. It uses MQTT discovery: `homeassistant/<component>/<id>/<object>/config`, or the device-based discovery topic if the HA version supports it. Expose at least:

- Wi-Fi RSSI (diagnostic sensor)
- Firmware version (diagnostic sensor)
- Last action (sensor: the `item_id` of the last successful request)

Re-publish discovery on connect and whenever `homeassistant/status` becomes `online`.

## 5. Menu model

The menu is published retained by HA. The schema is versioned so the firmware can reject a menu it doesn't understand.

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

### Default mapping for the ANO wheel (configurable)

| Hardware | Event |
|---|---|
| Ring clockwise / counter-clockwise | `NAV_NEXT` / `NAV_PREV` (or value up/down in edit mode) |
| Center | `SELECT` |
| Left | `BACK` |
| Up | `HOME` |
| Dedicated Oops button (or ANO right, if no dedicated button) | `UNDO` |

### Screens

- **Menu list:** title or breadcrumb, highlighted item, and a value preview where relevant (for example "Tomorrow's wake-up 6:45").
- **Edit (time):** a large value, and a hint line such as "Turn to change · Press to save · ◀ cancel".
- **Confirm:** "Summon the kids?" with the selection on **No** by default, so a stray press does nothing.
- **Result toast:** "Done", or the error message, for about 2 s.
- **Undo banner:** after an undoable action, show "Oops? (Ns)" with a countdown until `undo_until`. Pressing `UNDO` sends `op: "undo"` with the original `item_id`. Outside the window, `UNDO` shows "Nothing to undo".
- **Status:** a small indicator for Wi-Fi/MQTT/HA connectivity, always visible.

### Behavior

- Return to the top menu after about 30 s of inactivity. Dim the backlight after about 60 s, and wake on any input without acting on that input.
- No feature may be reachable only by long-press or by combinations of buttons.

## 7. Home Assistant side (also deliver this)

Deliver this as a YAML package (`packages/kitchen_panel.yaml`) the owner can drop in. It contains:

1. **Menu publisher.** A script plus an automation that publish the menu JSON, retained, to `panel/kitchenpanel/menu`. It runs on HA start and can be run manually. Keep the menu definition in one obvious place in the YAML.
2. **State publisher.** An automation that publishes the current state of each entity referenced by the menu to `panel/kitchenpanel/state/<entity_id>` (retained). It runs on change and on HA start. HA's built-in `mqtt_statestream` is an alternative; use whichever is cleaner, but keep the topic contract in §4.
3. **Dispatcher.** An automation triggered by MQTT on `panel/kitchenpanel/request` that maps `item_id` to an HA script or service call and publishes a `result`.
   - For `undo: true` items: before running the action, call `scene.create` with `snapshot_entities` for the entities that action affects. Use a per-item undo scene, and record the deadline (now + 30 s).
   - `op: "undo"`: if the window is open, run `scene.turn_on` on the snapshot, then publish the result. Otherwise return an error.
   - `op: "set"` on `wake_time`: call `input_datetime.set_datetime`. Snapshot the previous value so that undo restores it.
4. **Action scripts.** Thin wrappers that call the owner's existing scripts and automations ("Good night", "Brighten living room", "Summon the kids"). Leave clearly marked placeholders where the owner must fill in real entity or script IDs, and do not invent entity IDs as if they were real. Known: `input_datetime.house_wakeup_time` holds the house wake-up time. Confirm its exact entity ID with the owner.

## 8. Firmware structure (suggested)

```
main/
  app_main.c        — init, task wiring
  net/              — wifi, mqtt client, discovery, reconnect/backoff
  menu/             — JSON parse/validate, NVS persistence, tree model
  ui/               — screens and state machine (consumes logical events only)
  input/            — input event queue + drivers (ano_seesaw.c, encoder.c, button.c)
  display/          — LCD driver + LVGL port (OPEN: depends on panel)
  power/            — backlight / future battery abstraction
```

- Use a FreeRTOS queue between input drivers and the UI task.
- The UI state machine (`menu/` + `ui/` logic, minus rendering) should be testable on the host using the IDF `linux` target or plain Unity tests. Cover menu parsing, navigation, edit/commit/cancel, confirm defaults, and undo-window logic.
- Configuration: Wi-Fi SSID/password, MQTT URI/user/password, and device ID come from `menuconfig` defaults, are stored in NVS, and are never committed to the repo. Include an `sdkconfig.defaults` and a `.gitignore` covering secrets.
- Wi-Fi and MQTT reconnect with backoff. The UI keeps running and shows offline state.

## 9. Acceptance criteria

1. On first boot with credentials, the device connects, appears in HA under MQTT devices, and renders the menu published by HA.
2. Editing the menu YAML in HA and re-running the publisher updates the device's menu with no reflash.
3. Turning the wheel moves the selection with no perceptible lag, including while HA is unreachable.
4. "Good night" runs, shows "Done", and offers a 30 s Oops window. Oops restores the snapshotted lights.
5. "Summon the kids" asks for confirmation, with No selected by default.
6. Tomorrow's wake-up time can be changed in 15-minute steps. HA reflects the new value, the dashboard shows it, and Oops restores the old value.
7. When Mosquitto is stopped, the device shows offline status, refuses actions with a visible message, and recovers on its own when the broker returns.
8. After a power cycle with HA down, the last known menu is shown from NVS.

## 10. Out of scope for v1

- Driving the kitchen wall display (for example navigating its dashboard). This may be added later as a menu action type.
- Battery operation and deep sleep.
- OTA updates (nice to have; ask before adding).
- Multiple panels. Keep the design ready for them (namespaced topics, configurable ID) but build one.

## 11. Questions to ask the owner before starting

1. Which ESP32 board and which LCD (model / controller / interface)?
2. Which encoder hardware will be used, and will there be a dedicated Oops button?
3. Exact entity and script IDs for Wake-up, Good night, Brighten living room, Summon the kids, and any other actions for the initial menu.
4. Should Wi-Fi/MQTT credentials be compile-time (`menuconfig`) only, or also settable on-device?
