/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file lotus-state.h
 * @brief Input context state management for fcitx5-vanh.
 */

#ifndef _FCITX5_LOTUS_STATE_H_
#define _FCITX5_LOTUS_STATE_H_

#include "emoji-entry.h"
#include "key_processor.h"
#include "lotus-key-buffer.h"
#include "common-protocol.h"
#include "lotus-utils.h"

#include <chrono>
#include <cstddef>
#include <fcitx-utils/event.h>
#include <fcitx/inputcontext.h>
#include <memory>

namespace fcitx {
    class LotusEngine;
    class CommonCandidateList;
    class SurroundingText;

    /**
     * @brief Per-input-context state for Lotus input method.
     *
     * Manages the input state, buffers, and mode-specific handling for each input context.
     */
    class LotusState final : public InputContextProperty {
      public:
        /**
         * @brief Constructs a new state instance.
         * @param engine Pointer to the Lotus engine.
         * @param ic Pointer to the input context.
         */
        LotusState(LotusEngine* engine, InputContext* ic);

        /** @brief Initializes the bamboo engine for this state. */
        void setEngine();

        /** @brief Applies current options to the engine. */
        void setOption();

        /**
         * @brief Main key event handler.
         * @param keyEvent The key event to process.
         */
        void keyEvent(KeyEvent& keyEvent);

        /**
         * @brief Resets the input state.
         * @param isFocusOut If true, the reset comes from a focus-out event. Focus-out never
         *        commits text: fcitx5 core or the client (CapabilityFlag::ClientUnfocusCommit)
         *        owns that commit.
         */
        void reset(bool isFocusOut = false);

        /** @brief Applies the reset that a focus-out deferred in uinput modes. */
        void flushDeferredFocusOut();

        /** @brief Drops the reset deferred by a focus-out (the focus-out was a bounce). */
        void cancelDeferredFocusOut();

        /** @brief Finishes a replacement whose backspace echoes were lost (focus-out mid pump). */
        void finishOverdueReplacement();

        /** @brief Commits the current buffer. */
        void commitBuffer();

        /**
         * @brief Clears all internal buffers.
         * @param keepSentenceState Keep the sentence state (shouldCapitalize_, isPrevPunctuation_)
         */
        void clearAllBuffers(bool keepSentenceState = false);

        /**
         * @brief Checks if history buffer is empty.
         * @return True if no history.
         */
        [[nodiscard]] bool isEmptyHistory() const;
        friend class EmojiCandidateWord;
        friend class LotusEngine;

