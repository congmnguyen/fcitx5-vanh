/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 * SPDX-FileCopyrightText: 2026 kimxuanhong <kimxuanhong@outlook.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file lotus-engine.h
 * @brief Main engine implementation for fcitx5-vanh Vietnamese input method.
 */

#ifndef _FCITX5_LOTUS_ENGINE_H_
#define _FCITX5_LOTUS_ENGINE_H_

#include "lotus-config.h"
#include <mutex>
#include <memory>
#include <vector>
#include <fcitx-config/iniparser.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/keysym.h>
#include "common-export.h"
#include "lotus-kb-sender.h"

// Owned through shared/unique pointers, so the full types stay in the .cpp files.
namespace lotus {
    class Dictionary;
    class Learned;
    class MacroTable;
}
class EmojiLoader;

namespace fcitx {

    class LotusState;
    class WindowFocusTracker;

    /**
     * @brief Main engine class for Lotus input method.
     *
     * Handles input processing, configuration management, and UI actions.
     * Implements fcitx InputMethodEngine interface.
     */
    class LOTUS_EXPORT LotusEngine final : public InputMethodEngineV2 {
      public:
        /**
         * @brief Gets the fcitx instance.
         * @return Pointer to the fcitx instance.
         */
        Instance* instance() const {
            return &instance_;
        }

        /**
         * @brief Gets the keyboard request sender that owns the uinput keyboard socket.
         * @return Sender used by every burst.
         */
        KbSender& kbSender() {
            return kbSender_;
        }

        /**
         * @brief Whether the uinput server types Enter (KbOp::Return); false once a request for
         *        one went unanswered (a server older than the addon, until fcitx restarts).
         */
        bool uinputTypesReturn() const {
            return uinputTypesReturn_;
        }
        void setUinputTypesReturn(bool value) {
            uinputTypesReturn_ = value;
        }

        /**
         * @brief Constructs the Lotus engine.
         * @param instance Pointer to fcitx instance.
         */
        explicit LotusEngine(Instance* instance);

        /**
         * @brief Destroys the engine and releases resources.
         */
        ~LotusEngine() override;

        // Rule of five
        LotusEngine(const LotusEngine&)            = delete;
        LotusEngine& operator=(const LotusEngine&) = delete;
        LotusEngine(LotusEngine&&)                 = delete;
        LotusEngine& operator=(LotusEngine&&)      = delete;

        /**
         * @brief Get the resolved rule (mode + delay overrides) for an application.
         * @param appName Application/program name.
         * @return The stored rule, or the global mode with zeroed delay overrides.
         */
        LotusAppRuleSetting getAppRuleSetting(const std::string& appName) const;

        /**
         * @brief Activates the input method for an input context.
         * @param entry Input method entry.
         * @param event Activation event.
         */
        void activate(const InputMethodEntry& entry, InputContextEvent& event) override;

        /**
         * @brief Deactivates the input method.
         * @param entry Input method entry.
         * @param event Deactivation event.
         */
        void deactivate(const InputMethodEntry& entry, InputContextEvent& event) override;

        /**
         * @brief Processes key events.
         * @param entry Input method entry.
         * @param keyEvent The key event to process.
         */
        void keyEvent(const InputMethodEntry& entry, KeyEvent& keyEvent) override;

        /**
         * @brief Resets the input method state.
         * @param entry Input method entry.
         * @param event Reset event.
         */
        void reset(const InputMethodEntry& entry, InputContextEvent& event) override;

        /**
         * @brief Reloads configuration from disk.
         */
        void reloadConfig() override;

        /**
         * @brief Gets the current configuration.
         * @return Pointer to configuration object.
         */
        const Configuration* getConfig() const override {
            return &config_;
        }

        /**
         * @brief Gets a sub-configuration by path.
         * @param path Sub-config path.
         * @return Pointer to sub-configuration.
         */
        const Configuration* getSubConfig(const std::string& path) const override;

        /**
         * @brief Applies configuration changes.
         * @param config New configuration.
         */
        void setConfig(const RawConfig& config) override;

