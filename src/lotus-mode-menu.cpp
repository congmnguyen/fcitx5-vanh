/*
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file lotus-mode-menu.cpp
 * @brief Mode Order resolution and the mode menu shortcuts.
 */

#include "lotus-mode-menu.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/stringutils.h>

namespace fcitx {
    namespace {
        /// One mode name accepted in Mode Order.
        struct ModeNameEntry {
            std::string_view         name; ///< Name used in Mode Order.
            std::optional<LotusMode> mode; ///< Mode of the name; nothing for the Default entry.
        };

        /// Mode Order names, in the fallback order used when a name is not listed.
        constexpr auto kModeNames = std::to_array<ModeNameEntry>({
            {.name = "Smooth", .mode = LotusMode::Smooth},
            {.name = "Select", .mode = LotusMode::Select},
            {.name = "SurroundingText", .mode = LotusMode::SurroundingText},
            {.name = "Preedit", .mode = LotusMode::Preedit},
            {.name = "Emoji", .mode = LotusMode::Emoji},
            {.name = "Off", .mode = LotusMode::Off},
            {.name = "SuperSmooth", .mode = LotusMode::SuperSmooth},
            {.name = "Default", .mode = std::nullopt},
        });

        KeySym         shortcutKey(const std::string& shortcut) {
            return Key(shortcut).sym();
        }

        bool modeVisible(const lotusConfig& config, LotusMode mode) {
            switch (mode) {
                case LotusMode::Smooth:
                    return *config.showModeSmooth;
                case LotusMode::SuperSmooth:
                    return *config.showModeSuperSmooth;
                case LotusMode::Select:
                    return *config.showModeSelect;
                case LotusMode::SurroundingText:
                    return *config.showModeSurroundingText;
                case LotusMode::Preedit:
                    return *config.showModePreedit;
                case LotusMode::Emoji:
                    return *config.showModeEmoji;
                case LotusMode::Off:
                    return *config.showModeOff;
            }
            return false;
        }

        KeySym modeShortcut(const lotusConfig& config, LotusMode mode) {
            switch (mode) {
                case LotusMode::Smooth:
                    return shortcutKey(*config.shortcutSmooth);
                case LotusMode::SuperSmooth:
                    return shortcutKey(*config.shortcutSuperSmooth);
                case LotusMode::Select:
                    return shortcutKey(*config.shortcutSelect);
                case LotusMode::SurroundingText:
                    return shortcutKey(*config.shortcutSurroundingText);
                case LotusMode::Preedit:
                    return shortcutKey(*config.shortcutPreedit);
                case LotusMode::Emoji:
                    return shortcutKey(*config.shortcutEmoji);
                case LotusMode::Off:
                    return shortcutKey(*config.shortcutOff);
            }
            return FcitxKey_None;
        }
    } // namespace

    std::string modeLabel(const ModeOrderEntry& entry) {
        if (entry.isDefault) {
            return _("Default Typing");
        }
        return LotusModeI18NAnnotation::toString(entry.mode);
    }

    std::vector<ModeOrderEntry> modeOrderEntries(const lotusConfig& config) {
        const auto order = stringutils::split(*config.modeOrder, ",");

        auto       resolve = [&config](const ModeNameEntry& nameEntry) {
            if (nameEntry.mode.has_value()) {
                const LotusMode mode = *nameEntry.mode;
                return ModeOrderEntry{
                          .mode      = mode,
                          .key       = modeShortcut(config, mode),
                          .visible   = modeVisible(config, mode),
                          .isDefault = false,
                };
            }
            return ModeOrderEntry{
                      .mode      = config.mode.value(),
                      .key       = shortcutKey(*config.shortcutDefault),
                      .visible   = *config.showModeDefault,
                      .isDefault = true,
            };
        };

        std::vector<ModeOrderEntry> entries;
        for (const auto& name : order) {
            const auto* const it =
                std::ranges::find_if(kModeNames, [&name](const ModeNameEntry& entry) {
                    return entry.name == name;
                });
            if (it != kModeNames.end()) {
                entries.push_back(resolve(*it));
            }
        }
        for (const auto& nameEntry : kModeNames) {
            if (std::ranges::find(order, nameEntry.name) == order.end()) {
                entries.push_back(resolve(nameEntry));
            }
        }
        return entries;
    }

    KeySym typeKeyForModeMenuHotkey(KeySym hotkeySym, const lotusConfig& config) {
        // The mode shortcuts (every mode is in Mode Order, listed or not) and the menu
        // navigation keys cannot be typed through the menu.
        static const auto reservedMenuKeys = std::to_array<KeySym>({
            FcitxKey_Escape,
            FcitxKey_Tab,
            FcitxKey_ISO_Left_Tab,
            FcitxKey_Return,
            FcitxKey_space,
            FcitxKey_Up,
            FcitxKey_Down,
        });
        const auto        entries          = modeOrderEntries(config);
        const bool        reserved =
            std::ranges::find(reservedMenuKeys, hotkeySym) != reservedMenuKeys.end() ||
            std::ranges::any_of(entries, [hotkeySym](const auto& e) {
                return e.key == hotkeySym;
            });
        return reserved ? FcitxKey_f : hotkeySym;
    }

} // namespace fcitx
