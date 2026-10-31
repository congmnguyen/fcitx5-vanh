# SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Internationalization setup for the application.
"""

import gettext
import locale
import os

try:
    from paths import LOCALE_DIR
except ImportError:
    LOCALE_DIR = "/usr/share/locale"


def setup_i18n():
    """Initialize gettext with system locale."""
    try:
        locale.setlocale(locale.LC_ALL, "")
        domain = "fcitx5-vanh"
        localedir = LOCALE_DIR

        if os.path.exists(localedir):
            gettext.bindtextdomain(domain, localedir)
            gettext.textdomain(domain)
    except Exception as e:
        print(f"Failed to initialize i18n: {e}")


_ = gettext.gettext


def N_(text):
    """Marker for strings that should be translated lazily."""
    return text
