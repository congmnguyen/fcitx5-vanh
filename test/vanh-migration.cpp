// SPDX-FileCopyrightText: 2026 congmnguyen <congminhnguyen@proton.me>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rename from "lotus" to "vanh" must carry the user's data over: the configuration files are
// copied to their new names (never overwriting a newer vanh file, never touching the old one), the
// user dictionary directory follows, and the fcitx5 profile lists "vanh" where it listed "lotus".
// Every step must be a no-op the second time.
#include "vanh-migration.h"

#include <unistd.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

    int failures = 0;

    void check(const char* name, bool condition) {
        if (!condition) {
            std::printf("FAIL %s\n", name);
            ++failures;
        }
    }

    std::string readText(const std::filesystem::path& path) {
        std::ifstream file(path);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    void writeText(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

    // What fcitx5 writes, with lotus as the second item and the default.
    constexpr const char* kProfile = "[Groups/0]\n"
                                     "# Group Name\n"
                                     "Name=Default\n"
                                     "# Layout\n"
                                     "Default Layout=us\n"
                                     "# Default Input Method\n"
                                     "DefaultIM=lotus\n"
                                     "\n"
                                     "[Groups/0/Items/0]\n"
                                     "# Name\n"
                                     "Name=keyboard-us\n"
                                     "# Layout\n"
                                     "Layout=\n"
                                     "\n"
                                     "[Groups/0/Items/1]\n"
                                     "# Name\n"
                                     "Name=lotus\n"
                                     "# Layout\n"
                                     "Layout=\n"
                                     "\n"
                                     "[GroupOrder]\n"
                                     "0=Default\n";

    void testProfileText() {
        const auto migrated = vanh::migration::migrateProfileText(kProfile);
        check("profile with lotus changes", migrated.has_value());
        if (!migrated) {
            return;
        }
        std::string expected = kProfile;
        expected.replace(expected.find("DefaultIM=lotus"), 15, "DefaultIM=vanh");
        expected.replace(expected.find("Name=lotus"), 10, "Name=vanh");
        check("profile: item and default renamed, rest byte-identical", *migrated == expected);
        check("profile migration is idempotent", !vanh::migration::migrateProfileText(*migrated).has_value());

        const auto groups = vanh::migration::parseProfileGroups(*migrated);
        check("parse: one group", groups.size() == 1);
        if (groups.size() == 1) {
            check("parse: group name", groups[0].name == "Default");
            check("parse: default im", groups[0].defaultIM == "vanh");
            check("parse: item order kept", groups[0].items.size() == 2 && groups[0].items[0].name == "keyboard-us" && groups[0].items[1].name == "vanh");
        }

        check("profile without lotus is left alone", !vanh::migration::migrateProfileText("[Groups/0]\nName=Default\nDefaultIM=keyboard-us\n"
                                                                                          "[Groups/0/Items/0]\nName=keyboard-us\nLayout=\n")
                                                          .has_value());
        check("empty profile is left alone", !vanh::migration::migrateProfileText("").has_value());

        // A group that already has vanh keeps its stale lotus item (fcitx5 drops it), but the
        // default still moves to vanh.
        const std::string both = "[Groups/0]\nName=Default\nDefaultIM=lotus\n"
                                 "[Groups/0/Items/0]\nName=vanh\nLayout=\n"
                                 "[Groups/0/Items/1]\nName=lotus\nLayout=\n";
        const auto        bothMigrated = vanh::migration::migrateProfileText(both);
        check("group with vanh: changed", bothMigrated.has_value());
        if (bothMigrated) {
            check("group with vanh: no duplicate vanh item", bothMigrated->find("Name=lotus") != std::string::npos);
            check("group with vanh: default renamed", bothMigrated->find("DefaultIM=vanh") != std::string::npos);
        }

        // Two groups: only the group with lotus changes; the other keeps its own vanh.
        const std::string twoGroups = "[Groups/0]\nName=A\nDefaultIM=keyboard-us\n"
                                      "[Groups/0/Items/0]\nName=keyboard-us\nLayout=\n"
                                      "[Groups/1]\nName=B\nDefaultIM=lotus\n"
                                      "[Groups/1/Items/0]\nName=lotus\nLayout=us\n"
                                      "[GroupOrder]\n0=A\n1=B\n";
        const auto        twoMigrated = vanh::migration::migrateProfileText(twoGroups);
        check("two groups: changed", twoMigrated.has_value());
        if (twoMigrated) {
            const auto parsed = vanh::migration::parseProfileGroups(*twoMigrated);
            check("two groups: parsed", parsed.size() == 2);
            if (parsed.size() == 2) {
                check("two groups: A untouched", parsed[0].items.size() == 1 && parsed[0].items[0].name == "keyboard-us" && parsed[0].defaultIM == "keyboard-us");
                check("two groups: B renamed with layout", parsed[1].items.size() == 1 && parsed[1].items[0].name == "vanh" && parsed[1].items[0].layout == "us");
                check("two groups: B default", parsed[1].defaultIM == "vanh");
            }
        }

        // "lotus" as a value of another key, or a group order entry, is not an input method item.
        check("unrelated lotus values untouched", !vanh::migration::migrateProfileText("[Groups/0]\nName=lotus\n[GroupOrder]\n0=lotus\n").has_value());
        // CRLF line endings survive.
        const auto crlf = vanh::migration::migrateProfileText("[Groups/0/Items/0]\r\nName=lotus\r\n");
        check("crlf kept", crlf.has_value() && *crlf == "[Groups/0/Items/0]\r\nName=vanh\r\n");
    }

    void testMainConfigText() {
        const std::string text = "# Use Lotus Status Icons\nUseLotusIcons=True\nMode=Preedit\n";
        check("main config key renamed", vanh::migration::migrateMainConfigText(text) == "# Use Lotus Status Icons\nUseVanhIcons=True\nMode=Preedit\n");
        check("main config without key unchanged", vanh::migration::migrateMainConfigText("Mode=Preedit") == "Mode=Preedit");
    }

    void testFiles() {
        const auto root   = std::filesystem::temp_directory_path() / ("fcitx5-vanh-migration-" + std::to_string(::getpid()));
        const auto config = root / "config/fcitx5";
        const auto data   = root / "data/fcitx5";
        std::filesystem::remove_all(root);

        writeText(config / "conf/lotus.conf", "UseLotusIcons=True\nMode=Smooth\n");
        writeText(config / "conf/lotus-macro-table.conf", "[Macro/0]\nKey=ko\nValue=không\n");
        writeText(config / "conf/lotus-custom-keymap.conf", "old keymap\n");
        writeText(config / "conf/vanh-custom-keymap.conf", "newer vanh keymap\n");
        writeText(config / "conf/lotus-app-rules.conf", "firefox=1\n");
        writeText(config / "conf/lotus-emoji-history.conf", "😄\n");
        writeText(data / "lotus/vietnamese.cm.dict", "xin chào\n");
        writeText(config / "profile", kProfile);

        const auto log = vanh::migration::migrateUserFiles(config, data);
        check("files: five migrations logged", log.size() == 5);
        check("files: main config copied and rewritten", readText(config / "conf/vanh.conf") == "UseVanhIcons=True\nMode=Smooth\n");
        check("files: old main config kept", readText(config / "conf/lotus.conf") == "UseLotusIcons=True\nMode=Smooth\n");
        check("files: macro table copied", readText(config / "conf/vanh-macro-table.conf") == "[Macro/0]\nKey=ko\nValue=không\n");
        check("files: existing vanh file not overwritten", readText(config / "conf/vanh-custom-keymap.conf") == "newer vanh keymap\n");
        check("files: app rules copied", readText(config / "conf/vanh-app-rules.conf") == "firefox=1\n");
        check("files: emoji history copied", readText(config / "conf/vanh-emoji-history.conf") == "😄\n");
        check("files: dictionary copied", readText(data / "vanh/vietnamese.cm.dict") == "xin chào\n");
        check("files: second run does nothing", vanh::migration::migrateUserFiles(config, data).empty());

        check("profile file: pending", vanh::migration::profileNeedsMigration(config / "profile"));
        check("profile file: migrated once", vanh::migration::migrateProfileFile(config / "profile").size() == 1);
        check("profile file: content", readText(config / "profile").find("Name=vanh") != std::string::npos);
        check("profile file: no longer pending", !vanh::migration::profileNeedsMigration(config / "profile"));
        check("profile file: second run does nothing", vanh::migration::migrateProfileFile(config / "profile").empty());
        check("profile file: missing profile is fine", vanh::migration::migrateProfileFile(root / "nope").empty());

        const auto empty = root / "empty";
        check("files: nothing to migrate", vanh::migration::migrateUserFiles(empty / "config", empty / "data").empty());

        std::filesystem::remove_all(root);
    }

} // namespace

int main() {
    try {
        testProfileText();
        testMainConfigText();
        testFiles();
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("vanh migration: all checks passed\n");
    return 0;
}