      private:
        LotusEngine&  engine_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
        InputContext& ic_;     // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
        std::shared_ptr<lotus::KeyProcessor> lotusEngine_;
        std::u32string                       oldPreBuffer_;
        /// Word before the cursor as we last wrote it (surrounding path)
        std::u32string surrWord_;
        bool           surrKnown_       = false;
        int64_t        surrSoftResetAt_ = 0;
        /// Surrounding edit held back for GNOME Shell, see replaceSurrounding()
        bool                             held_        = false;
        size_t                           heldDeleted_ = 0;
        std::u32string                   heldAdded_;
        int64_t                          lastSurroundingEditMs_ = 0;
        std::unique_ptr<EventSourceTime> heldTimer_;
        bool                             hasHistory_            = false;
        size_t                           expectedBackspaces_    = 0;
        size_t                           currentBackspaceCount_ = 0;
        /// when real shift +left is send
        bool realSelect_ = false;
        /// kind of the most recent burst, for replay
        bool           lastSelect_ = false;
        std::u32string pendingCommitString_;
        /// Text of the replacement batch, sent as one commitString
        std::u32string commitBatch_;
        /// Pending dbus commit timer
        std::unique_ptr<EventSourceTime> deferredCommitTimer_;
        /// Text waiting for the deferred commit
        std::u32string deferredCommitText_;
        /// Timer armed and not fired yet
        bool deferredCommitPending_ = false;
        /// Route the batch through the deferred timer
        bool deferCommits_ = false;
        /// Per-app rule (mode + delay overrides) pushed by the engine
        LotusAppRuleSetting     appRuleSetting_;
        std::u32string          emojiBuffer_;
        std::vector<EmojiEntry> emojiCandidates_;
        /// Keystrokes buffered during replacement
        KeyBuffer bufferedKeys_;
        bool      isPrevSpace_        = false;
        bool      isPrevHyphen_       = false;
        bool      shouldCapitalize_   = false;
        bool      isPrevPunctuation_  = false;
        int64_t   lastDeactivateTime_ = 0;
        /// when deactivate() cut an in-flight replacement (0 = none)
        int64_t deletionInterruptedAt_ = 0;
        /// uinput focus-out reset waiting for activate()
        bool deferredFocusOut_ = false;
        /// Selected modifier held, waiting for consecutive keyup
        bool trackingModifierTap_ = false;
        /// Live Shift press state tracked from key events
        bool shiftHeld_ = false;
        /// Macro disabled for the current word
        bool macroSkip_     = false;
        bool isLibreoffice_ = false;
        bool isChromium_    = false;
        /// ms by which every injected backspace must be echoed (0 = no replacement in flight)
        int64_t replacementDeadlineMs_ = 0;
        /// deadline grace re-armed on each echo
        int64_t                               replacementEchoGrace_ = 0;
        std::chrono::steady_clock::time_point lastCommitTime_;
        /// Enter held back until the restore of the word before it is on screen
        bool returnAfterReplacement_ = false;
        /// the uinput server types that Enter: let its echo through
        bool returnEchoPending_ = false;
        /// ask the server for that Enter once the deferred commit is out
        bool returnAfterDeferredCommit_ = false;
        /// Replacement aborted at focus-out, committed once focus returns
        bool uinputAbortPending_ = false;
        /// replacement text that must not be lost
        std::u32string stashedCommit_;
        /// post-replacement word (oldPreBuffer_ at abort time)
        std::u32string stashedNewWord_;
        /// when the abort happened
        int64_t uinputAbortTimeMs_ = 0;

        /**
         * @brief Performs a reset without the uinput focus-out deferral.
         * @param isFocusOut True when the reset comes from a focus-out event. Only cleans up
         *        composition state; it never commits text.
         */
        void resetNow(bool isFocusOut);

        /** @brief Whether the frontend of the input context is dbus. */
        [[nodiscard]] bool isDbus() const;

        /** @brief Post-delay of the uinput bursts: app rule, else the configured default. */
        [[nodiscard]] uint32_t configuredPostDelay() const;

        /** @brief Resets the engine word and the shown-word state, optionally the context too. */
        void clearWord(bool forgetContext = false);

        /** @brief Ends the in-flight replacement: counters, deadline and g_isDeleting. */
        void stopReplacement();

        /**
         * @brief Deletes text before the cursor through the input method protocol, then commits.
         * @param deleted Number of code points to delete before the cursor.
         * @param added Text to commit.
         */
        void replaceSurrounding(size_t deleted, std::u32string_view added);

        /** @brief Sends the surrounding edit held back by replaceSurrounding(), if any. */
        void sendHeldEdit();

        /**
         * @brief Sends a keyboard request to the uinput server (reconnect on failure).
         * @param op Operation to request.
         * @param count Number of keys to inject.
         * @param preDelay Delay in milliseconds before executing the keyboard operation.
         * @param interval Delay in milliseconds between consecutive injected keys.
         * @param postDelay Delay in milliseconds after completing the keyboard operation.
         */
        void sendKbMsg(
            KbOp op, size_t count, uint32_t preDelay, uint32_t interval, uint32_t postDelay);

        /** @brief Sends Cancel to drop the keys still pending from the in-flight replacement. */
        void cancelPendingUinput();

