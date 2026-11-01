/*
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file lotus-mode-menu.h
 * @brief Mode Order resolution and the mode menu shortcuts.
 */

#ifndef _FCITX5_LOTUS_MODE_MENU_H_
#define _FCITX5_LOTUS_MODE_MENU_H_

#include "lotus-config.h"

#include <string>
#include <vector>
#include <fcitx-utils/keysym.h>

namespace fcitx {

    /**
     * @brief One mode resolved from Mode Order, in menu order.
     */
    struct ModeOrderEntry {
        LotusMode mode;      ///< Mode of the entry.
        KeySym    key;       ///< Shortcut key that selects the entry.
        bool      visible;   ///< Whether the entry is shown.
        bool      isDefault; ///< Whether the entry is the synthetic Default Typing one.
    };

    /**
     * @brief Label of a menu entry, taken from the translated enum names.
     * @param entry Menu entry.
     * @return Translated label of the entry.
     */
    std::string modeLabel(const ModeOrderEntry& entry);

    /**
     * @brief Resolves the modes in Mode Order order, then the names missing from it.
     * @param config Current configuration.
     * @return The menu entries in menu order.
     */
    std::vector<ModeOrderEntry> modeOrderEntries(const lotusConfig& config);

    /**
     * @brief Key symbol that triggers the "type the hotkey character" menu entry.
     * @param hotkeySym Key symbol of the mode menu hotkey.
     * @param config Current configuration.
     * @return Key symbol used to type the hotkey character.
     */
    KeySym typeKeyForModeMenuHotkey(KeySym hotkeySym, const lotusConfig& config);

} // namespace fcitx

#endif // _FCITX5_LOTUS_MODE_MENU_H_
