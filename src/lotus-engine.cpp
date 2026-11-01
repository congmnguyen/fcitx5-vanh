/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 * SPDX-FileCopyrightText: 2026 kimxuanhong <kimxuanhong@outlook.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
#include "lotus-engine.h"
#include "common-fd-guard.h"
#include "fcitx-utils/keysym.h"
#include "lotus-config.h"
#include "lotus-log.h"
#include "lotus-mode-menu.h"
#include "lotus-state.h"
#include "lotus-candidates.h"
#include "lotus-notify.h"
#include "lotus-utils.h"
#include "lotus-plasma-theme.h"
#include "vanh-migration.h"
#include "window_focus_tracker.h"

#include "dictionary.h"
#include "key_processor.h"
#include "emoji.h"
#include "macro_table.h"
#include "utf8.h"

#include <optional>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <fcitx-config/iniparser.h>
#include <fcitx/menu.h>
#include <fcitx/userinterfacemanager.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/utf8.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_set>

#include <fcntl.h>

namespace fcitx {
    constexpr const char* CharsetActionPrefix = "vanh-charset-";
    constexpr const char* CustomKeymapFile    = "conf/vanh-custom-keymap.conf";
    constexpr const char* MacroTableFile      = "conf/vanh-macro-table.conf";

    static std::shared_ptr<const lotus::MacroTable> buildMacroTable(const lotusMacroTable& table) {
        auto macros = std::make_shared<lotus::MacroTable>();
        for (const auto& keymap : *table.macros) {
            macros->set(*keymap.key, *keymap.value);
        }
        return macros;
    }

    static std::string learnedPath() {
        return lotusConfigDir() + "/vanh-learned.tsv";
    }

    EmojiLoader& LotusEngine::emojiLoader() {
        if (!emojiLoader_) {
            emojiLoader_ = std::make_unique<EmojiLoader>(&instance_.addonManager());
        }
        return *emojiLoader_;
    }

    bool LotusEngine::isDarkMode() {
        // Each probe spawns subprocesses, and subModeIconImpl calls this on
        // every tray update while IconTheme is Auto.  Cache the result briefly
        // so the cost is paid at most once per few seconds.
        static int64_t lastCheckMs = 0;
        static bool    cachedValue = false;
        const int64_t  now         = nowMs();
        if (now - lastCheckMs < 5000) {
            return cachedValue;
        }
        lastCheckMs = now;
        cachedValue = []() {
            // KDE Plasma: the tray sits on the panel, painted by the Plasma Style,
            // while the portal below reports the application colour scheme.  The
            // two differ in the default Fedora/Kubuntu look (#374).
            if (isKdePlasmaSession(getEnv("XDG_CURRENT_DESKTOP"))) {
                if (const auto dark = isPlasmaPanelDark(plasmaThemeSearchPathsFromEnv())) {
                    return *dark;
                }
            }

            // GTK_THEME is honored by lightweight DEs that lack the settings
            // portal; covers XFCE, openbox, etc. with a dark theme.
            std::string theme = getEnv("GTK_THEME");
            std::ranges::transform(theme, theme.begin(), ::tolower);
            if (theme.find("dark") != std::string::npos) {
                return true;
            }

            const auto openPipe = [](const char* command) {
                return std::unique_ptr<FILE, int (*)(FILE*)>(popen(command, "r"), pclose);
            };
            std::array<char, 256> buffer{};
            auto                  pipe =
                openPipe("dbus-send --session --dest=org.freedesktop.portal.Desktop --print-reply "
                         "/org/freedesktop/portal/desktop org.freedesktop.portal.Settings.ReadOne "
                         "string:'org.freedesktop.appearance' string:'color-scheme' 2>/dev/null");
            while (pipe && fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                uint32_t value = 0;
                // The sscanf return value is checked and the value is only compared against 1.
                // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
                if (sscanf(buffer.data(), "%*[^v]variant uint32 %u", &value) == 1) {
                    return value == 1;
                }
            }

            pipe = openPipe("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null");
            if (pipe && fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                return strstr(buffer.data(), "prefer-dark") != nullptr;
            }

            // GTK settings file — covers DEs where the dark preference is stored
            // there instead of being exposed via portal/gsettings.
            const std::string home = getEnv("HOME");
            if (home.empty()) {
                return false;
            }
            std::ifstream settingsFile(home + "/.config/gtk-3.0/settings.ini");
            std::string   line;
            while (std::getline(settingsFile, line)) {
                if (line.find("gtk-application-prefer-dark-theme=1") != std::string::npos) {
                    return true;
                }
            }
            return false;
        }();
        return cachedValue;
    }

    bool LotusEngine::isLikeAddressBarChromium() const {
        return windowFocusTracker_->isLikeAddressBarChromium();
    }

