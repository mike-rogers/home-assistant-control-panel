# Kitchen Panel

An ESP32 panel with an LCD and an Adafruit ANO wheel that runs Home
Assistant actions from a menu composed in HA. The firmware is a generic
menu engine; the `kitchen_panel` custom integration owns the menu, runs the
actions, and handles undo ("Oops").

- Requirements and decisions: [`kitchen-panel-spec.md`](kitchen-panel-spec.md)
- Progress: [`MILESTONES.md`](MILESTONES.md)

## Layout

| Path | What |
|---|---|
| `components/panel_core/` | Pure-C engine: menu parsing, UI state machine, protocol |
| `custom_components/kitchen_panel/` | Home Assistant integration |
| `test/host/` | Unity tests for `panel_core` |
| `tests/unit/` | Pure-Python tests for the menu model and dispatcher |
| `tests/ha/` | Integration tests inside HA core (MQTT mocked) |
| `tests/card/` | Menu editor card driven in jsdom |

## Hardware

- SparkFun Pro Micro ESP32-C3
- Lonely Binary 2" or 2.4" TFT, 240×320, ST7789, SPI (used in landscape)
- Adafruit ANO directional wheel on its I2C seesaw adapter (product 5740)

Wiring (defaults, changeable in `idf.py menuconfig` → Kitchen Panel
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
2. HA then discovers it ("Kitchen Panel (panel-xxxxxx)"). Adopting it asks
   for the MQTT broker details, pushes them to the panel, and the panel
   restarts onto your broker.
3. Add the **Kitchen Panel menu** card (`type: custom:kitchen-panel-card`)
   to a dashboard and compose the menu there; changes reach the panel
   immediately.

To run without a display (views go to the serial log):

```sh
idf.py -B build-log -DSDKCONFIG=build-log/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.log" build flash monitor
```

Firmware updates: bump `PANEL_FW_VERSION` in `main/fw_version.h`,
`idf.py build`, `scripts/release_firmware.sh`, copy
`custom_components/kitchen_panel/` to HA, then Install from the panel's
device page.

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

## Trying the integration before the hardware exists

Copy `custom_components/kitchen_panel/` into HA's config directory, restart,
and add **Kitchen Panel** under Settings → Devices & Services (device id
`kitchenpanel`). Set a menu from Developer Tools → Actions with
`kitchen_panel.set_menu`, then act as the device:

```sh
mosquitto_sub -h <ha-ip> -u <user> -P <pass> -v -t 'panel/kitchenpanel/#'
mosquitto_pub -h <ha-ip> -u <user> -P <pass> -t panel/kitchenpanel/request \
  -m '{"request_id":"t1","item_id":"goodnight","op":"run","value":null}'
```
