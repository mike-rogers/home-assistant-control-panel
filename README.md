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
