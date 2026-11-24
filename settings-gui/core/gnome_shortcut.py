# SPDX-FileCopyrightText: 2026 Cong Nguyen <congmnguyen@users.noreply.github.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""GNOME Super+Space setup so GNOME Shell toggles Fcitx5 without opening Activities."""

import ast
import os
import shutil
import subprocess
import tempfile

GNOME_CUSTOM_KEYBINDING_BASE = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings"
GNOME_TOGGLE_KEY_PATH = f"{GNOME_CUSTOM_KEYBINDING_BASE}/fcitx5-toggle/"
GNOME_CUSTOM_SCHEMA = (
    f"org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:{GNOME_TOGGLE_KEY_PATH}"
)
GNOME_MEDIA_KEYS_SCHEMA = "org.gnome.settings-daemon.plugins.media-keys"
GNOME_WM_KEYBINDINGS_SCHEMA = "org.gnome.desktop.wm.keybindings"
GNOME_MUTTER_SCHEMA = "org.gnome.mutter"

_FCITX5_SUPER_SPACE_SECTIONS = frozenset(
    {
        "[Hotkey/TriggerKeys]",
        "[Hotkey/EnumerateGroupForwardKeys]",
    }
)


def is_gnome_session(env: dict | None = None) -> bool:
    """Returns True when running in a GNOME desktop session."""
    source = os.environ if env is None else env
    desktop = source.get("XDG_CURRENT_DESKTOP", "")
    return "GNOME" in desktop.upper()


def default_fcitx5_config_path() -> str:
    """Returns the path to Fcitx5's main config file."""
    xdg_config_home = os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config"))
    return os.path.join(xdg_config_home, "fcitx5", "config")


def parse_gsettings_str_list(raw: str) -> list[str]:
    """Parses a gsettings string-array output (e.g. '@as []' or \"['a', 'b']\")."""
    text = raw.strip()
    if text.startswith("@as"):
        text = text[3:].strip()
    if not text:
        return []
    parsed = ast.literal_eval(text)
    if not isinstance(parsed, list):
        return []
    return [str(item) for item in parsed]


def format_gsettings_str_list(items: list[str]) -> str:
    """Formats a Python list of strings as a gsettings string-array literal."""
    if not items:
        return "[]"
    escaped = ", ".join(
        "'" + item.replace("\\", "\\\\").replace("'", "\\'") + "'" for item in items
    )
    return f"[{escaped}]"


def strip_super_space_from_fcitx5_config(text: str) -> tuple[str, bool]:
    """Removes Super+space trigger entries from Fcitx5 config text."""
    out = []
    section = None
    changed = False
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped
            out.append(line)
            continue
        if (
            section in _FCITX5_SUPER_SPACE_SECTIONS
            and "=" in stripped
            and stripped.split("=", 1)[1].strip().lower() == "super+space"
        ):
            changed = True
            continue
        out.append(line)
    return "".join(out), changed


def _default_run(args: list[str]) -> tuple[int, str, str]:
    proc = subprocess.run(args, capture_output=True, text=True, check=False)
    return proc.returncode, proc.stdout, proc.stderr


def is_gnome_super_space_configured(run=_default_run) -> bool:
    """Returns True when GNOME Shell already routes Super+Space to fcitx5-remote -t."""
    if run is _default_run and shutil.which("gsettings") is None:
        return False
    code, out, _ = run(["gsettings", "get", GNOME_WM_KEYBINDINGS_SCHEMA, "switch-input-source"])
    if code != 0 or parse_gsettings_str_list(out):
        return False
    code, out, _ = run(["gsettings", "get", GNOME_MEDIA_KEYS_SCHEMA, "custom-keybindings"])
    if code != 0 or GNOME_TOGGLE_KEY_PATH not in parse_gsettings_str_list(out):
        return False
    code, binding, _ = run(["gsettings", "get", GNOME_CUSTOM_SCHEMA, "binding"])
    if code != 0 or binding.strip().strip("'") != "<Super>space":
        return False
    code, cmd, _ = run(["gsettings", "get", GNOME_CUSTOM_SCHEMA, "command"])
    return code == 0 and "fcitx5-remote" in cmd and "-t" in cmd


def configure_gnome_super_space(
    fcitx5_config_path: str | None = None,
    run=_default_run,
    restore_overlay_key: bool = True,
    fcitx5_remote_path: str | None = None,
) -> tuple[bool, str]:
    """Configures GNOME Shell to handle Super+Space and toggle Fcitx5 cleanly."""
    if run is _default_run and shutil.which("gsettings") is None:
        return False, "gsettings not found"

    remote_bin = fcitx5_remote_path or shutil.which("fcitx5-remote") or "/usr/bin/fcitx5-remote"
    cfg_path = fcitx5_config_path or default_fcitx5_config_path()

    # 1. Remove Super+space from Fcitx5 so it never grabs the key behind Mutter's back.
    if os.path.exists(cfg_path):
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                original = f.read()
            updated, changed = strip_super_space_from_fcitx5_config(original)
            if changed:
                target_dir = os.path.dirname(cfg_path) or "."
                with tempfile.NamedTemporaryFile(
                    "w", dir=target_dir, encoding="utf-8", delete=False
                ) as tf:
                    tf.write(updated)
                    tmp_name = tf.name
                os.replace(tmp_name, cfg_path)
                run([remote_bin, "-r"])
        except OSError as exc:
            return False, str(exc)

    # 2. Unbind GNOME's built-in IBus Super+Space shortcut.
    for key in ("switch-input-source", "switch-input-source-backward"):
        code, _, err = run(["gsettings", "set", GNOME_WM_KEYBINDINGS_SCHEMA, key, "[]"])
        if code != 0:
            return False, err.strip() or f"Failed to clear {key}"

    # 3. Register our custom keybinding while preserving all existing custom keybindings.
    code, out, err = run(["gsettings", "get", GNOME_MEDIA_KEYS_SCHEMA, "custom-keybindings"])
    if code != 0:
        return False, err.strip() or "Failed to read GNOME custom-keybindings"
    existing = parse_gsettings_str_list(out)
    if GNOME_TOGGLE_KEY_PATH not in existing:
        existing.append(GNOME_TOGGLE_KEY_PATH)

    steps = [
        ["gsettings", "set", GNOME_CUSTOM_SCHEMA, "name", "Toggle Input Method (Fcitx5)"],
        ["gsettings", "set", GNOME_CUSTOM_SCHEMA, "command", f"{remote_bin} -t"],
        ["gsettings", "set", GNOME_CUSTOM_SCHEMA, "binding", "<Super>space"],
        [
            "gsettings",
            "set",
            GNOME_MEDIA_KEYS_SCHEMA,
            "custom-keybindings",
            format_gsettings_str_list(existing),
        ],
    ]
    if restore_overlay_key:
        steps.append(["gsettings", "reset", GNOME_MUTTER_SCHEMA, "overlay-key"])

    for args in steps:
        code, _, err = run(args)
        if code != 0:
            return False, err.strip() or f"Command failed: {' '.join(args)}"

    return True, ""
