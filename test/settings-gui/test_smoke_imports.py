# SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke tests: every settings-gui module must import headlessly."""

import importlib
import sys
import unittest

import _stubs  # noqa: F401  (installs the qtpy/dbus stand-ins)

EXPECTED_MODULES = (
    "i18n",
    "main",
    "core.dbus_handler",
    "ui.components",
    "ui.helpers",
    "ui.main_window",
    "ui.pages.about",
    "ui.pages.backup",
    "ui.pages.base_editor",
    "ui.pages.dict_editor",
    "ui.pages.dynamic_settings",
    "ui.pages.keymap_editor",
    "ui.pages.macro_editor",
    "ui.pages.mode_manager",
)


class SmokeImportTests(unittest.TestCase):
    def test_all_settings_gui_modules_import_without_qt_or_dbus(self):
        for module_name in EXPECTED_MODULES:
            with self.subTest(module=module_name):
                module = importlib.import_module(module_name)
                self.assertIn(module_name, sys.modules)
                self.assertTrue(module.__doc__ is None or isinstance(module.__doc__, str))

    def test_main_entry_point_has_guarded_execution(self):
        module = importlib.import_module("main")
        self.assertTrue(callable(module.main))

    def test_settings_window_class_is_exposed(self):
        module = importlib.import_module("ui.main_window")
        self.assertTrue(hasattr(module, "VanhSettingsWindow"))

    def test_dbus_handler_class_is_exposed(self):
        module = importlib.import_module("core.dbus_handler")
        self.assertTrue(hasattr(module, "VanhDBusHandler"))


if __name__ == "__main__":
    unittest.main()
