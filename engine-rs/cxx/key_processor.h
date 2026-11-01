/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 * SPDX-FileCopyrightText: 2026-2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file key_processor.h
 * @brief Key event processor of the Rust engine (engine-rs/src/key_processor.rs), from
 *        bamboo/fcitxbambooengine.go plus the fork's English restore.
 *
 * The class is a thin owner of a LotusKeyProcessor handle: every method forwards to the C ABI.
 * Key values stay in the fcitx keysym domain, texts are UTF-32, and UTF-8 only appears in the
 * configuration (macro table, dictionary, time formats).
 */

#ifndef _LOTUS_KEY_PROCESSOR_H_
#define _LOTUS_KEY_PROCESSOR_H_

#include "dictionary.h"
#include "encoder.h"
#include "input_method.h"
#include "macro_table.h"
#include "types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct LotusKeyProcessor;
struct LotusLearned;

namespace lotus {

    /** @brief Keysyms of the special keys the processor reacts to, 1:1 with Go's constants. */
    inline constexpr uint32_t kFcitxBackSpace = 0xFF08U;
    inline constexpr uint32_t kFcitxSpace     = 0x020U;
    inline constexpr uint32_t kFcitxTab       = 0xFF09U;

    /** @brief Options of the processor, 1:1 with Go's FcitxBambooEngineOption (bamboo-c.go). */
    struct Options {
        bool    autoNonVnRestore    = true;             // Restore the raw key strokes of a non-Vietnamese word
        bool    ddFreeStyle         = true;             // Allow "dd" inside a non-Vietnamese sequence
        bool    englishRestore      = false;            // Restore an English word to its keys at a word break (Vikey lists)
        bool    macroEnabled        = false;            // Expand macros at a word break and on Tab
        bool    autoCapitalizeMacro = false;            // Follow the case of the trigger word
        bool    spellCheckWithDicts = true;             // Spell check against the custom dictionary
        bool    modernStyle         = true;             // Tone placement style (EngineFlags::StdToneStyle)
        bool    freeMarking         = true;             // Free tone marking style
        Charset outputCharset       = Charset::Unicode; // Charset of the committed text
        // Empty optional = follow the engine flags, the mode a fresh engine starts in. The addon always
        // fills them from the configuration, so it only ever sees the three public enumerators.
        std::optional<W2uMode>              w2u              = std::nullopt; // "w" to "ư" mode
        std::optional<BracketTransformMode> bracketTransform = std::nullopt; // "[ -> ơ, ] -> ư" mode
        std::string                         timeFormat       = "%H:%M";      // Format of the $TIME macro
        std::string                         dateFormat       = "%d/%m/%Y";   // Format of the $DATE macro
    };

    /**
     * @brief Key event wrapper driving an engine, like Go's FcitxBambooEngine.
     *
     * The processor shares the macro table and the dictionary with the other input contexts; it is
     * movable but not copyable.
     */
    /**
     * @brief Preferences learned from reverts (opt-in, local): per typed keys, keep the keys or
     *        the Vietnamese reading. Shared by every input context; the addon persists it.
     */
    class Learned {
      public:
        Learned();
        /** @brief Replaces the table with a saved one ("keys<TAB>score" lines). */
        void loadFromText(std::string_view text);
        /** @brief The table in the saved form. */
        [[nodiscard]] std::string toText() const;
        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] const LotusLearned* handle() const {
            return handle_.get();
        }

