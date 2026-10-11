# Control Panel

An ESP32 panel with an LCD and an Adafruit ANO wheel that runs Home
Assistant actions from a menu composed in HA. The firmware is a generic
menu engine; the `control_panel` custom integration owns the menu, runs the
actions, and handles undo ("Oops").

- Requirements and decisions: [`control-panel-spec.md`](control-panel-spec.md)
- Progress: [`MILESTONES.md`](MILESTONES.md)
- License: MIT ([`LICENSE`](LICENSE))

## Layout

| Path | What |
|---|---|
| `components/panel_core/` | Pure-C engine: menu parsing, UI state machine, protocol |
| `custom_components/control_panel/` | Home Assistant integration |
| `test/host/` | Unity tests for `panel_core` |
| `tests/unit/` | Pure-Python tests for the menu model and dispatcher |
| `tests/ha/` | Integration tests inside HA core (MQTT mocked) |
| `tests/card/` | Menu editor card driven in jsdom |

## Hardware

- SparkFun Pro Micro ESP32-C3
- Lonely Binary 2" or 2.4" TFT, 240×320, ST7789, SPI (used in landscape)
- Adafruit ANO directional wheel on its I2C seesaw adapter (product 5740)

Wiring (defaults, changeable in `idf.py menuconfig` → Control Panel
Configuration → Pins). It matches taskpad, which uses the same board:

| LCD pin    | ESP32-C3 GPIO |
|------------|---------------|
| VCC        | 3V3           |
| GND        | GND           |
| SCL / SCK  | 6             |
| SDA / MOSI | 7             |
| CS         | 10            |
| DC         | 4             |
| RES / RST  | 5             |
| BLK        | 3 (PWM-dimmed)|

| ANO adapter (STEMMA QT) | ESP32-C3 GPIO |
|-------------------------|---------------|
| VIN (red)               | 3V3           |
| GND (black)             | GND           |
| SDA (blue)              | 0             |
| SCL (yellow)            | 2             |

If the picture is upside down, enable "Rotate 180 degrees"; if the colors
look like a photo negative, toggle "Invert LCD colors".

## Build, flash, set up

```sh
idf.py build flash monitor
```

1. On first boot the panel asks for Wi-Fi: open the Home Assistant
   companion app, which offers to set up the panel over Bluetooth (Improv).
2. HA then discovers it ("Control Panel (panel-xxxxxx)"). Adopting it asks
   for the MQTT broker details, pushes them to the panel, and the panel
   restarts onto your broker.
3. Add the **Control Panel menu** card (`type: custom:control-panel-card`)
   to a dashboard and compose the menu there; changes reach the panel
   immediately.

To run without a display (views go to the serial log):

```sh
idf.py -B build-log -DSDKCONFIG=build-log/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.log" build flash monitor
```

Firmware updates normally arrive with HACS releases; for local builds see
[`RELEASING.md`](RELEASING.md).

## Tests

```sh
# Firmware core (plain gcc + ASan/UBSan). First time only:
#   (cd test/host && idf.py --preview set-target linux reconfigure)
test/host/run_plain.sh

# Integration, pure modules (Python 3.12+)
pip install -r tests/unit/requirements.txt
(cd tests/unit && pytest)

# Integration inside HA core (Python 3.14)
pip install -r tests/ha/requirements.txt
(cd tests/ha && pytest)

# Menu editor card (Node 22)
(cd tests/card && npm install && npm test)
```

## Installing the integration (HACS)

HACS is the supported install path. In HA: HACS → ⋮ → Custom repositories →
add `https://github.com/mike-rogers/home-assistant-control-panel` as an
**Integration**, install
**Control Panel**, and restart. The integration registers the dashboard
card itself; hard-refresh open browsers once (or reset the Companion app's
frontend cache) or they report `custom element doesn't exist`. Each release bundles the matching panel
firmware, so after a HACS update the panels offer it on their device pages.
See [`RELEASING.md`](RELEASING.md).

## Trying the integration before the hardware exists

Add **Control Panel** by hand under Settings → Devices & Services (device id
e.g. `testpanel`), compose a menu in the card, then act as the device:

```sh
mosquitto_sub -h <ha-ip> -u <user> -P <pass> -v -t 'panel/testpanel/#'
mosquitto_pub -h <ha-ip> -u <user> -P <pass> -t panel/testpanel/request \
  -m '{"request_id":"t1","item_id":"goodnight","op":"run","value":null}'
```
