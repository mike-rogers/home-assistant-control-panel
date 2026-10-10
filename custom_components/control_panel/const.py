"""Constants for the Control Panel integration."""

DOMAIN = "control_panel"

CONF_DEVICE_ID = "device_id"
CONF_DEVICE_HOST = "device_host"
CONF_MQTT_URI = "mqtt_uri"
CONF_MQTT_USERNAME = "mqtt_username"
CONF_MQTT_PASSWORD = "mqtt_password"

STORAGE_VERSION = 1

TOPIC_ROOT = "panel"

EVENT_ACTION = "control_panel_action"
SIGNAL_LAST_ACTION = "control_panel_last_action_{}"

SERVICE_SET_MENU = "set_menu"
SERVICE_GET_MENU = "get_menu"
ATTR_CONFIG_ENTRY_ID = "config_entry_id"
ATTR_MENU = "menu"

FIRMWARE_URL = "/control_panel_files/firmware.bin"
CARD_URL = "/control_panel_files/control-panel-card.js"
FIRMWARE_FILE = "control_panel.bin"

VERSION = "0.1.0"  # keep in sync with manifest.json