      private:
        std::unique_ptr<LotusLearned, void (*)(LotusLearned*)> handle_;
    };

    class KeyProcessor {
      public:
        /**
         * @brief Builds a processor, like Go's NewEngine / NewCustomEngine.
         * @param inputMethod Input method of the underlying engine (the processor keeps its own copy).
         * @param spellCheckWithDictsDefault Initial value of Options::spellCheckWithDicts (Go's
         *                                   NewEngine passes true, NewCustomEngine passes false).
         */
        KeyProcessor(const InputMethod& inputMethod, bool spellCheckWithDictsDefault);

        /** @brief Applies a whole option set, like Go's EngineSetOption (recomputes the engine flags). */
        void setOptions(const Options& options);

        /** @brief Enables or disables macro expansion, like Go's EngineSetMacroEnabled. */
        void setMacroEnabled(bool enabled);

        /** @brief Arms the "restore key strokes" state of the Shift+Space workaround. */
        void setShouldRestoreKeyStrokes(bool restore);

        /** @brief True when the next key event restores the raw key strokes. */
        [[nodiscard]] bool shouldRestoreKeyStrokes() const;

        /** @brief Shares a macro table with this processor (null disables the lookups). */
        void setMacroTable(std::shared_ptr<const MacroTable> table);

        /** @brief Shares a custom dictionary with this processor (null disables the lookups). */
        void setDictionary(std::shared_ptr<const Dictionary> dictionary);

        /**
         * @brief Processes one key event, like Go's preeditProcessKeyEvent.
         * @param keyVal Keysym of the key (kFcitxBackSpace / kFcitxSpace / kFcitxTab / a code point).
         * @param states Modifier states of the event.
         * @return True when the processor handled the key, false when the addon must let it through.
         */
        bool processKey(uint32_t keyVal, KeyStates states);

        /** @brief True for space, backspace, a word break symbol, a macro Tab or an engine key (Go's canProcessKey). */
        [[nodiscard]] bool canProcessKey(uint32_t keyVal) const;

        /** @brief False when control, alt, super, hyper, meta or the forward modifier is held (Go's isValidState). */
        [[nodiscard]] static bool isValidState(KeyStates states) {
            return !states.intersects(kInvalidStates);
        }

        /** @brief Charset encoded preedit text to display, like Go's preeditText. */
        [[nodiscard]] std::u32string preedit() const;

        /** @brief Takes the charset encoded text to commit and clears it, like Go's EnginePullCommit. */
        std::u32string takeCommit();

        /** @brief Commits the current preedit text, like Go's EngineCommitPreedit. */
        void commitPreedit();

        /** @brief Forgets the composition and the pending commit, like Go's ResetEngine. */
        void reset();

        /** @brief Rebuilds the composition from a text, like Go's EngineRebuildFromText. */
        void rebuildFromText(std::u32string_view text);

        /**
         * @brief Allows English restore to rewrite the words before the current one ("thí makes" ->
         *        "this makes"). Only for modes that can delete committed text (uinput, surrounding).
         */
        void setRewriteAllowed(bool allowed);

        /**
         * @brief Takes the on-screen text of the earlier words the last commit rewrites (with the
         *        spaces after them), to delete before the commit. Empty when nothing is rewritten.
         */
        std::u32string takeRewrittenBefore();

        /**
         * @brief Forgets the words before the cursor (click, cursor move, focus change, Backspace
         *        into an earlier word): English restore then judges the next word on its own.
         */
        void forgetContext();

        /**
         * @brief Enter after a word: decides the word as a word break would (an English word last
         *        on a line goes back to its keys). The text goes to takeCommit(), the text to
         *        delete first to takeRewrittenBefore(); the engine is reset and the context
         *        forgotten. True when the shown word must be replaced before Enter is passed on.
         */
        bool endWord();

        /**
         * @brief One-key correction right after a word break: the word just finished changes to
         *        its other reading (keys <-> Vietnamese). Same outputs as endWord(); with learning
         *        on, the choice is remembered. False when there is nothing to revert.
         */
        bool revertLastSwitch();

        /** @brief Shares the preferences learned from reverts; null turns learning off. */
        void setLearned(std::shared_ptr<Learned> learned);

        /**
         * @brief Formats the current date or time, like Go's formatTime.
         * @param format Format configured for the $TIME / $DATE macro.
         * @return Formatted current local time, empty for an empty format.
         */
        [[nodiscard]] static std::string formatTime(std::string_view format);

      private:
        std::unique_ptr<LotusKeyProcessor, void (*)(LotusKeyProcessor*)> handle_;
        std::shared_ptr<const MacroTable>                                macroTable_; ///< Keeps the C++ owner alive
        std::shared_ptr<const Dictionary>                                dictionary_; ///< Keeps the C++ owner alive
        std::shared_ptr<Learned>                                         learned_;    ///< Keeps the C++ owner alive
    };

} // namespace lotus

#endif // _LOTUS_KEY_PROCESSOR_H_
