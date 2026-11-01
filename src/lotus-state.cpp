/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 * SPDX-FileCopyrightText: 2026 kimxuanhong <kimxuanhong@outlook.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
#include "lotus-state.h"
#include "lotus-engine.h"
#include "lotus-candidates.h"
#include "lotus-fcitx-text.h"
#include "lotus-log.h"
#include "lotus-utils.h"

#include "emoji.h"
#include "input_method.h"
#include "utf8.h"

#include <cassert>
#include <cstddef>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputpanel.h>
#include <fcitx/userinterface.h>

#include <algorithm>
#include <string>
#include <thread>

namespace fcitx {
    namespace {
        constexpr int     kMaxScanLength         = 15;
        constexpr int64_t kReplacementAckSlackMs = 200;
        /// Least time between two surrounding edits (GNOME Shell merges closer ones)
        constexpr int64_t kSurroundingPaceMs = 30;
        constexpr int     kMinDelayMs        = 1;
        constexpr int     kMaxDelayMs        = 1000;

        /// Delay of a uinput burst: the app rule when set, else the configured default.
        uint32_t ruleDelay(int rule, int fallback) {
            return static_cast<uint32_t>(
                std::clamp(rule > 0 ? rule : fallback, kMinDelayMs, kMaxDelayMs));
        }

        /// Space, tab, newline, carriage return, null, or punctuation/symbols (: ; < = > ? @).
        bool isWordBreak(uint32_t ucs4) {
            return ucs4 == ' ' || ucs4 == '\t' || ucs4 == '\n' || ucs4 == '\r' || ucs4 == 0 ||
                (ucs4 >= 58 && ucs4 <= 64);
        }

        /// The key that types a shifted symbol when Shift is not held.
        KeySym unshiftKeySym(KeySym sym) {
            if (sym >= FcitxKey_A && sym <= FcitxKey_Z) {
                return static_cast<KeySym>(sym + (FcitxKey_a - FcitxKey_A));
            }
            switch (sym) {
                case FcitxKey_asciitilde:
                    return FcitxKey_grave;
                case FcitxKey_exclam:
                    return FcitxKey_1;
                case FcitxKey_at:
                    return FcitxKey_2;
                case FcitxKey_numbersign:
                    return FcitxKey_3;
                case FcitxKey_dollar:
                    return FcitxKey_4;
                case FcitxKey_percent:
                    return FcitxKey_5;
                case FcitxKey_asciicircum:
                    return FcitxKey_6;
                case FcitxKey_ampersand:
                    return FcitxKey_7;
                case FcitxKey_asterisk:
                    return FcitxKey_8;
                case FcitxKey_parenleft:
                    return FcitxKey_9;
                case FcitxKey_parenright:
                    return FcitxKey_0;
                case FcitxKey_underscore:
                    return FcitxKey_minus;
                case FcitxKey_plus:
                    return FcitxKey_equal;
                case FcitxKey_braceleft:
                    return FcitxKey_bracketleft;
                case FcitxKey_braceright:
                    return FcitxKey_bracketright;
                case FcitxKey_bar:
                    return FcitxKey_backslash;
                case FcitxKey_colon:
                    return FcitxKey_semicolon;
                case FcitxKey_quotedbl:
                    return FcitxKey_apostrophe;
                case FcitxKey_less:
                    return FcitxKey_comma;
                case FcitxKey_greater:
                    return FcitxKey_period;
                case FcitxKey_question:
                    return FcitxKey_slash;
                default:
                    return sym;
            }
        }
    } // namespace

    LotusState::LotusState(LotusEngine* engine, InputContext* ic) : engine_(*engine), ic_(*ic) {
        setEngine();
    }

    void LotusState::setEngine() {
        if (engine_.config().inputMethod.value() == lotus::InputMethodId::Custom) {
            std::unordered_map<std::string, std::string> definition;
            for (const auto& keymap : *engine_.customKeymap().customKeymap) {
                definition.emplace(*keymap.key, *keymap.value);
            }
            // Go's NewCustomEngine parsed the custom definition and left spell checking off.
            lotusEngine_ =
                std::make_shared<lotus::KeyProcessor>(lotus::parseInputMethod(definition), false);
        } else {
            lotusEngine_ = std::make_shared<lotus::KeyProcessor>(
                lotus::cachedInputMethod(engine_.config().inputMethod.value()), true);
        }
        lotusEngine_->setMacroTable(engine_.macroTable());
        lotusEngine_->setDictionary(engine_.dictionary());
        setOption();
        resetMacroSkip();
    }

    void LotusState::setOption() {
        const auto& config = engine_.config();
        lotusEngine_->setOptions({
            .autoNonVnRestore    = *config.autoNonVnRestore,
            .ddFreeStyle         = *config.ddFreeStyle,
            .englishRestore      = *config.englishRestore,
            .macroEnabled        = *config.enableMacro,
            .autoCapitalizeMacro = *config.capitalizeMacro,
            .spellCheckWithDicts = *config.spellCheck,
            .modernStyle         = *config.modernStyle,
            .freeMarking         = *config.freeMarking,
            .outputCharset       = *config.outputCharset,
            .w2u                 = *config.w2u,
            .bracketTransform    = *config.bracketTransform,
            .timeFormat          = *config.timeFormat,
            .dateFormat          = *config.dateFormat,
        });
        lotusEngine_->setLearned(engine_.learned());
    }

    bool LotusState::isDbus() const {
        return getFrontendName(&ic_) == "dbus";
    }

    uint32_t LotusState::configuredPostDelay() const {
        return ruleDelay(appRuleSetting_.postDelay, *engine_.config().defaultPostDelay);
    }

    void LotusState::clearWord(bool forgetContext) {
        hasHistory_ = false;
        lotusEngine_->reset();
        if (forgetContext) {
            lotusEngine_->forgetContext();
        }
        oldPreBuffer_.clear();
    }

    void LotusState::stopReplacement() {
        expectedBackspaces_    = 0;
        currentBackspaceCount_ = 0;
        replacementDeadlineMs_ = 0;
        realSelect_            = false;
        g_isDeleting.store(false, std::memory_order_release);
    }

