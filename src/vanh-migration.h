/*
 * SPDX-FileCopyrightText: 2026 congmnguyen <congminhnguyen@proton.me>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
/**
 * @file vanh-migration.h
 * @brief One-way migration of the user data of the former "lotus" addon to the "vanh" names.
 *
 * Vanh was published as fcitx5-lotus: the addon, the input method, the configuration files and
 * the dictionary directory were all called "lotus". The migration copies each old file to its new
 * name when the new one does not exist yet (the old file is kept, so a downgrade still works) and
 * renames the input method in the fcitx5 profile. Every step is idempotent.
 *
 * The text functions are pure; the file functions take the directories explicitly so the tests
 * run against a temporary tree.
 */
#ifndef _FCITX5_VANH_MIGRATION_H_
#define _FCITX5_VANH_MIGRATION_H_

#include "common-export.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vanh::migration {

    /// Input method / addon id used before the rename.
    inline constexpr std::string_view kOldId = "lotus";
    /// Input method / addon id used now.
    inline constexpr std::string_view kNewId = "vanh";

    /**
     * @brief One user file to carry over, relative to the fcitx5 user config directory.
     */
    struct FileRename {
        std::string_view from; ///< old path, e.g. "conf/lotus.conf"
        std::string_view to;   ///< new path, e.g. "conf/vanh.conf"
    };

    /**
     * @brief The user configuration files the addon writes, old name -> new name.
     * @return Paths relative to the fcitx5 user config directory (~/.config/fcitx5).
     */
    LOTUS_EXPORT const std::vector<FileRename>& configFileRenames();

    /**
     * @brief Rewrites the content of the old main configuration (conf/lotus.conf) for the new name.
     *
     * Only the option key that carried the old name changes (UseLotusIcons -> UseVanhIcons);
     * every other line is kept byte for byte.
     * @param text Content of conf/lotus.conf.
     * @return Content for conf/vanh.conf.
     */
    LOTUS_EXPORT std::string migrateMainConfigText(std::string_view text);

    /**
     * @brief Renames the input method in the text of the fcitx5 profile (~/.config/fcitx5/profile).
     *
     * In every group without a "vanh" item, each item `Name=lotus` becomes `Name=vanh`, keeping its
     * position and layout. `DefaultIM=lotus` becomes `DefaultIM=vanh`. A group that already has a
     * "vanh" item keeps its "lotus" item untouched (fcitx5 drops unknown input methods itself).
     * @param text Profile content.
     * @return The migrated profile, or std::nullopt when nothing had to change.
     */
    LOTUS_EXPORT std::optional<std::string> migrateProfileText(std::string_view text);

    /**
     * @brief One input method group of a profile, as listed in the file.
     */
    struct ProfileGroup {
        /// One item of the group.
        struct Item {
            std::string name;   ///< input method id
            std::string layout; ///< keyboard layout override, often empty
        };
        std::string       name;      ///< group name (`Name=` of `[Groups/N]`)
        std::string       defaultIM; ///< `DefaultIM=` of `[Groups/N]`
        std::vector<Item> items;     ///< items in `[Groups/N/Items/M]` order of appearance
    };

    /**
     * @brief Reads the groups of a profile text.
     * @param text Profile content.
     * @return The groups, in the order of their sections.
     */
    LOTUS_EXPORT std::vector<ProfileGroup> parseProfileGroups(std::string_view text);

    /**
     * @brief Copies the old user configuration files and the old user data directory.
     *
     * For each entry of configFileRenames() under @p fcitxConfigDir, and for every file under
     * `<fcitxDataDir>/lotus/` (the user dictionary), the old file is copied to the new name when the
     * new one does not exist. Old files are never modified or removed.
     * @param fcitxConfigDir fcitx5 user config directory (~/.config/fcitx5).
     * @param fcitxDataDir fcitx5 user data directory (~/.local/share/fcitx5).
     * @return One human-readable line per migrated file or failure, empty when nothing was done.
     */
    LOTUS_EXPORT std::vector<std::string> migrateUserFiles(const std::filesystem::path& fcitxConfigDir, const std::filesystem::path& fcitxDataDir);

    /**
     * @brief Applies migrateProfileText() to the profile file, atomically.
     * @param profilePath Path of the fcitx5 profile.
     * @return A human-readable line when the profile was rewritten or could not be, empty otherwise.
     */
    LOTUS_EXPORT std::vector<std::string> migrateProfileFile(const std::filesystem::path& profilePath);

    /**
     * @brief Reports whether the profile file still lists the old input method in a way that
     *        migrateProfileText() would change.
     * @param profilePath Path of the fcitx5 profile.
     * @return True when a migration is pending.
     */
    LOTUS_EXPORT bool profileNeedsMigration(const std::filesystem::path& profilePath);

    /**
     * @brief The fcitx5 user config directory of the current user (~/.config/fcitx5).
     */
    LOTUS_EXPORT std::filesystem::path userConfigDir();

    /**
     * @brief The fcitx5 user data directory of the current user (~/.local/share/fcitx5).
     */
    LOTUS_EXPORT std::filesystem::path userDataDir();

} // namespace vanh::migration

#endif // _FCITX5_VANH_MIGRATION_H_
