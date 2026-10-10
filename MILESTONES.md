# Kitchen Panel Milestones

Each milestone leaves the whole system working end-to-end before the next
begins. Requirements and the decision log live in
[`kitchen-panel-spec.md`](kitchen-panel-spec.md).

## M0 — Core engine, host-tested (done, 2026-10-10)

`components/panel_core/`: pure C, depends only on cJSON, so it builds for
the device and for the IDF `linux` target alike.

- [x] Menu model: parse/validate schema 1 (depth ≤ 3, item and per-list
      limits, unknown types kept greyed, confirm defaults to !undo)
- [x] Entity state store fed by `state/<entity_id>` payloads
- [x] Protocol: request serialization, result parsing (`undo_seconds`)
- [x] UI state machine: menu navigation, time edit/commit/cancel with
      step snapping, confirm (No by default), pending request + timeout,
      toasts, Oops item lifecycle, inactivity → home, dim/wake swallowing
      the first input, offline refusal, menu replacement preserving position
- [x] View model the renderer draws from (no LVGL in the state machine)
- [x] `test/host`: 36 Unity tests covering all of the above

Note: two ways to run the tests. `test/host/run_plain.sh` builds with
plain gcc under ASan/UBSan (fast; needs `idf.py reconfigure` in
`test/host` once to fetch cJSON). The IDF linux target
(`cd test/host && idf.py --preview set-target linux build && ./build/panel_core_host_test.elf`)
additionally needs the `libbsd-dev` system package.

## M1 — Integration core (`custom_components/kitchen_panel`)

Testable with `mosquitto_pub`/`mosquitto_sub` before any hardware exists.

- [ ] Config entry per panel (manual entry by device id for now)
- [ ] Menu tree in `.storage`; publish retained `panel/<id>/menu` on
      startup and on change; editing services
- [ ] State publisher for every entity the menu references
- [ ] Dispatcher: run / set / undo, `scene.create` snapshots derived from
      the target's referenced entities (nested scripts, devices, areas),
      per-item override, 30 s window, single-level undo
- [ ] Entities: connectivity, RSSI, last action

## M2 — Firmware platform (MicroMod ESP32 + ANO, serial-only UI)

- [ ] Project skeleton for `esp32`, partition table with two OTA slots
- [ ] Copied from taskpad: Improv BLE, Wi-Fi, adoption server
      (`_kpanel._tcp`), OTA with rollback
- [ ] MQTT client on the §4 contract; state/menu/result into the UI task
- [ ] Last good menu cached in NVS
- [ ] ANO input → logical events
- [ ] Renderer stub that logs the view model, so M0's engine runs on the
      device before the LCD is chosen
- [ ] Integration: zeroconf adoption flow + firmware update entity

## M3 — Display (blocked: LCD not chosen)

- [ ] LCD driver + `esp_lvgl_port`; LVGL renderer for the view model
- [ ] Backlight PWM dimming behind `power/`
- [ ] Response-time check against the 50 ms target

## M4 — Menu editor card

- [ ] `custom:kitchen-panel-card`: tree view, add via entity pickers,
      submenus, reorder, per-item label/confirm/undo/step/snapshot override
- [ ] Saves publish immediately (acceptance criterion 2)

## M5 — Acceptance pass

- [ ] Walk spec §9 criteria 1–9 on real hardware
