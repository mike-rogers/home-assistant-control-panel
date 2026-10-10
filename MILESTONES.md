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

## M1 — Integration core (`custom_components/kitchen_panel`) (done, 2026-10-10)

Testable with `mosquitto_pub`/`mosquitto_sub` before any hardware exists.

- [x] Config entry per panel (manual entry by device id for now)
- [x] Menu tree in `.storage`; publish retained `panel/<id>/menu` on
      startup and on change; `kitchen_panel.set_menu` / `get_menu` services
- [x] State publisher for every entity the menu references (stale retained
      topics cleared when items go away)
- [x] Dispatcher: run / set / undo, snapshots derived from the target's
      references (nested scripts/automations/scenes, devices, areas, floors,
      labels), per-item override, 30 s window, single-level undo
- [x] Entities: connectivity, RSSI, last action (+ `kitchen_panel_action`
      bus event)

Note: verified in tests only, not yet on a live HA. `tests/unit` covers the
pure menu model and dispatcher (40 tests, Python 3.12+); `tests/ha` runs
the integration inside HA 2026.10 core with MQTT mocked (12 tests,
Python 3.14). The HA tests caught a real gap: HA's `referenced_entities`
misses scripts called by name (`action: script.porch_off`), so the
snapshot derivation also walks the action config for those.

## M2 — Firmware platform (Pro Micro ESP32-C3 + ANO) (built, 2026-10-10)

- [x] Project skeleton for `esp32c3`, partition table with two OTA slots
- [x] Copied from taskpad: Improv BLE, Wi-Fi, adoption server
      (`_kpanel._tcp`, device id `panel-xxxxxx` from the MAC), OTA with
      rollback
- [x] MQTT client on the §4 contract; menu/state/result into the UI task;
      requests enqueued non-blocking and dropped from the outbox after 4 s
      (never late); HA de-duplicates QoS 1 redeliveries by `request_id`
- [x] Last good menu cached in NVS and shown at boot, before Wi-Fi
- [x] ANO input → logical events, re-probing if the wheel is unplugged
- [x] Serial renderer (`sdkconfig.defaults.log` overlay) for running
      without a display
- [x] Integration: zeroconf discovery + adoption flow, options-flow
      re-adoption, firmware update entity

Note: compiles for esp32c3 and the integration tests pass (19 in HA core);
not yet run on hardware. The ANO driver's init now releases the I2C bus
on every failure path (taskpad's leaked it after the device was added,
which only mattered once retrying was introduced).

## M3 — Display (Lonely Binary ST7789 240×320, landscape) (built, 2026-10-10)

- [x] ST7789 + `esp_lvgl_port` bring-up (taskpad's, known-good on this
      board); landscape via LVGL rotation, 180° and portrait in menuconfig
- [x] LVGL renderer for the view model: header with breadcrumb and
      connectivity, scrolling rows with value previews, amber Oops row,
      big time editor, No/Yes confirm, toast, usage hints in the footer
- [x] Objects created once and updated in place, so a wheel step redraws
      a couple of rows, not the whole frame
- [x] Backlight PWM dimming behind `backlight.h` (the power abstraction)
- [ ] Response-time check against the 50 ms target (on hardware)

Note: the image is 1.69 MB with `-Os` (13% OTA-slot headroom; it was 4% at
the default `-Og`). Static DRAM is 191 KB, on par with taskpad (193 KB),
which runs Wi-Fi + BLE + LVGL on the same board. Rotation direction and
color inversion are per-panel guesses until it's on the bench.

## M4 — Menu editor card (done, 2026-10-10)

- [x] `custom:kitchen-panel-card` (vanilla JS, served and auto-registered
      as a Lovelace resource by the integration): tree view, add/edit via
      domain-filtered entity choices (friendly names, no typed ids),
      submenus, reorder, move between lists, per-item label / undo /
      "are you sure?" / step size
- [x] Shows what Oops will restore (derived live by the integration), with
      "choose these myself" to override
- [x] Every change publishes immediately (acceptance criterion 2);
      validation errors are shown with the form kept open
- [x] Websocket API: panels, menu get/set (set is admin-only), snapshot
      preview

Note: `tests/card` drives the real card in jsdom against a fake backend
(8 tests); `tests/ha` adds the websocket API (25 tests total). Not yet
tried in a real browser against a live HA.

## M4.5 — HACS packaging

- [x] `hacs.json` (zip release), CI: all test suites, firmware build,
      release zip with firmware baked in, `RELEASING.md`
- [ ] GitHub repository (public, so HACS can install it); fill in
      `documentation` / `issue_tracker` in the manifest
- [ ] First tagged release installed through HACS

## M5 — Acceptance pass

- [ ] Walk spec §9 criteria 1–9 on real hardware