        /** @brief Cancels the in-flight burst, stashing the replacement for the refocus commit. */
        void abortPendingUinputReplacement();

        /**
         * @brief Sends the Enter held back.
         * @return True when the uinput server types it (a burst is in flight).
         */
        bool sendReturn();

        /** @brief Asks the uinput server for the Enter that waited for the deferred commit. */
        void requestDeferredReturn();

        /**
         * @brief The revert key right after a word break: the word switches to its other reading.
         * @return True when there was a word to revert (the key is consumed).
         */
        bool handleRevertKey(KeyEvent& keyEvent);

        /**
         * @brief Handles a double press (space to period, hyphen to em-dash).
         * @param keyEvent The key event.
         * @param isKey Whether the key is the one that doubles.
         * @param prev Whether the previous key was the same one, updated here.
         * @param replacement Deleted and added text of the replacement.
         * @param logMsg Message to log once the replacement is issued.
         * @return True when the shortcut fired and the event was consumed.
         */
        bool handleDoubleKey(
            KeyEvent& keyEvent, bool isKey, bool& prev, DiffPart replacement, const char* logMsg);

        /** @brief Checks if autofill is certain for the surrounding text. */
        bool isAutofillCertain();

        /** @brief Handles key events in preedit mode. */
        void handlePreeditMode(KeyEvent& keyEvent, KeySym currentSym);

        /** @brief Updates emoji page status in candidate list. */
        void updateEmojiPageStatus(CommonCandidateList& commonList);

        /** @brief Handles key events in emoji mode. */
        void handleEmojiMode(KeyEvent& keyEvent);

        /** @brief Updates preedit display for emoji mode. */
        void updateEmojiPreedit();

        /**
         * @brief Performs text replacement via uinput.
         * @param diffPart pairs of deleted part and added part
         */
        void performReplacement(DiffPart diffPart);

        /**
         * @brief Forwards special keys (cursor move, Tab, Escape, modifiers) and maps keypad keys.
         * @param keyEvent The key event.
         * @param currentSym The current key symbol, rewritten for keypad keys.
         * @return True when the event was forwarded.
         */
        bool forwardSpecialKey(KeyEvent& keyEvent, KeySym& currentSym);

        /** @brief Handles uinput mode processing. */
        void handleUinputMode(KeyEvent& keyEvent, KeySym currentSym);

        /** @brief Handles surrounding text mode. */
        void handleSurroundingText(KeyEvent& keyEvent, KeySym currentSym);

        /** @brief Whether the surrounding text path is usable for this key (auto mode). */
        bool shouldUseSurroundingText() const;

        /**
         * @brief Forgets the tracked surrounding word.
         * @param hard False keeps it for a soft reset (focus bounce).
         */
        void invalidateSurroundingWord(bool hard);

        /** @brief Handles Off mode with macro shadow processing. */
        void handleOffModeMacro(KeyEvent& keyEvent, KeySym currentSym);

        /** @brief Sends the batch as one commitString, now or through the deferred timer. */
        void flushCommitBatch();

        /** @brief Arms the deferred commit timer when it is not armed yet. */
        void scheduleDeferredCommit();

        /** @brief Delivers the text waiting for the deferred timer, then the current batch. */
        void deliverPendingCommits();

        /** @brief Ends a burst: flushes the batch, replays (or schedules) the buffered keys. */
        void flushAndReplay();

        /**
         * @brief Replays keystrokes buffered during replacement.
         *
         * When g_isDeleting is true, non-special keystrokes are buffered
         * instead of being discarded. This method replays them after the
         * replacement completes.
         */
        void replayBufferedKeys();

        /** @brief Forgets the tracked Shift state (a focus-out may have swallowed a release). */
        void forgetShiftState();

        /** @brief Clears the macro-skip state and re-syncs the engine. */
        void resetMacroSkip();
    };

} // namespace fcitx

#endif // _FCITX5_LOTUS_STATE_H_