        /**
         * @brief Applies sub-configuration changes.
         * @param path Sub-config path.
         * @param config New sub-configuration.
         */
        void setSubConfig(const std::string& path, const RawConfig& config) override;

        /**
         * @brief Gets the current sub-mode label.
         * @param entry Input method entry.
         * @param inputContext Current input context.
         * @return Mode label string.
         */
        std::string subMode(const InputMethodEntry& entry, InputContext& inputContext) override;

        /**
         * @brief Sets the current sub-mode icon.
         * @param entry Input method entry.
         * @param inputContext Current input context.
         * @return Icon name string.
         */
        std::string subModeIconImpl(
            const InputMethodEntry& entry, InputContext& inputContext) override;

        /**
         * @brief Sets the current sub-mode label.
         * @param entry Input method entry.
         * @param inputContext Current input context.
         * @return Label string.
         */
        std::string subModeLabelImpl(
            const InputMethodEntry& entry, InputContext& inputContext) override;

        /**
         * @brief Gets the current configuration.
         * @return Reference to lotus configuration.
         */
        const auto& config() const {
            return config_;
        }

        /**
         * @brief Gets the custom keymap configuration.
         * @return Reference to custom keymap.
         */
        const lotusCustomKeymap& customKeymap() const;

        /**
         * @brief Gets the dictionary shared with every input context.
         * @return Shared dictionary (null when no dictionary file was loaded).
         */
        const std::shared_ptr<const lotus::Dictionary>& dictionary() const {
            return dictionary_;
        }

        /**
         * @brief Preferences learned from reverts, shared with every input context.
         * @return Null when learning from reverts is off.
         */
        const std::shared_ptr<lotus::Learned>& learned() const {
            return learned_;
        }

        /** @brief Writes the learned preferences to their local file (0600). */
        void saveLearned() const;

        /**
         * @brief Gets the macro table shared with every input context.
         * @return Shared macro table.
         */
        const std::shared_ptr<const lotus::MacroTable>& macroTable() const {
            return macroTable_;
        }

        /**
         * @brief Gets the emoji loader.
         * @return Reference to emoji loader instance.
         */
        EmojiLoader& emojiLoader();

        /**
         * @brief Reports whether the AT-SPI focus is a Chromium address bar.
         * @return True when the focused accessibility node is the Omnibox of a Chromium browser.
         */
        [[nodiscard]] bool isLikeAddressBarChromium() const;

      private:
        Instance&                                            instance_;
        lotusConfig                                          config_;
        lotusCustomKeymap                                    customKeymap_;
        lotusCustomKeymap                                    emptyCustomKeymap_;
        lotusMacroTable                                      macroTables_;
        std::shared_ptr<const lotus::MacroTable>             macroTable_;
        lotusAppRules                                        appRulesTables_;
        KbSender                                             kbSender_;
        bool                                                 uinputTypesReturn_ = true;
        FactoryFor<LotusState>                               factory_;
        std::unique_ptr<SimpleAction>                        charsetAction_;
        std::vector<std::unique_ptr<SimpleAction>>           charsetSubAction_;
        std::unique_ptr<Menu>                                charsetMenu_;
        std::unique_ptr<SimpleAction>                        spellCheckAction_;
        std::unique_ptr<SimpleAction>                        macroAction_;
        std::unique_ptr<SimpleAction>                        capitalizeMacroAction_;
        std::unique_ptr<SimpleAction>                        autoNonVnRestoreAction_;
        std::unique_ptr<SimpleAction>                        enableDictionaryAction_;
        std::unique_ptr<SimpleAction>                        settingsAction_;
        std::vector<SimpleAction*>                           toggleActions_;
        std::vector<ScopedConnection>                        connections_;
        std::shared_ptr<const lotus::Dictionary>             dictionary_;
        std::shared_ptr<lotus::Learned>                      learned_;
        std::unordered_map<std::string, LotusAppRuleSetting> appRules_;
        std::string                                          appRulesPath_;
        bool                                                 isSelectingAppMode_ = false;
        std::string                                          currentConfigureApp_;
        std::unique_ptr<EventSourceTime>                     cycleModeNotificationTimer_;
        static constexpr uint64_t kCycleModeNotificationTimeoutUsec = 800000; // 800ms
        FCITX_ADDON_DEPENDENCY_LOADER(emoji, instance_.addonManager());
        std::unique_ptr<EmojiLoader>          emojiLoader_;
        std::unique_ptr<WindowFocusTracker>   windowFocusTracker_;
        bool                                  isGnome_ = false;
        mutable std::mutex                    appRulesMutex_;
        std::unordered_map<KeySym, LotusMode> modeMenuMapping_;
        std::unique_ptr<EventSourceIO>        mouseEventSource_;

