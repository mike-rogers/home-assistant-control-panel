# Releasing Control Panel

HACS is the supported way to install the integration. A release ships the
integration **with the matching firmware baked in**, so updating through
HACS also offers the new firmware on every panel's device page.

## Release (via CI)

1. Bump the versions that changed:
   - firmware: `PANEL_FW_VERSION` in `main/fw_version.h`
   - integration: `version` in `custom_components/control_panel/manifest.json`
     **and** `VERSION` in `custom_components/control_panel/const.py` (they
     must match: the versioned Lovelace resource URL busts card caches)
   - card: `CARD_VERSION` in the card JS (only shown in the browser console)
2. Commit, tag and push:

   ```sh
   git tag vX.Y.Z
   git push origin main --tags
   ```

3. CI (`.github/workflows/ci.yml`) runs all test suites, builds the
   firmware with ESP-IDF v6.0, and attaches to the GitHub release:
   - `control_panel.zip`: the integration plus firmware; HACS installs this
   - `control_panel.bin`: the OTA image
   - `control_panel-factory.bin`: full-flash image for a fresh board
4. In HA: HACS → Control Panel → update, restart, then **Install** on each
   panel's Firmware entity. The panel pulls the image, flashes its idle OTA
   slot and reboots; a boot that never reaches MQTT rolls back on its own.

## Local builds (no release)

1. `idf.py build`
2. `scripts/release_firmware.sh`: stages `control_panel.bin` and
   `version.txt` into `custom_components/control_panel/firmware/`
3. Copy `custom_components/control_panel/` over the HACS-installed copy in
   HA's config directory (plain `cp`; Finder copies fail on the Samba
   share), restart HA, then Install from the panel's device page. The
   firmware entity polls about every 15 minutes; force it with
   `homeassistant.update_entity`.

## Invariants

- Firmware and integration versions are independent. The Firmware entity
  compares the panel's reported version (retained `panel/<id>/status`)
  with `firmware/version.txt`, and offers an update only when the binary
  sits next to it.
- OTA slots are 1984 KB; the `-Os` image is ~1.69 MB (13% headroom).
  Budget flash before adding large features (fonts are the usual culprit).
- Requests never fire late: the outbox drops them after 4 s
  (`CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS`), inside the panel's 5 s
  "No response" window.
