/*
 * SPDX-FileCopyrightText: 2026 congmnguyen <congminhnguyen@proton.me>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
/**
 * @file vanh-migrate.cpp
 * @brief The "vanh-migrate" module: renames the former "lotus" input method in the fcitx5 profile.
 *
 * fcitx5 drops every input method it does not know while it reads the profile, and the Vanh
 * engine addon is loaded on demand, i.e. only once "vanh" is already in a group. A profile that
 * still says "lotus" would therefore lose the input method before the engine could fix it. This
 * module is not on demand: fcitx5 constructs it while loading the addons, which happens before the
 * input method manager reads the profile, so the profile on disk is migrated in time.
 *
 * When fcitx5 is started with --replace, the replaced instance saves its in-memory profile (still
 * "lotus") when it exits, possibly after this module ran. A delayed check catches that: if the
 * profile on disk needs a migration again, the groups of the running instance are fixed through the
 * InputMethodManager API and saved, which also rewrites the file.
 */
#include "vanh-migration.h"

#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/instance.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iterator>
#include <memory>

namespace {

    FCITX_DEFINE_LOG_CATEGORY(vanhMigrateLog, "vanh-migrate", fcitx::LogLevel::Info);
#define VANH_MIGRATE_INFO() FCITX_LOGC(vanhMigrateLog, Info)

    /// Delay of the second look at the profile (the --replace race), in microseconds.
    constexpr uint64_t kRecheckDelayUs = 5ULL * 1000 * 1000;

    void logAll(const std::vector<std::string>& lines) {
        for (const auto& line : lines) {
            VANH_MIGRATE_INFO() << line;
        }
    }

    /**
     * @brief Puts the new input method back into the groups of the running instance.
     *
     * The in-memory groups already lost the unknown "lotus" item; the migrated profile text says
     * which groups had it, at which position and which one was the default.
     * @return True when a group changed.
     */
    bool migrateLoadedGroups(fcitx::InputMethodManager& imManager, const std::string& profileText) {
        const auto migrated = vanh::migration::migrateProfileText(profileText);
        if (!migrated) {
            return false;
        }
        const std::string newId(vanh::migration::kNewId);
        bool              changed = false;
        for (const auto& wanted : vanh::migration::parseProfileGroups(*migrated)) {
            const auto hasNew = [&newId](const auto& item) { return item.name == newId; };
            if (!std::ranges::any_of(wanted.items, hasNew)) {
                continue;
            }
            const auto* current = imManager.group(wanted.name);
            if (current == nullptr ||
                std::ranges::any_of(current->inputMethodList(), [&newId](const fcitx::InputMethodGroupItem& item) { return item.name() == newId; })) {
                continue;
            }
            fcitx::InputMethodGroup group = *current;
            auto&                   list  = group.inputMethodList();
            list.clear();
            for (const auto& item : wanted.items) {
                if (imManager.entry(item.name) != nullptr) {
                    list.emplace_back(item.name);
                    list.back().setLayout(item.layout);
                }
            }
            if (!wanted.defaultIM.empty() && imManager.entry(wanted.defaultIM) != nullptr) {
                group.setDefaultInputMethod(wanted.defaultIM);
            }
            imManager.setGroup(std::move(group));
            changed = true;
        }
        return changed;
    }

    class VanhMigrate : public fcitx::AddonInstance {
      public:
        explicit VanhMigrate(fcitx::Instance* instance) : instance_(instance) {
            const auto configDir = vanh::migration::userConfigDir();
            logAll(vanh::migration::migrateUserFiles(configDir, vanh::migration::userDataDir()));
            hadOldProfile_ = vanh::migration::profileNeedsMigration(configDir / "profile");
            logAll(vanh::migration::migrateProfileFile(configDir / "profile"));
            if (hadOldProfile_) {
                recheck_ = instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kRecheckDelayUs, 0,
                                                               [this](fcitx::EventSourceTime*, uint64_t) {
                                                                   recheckProfile();
                                                                   return true;
                                                               });
            }
        }

      private:
        void recheckProfile() {
            const auto profile = vanh::migration::userConfigDir() / "profile";
            if (!vanh::migration::profileNeedsMigration(profile)) {
                return;
            }
            // The replaced instance wrote the old profile back. Fix what this instance loaded, then
            // save, which rewrites the file with the groups held in memory.
            std::ifstream     file(profile);
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            auto&             imManager = instance_->inputMethodManager();
            if (migrateLoadedGroups(imManager, text)) {
                VANH_MIGRATE_INFO() << "Re-added input method \"vanh\" to the running groups";
            }
            imManager.save();
            VANH_MIGRATE_INFO() << "Saved the migrated profile again";
        }

        fcitx::Instance*                        instance_;
        bool                                    hadOldProfile_ = false;
        std::unique_ptr<fcitx::EventSourceTime> recheck_;
    };

    class VanhMigrateFactory : public fcitx::AddonFactory {
      public:
        fcitx::AddonInstance* create(fcitx::AddonManager* manager) override {
            return new VanhMigrate(manager->instance()); // NOLINT
        }
    };

} // namespace

FCITX_ADDON_FACTORY(VanhMigrateFactory)
