# SPDX-FileCopyrightText: 2026 Cong Nguyen <congmnguyen@users.noreply.github.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for core.gnome_shortcut."""

import os
import tempfile
import unittest

import _stubs  # noqa: F401
from core.gnome_shortcut import (
    GNOME_CUSTOM_SCHEMA,
    GNOME_MEDIA_KEYS_SCHEMA,
    GNOME_MUTTER_SCHEMA,
    GNOME_TOGGLE_KEY_PATH,
    GNOME_WM_KEYBINDINGS_SCHEMA,
    configure_gnome_super_space,
    format_gsettings_str_list,
    is_gnome_session,
    is_gnome_super_space_configured,
    parse_gsettings_str_list,
    strip_super_space_from_fcitx5_config,
)


class GnomeShortcutHelperTests(unittest.TestCase):
    def test_is_gnome_session(self):
        self.assertTrue(is_gnome_session({"XDG_CURRENT_DESKTOP": "ubuntu:GNOME"}))
        self.assertTrue(is_gnome_session({"XDG_CURRENT_DESKTOP": "GNOME"}))
        self.assertFalse(is_gnome_session({"XDG_CURRENT_DESKTOP": "KDE"}))
        self.assertFalse(is_gnome_session({}))

    def test_parse_and_format_gsettings_str_list(self):
        self.assertEqual(parse_gsettings_str_list("@as []\n"), [])
        self.assertEqual(parse_gsettings_str_list("[]"), [])
        parsed = parse_gsettings_str_list("['/a/', '/b/']\n")
        self.assertEqual(parsed, ["/a/", "/b/"])
        self.assertEqual(format_gsettings_str_list([]), "[]")
        self.assertEqual(format_gsettings_str_list(["/a/", "/b/"]), "['/a/', '/b/']")

    def test_strip_super_space_only_from_trigger_sections(self):
        sample = (
            "[Hotkey/TriggerKeys]\n"
            "0=Super+space\n"
            "\n"
            "[Hotkey/EnumerateGroupForwardKeys]\n"
            "0=Super+space\n"
            "\n"
            "[Hotkey/EnumerateGroupBackwardKeys]\n"
            "0=Shift+Super+space\n"
        )
        updated, changed = strip_super_space_from_fcitx5_config(sample)
        self.assertTrue(changed)
        self.assertNotIn("0=Super+space", updated)
        self.assertIn("0=Shift+Super+space", updated)
        again, changed_again = strip_super_space_from_fcitx5_config(updated)
        self.assertFalse(changed_again)
        self.assertEqual(again, updated)

    def test_configure_and_detect_round_trip(self):
        store = {
            (GNOME_WM_KEYBINDINGS_SCHEMA, "switch-input-source"): "['<Super>space']",
            (
                GNOME_WM_KEYBINDINGS_SCHEMA,
                "switch-input-source-backward",
            ): "['<Shift><Super>space']",
            (
                GNOME_MEDIA_KEYS_SCHEMA,
                "custom-keybindings",
            ): "['/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/obs-record/']",
            (GNOME_MUTTER_SCHEMA, "overlay-key"): "''",
        }
        reloaded = []

        def fake_run(args):
            if args[0].endswith("fcitx5-remote") and args[1:] == ["-r"]:
                reloaded.append(args)
                return 0, "", ""
            if args[:2] == ["gsettings", "get"]:
                val = store.get((args[2], args[3]))
                return (0, val, "") if val is not None else (1, "", "missing")
            if args[:2] == ["gsettings", "set"]:
                store[(args[2], args[3])] = args[4]
                return 0, "", ""
            if args[:2] == ["gsettings", "reset"]:
                store[(args[2], args[3])] = "'Super'"
                return 0, "", ""
            return 1, "", "unexpected"

        self.assertFalse(is_gnome_super_space_configured(run=fake_run))

        with tempfile.TemporaryDirectory() as tmpdir:
            cfg = os.path.join(tmpdir, "config")
            with open(cfg, "w", encoding="utf-8") as f:
                f.write("[Hotkey/TriggerKeys]\n0=Super+space\n")

            ok, err = configure_gnome_super_space(
                fcitx5_config_path=cfg,
                run=fake_run,
                restore_overlay_key=True,
                fcitx5_remote_path="/usr/bin/fcitx5-remote",
            )
            self.assertTrue(ok, err)
            self.assertEqual(len(reloaded), 1)
            with open(cfg, "r", encoding="utf-8") as f:
                self.assertNotIn("Super+space", f.read())

        self.assertTrue(is_gnome_super_space_configured(run=fake_run))
        custom = parse_gsettings_str_list(store[(GNOME_MEDIA_KEYS_SCHEMA, "custom-keybindings")])
        self.assertEqual(
            custom,
            [
                "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/obs-record/",
                GNOME_TOGGLE_KEY_PATH,
            ],
        )
        self.assertEqual(store[(GNOME_CUSTOM_SCHEMA, "binding")], "<Super>space")
        self.assertEqual(store[(GNOME_MUTTER_SCHEMA, "overlay-key")], "'Super'")


if __name__ == "__main__":
    unittest.main()