    void LotusState::replaceSurrounding(size_t deleted, std::u32string_view added) {
        // GNOME Shell relays text-input-v3 apps (native Wayland Brave, GTK4) and merges the edits
        // that reach it together, keeping one deletion: "được" typed fast became "đươcợc". An
        // edit waits until kSurroundingPaceMs after the previous one; the edits made meanwhile
        // join it, so the app gets them as one exact edit.
        const int64_t nowAt = nowMs();
        if (!held_ &&
            (nowAt - lastSurroundingEditMs_ >= kSurroundingPaceMs ||
                ic_.program() != "gnome-shell")) {
            if (deleted > 0) {
                ic_.deleteSurroundingText(-static_cast<int>(deleted), static_cast<int>(deleted));
            }
            lotus::fcitx_text::commit(&ic_, added, lastCommitTime_);
            lastSurroundingEditMs_ = nowAt;
            return;
        }
        const size_t eaten = std::min(deleted, heldAdded_.size());
        heldAdded_.resize(heldAdded_.size() - eaten);
        heldDeleted_ += deleted - eaten;
        heldAdded_ += added;
        if (held_) {
            return;
        }
        held_             = true;
        const uint64_t at = now(CLOCK_MONOTONIC) +
            static_cast<uint64_t>(lastSurroundingEditMs_ + kSurroundingPaceMs - nowAt) * 1000;
        if (heldTimer_) {
            heldTimer_->setTime(at);
            heldTimer_->setOneShot();
            return;
        }
        heldTimer_ = engine_.instance()->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, at, 1000, [this](EventSourceTime*, uint64_t) {
                sendHeldEdit();
                return true;
            });
    }

    void LotusState::sendHeldEdit() {
        if (!held_) {
            return;
        }
        held_ = false;
        // Sent early for a key that reaches the app another way (forwarded keys do not reach
        // apps through GNOME Shell, so the key cannot wait instead): block for the rest of the
        // pace, at most kSurroundingPaceMs.
        const int64_t wait = lastSurroundingEditMs_ + kSurroundingPaceMs - nowMs();
        if (wait > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        }
        if (heldDeleted_ > 0) {
            ic_.deleteSurroundingText(
                -static_cast<int>(heldDeleted_), static_cast<int>(heldDeleted_));
        }
        lotus::fcitx_text::commit(&ic_, heldAdded_, lastCommitTime_);
        heldDeleted_ = 0;
        heldAdded_.clear();
        lastSurroundingEditMs_ = nowMs();
    }

    void LotusState::sendKbMsg(
        KbOp op, size_t count, uint32_t preDelay, uint32_t interval, uint32_t postDelay) {
        engine_.kbSender().send({
            .op        = op,
            .count     = count,
            .preDelay  = preDelay,
            .interval  = interval,
            .postDelay = postDelay,
        });
    }

    void LotusState::cancelPendingUinput() {
        if (!g_isDeleting.load(std::memory_order_acquire)) {
            return;
        }
        sendKbMsg(KbOp::Cancel, 0, 0, 0, 0);
        LOTUS_INFO("Cancel the burst still pending on the uinput server");
    }

    void LotusState::abortPendingUinputReplacement() {
        if (!g_isDeleting.load(std::memory_order_acquire)) {
            return;
        }
        cancelPendingUinput();
        // The server finishes the selection on Cancel, so the old text is always
        // fully removed: stash the replacement text for the refocus commit.
        stashedCommit_      = pendingCommitString_;
        stashedNewWord_     = oldPreBuffer_;
        uinputAbortTimeMs_  = nowMs();
        uinputAbortPending_ = !stashedCommit_.empty() || !bufferedKeys_.empty();
        pendingCommitString_.clear();
        stopReplacement();
        deletionInterruptedAt_  = 0;
        returnAfterReplacement_ = false;
        returnEchoPending_      = false;
        oldPreBuffer_.clear();
        hasHistory_   = false;
        deferCommits_ = isDbus();
    }

    bool LotusState::isAutofillCertain() {
        const auto& s = ic_.surroundingText();
        if (!s.isValid() || oldPreBuffer_.empty() || isChromium_) {
            return false;
        }
        const auto           textRange = fcitx::utf8::MakeUTF8CharRange(s.text());
        const std::u32string u32Text(textRange.begin(), textRange.end());
        const unsigned int   cursor  = s.cursor();
        const unsigned int   anchor  = s.anchor();
        const size_t         textLen = u32Text.length();

        // Surrounding text can lag behind IME state, so confirm oldPreBuffer_ is still near the
        // cursor before trusting it.
        const size_t buffLen    = oldPreBuffer_.length();
        const size_t pb         = u32Text.find(oldPreBuffer_);
        const size_t rangeStart = cursor >= buffLen ? cursor - buffLen : 0;
        const bool   sameprefix = pb != std::u32string::npos && pb >= rangeStart && pb <= cursor;

        // Browser autofill/autocomplete suggestion: the selection extends from the cursor
        // (common address bar behavior).
        if (cursor < anchor) {
            if (!sameprefix) {
                return false;
            }
            // A newline in the selection means a multiline editor (AI ghost text), not a
            // single-line URL/Search bar.
            const size_t p = u32Text.find(U'\n', cursor);
            return p == std::u32string::npos || p >= anchor;
        }
        if (textLen == cursor) {
            g_realTextLen.store(textLen, std::memory_order_release);
            return false;
        }
        // Gecko/Firefox: if buffLen > textLen, surrounding text is stale (async update race)
        if (buffLen > textLen) {
            return false;
        }
        // Heuristic: rapid text growth in a single-line context. Applied only when no newline is
        // present after the cursor to distinguish from AI text in editors.
        if (textLen > cursor + 1 && cursor == g_realTextLen.load(std::memory_order_acquire) &&
            u32Text.find(U'\n', cursor) == std::u32string::npos && sameprefix) {
            return true;
        }
        auto v = g_realTextLen.load(std::memory_order_acquire);
        while (v < cursor &&
            !g_realTextLen.compare_exchange_weak(v, cursor, std::memory_order_acq_rel)) {
        }
        return false;
    }

    void LotusState::handlePreeditMode(KeyEvent& keyEvent, KeySym currentSym) {
        const uint32_t keyVal = engineKeyValue(currentSym);
        // Enter: the word in the preedit is decided as at a word break (an English word last on a
        // line is committed as its keys), then Enter goes on to the app.
        if (currentSym == FcitxKey_Return && !keyEvent.key().hasModifier()) {
            lotusEngine_->endWord();
            LOTUS_DEBUG("Preedit: Enter ends the word");
        } else if (lotusEngine_->processKey(keyVal,
                       lotus::keyStatesFromBits(
                           static_cast<uint32_t>(keyEvent.rawKey().states())))) {
            keyEvent.filterAndAccept();
        } else {
            LOTUS_DEBUG("Preedit: key not processed, forwarding (sym=" +
                std::to_string(currentSym) + ", keyVal=" + std::to_string(keyVal) + ")");
        }
        lotus::fcitx_text::commit(&ic_, lotusEngine_->takeCommit(), lastCommitTime_);
        ic_.inputPanel().reset();
        const std::u32string preeditText = lotusEngine_->preedit();
        if (!preeditText.empty()) {
            const Text text = lotus::fcitx_text::toText(preeditText);
            if (ic_.capabilityFlags().test(CapabilityFlag::Preedit)) {
                ic_.inputPanel().setClientPreedit(text);
            } else {
                ic_.inputPanel().setPreedit(text);
            }
        }
        ic_.updatePreedit();
        ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void LotusState::updateEmojiPageStatus(CommonCandidateList& commonList) {
        assert(!commonList.empty());
        const int         pageSize   = commonList.pageSize();
        const int         totalPages = (commonList.totalSize() + pageSize - 1) / pageSize;
        const std::string status = _("Page ") + std::to_string(commonList.currentPage() + 1) + "/" +
            std::to_string(totalPages);
        ic_.inputPanel().setAuxDown(Text(status));
    }

    void LotusState::handleEmojiMode(KeyEvent& keyEvent) {
        const KeySym currentSym = keyEvent.rawKey().sym();
        const bool   isCtrlBackspace =
            isBackspace(currentSym) && ((keyEvent.rawKey().states() & KeyState::Ctrl) != 0U);
        if (keyEvent.key().hasModifier() && !isCtrlBackspace) {
            keyEvent.forward();
            return;
        }

        auto commonList =
            std::dynamic_pointer_cast<CommonCandidateList>(ic_.inputPanel().candidateList());
        if (commonList && currentSym >= FcitxKey_1 && currentSym <= FcitxKey_9) {
            const int globalIndex =
                (commonList->currentPage() * commonList->pageSize()) + (currentSym - FcitxKey_1);
            if (globalIndex < commonList->totalSize()) {
                commonList->candidateFromAll(globalIndex).select(&ic_);
                keyEvent.filterAndAccept();
                return;
            }
        }

        if (commonList && !commonList->empty()) {
            const int globalCursorIndex = commonList->globalCursorIndex();
            const int totalSize         = commonList->totalSize();
            const int currentPage       = commonList->currentPage();
            const int pageSize          = commonList->pageSize();
            const int localCursorIndex  = globalCursorIndex - (currentPage * pageSize);
            bool      handled           = false;
            switch (currentSym) {
                case FcitxKey_Tab:
                case FcitxKey_Down: {
                    if (localCursorIndex < pageSize - 1 && globalCursorIndex < totalSize - 1) {
                        commonList->setGlobalCursorIndex(globalCursorIndex + 1);
                    } else {
                        commonList->setGlobalCursorIndex(currentPage * pageSize);
                    }
                    handled = true;
                    break;
                }
                case FcitxKey_ISO_Left_Tab:
                case FcitxKey_Up: {
                    if (localCursorIndex > 0) {
                        commonList->setGlobalCursorIndex(globalCursorIndex - 1);
                    } else {
                        commonList->setGlobalCursorIndex(
                            std::min((currentPage * pageSize) + pageSize - 1, totalSize - 1));
                    }
                    handled = true;
                    break;
                }
                case FcitxKey_Page_Down:
                case FcitxKey_Right: {
                    handled = commonList->hasNext();
                    if (handled) {
                        commonList->next();
                        commonList->setGlobalCursorIndex(commonList->currentPage() * pageSize);
                    }
                    break;
                }
                case FcitxKey_Page_Up:
                case FcitxKey_Left: {
                    handled = commonList->hasPrev();
                    if (handled) {
                        commonList->prev();
                        commonList->setGlobalCursorIndex(commonList->currentPage() * pageSize);
                    }
                    break;
                }
                default:
                    break;
            }
            if (handled) {
                updateEmojiPageStatus(*commonList);
                ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
                keyEvent.filterAndAccept();
                return;
            }
        }

        if (isBackspace(currentSym)) {
            if (!emojiBuffer_.empty()) {
                if (isCtrlBackspace) {
                    emojiBuffer_.clear();
                } else {
                    emojiBuffer_.pop_back();
                }
                keyEvent.filterAndAccept();
            } else {
                keyEvent.forward();
            }
            updateEmojiPreedit();
            return;
        }

        switch (currentSym) {
            case FcitxKey_space:
            case FcitxKey_Return: {
                if (commonList && !commonList->empty()) {
                    commonList->candidateFromAll(commonList->globalCursorIndex()).select(&ic_);
                    keyEvent.filterAndAccept();
                } else if (currentSym == FcitxKey_Return && !emojiBuffer_.empty()) {
                    lotus::fcitx_text::commit(&ic_, emojiBuffer_, lastCommitTime_);
                    emojiBuffer_.clear();
                    updateEmojiPreedit();
                    keyEvent.filterAndAccept();
                } else {
                    keyEvent.forward();
                }
                return;
            }
            case FcitxKey_Escape: {
                emojiBuffer_.clear();
                emojiCandidates_.clear();
                ic_.inputPanel().reset();
                ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
                keyEvent.filterAndAccept();
                return;
            }
            default:
                break;
        }

        const uint32_t codePoint = Key::keySymToUnicode(currentSym);
        if (codePoint == 0U) {
            keyEvent.forward();
            return;
        }
        emojiBuffer_.push_back(static_cast<char32_t>(codePoint));
        keyEvent.filterAndAccept();
        updateEmojiPreedit();
    }

    void LotusState::updateEmojiPreedit() {
        if (emojiBuffer_.empty()) {
            emojiCandidates_ = engine_.emojiLoader().history();
            if (emojiCandidates_.empty()) {
                ic_.inputPanel().reset();
                ic_.updatePreedit();
                ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
                return;
            }
            ic_.inputPanel().setClientPreedit(Text());
            ic_.inputPanel().setPreedit(Text());
        } else {
            emojiCandidates_ = engine_.emojiLoader().search(lotus::utf8::encode(emojiBuffer_));
            Text preeditText;
            preeditText.append(lotus::utf8::encode(emojiBuffer_), TextFormatFlag::Underline);
            preeditText.setCursor(static_cast<int>(preeditText.textLength()));
            ic_.inputPanel().setPreedit(preeditText);
        }

        if (emojiCandidates_.empty()) {
            ic_.inputPanel().setCandidateList(nullptr);
        } else {
            auto candidateList = std::make_unique<CommonCandidateList>();
            candidateList->setLayoutHint(CandidateLayoutHint::Vertical);
            candidateList->setPageSize(9);
            for (size_t i = 0; i < emojiCandidates_.size(); ++i) {
                const EmojiEntry& entry = emojiCandidates_[i];
                Text              displayLabel;
                displayLabel.append(std::to_string((i % 9) + 1) + ": " +
                        (emojiBuffer_.empty() ? entry.output : entry.trigger + " " + entry.output),
                    TextFormatFlag::NoFlag);
                candidateList->append(
                    std::make_unique<EmojiCandidateWord>(displayLabel, this, entry));
            }
            candidateList->setGlobalCursorIndex(0);
            auto& list = *candidateList;
            ic_.inputPanel().setCandidateList(std::move(candidateList));
            updateEmojiPageStatus(list);
        }
        ic_.updatePreedit();
        ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void LotusState::performReplacement(DiffPart diffPart) {
        // WA for libreoffice (issue #162)
        // force use deleteSurroundingText func
        if (isLibreoffice_) {
            replaceSurrounding(diffPart.deletedPart.size(), diffPart.addedPart);
            return;
        }
        const uint32_t commitMs =
            ruleDelay(appRuleSetting_.commitInterval, *engine_.config().defaultCommitInterval);
        const uint32_t interval = ruleDelay(
            appRuleSetting_.backspaceInterval, *engine_.config().defaultBackspaceInterval);
        uint32_t preDelay = 0;
        if (lastCommitTime_.time_since_epoch().count() > 0) {
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lastCommitTime_)
                                       .count();
            if (elapsedMs < commitMs) {
                preDelay = static_cast<uint32_t>(commitMs - elapsedMs);
            }
        }
        LOTUS_INFO( // NOLINT
            "Perform replacement: " + lotus::utf8::encode(diffPart.deletedPart) + " -> " +
            lotus::utf8::encode(diffPart.addedPart));
        currentBackspaceCount_     = 0;
        pendingCommitString_       = diffPart.addedPart;
        expectedBackspaces_        = diffPart.deletedPart.size();
        const bool autofillCertain = isAutofillCertain();
        if (currentMode() != LotusMode::SuperSmooth && autofillCertain) {
            ++expectedBackspaces_;
        }
        const bool addressBar = isChromium_ &&
            (ic_.capabilityFlags().test(CapabilityFlag::Url) || engine_.isLikeAddressBarChromium());
        realSelect_ = (currentMode() == LotusMode::Select && !autofillCertain) ||
            (isUinputMode(currentMode()) && addressBar);
        lastSelect_ = realSelect_;
        g_isDeleting.store(true, std::memory_order_release);

        const uint32_t post     = configuredPostDelay();
        const auto     injectMs = static_cast<int64_t>(expectedBackspaces_) * interval;
        replacementDeadlineMs_  = nowMs() + preDelay + injectMs + post + kReplacementAckSlackMs;
        replacementEchoGrace_   = static_cast<int64_t>(interval) + post + kReplacementAckSlackMs;
        // Select mode: the uinput server selects with Shift+Left instead. The trigger-key
        // compensation above still applies because the last echoed Left is swallowed; autofill
        // compensation does not.
        const std::string count = std::to_string(expectedBackspaces_);
        sendKbMsg(realSelect_ ? KbOp::Select : KbOp::Backspace, expectedBackspaces_, preDelay,
            interval, post);
        LOTUS_INFO((realSelect_ ? "Send select of " + count + " characters" :
                                  "Send " + count + " backspaces"));
    }

    bool LotusState::forwardSpecialKey(KeyEvent& keyEvent, KeySym& currentSym) {
        if (keyEvent.key().isCursorMove() || currentSym == FcitxKey_Tab ||
            currentSym == FcitxKey_KP_Tab || currentSym == FcitxKey_ISO_Left_Tab ||
            currentSym == FcitxKey_Escape || keyEvent.key().hasModifier()) {
            stopReplacement();
            pendingCommitString_.clear();
            returnAfterReplacement_ = false;
            returnEchoPending_      = false;
            clearWord(true);
            keyEvent.forward();
            return true;
        }
        if (currentSym == FcitxKey_Delete) {
            lotusEngine_->forgetContext();
            keyEvent.forward();
            return true;
        }
        if (currentSym >= FcitxKey_KP_0 && currentSym <= FcitxKey_KP_9) {
            currentSym = static_cast<KeySym>(FcitxKey_0 + (currentSym - FcitxKey_KP_0));
            return false;
        }
        static constexpr std::pair<KeySym, KeySym> kKeypadKeys[] = {
            {FcitxKey_KP_Add, FcitxKey_plus},
            {FcitxKey_KP_Subtract, FcitxKey_minus},
            {FcitxKey_KP_Divide, FcitxKey_slash},
            {FcitxKey_KP_Multiply, FcitxKey_asterisk},
            {FcitxKey_KP_Decimal, FcitxKey_period},
            {FcitxKey_KP_Enter, FcitxKey_Return},
            {FcitxKey_KP_Equal, FcitxKey_equal},
            {FcitxKey_KP_Space, FcitxKey_space},
        };
        for (const auto& [keypad, plain] : kKeypadKeys) {
            if (currentSym == keypad) {
                currentSym = plain;
                break;
            }
        }
        return false;
    }

    void LotusState::handleUinputMode(KeyEvent& keyEvent, KeySym currentSym) {
        if (forwardSpecialKey(keyEvent, currentSym)) {
            return;
        }
        engine_.kbSender().ensureConnected();

        if (isBackspace(currentSym)) {
            hasHistory_ = true;
            lotusEngine_->processKey(FcitxKey_BackSpace, {});
            oldPreBuffer_ = lotusEngine_->preedit();
            keyEvent.forward();
            return;
        }

        if (currentSym == FcitxKey_Return) {
            // Enter waits for the replacement and then reaches the app as a forwarded key, or,
            // with the dbus frontend (fcitx5-gtk drops forwarded keys in GTK4 apps such as
            // Ghostty), typed by the uinput server. Without either the word can only be replaced
            // through the surrounding text, which reaches the app before the Enter passed
            // through now.
            const bool canForward =
                !isDbus() || (engine_.kbSender().connected() && engine_.uinputTypesReturn());
            const bool surrounding = ic_.capabilityFlags().test(CapabilityFlag::SurroundingText) &&
                ic_.surroundingText().isValid();
            if (canForward || surrounding) {
                // the word may also rewrite the ones before it, as at a space
                lotusEngine_->setRewriteAllowed(true);
                if (!oldPreBuffer_.empty() && lotusEngine_->endWord()) {
                    const std::u32string commitStr = lotusEngine_->takeCommit();
                    // outlives the views of diffPart
                    const std::u32string shown =
                        lotusEngine_->takeRewrittenBefore() + oldPreBuffer_;
                    const DiffPart diffPart = compareAndSplitU32(shown, commitStr);
                    clearWord(true);
                    if (diffPart.deletedPart.empty() && (diffPart.addedPart.empty() || !isDbus())) {
                        // nothing to delete: Enter follows the commit on the same channel
                        lotus::fcitx_text::commit(&ic_, diffPart.addedPart, lastCommitTime_);
                        keyEvent.forward();
                    } else if (diffPart.deletedPart.empty() && canForward) {
                        // dbus: a commit made while the app's own Enter passes was seen to lose
                        // the Enter in Ghostty; the Enter follows the commit through the uinput
                        // server.
                        commitBatch_ += diffPart.addedPart;
                        keyEvent.filterAndAccept();
                        deferCommits_ = true;
                        sendReturn();
                    } else if (!canForward) {
                        // Only when the text before the cursor is the word shown (surrounding
                        // text can be stale).
                        const auto& st     = ic_.surroundingText();
                        const auto  before = lotus::utf8::decode(st.text()).substr(0, st.cursor());
                        if (before.ends_with(diffPart.deletedPart)) {
                            replaceSurrounding(diffPart.deletedPart.size(), diffPart.addedPart);
                        }
                        keyEvent.forward();
                    } else if (!engine_.kbSender().connected()) {
                        keyEvent.forward(); // no way to delete the shown word: keep it
                    } else {
                        // The uinput Backspaces arrive later than a forwarded key would: Enter
                        // waits for the replacement and is sent after its commit (see the echo
                        // handling in keyEvent).
                        returnAfterReplacement_ = true;
                        performReplacement(diffPart);
                        keyEvent.filterAndAccept();
                        LOTUS_INFO(
                            "Restore before Enter: " + lotus::utf8::encode(diffPart.deletedPart) +
                            " -> " + lotus::utf8::encode(diffPart.addedPart));
                    }
                    return;
                }
            }
            clearWord(true);
            keyEvent.forward();
            return;
        }

        const uint32_t keyCodePoint = Key::keySymToUnicode(currentSym);
        if (keyCodePoint == 0U) {
            keyEvent.forward();
            return;
        }
        const std::u32string keyText(1, static_cast<char32_t>(keyCodePoint));
        const bool           wasAutoCapitalized = currentSym != keyEvent.rawKey().sym();

        lotusEngine_->setRewriteAllowed(true);
        const bool           processed = lotusEngine_->processKey(engineKeyValue(currentSym),
                      lotus::keyStatesFromBits(static_cast<uint32_t>(keyEvent.rawKey().states())));

        const std::u32string commitStr = lotusEngine_->takeCommit();
        if (!commitStr.empty()) {
            // English restore may rewrite the words before ("thí makes" -> "this makes").
            // DiffPart holds views: the text it splits must outlive it.
            const std::u32string shown    = lotusEngine_->takeRewrittenBefore() + oldPreBuffer_;
            const DiffPart       diffPart = compareAndSplitU32(shown, commitStr);
            if (!diffPart.deletedPart.empty()) {
                performReplacement(diffPart);
                keyEvent.filterAndAccept();
            } else if (!diffPart.addedPart.empty() &&
                (keyText != diffPart.addedPart || wasAutoCapitalized)) {
                lotus::fcitx_text::commit(&ic_, diffPart.addedPart, lastCommitTime_);
                keyEvent.filterAndAccept();
            } else {
                keyEvent.forward();
            }
            clearWord();
            return;
        }

        if (!processed) {
            if (lotusEngine_->preedit().empty()) {
                clearWord();
                keyEvent.forward();
            }
            return;
        }

        hasHistory_ = true;
        g_realTextLen.fetch_add(1, std::memory_order_acq_rel);
        const std::u32string preeditStr = lotusEngine_->preedit();
        const DiffPart       diffPart   = compareAndSplitU32(oldPreBuffer_, preeditStr);
        if (diffPart.deletedPart.empty()) {
            if (diffPart.addedPart.empty()) {
                keyEvent.forward();
                return;
            }
            oldPreBuffer_ = preeditStr;
            if (wasAutoCapitalized || diffPart.addedPart != keyText) {
                lotus::fcitx_text::commit(&ic_, diffPart.addedPart, lastCommitTime_);
                keyEvent.filterAndAccept();
            } else {
                keyEvent.forward();
            }
            return;
        }

        if (!engine_.kbSender().connected()) {
            LOTUS_ERROR("Cannot connect to uinput server, commit rawkey");
            const std::string rawKey = keyEvent.key().toString();
            if (!rawKey.empty()) {
                ic_.commitString(rawKey);
            }
            return;
        }
        g_isDeleting.store(false, std::memory_order_release);
        keyEvent.filterAndAccept();
        performReplacement(diffPart);
        oldPreBuffer_ = preeditStr;
    }

    bool LotusState::sendReturn() {
        returnAfterReplacement_ = false;
        flushCommitBatch();
        if (!isDbus() || !engine_.kbSender().connected() || !engine_.uinputTypesReturn()) {
            ic_.forwardKey(Key(FcitxKey_Return), false);
            ic_.forwardKey(Key(FcitxKey_Return), true);
            return false;
        }
        // fcitx5-gtk drops forwarded keys in GTK4 apps (Ghostty): the uinput server types the
        // Enter instead. It comes back through the app's IM after the commit just sent on the
        // same D-Bus connection, so it lands after the word; keys typed meanwhile stay buffered
        // until the anchor that follows it.
        const uint32_t post    = configuredPostDelay();
        currentBackspaceCount_ = 0;
        expectedBackspaces_    = 0;
        pendingCommitString_.clear();
        realSelect_            = false;
        returnEchoPending_     = true;
        replacementDeadlineMs_ = nowMs() + post + kReplacementAckSlackMs;
        replacementEchoGrace_  = post + kReplacementAckSlackMs;
        g_isDeleting.store(true, std::memory_order_release);
        if (deferredCommitPending_) {
            returnAfterDeferredCommit_ = true; // the commit goes first, see scheduleDeferredCommit
        } else {
            sendKbMsg(KbOp::Return, 1, 0, 0, post);
            LOTUS_INFO("Send Return through the uinput server");
        }
        return true;
    }

    bool LotusState::handleRevertKey(KeyEvent& keyEvent) {
        // Matched on the raw key: Key::normalize() drops Shift from Shift+BackSpace.
        const KeyStates mods{KeyState::Ctrl, KeyState::Alt, KeyState::Shift, KeyState::Super};
        const auto      matches = [&](const Key& k) {
            return k.sym() == keyEvent.rawKey().sym() &&
                (keyEvent.rawKey().states() & mods) == (k.states() & mods);
        };
        if (std::ranges::none_of(*engine_.config().revertKey, matches) ||
            g_isDeleting.load(std::memory_order_acquire)) {
            return false;
        }
        const bool uinput      = isUinputMode(currentMode()) && !shouldUseSurroundingText();
        const bool surrounding = !uinput &&
            ic_.capabilityFlags().test(CapabilityFlag::SurroundingText) &&
            ic_.surroundingText().isValid();
        if ((!uinput && !surrounding) || (uinput && !oldPreBuffer_.empty()) ||
            !lotusEngine_->revertLastSwitch()) {
            return false;
        }
        const std::u32string after    = lotusEngine_->takeCommit();
        const std::u32string before   = lotusEngine_->takeRewrittenBefore();
        const DiffPart       diffPart = compareAndSplitU32(before, after);
        if (uinput) {
            if (!engine_.kbSender().connected()) {
                return false;
            }
            performReplacement(diffPart);
        } else {
            replaceSurrounding(diffPart.deletedPart.size(), diffPart.addedPart);
            invalidateSurroundingWord(true);
        }
        engine_.saveLearned();
        LOTUS_INFO("Revert: " + lotus::utf8::encode(before) + " -> " + lotus::utf8::encode(after));
        keyEvent.filterAndAccept();
        return true;
    }

    void LotusState::invalidateSurroundingWord(bool hard) {
        if (hard || !surrKnown_) {
            surrKnown_ = false;
            surrWord_.clear();
            surrSoftResetAt_ = 0;
            return;
        }
        // Soft: a reset/focus bounce. Chromium bounces focus on every key in some editors,
        // so keep the tracked word if typing resumes right away.
        surrSoftResetAt_ = nowMs();
    }

    void LotusState::handleSurroundingText(KeyEvent& keyEvent, KeySym currentSym) {
        if (forwardSpecialKey(keyEvent, currentSym)) {
            invalidateSurroundingWord(true);
            return;
        }
        const bool  supported   = ic_.capabilityFlags().test(CapabilityFlag::SurroundingText);
        const auto& surrounding = ic_.surroundingText();
        if (!supported || !surrounding.isValid()) {
            LOTUS_WARN(
                (supported ? "Surrounding text is invalid" : "Surrounding text not supported"));
            invalidateSurroundingWord(true);
            keyEvent.forward();
            return;
        }

        if (isBackspace(keyEvent.rawKey().sym())) {
            lotusEngine_->reset();
            if (surrKnown_ && !surrWord_.empty()) {
                surrWord_.pop_back();
            } else {
                invalidateSurroundingWord(true);
                lotusEngine_->forgetContext();
            }
            keyEvent.forward();
            return;
        }

        std::u32string oldWord;
        bool           keepEngine = false;
        if (surrKnown_ && (surrSoftResetAt_ == 0 || held_)) {
            // No reset since our last key: our own record of the word is authoritative. The app's
            // surrounding text lags behind when typing fast, and rebuilding from it then deletes
            // or keeps the wrong characters.
            oldWord = surrWord_;
            // Keep the engine's own key history when it still matches, so a word break sees the
            // keys actually typed ("more"), not a rebuild of the text ("moer").
            keepEngine = !oldWord.empty() && lotusEngine_->preedit() == oldWord;
        } else {
            // First key, or the app reset/refocused (it may have changed the text): trust the app,
            // which re-sends its text on focus/reset. Only the slice before the cursor (at most
            // kMaxScanLength code points) is decoded.
            const std::string& text   = surrounding.text();
            const unsigned int cursor = std::min(surrounding.anchor(), surrounding.cursor());
            if (cursor > 0 && cursor <= lotus::utf8::length(text)) {
                auto       startIter = utf8::nextNChar(text.begin(), cursor);
                const auto endIter   = startIter;
                for (int scanCount = 0; startIter != text.begin() && scanCount < kMaxScanLength;
                    ++scanCount) {
                    auto prev = startIter;
                    --prev;
                    while (prev != text.begin() &&
                        ((static_cast<unsigned char>(*prev) & 0xC0U) == 0x80)) {
                        --prev;
                    }
                    if (isWordBreak(utf8::getChar(prev, text.end()))) {
                        break;
                    }
                    startIter = prev;
                }
                oldWord = lotus::utf8::decode(std::string(startIter, endIter));
            }
            if (surrKnown_ && oldWord != surrWord_) {
                LOTUS_DEBUG("Surrounding reseed: tracked '" + lotus::utf8::encode(surrWord_) +
                    "' app '" + lotus::utf8::encode(oldWord) + "'");
            }
        }
        surrSoftResetAt_ = 0;

        if (!keepEngine) {
            lotusEngine_->reset();
            if (!oldWord.empty()) {
                lotusEngine_->rebuildFromText(oldWord);
            }
        }

        lotusEngine_->setRewriteAllowed(true);
        if (currentSym == FcitxKey_Return) {
            // Enter after a word: restore it like a word break would ("... is" + Enter, not "í"),
            // through the same channel as the Enter that follows.
            if (!oldWord.empty() && lotusEngine_->endWord()) {
                const std::u32string committed = lotusEngine_->takeCommit();
                // outlives the views of diffPart
                const std::u32string shown    = lotusEngine_->takeRewrittenBefore() + oldWord;
                const DiffPart       diffPart = compareAndSplitU32(shown, committed);
                replaceSurrounding(diffPart.deletedPart.size(), diffPart.addedPart);
            }
            lotusEngine_->reset();
            lotusEngine_->forgetContext();
            invalidateSurroundingWord(true);
            sendHeldEdit(); // the restore, before the Enter
            return;
        }
        const bool processed = lotusEngine_->processKey(engineKeyValue(currentSym),
            lotus::keyStatesFromBits(static_cast<uint32_t>(keyEvent.rawKey().states())));

        if (!processed) {
            // Word break (space, punctuation...) or a key the engine doesn't take.
            lotusEngine_->reset();
            surrKnown_ = true;
            surrWord_.clear();
            // Printable keys go through the same channel as replacements (commit), so a forwarded
            // key and a following deleteSurroundingText can never arrive at the app out of order.
            const uint32_t cp = Key::keySymToUnicode(currentSym);
            if (keyEvent.rawKey().states().testAny(KeyStates{KeyState::Ctrl, KeyState::Alt,
                    KeyState::Super, KeyState::Hyper, KeyState::Meta}) ||
                cp < 0x20 || cp == 0x7F) {
                invalidateSurroundingWord(true);
                keyEvent.forward();
                return;
            }
            replaceSurrounding(0, std::u32string(1, static_cast<char32_t>(cp)));
            keyEvent.filterAndAccept();
            return;
        }

        const std::u32string committed = lotusEngine_->takeCommit();
        const std::u32string preedit   = lotusEngine_->preedit();
        const std::u32string newWord   = committed + preedit;
        if (!committed.empty()) {
            lotusEngine_->reset();
        }
        // English restore may rewrite the words before ("thí makes" -> "this makes").
        oldWord = lotusEngine_->takeRewrittenBefore() + oldWord;

        const DiffPart diffPart = compareAndSplitU32(oldWord, newWord);
        replaceSurrounding(diffPart.deletedPart.size(), diffPart.addedPart);
        LOTUS_DEBUG("Surrounding: '" + lotus::utf8::encode(oldWord) + "' -> '" +
            lotus::utf8::encode(newWord) + "' del=" + std::to_string(diffPart.deletedPart.size()));

        // Keep only the part after the last word break as the tracked word. After a commit that is
        // the engine's own preedit: the engine also breaks words at characters this tracker keeps
        // ("~/.config/fcitx5/profile", "max_tokens"), and a tracked word longer than the preedit
        // made the next key rebuild the engine from the text, losing the keys typed.
        surrWord_ = committed.empty() ? newWord : preedit;
        for (size_t i = surrWord_.size(); i > 0; --i) {
            if (isWordBreak(static_cast<uint32_t>(surrWord_[i - 1]))) {
                surrWord_.erase(0, i);
                break;
            }
        }
        surrKnown_ = true;
        keyEvent.filterAndAccept();
    }

    void LotusState::handleOffModeMacro(KeyEvent& keyEvent, KeySym currentSym) {
        if (forwardSpecialKey(keyEvent, currentSym)) {
            return;
        }
        engine_.kbSender().ensureConnected();

        if (isBackspace(currentSym)) {
            lotusEngine_->processKey(FcitxKey_BackSpace, {});
            oldPreBuffer_ = lotusEngine_->preedit();
            keyEvent.forward();
            return;
        }

        if (currentSym == FcitxKey_Return) {
            if (!oldPreBuffer_.empty()) {
                lotusEngine_->reset();
                oldPreBuffer_.clear();
            }
            keyEvent.forward();
            return;
        }

        const uint32_t       keyCodePoint = Key::keySymToUnicode(currentSym);
        const std::u32string keyText      = keyCodePoint != 0U ?
                 std::u32string(1, static_cast<char32_t>(keyCodePoint)) :
                 std::u32string{};
        const bool           processed    = lotusEngine_->processKey(engineKeyValue(currentSym),
                         lotus::keyStatesFromBits(static_cast<uint32_t>(keyEvent.rawKey().states())));

        const std::u32string commitStr = lotusEngine_->takeCommit();
        if (processed && !commitStr.empty()) {
            // Without a macro the engine only confirms the typed text: forward the trigger key.
            if (commitStr != oldPreBuffer_ + keyText) {
                LOTUS_INFO("Macro expansion: '" + lotus::utf8::encode(oldPreBuffer_) + "' -> '" +
                    lotus::utf8::encode(commitStr) + "'");
                // Try uinput replacement first, fallback to deleteSurroundingText, then plain
                // commit
                if (engine_.kbSender().connected() && !oldPreBuffer_.empty()) {
                    performReplacement({.deletedPart = oldPreBuffer_, .addedPart = commitStr});
                } else {
                    const bool canDelete =
                        ic_.capabilityFlags().test(CapabilityFlag::SurroundingText) &&
                        ic_.surroundingText().isValid();
                    replaceSurrounding(canDelete ? oldPreBuffer_.size() : 0, commitStr);
                }
                keyEvent.filterAndAccept();
            } else {
                keyEvent.forward();
            }
            oldPreBuffer_.clear();
            hasHistory_ = false;
            return;
        }

        if (processed) {
            // Engine processed the key (building shadow state)
            oldPreBuffer_ = lotusEngine_->preedit();
        } else if (!commitStr.empty()) {
            // Engine committed old text but didn't process the new key
            oldPreBuffer_.clear();
            hasHistory_ = false;
        } else if (!oldPreBuffer_.empty()) {
            lotusEngine_->reset();
            oldPreBuffer_.clear();
        }
        keyEvent.forward();
    }

    void LotusState::keyEvent(KeyEvent& keyEvent) {
        assert(lotusEngine_);
        const KeySym rawSym = keyEvent.rawKey().sym();
        // fcitx5 exposes modifiers only per key event, so the addon keeps the running Shift
        // state itself to answer "is Shift held right now" at replay time. Known limitation: a
        // single boolean tracks both Shift keys; releasing either clears it.
        if (rawSym == FcitxKey_Shift_L || rawSym == FcitxKey_Shift_R) {
            shiftHeld_ = !keyEvent.isRelease();
        }
        if (currentMode() == LotusMode::Preedit) {
            if (keyEvent.rawKey().check(FcitxKey_Shift_L) ||
                keyEvent.rawKey().check(FcitxKey_Shift_R)) {
                return;
            }
        } else {
            if (keyEvent.rawKey().isModifier() && !g_isDeleting.load(std::memory_order_acquire)) {
                // Macro skip: a tap (keydown then consecutive keyup) of the selected modifier.
                const auto trigger = engine_.config().macroSkipTriggerModifier.value();
                if (trigger == MacroSkipTriggerModifier::Disabled ||
                    !*engine_.config().enableMacro) {
                    return;
                }
                // Left and right keysyms of Shift, Ctrl and Alt are adjacent.
                KeySym left = FcitxKey_Alt_L;
                if (trigger == MacroSkipTriggerModifier::Shift) {
                    left = FcitxKey_Shift_L;
                } else if (trigger == MacroSkipTriggerModifier::Ctrl) {
                    left = FcitxKey_Control_L;
                }
                const bool isTrigger = rawSym == left || rawSym == left + 1;
                if (!isTrigger) {
                    trackingModifierTap_ = false;
                } else if (!keyEvent.isRelease()) {
                    trackingModifierTap_ = true;
                } else if (trackingModifierTap_) {
                    trackingModifierTap_ = false;
                    macroSkip_           = true;
                    lotusEngine_->setMacroEnabled(false);
                    LOTUS_INFO("Macro skip enabled for next word");
                }
                return;
            }
            trackingModifierTap_ = false;
        }
        if (keyEvent.isRelease()) {
            return;
        }
        if (held_) {
            // A printable key joins the held edit; any other key would reach the app before it.
            const uint32_t cp           = Key::keySymToUnicode(rawSym);
            const bool     surroundPath = currentMode() == LotusMode::SurroundingText ||
                (isUinputMode(currentMode()) && shouldUseSurroundingText());
            // Enter joins its restore to it first, see handleSurroundingText().
            const bool enter = rawSym == FcitxKey_Return && surroundPath;
            if ((cp < 0x20 && !enter) || cp == 0x7F || !surroundPath ||
                keyEvent.rawKey().states().testAny(KeyStates{KeyState::Ctrl, KeyState::Alt,
                    KeyState::Super, KeyState::Hyper, KeyState::Meta})) {
                sendHeldEdit();
            }
        }

        if (!engine_.kbSender().connected()) {
            LOTUS_WARN("Cannot connect to uinput server, reconnecting....");
            engine_.kbSender().ensureConnected();
        }
        // Finish a replacement whose echoes overran its count.
        if (currentBackspaceCount_ > expectedBackspaces_ &&
            g_isDeleting.load(std::memory_order_acquire)) {
            stopReplacement();
            if (!bufferedKeys_.empty()) {
                deferCommits_ = isDbus();
                replayBufferedKeys();
                flushCommitBatch();
            }
        }
        finishOverdueReplacement();

        if (g_mouseClicked.load(std::memory_order_acquire) &&
            !g_isDeleting.load(std::memory_order_acquire)) {
            g_mouseClicked.store(false, std::memory_order_release);
            invalidateSurroundingWord(true);
            clearAllBuffers();
        }

        if (uinputAbortPending_) {
            uinputAbortPending_ = false;
            // Stale abort: the context stayed away too long, likely a real application switch.
            const bool stale = uinputAbortTimeMs_ != 0 && nowMs() - uinputAbortTimeMs_ > 2000;
            if (stale) {
                LOTUS_INFO("Drop the replacement aborted at focus out: it went stale");
            }
            const bool restore = !stale && !stashedCommit_.empty();
            if (restore) {
                // The server-side Cancel finished the selection: the replacement text belongs at
                // the cursor.
                commitBatch_ += stashedCommit_;
                oldPreBuffer_ = stashedNewWord_;
                hasHistory_   = !stashedNewWord_.empty();
            } else {
                // Nothing to put back; the buffered keys replay as fresh input.
                stashedCommit_.clear();
                stashedNewWord_.clear();
                uinputAbortTimeMs_ = 0;
                clearWord();
            }
            deferCommits_ = isDbus();
            if (!bufferedKeys_.empty()) {
                replayBufferedKeys();
            }
            flushCommitBatch();
            if (restore) {
                LOTUS_INFO("Commit the replacement text aborted at focus out");
            }
        }

        KeySym currentSym = rawSym;
        // Shortcuts must not change sentence capitalization state. Ignore the side effects of the
        // backspaces of an automated replacement.
        if (*engine_.config().autoCapitalizeAfterPunctuation && currentMode() != LotusMode::Off &&
            !keyEvent.key().hasModifier() &&
            !((g_isDeleting.load(std::memory_order_acquire) && isBackspace(currentSym)) ||
                currentSym == FcitxKey_Pause)) {
            if (shouldCapitalize_) {
                if (currentSym >= FcitxKey_a && currentSym <= FcitxKey_z) {
                    currentSym = static_cast<KeySym>(currentSym - (FcitxKey_a - FcitxKey_A));
                    keyEvent.setKey(Key(currentSym, keyEvent.rawKey().states()));
                    shouldCapitalize_ = false;
                } else if (currentSym != FcitxKey_space) {
                    shouldCapitalize_ = false;
                }
            }
            switch (currentSym) {
                case FcitxKey_period:
                case FcitxKey_exclam:
                case FcitxKey_question:
                    isPrevPunctuation_ = true;
                    break;
                case FcitxKey_Return:
                case FcitxKey_KP_Enter:
                    shouldCapitalize_  = true;
                    isPrevPunctuation_ = false;
                    break;
                case FcitxKey_space:
                    if (isPrevPunctuation_) {
                        shouldCapitalize_  = true;
                        isPrevPunctuation_ = false;
                    }
                    break;
                default:
                    isPrevPunctuation_ = false;
                    break;
            }
        }

        if (g_isDeleting.load(std::memory_order_acquire)) {
            // In Select mode the uinput server emits Shift+Left, so the echoed Left presses are
            // the selection signal. The final one is swallowed so the app only sees the count it
            // should select.
            const bool isPause = currentSym == FcitxKey_Pause;
            if (isBackspace(currentSym) || isPause ||
                (realSelect_ && currentSym == FcitxKey_Left)) {
                if (!isPause && g_realTextLen.load(std::memory_order_acquire) > 0) {
                    g_realTextLen.fetch_sub(1, std::memory_order_acq_rel);
                }
                currentBackspaceCount_ += 1;
                if (currentBackspaceCount_ <= expectedBackspaces_ && !isPause) {
                    // Re-arm the deadline so a slow burst is never abandoned. The echo reaches the
                    // app to clear autofill/old text or extend the selection.
                    replacementDeadlineMs_ = nowMs() + replacementEchoGrace_;
                    return;
                }
                // The anchor closes the burst even if echoes are lost: wait for the app to
                // settle, commit the pending replacement string, swallow the final echo and
                // replay buffered keys.
                commitBatch_ += pendingCommitString_;
                pendingCommitString_.clear();
                returnEchoPending_ = false;
                deferCommits_      = isDbus();
                stopReplacement();
                keyEvent.filterAndAccept();
                if (returnAfterReplacement_ && sendReturn()) {
                    return; // the buffered keys follow the Enter's anchor
                }
                flushAndReplay();
            } else if (returnEchoPending_ && currentSym == FcitxKey_Return) {
                returnEchoPending_ = false; // the Enter the uinput server typed: to the app as is
            } else if (realSelect_ && currentSym == FcitxKey_Delete) {
                keyEvent.forward();
            } else {
                // Buffer incoming keys during the short uinput replacement burst to prevent them
                // from interfering with backspaces and the pending commit. Some non-text keys may
                // be consumed during this window. Forwarding them immediately could change the
                // cursor before replacement completes. This is a known limitation of the
                // fast-typing buffer. The modifier is not decided here: at press time the echo
                // counter is a racy proxy for the uinput server's Shift hold in Select mode. The
                // raw event is buffered and the modifier is chosen when the key is popped for
                // replay, see replayBufferedKeys().
                if (!Key::keySymToUTF8(currentSym).empty()) {
                    LOTUS_WARN("Typing so fast, add key to queue");
                    bufferedKeys_.push({.sym = currentSym, .state = keyEvent.rawKey().states()});
                }
                keyEvent.filterAndAccept();
            }
            return;
        }

        if (currentMode() != LotusMode::Off && currentMode() != LotusMode::Emoji &&
            handleRevertKey(keyEvent)) {
            return;
        }

        if (currentMode() != LotusMode::Off) {
            if (*engine_.config().doubleSpaceToPeriod &&
                handleDoubleKey(keyEvent,
                    currentSym == FcitxKey_space || currentSym == FcitxKey_KP_Space, isPrevSpace_,
                    {U" ", U". "}, "Commit: . ")) {
                if (*engine_.config().autoCapitalizeAfterPunctuation) {
                    isPrevPunctuation_ = true;
                    shouldCapitalize_  = true;
                }
                return;
            }
            if (*engine_.config().doubleHyphenToEmDash &&
                handleDoubleKey(keyEvent,
                    currentSym == FcitxKey_minus || currentSym == FcitxKey_KP_Subtract,
                    isPrevHyphen_, {U"-", U"—"}, "Commit: — (em-dash)")) {
                return;
            }
        }

        // English restore may rewrite earlier words only where committed text can be deleted;
        // handleUinputMode and handleSurroundingText allow it.
        lotusEngine_->setRewriteAllowed(false);
        switch (currentMode()) {
            case LotusMode::Smooth:
            case LotusMode::SuperSmooth:
            case LotusMode::Select: {
                if (!shouldUseSurroundingText()) {
                    handleUinputMode(keyEvent, currentSym);
                    break;
                }
                // The app reports the text around the cursor (browsers, GTK text fields): replace
                // through the input method protocol instead of injected Backspaces, so deletion
                // and commit travel together and cannot race or be dropped.
                if (!oldPreBuffer_.empty()) {
                    clearWord();
                }
                handleSurroundingText(keyEvent, currentSym);
                break;
            }
            case LotusMode::SurroundingText:
                handleSurroundingText(keyEvent, currentSym);
                break;
            case LotusMode::Preedit:
                handlePreeditMode(keyEvent, currentSym);
                break;
            case LotusMode::Emoji:
                handleEmojiMode(keyEvent);
                break;
            default: {
                if (*engine_.config().enableMacroInOffMode && *engine_.config().enableMacro) {
                    handleOffModeMacro(keyEvent, currentSym);
                }
                break;
            }
        }

        // Re-enable macro when the current word ends (engine preedit empty).
        if (macroSkip_ && lotusEngine_->preedit().empty()) {
            macroSkip_ = false;
            lotusEngine_->setMacroEnabled(*engine_.config().enableMacro);
        }
    }

    void LotusState::finishOverdueReplacement() {
        if (replacementDeadlineMs_ == 0 || nowMs() < replacementDeadlineMs_) {
            return;
        }
        replacementDeadlineMs_ = 0;
        if (!g_isDeleting.load(std::memory_order_acquire)) {
            return;
        }

        cancelPendingUinput();
        LOTUS_WARN("Replacement ack timed out, finishing without the remaining backspace echoes");
        commitBatch_ += pendingCommitString_;
        pendingCommitString_.clear();
        if (returnEchoPending_) {
            // The server typed no Enter (an older server without KbOp::Return): forwarding is all
            // that is left, which GTK4 apps drop.
            returnEchoPending_ = false;
            engine_.setUinputTypesReturn(false);
            LOTUS_WARN("No Return from the uinput server (older than the addon?), forwarding it");
            ic_.forwardKey(Key(FcitxKey_Return), false);
            ic_.forwardKey(Key(FcitxKey_Return), true);
        }
        deletionInterruptedAt_ = 0;
        stopReplacement();
        const bool returned = returnAfterReplacement_ && sendReturn();
        clearWord();
        if (returned) {
            return;
        }
        deferCommits_ = isDbus();
        flushAndReplay();
    }

    bool LotusState::handleDoubleKey(
        KeyEvent& keyEvent, bool isKey, bool& prev, DiffPart replacement, const char* logMsg) {
        if (!isKey || keyEvent.key().hasModifier()) {
            prev = false;
            return false;
        }
        if (!prev) {
            prev = true;
            return false;
        }
        keyEvent.filterAndAccept();
        // Both replacements delete exactly one code point.
        if (currentMode() == LotusMode::SurroundingText) {
            replaceSurrounding(1, replacement.addedPart);
        } else {
            performReplacement(replacement);
        }
        LOTUS_INFO(logMsg);
        prev = false;
        return true;
    }

    bool LotusState::shouldUseSurroundingText() const {
        if (!*engine_.config().autoSurroundingText) {
            return false;
        }
        // Never switch path while an injected replacement is still in flight.
        if (g_isDeleting.load(std::memory_order_acquire)) {
            return false;
        }
        const auto flags = ic_.capabilityFlags();
        // Terminals and other apps that never send their text stay on uinput.
        const auto& surrounding = ic_.surroundingText();
        if (!flags.test(CapabilityFlag::SurroundingText) || !surrounding.isValid()) {
            return false;
        }
        // Password fields: let the uinput path (and its own guards) decide.
        if (flags.test(CapabilityFlag::Password) || flags.test(CapabilityFlag::Sensitive)) {
            return false;
        }
        // Chromium address bar: inline autocomplete keeps a selection after the cursor, and
        // deleteSurroundingText first eats that selection, leaving one character too many
        // ("chans" -> "chaán"). The uinput path already handles it with Select.
        if (flags.test(CapabilityFlag::Url) || engine_.isLikeAddressBarChromium()) {
            LOTUS_DEBUG("Auto surrounding: Chromium address bar, use uinput");
            return false;
        }
        if (surrounding.anchor() != surrounding.cursor()) {
            LOTUS_DEBUG("Auto surrounding: selection present, use uinput");
            return false;
        }
        return true;
    }

    void LotusState::reset(bool isFocusOut) {
        if (isFocusOut) {
            sendHeldEdit();
        }
        if (isFocusOut && isUinputMode(currentMode())) {
            deferredFocusOut_ = true;
            deliverPendingCommits();
            resetMacroSkip();
            ic_.inputPanel().reset();
            return;
        }
        deferredFocusOut_ = false;
        resetNow(isFocusOut);
    }

    void LotusState::resetNow(bool isFocusOut) {
        g_realTextLen.store(utf8::length(ic_.surroundingText().text()), std::memory_order_release);
        if (g_isDeleting.load(std::memory_order_acquire)) {
            return;
        }
        deliverPendingCommits();
        resetMacroSkip();

        isPrevSpace_  = false;
        isPrevHyphen_ = false;
        if (isFocusOut) {
            shouldCapitalize_  = false;
            isPrevPunctuation_ = false;
        }
        clearWord(true);
        if (!isDbus()) {
            clearAllBuffers(!isFocusOut);
        }

        switch (currentMode()) {
            case LotusMode::Preedit:
            case LotusMode::Emoji: {
                ic_.inputPanel().reset();
                ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
                ic_.updatePreedit();
                break;
            }
            case LotusMode::SurroundingText:
            case LotusMode::Smooth:
            case LotusMode::SuperSmooth:
            case LotusMode::Select:
                ic_.inputPanel().reset();
                break;
            default:
                break;
        }
    }

    void LotusState::flushDeferredFocusOut() {
        if (!deferredFocusOut_) {
            return;
        }
        deferredFocusOut_ = false;
        resetNow(true);
    }

    void LotusState::cancelDeferredFocusOut() {
        deferredFocusOut_ = false;
    }

    void LotusState::commitBuffer() {
        deliverPendingCommits();
        switch (currentMode()) {
            case LotusMode::Preedit: {
                ic_.inputPanel().reset();
                ic_.inputPanel().setClientPreedit(Text());
                ic_.inputPanel().setPreedit(Text());
                ic_.updatePreedit();
                ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
                lotusEngine_->commitPreedit();
                lotus::fcitx_text::commit(&ic_, lotusEngine_->takeCommit(), lastCommitTime_);
                lotusEngine_->reset();
                ic_.updateUserInterface(UserInterfaceComponent::InputPanel);
                ic_.updatePreedit();
                break;
            }
            case LotusMode::Smooth:
            case LotusMode::SurroundingText:
            case LotusMode::SuperSmooth:
            case LotusMode::Select:
                lotusEngine_->reset();
                break;
            default:
                break;
        }
    }

    void LotusState::clearAllBuffers(bool keepSentenceState) {
        deliverPendingCommits();
        LOTUS_DEBUG("Clear all buffers");
        if (g_isDeleting.load(std::memory_order_acquire)) {
            return;
        }
        resetMacroSkip();
        invalidateSurroundingWord(false);
        oldPreBuffer_.clear();
        hasHistory_         = false;
        uinputAbortPending_ = false;
        stashedCommit_.clear();
        stashedNewWord_.clear();
        uinputAbortTimeMs_     = 0;
        expectedBackspaces_    = 0;
        currentBackspaceCount_ = 0;
        replacementDeadlineMs_ = 0;
        pendingCommitString_.clear();
        emojiBuffer_.clear();
        emojiCandidates_.clear();
        bufferedKeys_.clear();
        if (!keepSentenceState) {
            shouldCapitalize_  = false;
            isPrevPunctuation_ = false;
        }
        isPrevSpace_  = false;
        isPrevHyphen_ = false;
        lotusEngine_->reset();
        lotusEngine_->forgetContext();
    }

    bool LotusState::isEmptyHistory() const {
        return !hasHistory_;
    }

    void LotusState::scheduleDeferredCommit() {
        if (deferredCommitPending_) {
            return;
        }
        deferredCommitPending_ = true;
        deferredCommitTimer_   = engine_.instance()->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC), 0, [this](EventSourceTime*, uint64_t) {
                deferredCommitPending_ = false;
                deferCommits_          = false;
                commitBatch_           = std::move(deferredCommitText_);
                deferredCommitText_.clear();
                if (!returnEchoPending_) {
                    replayBufferedKeys(); // else after the Enter the uinput server types
                }
                flushCommitBatch();
                requestDeferredReturn();
                return false;
            });
    }

    void LotusState::requestDeferredReturn() {
        if (!returnAfterDeferredCommit_) {
            return;
        }
        returnAfterDeferredCommit_ = false;
        if (!returnEchoPending_) {
            return; // the burst was given up meanwhile
        }
        const uint32_t post    = configuredPostDelay();
        replacementDeadlineMs_ = nowMs() + post + kReplacementAckSlackMs;
        sendKbMsg(KbOp::Return, 1, 0, 0, post);
        LOTUS_INFO("Send Return through the uinput server, after the commit");
    }

    void LotusState::flushCommitBatch() {
        if (commitBatch_.empty()) {
            return;
        }
        if (deferCommits_) {
            deferredCommitText_ += commitBatch_;
            commitBatch_.clear();
            scheduleDeferredCommit();
            return;
        }
        lotus::fcitx_text::commit(&ic_, commitBatch_, lastCommitTime_);
        commitBatch_.clear();
    }

    void LotusState::deliverPendingCommits() {
        deferCommits_ = false;
        if (!deferredCommitText_.empty()) {
            commitBatch_ += deferredCommitText_;
            deferredCommitText_.clear();
            deferredCommitPending_ = false;
        }
        flushCommitBatch();
        requestDeferredReturn();
    }

    void LotusState::flushAndReplay() {
        if (deferCommits_) {
            flushCommitBatch();
            if (!bufferedKeys_.empty()) {
                scheduleDeferredCommit(); // replays them, also after a burst without commit (Enter)
            }
            return;
        }
        replayBufferedKeys();
        flushCommitBatch();
    }

    void LotusState::replayBufferedKeys() {
        const bool stripSelectShift = lastSelect_;
        LOTUS_INFO("Starting replay buffered keys");
        while (!bufferedKeys_.empty()) {
            const KeyEntry entry = bufferedKeys_.pop();
            auto           sym   = static_cast<KeySym>(entry.sym);
            KeyStates      state(entry.state);
            // The modifier is chosen at pop time, not at press time. Replay runs after the
            // replacement burst finished, so the uinput server has already released the synthetic
            // Shift it held for the Shift+Left selection. A stored Shift bit is therefore stripped
            // unless Shift is still physically held right now (tracked live from key events),
            // which means the user is holding it for a genuine capital letter. Backspace-driven
            // modes never touch Shift, so their state is replayed untouched.
            // Known limitation: Shift is sampled at replay time, not press time. A capital typed
            // with Shift during the burst is replayed as lowercase if Shift was released before
            // the replay runs.
            if (stripSelectShift && state.test(KeyState::Shift) && !shiftHeld_) {
                sym   = unshiftKeySym(sym);
                state = state.unset(KeyState::Shift);
            }
            if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) {
                // Typed during a replacement: the word before it is decided as at a word break,
                // and Enter goes to the app as a key (committing "\r" as text submits nothing).
                std::u32string shown;
                std::u32string commitStr;
                DiffPart       diffPart; // views into shown and commitStr
                if (!oldPreBuffer_.empty() && lotusEngine_->endWord()) {
                    commitStr = lotusEngine_->takeCommit();
                    shown     = lotusEngine_->takeRewrittenBefore() + oldPreBuffer_;
                    diffPart  = compareAndSplitU32(shown, commitStr);
                }
                clearWord(true);
                if (!diffPart.deletedPart.empty() && engine_.kbSender().connected()) {
                    flushCommitBatch();
                    // Enter follows the replacement; remaining keys stay queued for the next
                    // replay cycle.
                    returnAfterReplacement_ = true;
                    performReplacement(diffPart);
                    return;
                }
                commitBatch_ += diffPart.addedPart;
                if (sendReturn()) {
                    return; // the rest follows the Enter's anchor
                }
                continue;
            }
            const uint32_t keyCodePoint = Key::keySymToUnicode(sym);
            if (keyCodePoint == 0U) {
                continue;
            }
            const std::u32string keyText(1, static_cast<char32_t>(keyCodePoint));
            const bool           processed = lotusEngine_->processKey(
                engineKeyValue(sym), lotus::keyStatesFromBits(state.toInteger()));

            const std::u32string commitStr = lotusEngine_->takeCommit();
            if (!commitStr.empty()) {
                // English restore may rewrite the words before ("thí makes" -> "this makes"), as
                // in handleUinputMode: without them here a fast typist got the earlier word twice.
                const std::u32string shown    = lotusEngine_->takeRewrittenBefore() + oldPreBuffer_;
                const DiffPart       diffPart = compareAndSplitU32(shown, commitStr);
                bool                 replacementStarted = false;
                if (!diffPart.deletedPart.empty()) {
                    flushCommitBatch();
                    // Remaining keys stay queued for the next replay cycle.
                    performReplacement(diffPart);
                    replacementStarted = true;
                } else {
                    commitBatch_ += diffPart.addedPart;
                }
                clearWord();
                if (replacementStarted) {
                    return;
                }
            } else if (!processed) {
                commitBatch_ += keyText;
            } else {
                hasHistory_ = true;
                g_realTextLen.fetch_add(1, std::memory_order_acq_rel);
                const std::u32string preeditStr = lotusEngine_->preedit();
                const DiffPart       diffPart   = compareAndSplitU32(oldPreBuffer_, preeditStr);
                if (diffPart.deletedPart.empty()) {
                    if (!diffPart.addedPart.empty()) {
                        commitBatch_ += diffPart.addedPart;
                        oldPreBuffer_ = preeditStr;
                    }
                } else if (!engine_.kbSender().connected()) {
                    commitBatch_ += keyText;
                } else {
                    g_isDeleting.store(false, std::memory_order_release);
                    flushCommitBatch();
                    // Remaining keys stay queued for the next replay cycle.
                    performReplacement(diffPart);
                    oldPreBuffer_ = preeditStr;
                    return;
                }
            }
        }
        LOTUS_INFO("Replay buffered keys done");
    }

    void LotusState::forgetShiftState() {
        shiftHeld_ = false;
    }

    void LotusState::resetMacroSkip() {
        trackingModifierTap_ = false;
        macroSkip_           = false;
        lotusEngine_->setMacroEnabled(*engine_.config().enableMacro);
    }

} // namespace fcitx