    LotusEngine::LotusEngine(Instance* instance) :
        instance_(*instance), kbSender_(buildSocketPath(VANH_KB_SOCKET_FILE)),
        factory_([this](InputContext& ic) {
            return new LotusState(this, &ic);
        }) { // NOLINT
        windowFocusTracker_ = std::make_unique<WindowFocusTracker>();
        windowFocusTracker_->start();
        isGnome_ = getEnv("XDG_CURRENT_DESKTOP").find("GNOME") != std::string::npos;

        auto& uiManager = instance_.userInterfaceManager();

        charsetAction_ = std::make_unique<SimpleAction>();
        charsetAction_->setShortText(_("Charset"));
        charsetAction_->setIcon("character-set");
        uiManager.registerAction("vanh-charset", charsetAction_.get());
        charsetMenu_ = std::make_unique<Menu>();
        charsetAction_->setMenu(charsetMenu_.get());

        for (std::size_t i = 0; i < lotus::kCharsetCount; ++i) {
            const auto        charset = static_cast<lotus::Charset>(i);
            const std::string name    = CharsetToString(charset);
            auto* action = charsetSubAction_.emplace_back(std::make_unique<SimpleAction>()).get();
            action->setShortText(name);
            action->setCheckable(true);
            uiManager.registerAction(stringutils::concat(CharsetActionPrefix, name), action);
            connections_.emplace_back(
                action->connect<SimpleAction::Activated>([this, charset](InputContext* ic) {
                    if (config_.outputCharset.value() == charset) {
                        return;
                    }
                    config_.outputCharset.setValue(charset);
                    saveConfig();
                    refreshEngine();
                    updateCharsetAction(ic);
                    if (ic) {
                        ic->updateUserInterface(UserInterfaceComponent::StatusArea);
                    }
                }));
            charsetMenu_->addAction(action);
        }

        initToggleAction(spellCheckAction_, config_.spellCheck, "vanh-spellcheck",
            "tools-check-spelling", _("Spell Check"), _("Spell Check"), uiManager);
        initToggleAction(macroAction_, config_.enableMacro, "vanh-macro", "document-edit",
            _("Macro"), _("Macro"), uiManager);
        initToggleAction(capitalizeMacroAction_, config_.capitalizeMacro, "vanh-capitalizemacro",
            "format-text-uppercase", _("Capitalize Macro"), _("Capitalize Macro"), uiManager);
        initToggleAction(autoNonVnRestoreAction_, config_.autoNonVnRestore, "vanh-autonvnrestore",
            "edit-undo", _("Auto Restore Invalid Words"), _("Auto Non-VN Restore"), uiManager);
        initToggleAction(enableDictionaryAction_, config_.enableDictionary, "vanh-dictionary",
            "accessories-dictionary", _("Custom Dictionary"), _("Custom Dictionary"), uiManager);

        settingsAction_ = std::make_unique<SimpleAction>();
        settingsAction_->setShortText(_("Settings"));
        settingsAction_->setIcon("configure");
        connections_.emplace_back(
            settingsAction_->connect<SimpleAction::Activated>([](InputContext*) {
                startProcess({FCITX5_LOTUS_SETTINGS_PATH});
            }));
        uiManager.registerAction("vanh-settings", settingsAction_.get());

        const std::string configDir = lotusConfigDir();
        std::filesystem::create_directories(configDir);
        appRulesPath_ = configDir + "/vanh-app-rules.conf";
        // Carry the files of the former "lotus" addon over before the first read. The vanh-migrate
        // module normally did this already at fcitx5 startup; repeating it is a no-op.
        for (const auto& line : vanh::migration::migrateUserFiles(
                 vanh::migration::userConfigDir(), vanh::migration::userDataDir())) {
            LOTUS_INFO(line);
        }
        reloadConfig();
        setMode(config_.mode.value(), nullptr);
        checkServerForMode(config_.mode.value());
        instance_.inputContextManager().registerProperty("LotusState", &factory_);
        toggleActions_ = {
            charsetAction_.get(),
            spellCheckAction_.get(),
            macroAction_.get(),
            capitalizeMacroAction_.get(),
            autoNonVnRestoreAction_.get(),
            enableDictionaryAction_.get(),
            settingsAction_.get(),
        };
    }

    void LotusEngine::initToggleAction(std::unique_ptr<SimpleAction>& action, Option<bool>& option,
        const std::string& actionId, const std::string& iconName, const std::string& textLong,
        const std::string& textOnOff, UserInterfaceManager& uiManager) {
        action = std::make_unique<SimpleAction>();
        action->setShortText(textLong);
        action->setIcon(iconName);
        action->setCheckable(false);
        auto* const actionPtr = action.get();
        auto* const optionPtr = &option;
        connections_.emplace_back(action->connect<SimpleAction::Activated>(
            [this, actionPtr, optionPtr, textOnOff](InputContext* ic) {
                optionPtr->setValue(!optionPtr->value());
                saveConfig();
                refreshOption();
                updateAction(ic, *actionPtr, *optionPtr, textOnOff);
            }));
        uiManager.registerAction(actionId, action.get());
    }

    void LotusEngine::updateAction(InputContext* ic, SimpleAction& action, Option<bool>& option,
        const std::string& textOnOff) {
        action.setShortText((option.value() ? "✔ " : "✖ ") + textOnOff);
        if (ic != nullptr) {
            action.update(ic);
        }
    }

    LotusEngine::~LotusEngine() {
        g_stopFlagMonitor.store(true, std::memory_order_release);
        mouseEventSource_.reset();
        FdGuard fd(g_mouseSocketFd.exchange(-1, std::memory_order_acq_rel));
        if (fd.isValid()) {
            shutdown(fd.get(), SHUT_RDWR);
        }
        LOTUS_INFO("Engine destroyed.");
    }

