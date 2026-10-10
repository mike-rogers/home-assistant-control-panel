"""Run the integration inside a real Home Assistant core (MQTT mocked).

Needs Python 3.14 and pytest-homeassistant-custom-component (see
requirements.txt); the repo root must be on sys.path so
custom_components.kitchen_panel imports.
"""

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).parents[2]))

pytest_plugins = "pytest_homeassistant_custom_component"


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    """Let HA load custom_components/kitchen_panel."""
    return


@pytest.fixture
def expected_lingering_timers() -> bool:
    """The mocked MQTT client leaves its periodic housekeeping timer
    running; HA's own MQTT tests opt out of this check the same way."""
    return True
