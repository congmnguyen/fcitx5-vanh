// SPDX-FileCopyrightText: 2026 congmnguyen <congminhnguyen@proton.me>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end check of the vanh-migrate module: a real fcitx5 Instance loads the built
// libvanh-migrate.so through the shared library loader, from a temporary tree only, with a profile
// that still lists "lotus". The module must run before the input method manager reads the profile,
// so the loaded group contains "vanh" (fcitx5 would have dropped an unknown "lotus") and the file on
// disk is migrated.
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/instance.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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

    int run() {
        const auto root = std::filesystem::temp_directory_path() / ("fcitx5-vanh-migrate-module-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        const auto config = root / "config";
        const auto data   = root / "data";
        const auto sys    = root / "sys";
        setenv("HOME", root.c_str(), 1);
        setenv("XDG_CONFIG_HOME", config.c_str(), 1);
        setenv("XDG_DATA_HOME", data.c_str(), 1);
        setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
        setenv("XDG_DATA_DIRS", sys.c_str(), 1);
        setenv("XDG_CONFIG_DIRS", (root / "sysconf").c_str(), 1);
        setenv("FCITX_ADDON_DIRS", VANH_MIGRATE_MODULE_DIR, 1);

        std::filesystem::create_directories(sys / "fcitx5/addon");
        std::filesystem::copy_file(VANH_MIGRATE_ADDON_CONF, sys / "fcitx5/addon/vanh-migrate.conf");
        std::filesystem::copy_file(VANH_ADDON_CONF, sys / "fcitx5/addon/vanh.conf");
        std::filesystem::create_directories(sys / "fcitx5/inputmethod");
        std::filesystem::copy_file(VANH_INPUTMETHOD_CONF, sys / "fcitx5/inputmethod/vanh.conf");
        writeText(config / "fcitx5/profile", "[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=lotus\n\n"
                                             "[Groups/0/Items/0]\nName=lotus\nLayout=\n\n"
                                             "[GroupOrder]\n0=Default\n");
        writeText(config / "fcitx5/conf/lotus.conf", "UseLotusIcons=True\n");

        {
            std::array<char, 32>  program{"vanh-migrate-module-test"};
            std::array<char, 64>  enable{"--enable=vanh-migrate"};
            std::array<char, 16>  disableAll{"--disable=all"};
            std::array<char*, 4>  argv{program.data(), disableAll.data(), enable.data(), nullptr};
            fcitx::Instance       instance(3, argv.data());
            instance.addonManager().registerDefaultLoader(nullptr);
            instance.initialize();

            check("module loaded", instance.addonManager().addon("vanh-migrate") != nullptr);
            const auto* group = instance.inputMethodManager().group("Default");
            check("group exists", group != nullptr);
            if (group != nullptr) {
                const auto& items = group->inputMethodList();
                check("group lists vanh", std::ranges::any_of(items, [](const fcitx::InputMethodGroupItem& item) { return item.name() == "vanh"; }));
                check("default im is vanh", group->defaultInputMethod() == "vanh");
            }
        }

        const auto profile = readText(config / "fcitx5/profile");
        check("profile on disk migrated", profile.find("Name=vanh") != std::string::npos && profile.find("Name=lotus") == std::string::npos);
        check("config copied", readText(config / "fcitx5/conf/vanh.conf") == "UseVanhIcons=True\n");
        std::filesystem::remove_all(root);

        if (failures != 0) {
            std::printf("%d failure(s)\n", failures);
            return 1;
        }
        std::printf("vanh-migrate module: all checks passed\n");
        return 0;
    }

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        std::printf("FAIL unexpected exception: %s\n", error.what());
        return 1;
    }
}