    const lotusCustomKeymap& LotusEngine::customKeymap() const {
        return config_.enableCustomKeymap.value() ? customKeymap_ : emptyCustomKeymap_;
    }

    void LotusEngine::reloadConfig() {
        readAsIni(config_, "conf/vanh.conf");
        readAsIni(customKeymap_, CustomKeymapFile);
        readAsIni(macroTables_, MacroTableFile);
        macroTable_ = buildMacroTable(macroTables_);
        loadDictionary();
        loadLearned();
        loadAppRules();
        populateConfig();
    }

    void LotusEngine::loadDictionary() {
        auto dictionary = std::make_shared<lotus::Dictionary>();
        // Go read the descriptor handed to it; the port reads the same descriptor into the text
        // Dictionary::loadFromText expects.
        auto loadFromFd = [&dictionary](int rawFd) {
            FdGuard                fd(rawFd);
            std::string            contents;
            std::array<char, 4096> buffer{};
            ssize_t                count = 0;
            while ((count = ::read(fd.get(), buffer.data(), buffer.size())) > 0) {
                contents.append(buffer.data(), static_cast<std::size_t>(count));
            }
            dictionary->loadFromText(contents);
        };
        if (config_.enableDictionary.value()) {
#if LOTUS_USE_MODERN_FCITX_API
            auto fd =
                StandardPaths::global().open(StandardPathsType::PkgData, "vanh/vietnamese.cm.dict");
#else
            auto fd = StandardPath::global().open(
                StandardPath::Type::PkgData, "vanh/vietnamese.cm.dict", O_RDONLY);
#endif
            if (fd.isValid()) {
                loadFromFd(fd.release());
            }
        } else {
#if LOTUS_USE_MODERN_FCITX_API
            auto paths = StandardPaths::global().locateAll(
                StandardPathsType::PkgData, "vanh/vietnamese.cm.dict");
#else
            auto paths = StandardPath::global().locateAll(
                StandardPath::Type::PkgData, "vanh/vietnamese.cm.dict");
#endif
            for (const auto& p : paths) {
#if LOTUS_USE_MODERN_FCITX_API
                if (!p.string().starts_with("/home/")) {
                    auto fd = fcitx::UnixFD(::open(p.c_str(), O_RDONLY));
                    if (fd.isValid()) {
                        loadFromFd(fd.release());
#else
                if (!p.starts_with("home/")) {
                    int fd = ::open(p.c_str(), O_RDONLY);
                    if (fd != -1) {
                        loadFromFd(fd);
#endif
                        break;
                    }
                }
            }
        }
        dictionary_ = std::move(dictionary);
    }

    void LotusEngine::loadLearned() {
        if (!*config_.learnFromReverts) {
            learned_.reset();
            return;
        }
        learned_ = std::make_shared<lotus::Learned>();
        std::ifstream file(learnedPath());
        if (file) {
            const std::istreambuf_iterator<char> begin(file);
            learned_->loadFromText(std::string(begin, {}));
        }
    }

    void LotusEngine::saveLearned() const {
        if (!learned_) {
            return;
        }
        // Written to a private temporary file, then renamed over the old one.
        std::string tmp = learnedPath() + ".tmp.XXXXXX";
        const int   fd  = ::mkstemp(tmp.data());
        if (fd < 0) {
            return;
        }
        const std::string text = learned_->toText();
        const bool        ok   = ::fchmod(fd, 0600) == 0 &&
            ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
        ::close(fd);
        if (!ok || ::rename(tmp.c_str(), learnedPath().c_str()) != 0) {
            ::unlink(tmp.c_str());
        }
    }

    const Configuration* LotusEngine::getSubConfig(const std::string& path) const {
        if (path == "custom_keymap") {
            return &customKeymap_;
        }
        if (path == "vanh-macro") {
            return &macroTables_;
        }
        if (path == "app_rules") {
            return &appRulesTables_;
        }
        return nullptr;
    }

    void LotusEngine::setConfig(const RawConfig& config) {
        config_.load(config, true);
        saveConfig();
        loadDictionary();
        loadLearned();
        populateConfig();
    }

    void LotusEngine::populateConfig() {
        refreshEngine();
        refreshOption();
        updateCharsetAction(nullptr);
        updateAction(nullptr, *spellCheckAction_, config_.spellCheck, _("Spell Check"));
        updateAction(nullptr, *macroAction_, config_.enableMacro, _("Macro"));
        updateAction(
            nullptr, *capitalizeMacroAction_, config_.capitalizeMacro, _("Capitalize Macro"));
        updateAction(
            nullptr, *autoNonVnRestoreAction_, config_.autoNonVnRestore, _("Auto Non-VN Restore"));
        updateAction(
            nullptr, *enableDictionaryAction_, config_.enableDictionary, _("Custom Dictionary"));
    }

    void LotusEngine::setSubConfig(const std::string& path, const RawConfig& config) {
        if (path == "custom_keymap") {
            customKeymap_.load(config, true);
            safeSaveAsIni(customKeymap_, CustomKeymapFile);
            refreshEngine();
        } else if (path == "vanh-macro") {
            macroTables_.load(config, true);
            safeSaveAsIni(macroTables_, MacroTableFile);
            macroTable_ = buildMacroTable(macroTables_);
            refreshEngine();
        } else if (path == "app_rules") {
            appRulesTables_.load(config, true);
            {
                std::scoped_lock lock(appRulesMutex_);
                std::erase_if(appRules_, [](const auto& rule) {
                    return !rule.first.starts_with("ctx_");
                });
                for (const auto& rule : *appRulesTables_.rules) {
                    appRules_[*rule.app] = {
                        .mode              = intToMode(*rule.mode),
                        .commitInterval    = *rule.commitInterval,
                        .backspaceInterval = *rule.backspaceInterval,
                        .postDelay         = *rule.postDelay,
                    };
                }
            }
            saveAppRules();
            refreshEngine();
        }
    }

    std::string LotusEngine::subMode(const InputMethodEntry&, InputContext&) {
        return InputMethodIdToString(*config_.inputMethod);
    }

    void LotusEngine::activate(const InputMethodEntry&, InputContextEvent& event) {
        auto*      ic        = event.inputContext();
        const bool surrvalid = ic->surroundingText().isValid();
        const bool isDbus    = getFrontendName(ic) == "dbus";
        setupMouseWatcher();

        if (ic->capabilityFlags().test(CapabilityFlag::Preedit)) {
            instance_.inputContextManager().setPreeditEnabledByDefault(true);
        }

        const std::string appName = getProgramName(ic);
        LOTUS_INFO("App name: " + appName);

        const LotusMode targetMode = getAppRule(appName);
        LOTUS_INFO("Target mode: " + LotusModeI18NAnnotation::toString(targetMode));

        updateCharsetAction(ic);

        auto* state           = ic->propertyFor(&factory_);
        state->isLibreoffice_ = (appName == "soffice");
        state->isChromium_    = isChromiumProgram(appName);
        state->finishOverdueReplacement();

        const bool focusBounce = isUinputMode(targetMode) && state->lastDeactivateTime_ > 0 &&
            nowMs() - state->lastDeactivateTime_ < 100;
        const bool resumeReplacement = focusBounce && state->deletionInterruptedAt_ > 0 &&
            g_isDeleting.load(std::memory_order_acquire);
        state->deletionInterruptedAt_ = 0;

        if (!resumeReplacement) {
            state->cancelPendingUinput();
            g_isDeleting.store(false, std::memory_order_release);
        }

        if (focusBounce) {
            state->cancelDeferredFocusOut();
            setRealMode(targetMode);
            ic->updateUserInterface(UserInterfaceComponent::StatusArea);
            LOTUS_INFO("Focus bounce: keep word buffers");
        } else {
            state->flushDeferredFocusOut();
            state->surroundingBroken_ = false;
            setMode(targetMode, ic);
        }
        if (event.type() == EventType::InputContextFocusIn && isDbus && !surrvalid) {
            LOTUS_INFO("Skip clearAllBuffers");
        } else if (surrvalid && !state->oldPreBuffer_.empty() &&
            nowMs() - state->lastDeactivateTime_ >= 100) {
            state->clearAllBuffers();
        }
        if (!resumeReplacement) {
            g_isDeleting.store(false, std::memory_order_release);
        }
        if (targetMode == LotusMode::Emoji) {
            state->updateEmojiPreedit();
        } else {
            ic->inputPanel().reset();
            ic->updateUserInterface(UserInterfaceComponent::InputPanel);
            const LotusMode activeMode = currentMode();
            if (activeMode == LotusMode::Preedit || activeMode == LotusMode::SurroundingText) {
                ic->updatePreedit();
            }
        }
        for (const auto& action : toggleActions_) {
            ic->statusArea().addAction(StatusGroup::InputMethod, action);
        }
    }

    void LotusEngine::keyEvent(const InputMethodEntry&, KeyEvent& keyEvent) {
        auto* ic    = keyEvent.inputContext();
        auto* state = ic->propertyFor(&factory_);

        if (ic->capabilityFlags().testAny(CapabilityFlag::Password)) {
            state->reset();
            ic->inputPanel().reset();
            ic->updateUserInterface(UserInterfaceComponent::InputPanel);
            return;
        }

        if (isSelectingAppMode_ && g_mouseClicked.load(std::memory_order_acquire)) {
            g_mouseClicked.store(false, std::memory_order_release);
            closeAppModeMenu(ic);
        }

        if (isSelectingAppMode_) {
            if (keyEvent.isRelease()) {
                return;
            }
            keyEvent.filterAndAccept();
            const KeySym keySym = keyEvent.key().sym();
            auto         menuList =
                std::dynamic_pointer_cast<CommonCandidateList>(ic->inputPanel().candidateList());
            const auto moveCursor = [&](int delta) {
                if (!menuList || menuList->totalSize() <= 1) {
                    return;
                }
                int cursorIndex = menuList->globalCursorIndex();
                if (cursorIndex < 0 || cursorIndex >= menuList->totalSize()) {
                    cursorIndex = 0;
                }
                int nextIndex = cursorIndex + delta;
                if (nextIndex < 0) {
                    nextIndex = menuList->totalSize() - 1;
                } else if (nextIndex >= menuList->totalSize()) {
                    nextIndex = 0;
                }
                menuList->setGlobalCursorIndex(nextIndex);
                ic->updateUserInterface(UserInterfaceComponent::InputPanel);
            };
            switch (keySym) {
                case FcitxKey_Tab:
                case FcitxKey_Down:
                    moveCursor(1);
                    return;
                case FcitxKey_ISO_Left_Tab:
                case FcitxKey_Up:
                    moveCursor(-1);
                    return;
                case FcitxKey_space:
                case FcitxKey_Return:
                    if (menuList && !menuList->empty()) {
                        int selectedIndex = menuList->globalCursorIndex();
                        if (selectedIndex < 0 || selectedIndex >= menuList->totalSize()) {
                            selectedIndex = 0;
                        }
                        menuList->candidateFromAll(selectedIndex).select(ic);
                    }
                    return;
                case FcitxKey_Escape:
                    isSelectingAppMode_ = false;
                    ic->inputPanel().reset();
                    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                    return;
                default:
                    break;
            }
            if (const auto it = modeMenuMapping_.find(keySym); it != modeMenuMapping_.end()) {
                const bool isDefault =
                    it->second != LotusMode::Emoji && keySym == Key(*config_.shortcutDefault).sym();
                LOTUS_INFO("Selected mode: " + LotusModeI18NAnnotation::toString(it->second));
                applyAppMode(it->second, isDefault, ic);
                return;
            }
            const auto& hotkeys = *config_.modeMenuKey;
            if (hotkeys.size() == 1 && !hotkeys[0].hasModifier()) {
                const std::string charStr = Key::keySymToUTF8(hotkeys[0].sym());
                if (!charStr.empty() &&
                    keySym == typeKeyForModeMenuHotkey(hotkeys[0].sym(), config_)) {
                    closeAppModeMenu(ic);
                    ic->commitString(charStr);
                }
            }
            return;
        }

        if (!keyEvent.isRelease() && !config_.cycleModeKey->empty() &&
            keyEvent.key().checkKeyList(*config_.cycleModeKey)) {
            LOTUS_INFO("Cycle mode key pressed");
            const std::string      appName = getProgramName(ic);
            const LotusMode        appMode = getAppRule(appName);
            std::vector<LotusMode> enabledModes;
            for (const auto& entry : modeOrderEntries(config_)) {
                if (entry.visible &&
                    std::ranges::find(enabledModes, entry.mode) == enabledModes.end()) {
                    enabledModes.push_back(entry.mode);
                }
            }
            if (!enabledModes.empty()) {
                const auto      found    = std::ranges::find(enabledModes, appMode);
                const LotusMode nextMode = found == enabledModes.end() ?
                    enabledModes[0] :
                    enabledModes[(found - enabledModes.begin() + 1) % enabledModes.size()];
                setMode(nextMode, ic);
                setAppRule(appName, nextMode);
                showCycleModeNotification(nextMode, ic);
            }
            keyEvent.filterAndAccept();
            return;
        }

        if (!keyEvent.isRelease() && !config_.modeMenuKey->empty() &&
            keyEvent.key().checkKeyList(*config_.modeMenuKey)) {
            LOTUS_INFO("Mode menu key pressed");
            state->commitBuffer();
            state->reset();
            currentConfigureApp_ = getProgramName(ic);
            g_mouseClicked.store(false, std::memory_order_release);
            setMode(getAppRule(currentConfigureApp_), ic);
            showAppModeMenu(ic);
            keyEvent.filterAndAccept();
            return;
        }

        state->keyEvent(keyEvent);
        const auto&  surrounding = ic->surroundingText();
        const size_t textLen     = lotus::utf8::length(surrounding.text());
        if (textLen == static_cast<size_t>(surrounding.cursor())) {
            g_realTextLen.store(static_cast<unsigned int>(textLen), std::memory_order_release);
        }
    }

    void LotusEngine::reset(const InputMethodEntry&, InputContextEvent& event) {
        if (g_isDeleting.load(std::memory_order_acquire)) {
            LOTUS_INFO("Ignore reset while is_deleting");
            return;
        }
        LOTUS_INFO("Reset engine");
        auto*      state    = event.inputContext()->propertyFor(&factory_);
        const bool focusOut = event.type() == EventType::InputContextFocusOut;
        if (!state->isEmptyHistory() && !focusOut) {
            return;
        }
        if (focusOut || event.type() == EventType::InputContextReset) {
            state->reset(focusOut);
        }
    }

    void LotusEngine::deactivate(const InputMethodEntry&, InputContextEvent& event) {
        auto*      ic         = event.inputContext();
        auto*      state      = ic->propertyFor(&factory_);
        const bool uinputMode = isUinputMode(currentMode());

        if (uinputMode && g_isDeleting.load(std::memory_order_acquire) &&
            state->expectedBackspaces_ > 0) {
            state->lastDeactivateTime_    = nowMs();
            state->deletionInterruptedAt_ = state->lastDeactivateTime_;
            if (state->realSelect_) {
                // Unfocused injected keys are never echoed back: cancel the burst right away.
                state->abortPendingUinputReplacement();
            }
            LOTUS_INFO("Replacement interrupted by focus out");
            return;
        }

        const bool surrvalid       = ic->surroundingText().isValid();
        const bool isDbus          = getFrontendName(ic) == "dbus";
        const bool focusOut        = event.type() == EventType::InputContextFocusOut;
        state->lastDeactivateTime_ = nowMs();
        if (focusOut) {
            state->forgetShiftState();
        }
        if (currentMode() == LotusMode::Preedit && !focusOut) {
            state->commitBuffer();
            return;
        }
        if (focusOut && isDbus && !surrvalid) {
            LOTUS_INFO("Skip clearAllBuffers");
        } else if ((!uinputMode || !focusOut) && surrvalid && !state->oldPreBuffer_.empty()) {
            state->clearAllBuffers();
        }

        g_isDeleting.store(false, std::memory_order_release);
        ic->inputPanel().reset();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        const LotusMode mode = currentMode();
        if (mode == LotusMode::Preedit || mode == LotusMode::Emoji ||
            mode == LotusMode::SurroundingText) {
            ic->updatePreedit();
        }
    }

    void LotusEngine::refreshEngine() {
        if (!factory_.registered()) {
            return;
        }
        instance_.inputContextManager().foreach ([this](InputContext* ic) {
            auto* state = ic->propertyFor(&factory_);
            state->setEngine();
            if (ic->hasFocus()) {
                // Re-resolve the focused window's rule; setEngine() must not
                // reset it to the global mode.
                setMode(getAppRule(getProgramName(ic)), ic);
                state->reset();
            }
            return true;
        });
    }

    void LotusEngine::refreshOption() {
        if (!factory_.registered()) {
            return;
        }
        instance_.inputContextManager().foreach ([this](InputContext* ic) {
            auto* state = ic->propertyFor(&factory_);
            state->setOption();
            if (ic->hasFocus()) {
                state->reset();
            }
            return true;
        });
    }

    void LotusEngine::updateCharsetAction(InputContext* ic) {
        const auto name =
            stringutils::concat(CharsetActionPrefix, CharsetToString(*config_.outputCharset));
        for (const auto& action : charsetSubAction_) {
            action->setChecked(action->name() == name);
            if (ic != nullptr) {
                action->update(ic);
            }
        }
    }

    void LotusEngine::loadAppRules() {
        std::scoped_lock lock(appRulesMutex_);
        std::erase_if(appRules_, [](const auto& rule) {
            return !rule.first.starts_with("ctx_");
        });
        std::ifstream file(appRulesPath_);
        std::string   line;
        while (std::getline(file, line)) {
            const auto delimiterPos = line.find('=');
            if (line.empty() || line[0] == '#' || delimiterPos == std::string::npos) {
                continue;
            }
            const std::string app   = line.substr(0, delimiterPos);
            const auto        parts = stringutils::split(line.substr(delimiterPos + 1), ",");
            if (parts.empty()) {
                LOTUS_WARN("Invalid mode value for app: " + app);
                continue;
            }
            const auto field = [&parts](size_t index) -> int {
                if (parts.size() <= index || parts[index].empty()) {
                    return 0;
                }
                return std::max(std::stoi(parts[index]), 0);
            };
            try {
                const LotusAppRuleSetting setting{
                    .mode              = intToMode(std::stoi(parts[0])),
                    .commitInterval    = field(1),
                    .backspaceInterval = field(2),
                    .postDelay         = field(3),
                };
                appRules_[app] = setting;
            } catch (const std::exception&) {
                LOTUS_WARN("Invalid mode value for app: " + app);
            }
        }

        std::vector<lotusAppRule> rules;
        for (const auto& [app, setting] : appRules_) {
            if (app.starts_with("ctx_")) {
                continue;
            }
            lotusAppRule& rule = rules.emplace_back();
            rule.app.setValue(app);
            rule.mode.setValue(modeToInt(setting.mode));
            rule.commitInterval.setValue(setting.commitInterval);
            rule.backspaceInterval.setValue(setting.backspaceInterval);
            rule.postDelay.setValue(setting.postDelay);
        }
        appRulesTables_.rules.setValue(std::move(rules));
    }

    void LotusEngine::saveAppRules() const {
        std::ofstream file(appRulesPath_, std::ios::trunc);
        if (!file.is_open()) {
            return;
        }
        file << "# Vanh Per-App Configuration\n";
        file << "# 0 = Off, 1 = Uinput (Smooth), 2 = Uinput (Smooth, legacy), 3 = Uinput (Super "
                "Smooth), 4 = Surrounding Text, 5 = Preedit,\n";
        file << "# 6 = Emoji Picker, 7 = Uinput (Select)\n";
        file << "# app=mode[,commit_interval_ms[,backspace_interval_ms[,post_delay_ms]]]   "
                "0/omitted = use defaults\n";
        std::scoped_lock lock(appRulesMutex_);
        for (const auto& [app, setting] : appRules_) {
            if (app.starts_with("ctx_")) {
                continue;
            }
            const int   commit    = std::max(setting.commitInterval, 0);
            const int   backspace = std::max(setting.backspaceInterval, 0);
            const int   post      = setting.postDelay;
            std::string value     = std::to_string(modeToInt(setting.mode));
            if (commit > 0 || backspace > 0 || post > 0) {
                value += "," + std::to_string(commit);
            }
            if (backspace > 0 || post > 0) {
                value += "," + std::to_string(backspace);
            }
            if (post > 0) {
                value += "," + std::to_string(post);
            }
            file << app << "=" << value << "\n";
        }
    }

    LotusAppRuleSetting LotusEngine::getAppRuleSetting(const std::string& appName) const {
        std::scoped_lock lock(appRulesMutex_);
        const auto       it = appRules_.find(appName);
        if (it != appRules_.end()) {
            return it->second;
        }
        return {.mode = config_.mode.value()};
    }

    LotusMode LotusEngine::getAppRule(const std::string& appName) const {
        return getAppRuleSetting(appName).mode;
    }

    void LotusEngine::setAppRule(const std::string& appName, LotusMode mode) {
        auto rules = *appRulesTables_.rules;
        auto rule  = std::ranges::find_if(rules, [&](const auto& r) {
            return *r.app == appName;
        });
        if (rule == rules.end()) {
            rule = rules.insert(rules.end(), lotusAppRule{});
            rule->app.setValue(appName);
        }
        rule->mode.setValue(modeToInt(mode));
        {
            std::scoped_lock lock(appRulesMutex_);
            appRules_[appName].mode = mode;
        }
        appRulesTables_.rules.setValue(std::move(rules));
    }

    void LotusEngine::closeAppModeMenu(InputContext* ic) {
        isSelectingAppMode_ = false;
        ic->inputPanel().reset();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
        auto* state = ic->propertyFor(&factory_);
        state->commitBuffer();
        state->reset();
    }

    void LotusEngine::applyAppMode(LotusMode mode, bool isDefault, InputContext* ic) {
        if (mode != LotusMode::Emoji) {
            if (isDefault) {
                clearAppRule(currentConfigureApp_);
            } else {
                setAppRule(currentConfigureApp_, mode);
                if (!currentConfigureApp_.starts_with("ctx_")) {
                    saveAppRules();
                }
            }
        }
        closeAppModeMenu(ic);
        setMode(mode, ic);
        if (mode == LotusMode::Emoji) {
            ic->propertyFor(&factory_)->updateEmojiPreedit();
        } else {
            showCycleModeNotification(mode, ic);
        }
    }

    void LotusEngine::showAppModeMenu(InputContext* ic) {
        isSelectingAppMode_ = true;

        auto candidateList = std::make_unique<CommonCandidateList>();
        candidateList->setLayoutHint(CandidateLayoutHint::Vertical);
        candidateList->setPageSize(10);

        int                        activeSelectionIdx  = -1;
        int                        currentCandidateIdx = 0;
        std::unordered_set<KeySym> usedModeKeys;
        modeMenuMapping_.clear();
        const LotusMode defaultMode = config_.mode.value();

        for (const auto& entry : modeOrderEntries(config_)) {
            if (!entry.visible) {
                continue;
            }
            const bool hasShortcut = entry.key != FcitxKey_None && entry.key != FcitxKey_VoidSymbol;
            if (hasShortcut && usedModeKeys.insert(entry.key).second) {
                modeMenuMapping_[entry.key] = entry.mode;
            }

            const std::string keyUtf8  = Key::keySymToUTF8(entry.key);
            const std::string keyLabel = keyUtf8.empty() ? "" : "[" + keyUtf8 + "] ";
            const std::string label    = keyLabel + modeLabel(entry);
            candidateList->append(std::make_unique<AppModeCandidateWord>(
                Text((entry.mode == currentMode() ? ">> " : "   ") + label),
                [this, mode = entry.mode, isDefault = entry.isDefault](InputContext* ic) {
                    applyAppMode(mode, isDefault, ic);
                }));

            const bool selectedByMode    = entry.mode == currentMode() && !entry.isDefault;
            const bool selectedByDefault = entry.isDefault &&
                getAppRule(currentConfigureApp_) == defaultMode &&
                !appRules_.contains(currentConfigureApp_);
            if (selectedByMode || selectedByDefault) {
                activeSelectionIdx = currentCandidateIdx;
            }
            currentCandidateIdx++;
        }

        const auto& hotkeys = *config_.modeMenuKey;
        if (hotkeys.size() == 1 && !hotkeys[0].hasModifier()) {
            const std::string charStr = Key::keySymToUTF8(hotkeys[0].sym());
            if (!charStr.empty()) {
                const KeySym      typeKeySym = typeKeyForModeMenuHotkey(hotkeys[0].sym(), config_);
                const std::string label =
                    "[" + Key::keySymToUTF8(typeKeySym) + "] " + _("Type") + " " + charStr;
                candidateList->append(std::make_unique<AppModeCandidateWord>(
                    Text(label), [this, charStr](InputContext* ic) {
                        closeAppModeMenu(ic);
                        ic->commitString(charStr);
                    }));
            }
        }

        if (activeSelectionIdx != -1) {
            candidateList->setGlobalCursorIndex(activeSelectionIdx);
        } else if (candidateList->totalSize() > 0) {
            candidateList->setGlobalCursorIndex(0);
        }

        ic->inputPanel().reset();
        ic->inputPanel().setCandidateList(std::move(candidateList));
        ic->inputPanel().setAuxDown(Text(_("App: ") + currentConfigureApp_));
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void LotusEngine::showCycleModeNotification(LotusMode mode, InputContext* ic) {
        auto candidateList = std::make_unique<CommonCandidateList>();
        candidateList->setLayoutHint(CandidateLayoutHint::Vertical);
        candidateList->setPageSize(1);
        candidateList->append(std::make_unique<AppModeCandidateWord>(
            Text("✓ " + LotusModeI18NAnnotation::toString(mode)), [](InputContext* ic) {
                ic->inputPanel().reset();
                ic->updateUserInterface(UserInterfaceComponent::InputPanel);
            }));
        candidateList->setGlobalCursorIndex(0);

        ic->inputPanel().reset();
        ic->inputPanel().setCandidateList(std::move(candidateList));
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);

        cycleModeNotificationTimer_ = instance_.eventLoop().addTimeEvent(CLOCK_MONOTONIC,
            ::fcitx::now(CLOCK_MONOTONIC) + kCycleModeNotificationTimeoutUsec, 0,
            [icRef = ic->watch()](EventSourceTime*, uint64_t) {
                if (auto* ic = icRef.get(); ic && ic->hasFocus()) {
                    ic->inputPanel().reset();
                    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                }
                return false;
            });
    }

    void LotusEngine::checkServerForMode(LotusMode mode) {
        if (isUinputMode(mode) && !kbSender_.ensureConnected()) {
            notifyMissingUinputServer();
        }
    }

    void LotusEngine::setMode(LotusMode mode, InputContext* ic) {
        const bool changed = currentMode() != mode;
        setRealMode(mode);
        if (ic != nullptr) {
            auto* state            = ic->propertyFor(&factory_);
            state->appRuleSetting_ = getAppRuleSetting(getProgramName(ic));
            state->clearAllBuffers();
            ic->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
        if (changed) {
            checkServerForMode(mode);
        }
    }

    std::string LotusEngine::subModeIconImpl(const InputMethodEntry&, InputContext&) {
        std::string icon = "fcitx-vanh";
        switch (currentMode()) {
            case LotusMode::Off:
                icon = "fcitx-vanh-off";
                break;
            case LotusMode::Emoji:
                icon = "fcitx-vanh-emoji";
                break;
            default:
                break;
        }
        if (*config_.useLotusIcons) {
            return icon;
        }
        const auto& iconTheme = config_.iconTheme.value();
        if (iconTheme == IconTheme::Light) {
            return icon + "-default-black";
        }
        if (iconTheme == IconTheme::Dark) {
            return icon + "-default";
        }
        if (isGnome_) {
            // GNOME's panel can be dark while applications prefer light.
            // Symbolic icons inherit the panel foreground, including when
            // its Shell theme changes, without guessing from app settings.
            return icon + "-default-symbolic";
        }
        return icon + (isDarkMode() ? "-default" : "-default-black");
    }

    std::string LotusEngine::subModeLabelImpl(const InputMethodEntry&, InputContext&) {
        switch (currentMode()) {
            case LotusMode::Off:
                return _("Vanh - Off");
            case LotusMode::Emoji:
                return "😄";
            default:
                return "vi";
        }
    }

    std::string LotusEngine::getProgramName(InputContext* ic) {
        assert(ic != nullptr);
        std::string programName = ic->program();
        if (programName.empty() || programName == "wayland" || programName == "x11") {
            // Fallback: InputContext address-based resolution
            // This ensures at least per-window separation.
            std::ostringstream oss;
            oss << "ctx_" << static_cast<const void*>(ic);
            programName = oss.str();
        }
        return programName;
    }

    void LotusEngine::clearAppRule(const std::string& appName) {
        {
            std::scoped_lock lock(appRulesMutex_);
            appRules_.erase(appName);
        }
        auto rules = *appRulesTables_.rules;
        std::erase_if(rules, [&appName](const auto& rule) {
            return *rule.app == appName;
        });
        appRulesTables_.rules.setValue(std::move(rules));
        if (!appName.starts_with("ctx_")) {
            saveAppRules();
        }
    }

    void LotusEngine::setupMouseWatcher() {
        if (mouseEventSource_) {
            return;
        }

        // Refuses a socket whose owner is not the uinput server user (verifyServerPeer()).
        FdGuard sock(
            connectServerSocket(buildSocketPath(VANH_MOUSE_SOCKET_FILE), true, "mouse socket"));
        if (!sock.isValid()) {
            return;
        }

        const int sockFd = sock.get();
        g_mouseSocketFd.store(sock.release(), std::memory_order_release);
        LOTUS_INFO("Mouse socket connected and registered to Fcitx EventLoop.");

        mouseEventSource_ = instance_.eventLoop().addIOEvent(
            sockFd, IOEventFlag::In, [this, sockFd](EventSourceIO*, int, IOEventFlags) {
                std::array<char, 16> buf{};
                const ssize_t        n = recv(sockFd, buf.data(), buf.size(), 0);
                if (n <= 0) {
                    LOTUS_ERROR(
                        "Mouse socket recv error or disconnected: " + std::string(strerror(errno)));
                    mouseEventSource_.reset();
                    g_mouseSocketFd.store(-1, std::memory_order_release);
                    FdGuard closed(sockFd);
                    return true;
                }
                if (buf[0] != 'C') {
                    return true;
                }
                LOTUS_DEBUG("Mouse click detected from server. Resetting.....");
                g_mouseClicked.store(true, std::memory_order_release);
                instance_.inputContextManager().foreachFocused([this](InputContext* ic) {
                    auto* state = ic->propertyFor(&factory_);
                    if (currentMode() == LotusMode::Preedit &&
                        !ic->capabilityFlags().test(CapabilityFlag::ClientSideInputPanel)) {
                        state->commitBuffer();
                    } else {
                        state->clearAllBuffers();
                    }
                    return true;
                });
                return true;
            });
    }
} // namespace fcitx