        /** @brief (Re)loads the custom dictionary from disk. */
        void loadDictionary();

        /** @brief (Re)loads the preferences learned from reverts. */
        void loadLearned();

        /** @brief Refreshes the engine of every input context with the current settings. */
        void refreshEngine();

        /** @brief Refreshes the engine options of every input context. */
        void refreshOption();

        /** @brief Saves the current configuration to disk. */
        void saveConfig() {
            safeSaveAsIni(config_, "conf/vanh.conf");
        }

        /**
         * @brief Creates a toggle action bound to a boolean option.
         * @param action Action to create.
         * @param option Option the action toggles.
         * @param actionId Id the action is registered under.
         * @param iconName Icon of the action.
         * @param textLong Text shown in the menu.
         * @param textOnOff Text shown after the on/off mark.
         * @param uiManager UI manager that registers the action.
         */
        void initToggleAction(std::unique_ptr<SimpleAction>& action, Option<bool>& option,
            const std::string& actionId, const std::string& iconName, const std::string& textLong,
            const std::string& textOnOff, UserInterfaceManager& uiManager);

        /** @brief Updates the text of a toggle action; @p ic may be null. */
        static void updateAction(InputContext* ic, SimpleAction& action, Option<bool>& option,
            const std::string& textOnOff);

        /** @brief Updates the charset action UI; @p ic may be null. */
        void updateCharsetAction(InputContext* ic);

        /** @brief Refreshes the engine, the options and the actions after a config change. */
        void populateConfig();

        /** @brief Loads the application-specific mode rules. */
        void loadAppRules();

        /** @brief Saves the application-specific mode rules. */
        void saveAppRules() const;

        /** @brief Mode of an application: its rule, or the global mode. */
        LotusMode getAppRule(const std::string& appName) const;

        /** @brief Stores the mode of an application. */
        void setAppRule(const std::string& appName, LotusMode mode);

        /** @brief Removes the rule of an application so it follows the global Mode again. */
        void clearAppRule(const std::string& appName);

        /** @brief Applies a mode chosen in the application mode menu and closes the menu. */
        void applyAppMode(LotusMode mode, bool isDefault, InputContext* ic);

        /** @brief Shows the application mode selection menu. */
        void showAppModeMenu(InputContext* ic);

        /** @brief Closes the application mode menu and flushes the word buffer. */
        void closeAppModeMenu(InputContext* ic);

        /** @brief Shows a brief notification of the cycled mode. */
        void showCycleModeNotification(LotusMode mode, InputContext* ic);

        /** @brief Sets the current input mode; @p ic may be null. */
        void setMode(LotusMode mode, InputContext* ic);

        /** @brief Warns through the desktop when a uinput mode is selected without the server. */
        void checkServerForMode(LotusMode mode);

        /** @brief Name of the program of an input context, or a per-context name. */
        static std::string getProgramName(InputContext* ic);

        /** @brief Detects dark mode; false when light or when detection failed. */
        static bool isDarkMode();

        /** @brief Connects the mouse click socket and watches it on the fcitx event loop. */
        void setupMouseWatcher();
    };

} // namespace fcitx

#endif // _FCITX5_LOTUS_ENGINE_H_
