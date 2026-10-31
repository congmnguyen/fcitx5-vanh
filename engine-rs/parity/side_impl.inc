// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One side of the parity harness. Included by old_side.cpp (where the former engine's `lotus`
// namespace is renamed to lotus_old by -Dlotus=lotus_old) and new_side.cpp (the Rust engine behind
// its C++ wrappers). PARITY_OLD selects the few spots where the two C++ interfaces differ, and
// PARITY_SIDE_FN names the accessor.

#include "driver.h"

#include "dictionary.h"
#include "encoder.h"
#include "input_method.h"
#include "key_processor.h"
#include "macro_table.h"

#include <unordered_map>

namespace {

    using namespace parity;

    lotus::Options toOptions(const Opts& o) {
        lotus::Options options;
        options.autoNonVnRestore    = o.autoNonVnRestore;
        options.ddFreeStyle         = o.ddFreeStyle;
        options.englishRestore      = o.englishRestore;
        options.macroEnabled        = o.macroEnabled;
        options.autoCapitalizeMacro = o.autoCapitalizeMacro;
        options.spellCheckWithDicts = o.spellCheckWithDicts;
        options.modernStyle         = o.modernStyle;
        options.freeMarking         = o.freeMarking;
        options.outputCharset       = static_cast<lotus::Charset>(o.charset);
        if (o.w2u >= 0) {
            options.w2u = static_cast<lotus::W2uMode>(o.w2u);
        }
        if (o.bracket >= 0) {
            options.bracketTransform = static_cast<lotus::BracketTransformMode>(o.bracket);
        }
        options.timeFormat = o.timeFormat;
        options.dateFormat = o.dateFormat;
        return options;
    }

    std::shared_ptr<const lotus::MacroTable> buildMacros(const Setup& setup) {
        auto table = std::make_shared<lotus::MacroTable>();
        for (const auto& [key, value] : setup.macros) {
            table->set(key, value);
        }
        return table;
    }

    std::shared_ptr<const lotus::Dictionary> buildDictionary(const Setup& setup) {
        auto dictionary = std::make_shared<lotus::Dictionary>();
        if (setup.dictionary == 1) {
            dictionary->loadFromWords(setup.dictWords);
        } else if (setup.dictionary == 2) {
            dictionary->loadFromText(setup.dictText);
        }
        return dictionary;
    }

    lotus::KeyProcessor makeProcessor(const Setup& setup) {
        if (setup.parseCustom) {
            std::unordered_map<std::string, std::string> definition(setup.customDefinition.begin(), setup.customDefinition.end());
            return lotus::KeyProcessor(lotus::parseInputMethod(definition), setup.spellDefault);
        }
        return lotus::KeyProcessor(lotus::cachedInputMethod(static_cast<lotus::InputMethodId>(setup.inputMethod)), setup.spellDefault);
    }

    class SideProc final : public Proc {
      public:
        SideProc(const Setup& setup, const Opts& opts) : setup_(setup), processor_(makeProcessor(setup)) {
            macros_     = buildMacros(setup);
            dictionary_ = buildDictionary(setup);
            reshare(setup.withMacros, setup.dictionary != 0);
            if (setup.rewriteAllowed) {
                processor_.setRewriteAllowed(true);
            }
            if (setup.setOptions) {
                processor_.setOptions(toOptions(opts));
            }
        }

        void setOptions(const Opts& opts) override {
            processor_.setOptions(toOptions(opts));
        }
        void setMacroEnabled(bool enabled) override {
            processor_.setMacroEnabled(enabled);
        }
        void setShouldRestoreKeyStrokes(bool on) override {
            processor_.setShouldRestoreKeyStrokes(on);
        }
        bool shouldRestoreKeyStrokes() override {
            return processor_.shouldRestoreKeyStrokes();
        }
        bool processKey(uint32_t key, uint32_t st) override {
            return processor_.processKey(key, lotus::keyStatesFromBits(st));
        }
        bool canProcessKey(uint32_t key) override {
            return processor_.canProcessKey(key);
        }
        std::u32string preedit() override {
            return std::u32string(processor_.preedit());
        }
        std::u32string takeCommit() override {
            return processor_.takeCommit();
        }
        void commitPreedit() override {
            processor_.commitPreedit();
        }
        void reset() override {
            processor_.reset();
        }
        void rebuildFromText(const std::u32string& text) override {
            processor_.rebuildFromText(text);
        }
        void setRewriteAllowed(bool allowed) override {
            processor_.setRewriteAllowed(allowed);
        }
        std::u32string takeRewrittenBefore() override {
            return processor_.takeRewrittenBefore();
        }
        void forgetContext() override {
            processor_.forgetContext();
        }
        void reshare(bool macros, bool dictionary) override {
            processor_.setMacroTable(macros ? macros_ : nullptr);
            processor_.setDictionary(dictionary ? dictionary_ : nullptr);
        }

      private:
        Setup                                    setup_;
        lotus::KeyProcessor                      processor_;
        std::shared_ptr<const lotus::MacroTable> macros_;
        std::shared_ptr<const lotus::Dictionary> dictionary_;
    };

    std::unique_ptr<Proc> make(const Setup& setup, const Opts& opts) {
        return std::make_unique<SideProc>(setup, opts);
    }

    std::string encode(int charset, const std::u32string& text) {
        return lotus::encode(static_cast<lotus::Charset>(charset), text);
    }

    std::string formatTime(const std::string& format) {
        return lotus::KeyProcessor::formatTime(format);
    }

    bool isValidState(uint32_t states) {
        return lotus::KeyProcessor::isValidState(lotus::keyStatesFromBits(states));
    }

    std::vector<std::size_t> dictionaryProbe(const Setup& setup, const std::vector<std::u32string>& words) {
        const auto               dictionary = buildDictionary(setup);
        std::vector<std::size_t> out;
        for (const auto& word : words) {
            out.push_back((dictionary->hasWord(word) ? 1U : 0U) | (dictionary->hasWordOrPrefix(word) ? 2U : 0U));
        }
#ifdef PARITY_OLD
        out.push_back(dictionary->words().size());
#else
        out.push_back(dictionary->size());
#endif
        return out;
    }

    std::vector<std::optional<std::u32string>> macroProbe(const Setup& setup, const std::vector<std::u32string>& keys) {
        const auto                                  table = buildMacros(setup);
        std::vector<std::optional<std::u32string>> out;
        for (const auto& key : keys) {
            const auto value = table->get(key);
            out.push_back(value ? std::optional<std::u32string>(std::u32string(*value)) : std::nullopt);
        }
        out.push_back(table->empty() ? std::optional<std::u32string>() : std::optional<std::u32string>(U"nonempty"));
        return out;
    }

} // namespace

const parity::Side& parity::PARITY_SIDE_FN() {
    static const Side kSide{make, encode, formatTime, isValidState, dictionaryProbe, macroProbe};
    return kSide;
}
