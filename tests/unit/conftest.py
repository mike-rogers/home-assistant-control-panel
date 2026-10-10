"""Expose the integration's pure modules without importing Home Assistant.

custom_components/kitchen_panel/__init__.py imports homeassistant; the
modules under test (menu_model, dispatcher) only need probatio. Registering
a bare package object lets their relative imports resolve while skipping
the package __init__.
"""

import pathlib
import sys
import types

PKG_DIR = pathlib.Path(__file__).parents[2] / "custom_components" / "kitchen_panel"

pkg = types.ModuleType("kitchen_panel")
pkg.__path__ = [str(PKG_DIR)]
sys.modules.setdefault("kitchen_panel", pkg)
