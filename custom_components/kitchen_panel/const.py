"""Constants for the Kitchen Panel integration."""

DOMAIN = "kitchen_panel"

CONF_DEVICE_ID = "device_id"

STORAGE_VERSION = 1

TOPIC_ROOT = "panel"

EVENT_ACTION = "kitchen_panel_action"
SIGNAL_LAST_ACTION = "kitchen_panel_last_action_{}"

SERVICE_SET_MENU = "set_menu"
SERVICE_GET_MENU = "get_menu"
ATTR_CONFIG_ENTRY_ID = "config_entry_id"
ATTR_MENU = "menu"

VERSION = "0.1.0"  # keep in sync with manifest.json
