/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "key_processor.h"

#include "ffi_util.h"

#include <utility>

namespace lotus {

    namespace {
        template <typename Mode>
        int8_t optionalMode(const std::optional<Mode>& mode) {
            return mode.has_value() ? static_cast<int8_t>(*mode) : int8_t{-1};
        }
    } // namespace

    std::string encode(Charset charset, std::u32string_view text) {
        std::string out;
        lotus_encode(static_cast<uint8_t>(charset), ffi::u32(text), text.size(), ffi::assignBytes, &out);
        return out;
    }

    KeyProcessor::KeyProcessor(const InputMethod& inputMethod, bool spellCheckWithDictsDefault) :
        handle_(lotus_key_processor_new(inputMethod.handle(), spellCheckWithDictsDefault), lotus_key_processor_free) {}

    void KeyProcessor::setOptions(const Options& options) {
        const LotusOptions raw{
            .auto_non_vn_restore    = options.autoNonVnRestore,
            .dd_free_style          = options.ddFreeStyle,
            .english_restore        = options.englishRestore,
            .macro_enabled          = options.macroEnabled,
            .auto_capitalize_macro  = options.autoCapitalizeMacro,
            .spell_check_with_dicts = options.spellCheckWithDicts,
            .modern_style           = options.modernStyle,
            .free_marking           = options.freeMarking,
            .output_charset         = static_cast<uint8_t>(options.outputCharset),
            .w2u                    = optionalMode(options.w2u),
            .bracket_transform      = optionalMode(options.bracketTransform),
            .time_format            = ffi::str(options.timeFormat),
            .date_format            = ffi::str(options.dateFormat),
        };
        lotus_key_processor_set_options(handle_.get(), &raw);
    }

    void KeyProcessor::setMacroEnabled(bool enabled) {
        lotus_key_processor_set_macro_enabled(handle_.get(), enabled);
    }

    void KeyProcessor::setShouldRestoreKeyStrokes(bool restore) {
        lotus_key_processor_set_should_restore_key_strokes(handle_.get(), restore);
    }

    bool KeyProcessor::shouldRestoreKeyStrokes() const {
        return lotus_key_processor_should_restore_key_strokes(handle_.get());
    }

    void KeyProcessor::setMacroTable(std::shared_ptr<const MacroTable> table) {
        lotus_key_processor_set_macro_table(handle_.get(), table ? table->handle() : nullptr);
        macroTable_ = std::move(table);
    }

    void KeyProcessor::setDictionary(std::shared_ptr<const Dictionary> dictionary) {
        lotus_key_processor_set_dictionary(handle_.get(), dictionary ? dictionary->handle() : nullptr);
        dictionary_ = std::move(dictionary);
    }

    bool KeyProcessor::processKey(uint32_t keyVal, KeyStates states) {
        return lotus_key_processor_process_key(handle_.get(), keyVal, states.bits);
    }

    bool KeyProcessor::canProcessKey(uint32_t keyVal) const {
        return lotus_key_processor_can_process_key(handle_.get(), keyVal);
    }

    std::u32string KeyProcessor::preedit() const {
        std::u32string out;
        lotus_key_processor_preedit(handle_.get(), ffi::assignU32, &out);
        return out;
    }

    std::u32string KeyProcessor::takeCommit() {
        std::u32string out;
        lotus_key_processor_take_commit(handle_.get(), ffi::assignU32, &out);
        return out;
    }

    void KeyProcessor::commitPreedit() {
        lotus_key_processor_commit_preedit(handle_.get());
    }

    void KeyProcessor::reset() {
        lotus_key_processor_reset(handle_.get());
    }

    void KeyProcessor::rebuildFromText(std::u32string_view text) {
        lotus_key_processor_rebuild_from_text(handle_.get(), ffi::u32(text), text.size());
    }

    void KeyProcessor::setRewriteAllowed(bool allowed) {
        lotus_key_processor_set_rewrite_allowed(handle_.get(), allowed);
    }

    std::u32string KeyProcessor::takeRewrittenBefore() {
        std::u32string out;
        lotus_key_processor_take_rewritten_before(handle_.get(), ffi::assignU32, &out);
        return out;
    }

    void KeyProcessor::forgetContext() {
        lotus_key_processor_forget_context(handle_.get());
    }

    bool KeyProcessor::endWord() {
        return lotus_key_processor_end_word(handle_.get());
    }

    bool KeyProcessor::revertLastSwitch() {
        return lotus_key_processor_revert_last_switch(handle_.get());
    }

    void KeyProcessor::setLearned(std::shared_ptr<Learned> learned) {
        lotus_key_processor_set_learned(handle_.get(), learned ? learned->handle() : nullptr);
        learned_ = std::move(learned);
    }

    Learned::Learned() : handle_(lotus_learned_new(), lotus_learned_free) {}

    void Learned::loadFromText(std::string_view text) {
        lotus_learned_load_from_text(handle_.get(), ffi::str(text));
    }

    std::string Learned::toText() const {
        std::string out;
        lotus_learned_to_text(handle_.get(), ffi::assignBytes, &out);
        return out;
    }

    std::size_t Learned::size() const {
        return lotus_learned_size(handle_.get());
    }

    std::string KeyProcessor::formatTime(std::string_view format) {
        std::string out;
        lotus_format_time(ffi::str(format), ffi::assignBytes, &out);
        return out;
    }

} // namespace lotus
