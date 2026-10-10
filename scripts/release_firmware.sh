#!/usr/bin/env bash
# Stage the built firmware for OTA distribution by the HA integration.
# Run after `idf.py build`; then deploy custom_components/control_panel/ to
# HA. Every panel's Firmware entity then offers the update.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/#define PANEL_FW_VERSION "\(.*\)"/\1/p' main/fw_version.h)
if [ -z "$VERSION" ]; then
    echo "could not read PANEL_FW_VERSION from main/fw_version.h" >&2
    exit 1
fi
if [ ! -f build/control_panel.bin ]; then
    echo "build/control_panel.bin missing - run idf.py build first" >&2
    exit 1
fi

mkdir -p custom_components/control_panel/firmware
cp build/control_panel.bin custom_components/control_panel/firmware/control_panel.bin
printf '%s' "$VERSION" > custom_components/control_panel/firmware/version.txt
echo "staged firmware $VERSION ($(wc -c < build/control_panel.bin | tr -d ' ') bytes)"
