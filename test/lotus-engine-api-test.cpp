// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smoke test of the C++ interface of the Rust engine (engine-rs/cxx over the C ABI). The engine
// logic itself is tested in Rust (`cargo test`, ctest entry lotus_engine_rust_tests); this only
// checks that every wrapper reaches it and carries the texts across intact.
#include "dictionary.h"
#include "encoder.h"
#include "input_method.h"
#include "key_processor.h"
#include "macro_table.h"
#include "utf8.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

    int failures = 0;

    void expect(bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL %s\n", what);
            ++failures;
        }
    }

    void expectText(const std::u32string& actual, std::string_view expected, const char* what) {
        const std::string text = lotus::utf8::encode(actual);
        if (text != expected) {
            std::printf("FAIL %s: expected [%.*s] actual [%s]\n", what, static_cast<int>(expected.size()), expected.data(), text.c_str());
            ++failures;
        }
    }

    void type(lotus::KeyProcessor& processor, std::string_view keys) {
        for (const char c : keys) {
            const auto key = static_cast<uint32_t>(static_cast<unsigned char>(c));
            processor.processKey(key, c >= 'A' && c <= 'Z' ? lotus::KeyStates(lotus::KeyState::Shift) : lotus::KeyStates{});
        }
    }

} // namespace

int run_engine_api() {
    // Key processor: preedit, commit, Backspace.
    {
        lotus::KeyProcessor processor(lotus::cachedInputMethod(lotus::InputMethodId::Telex), true);
        lotus::Options      options;
        options.spellCheckWithDicts = false; // no dictionary shared here
        processor.setOptions(options);
        type(processor, "Vieetj");
        expectText(processor.preedit(), "Việt", "telex preedit");
        expect(processor.processKey(lotus::kFcitxBackSpace, {}), "backspace accepted");
        expect(processor.preedit().size() == 3, "backspace removes a letter");
        processor.reset();
        type(processor, "Vieetj ");
        expectText(processor.takeCommit(), "Việt ", "commit on space");
        expectText(processor.takeCommit(), "", "commit taken once");
        expect(processor.canProcessKey('a'), "canProcessKey letter");
        expect(!lotus::KeyProcessor::isValidState(lotus::KeyState::Ctrl), "ctrl is not a valid state");
        processor.rebuildFromText(U"ngươi");
        type(processor, "f");
        expectText(processor.preedit(), "người", "rebuild then tone");
        processor.reset();
        expectText(processor.preedit(), "", "reset clears the preedit");
    }

    // English restore rewriting the word before ("thí makes" -> "this makes").
    {
        lotus::KeyProcessor processor(lotus::cachedInputMethod(lotus::InputMethodId::Telex), true);
        lotus::Options      options;
        options.englishRestore = true;
        processor.setOptions(options);
        processor.setRewriteAllowed(true);
        type(processor, "this ");
        const std::u32string first = processor.takeCommit();
        type(processor, "makes ");
        const std::u32string before = processor.takeRewrittenBefore();
        const std::u32string commit = processor.takeCommit();
        if (!before.empty()) {
            expectText(before, lotus::utf8::encode(first), "rewritten text is what was shown");
            expectText(commit, "this makes ", "rewritten commit");
        }
        processor.forgetContext();
        expectText(processor.takeRewrittenBefore(), "", "rewritten text taken once");
    }

    // Macro table shared with a processor, changed after sharing.
    {
        auto table = std::make_shared<lotus::MacroTable>();
        expect(table->empty(), "new macro table is empty");
        table->set("vn", "Việt Nam");
        expect(!table->empty(), "macro table not empty");
        expect(table->get(U"VN").value_or(U"") == U"Việt Nam", "macro lookup is case-insensitive");
        expect(!table->get(U"xyz").has_value(), "missing macro");
        lotus::KeyProcessor processor(lotus::cachedInputMethod(lotus::InputMethodId::Telex), true);
        lotus::Options      options;
        options.macroEnabled = true;
        processor.setOptions(options);
        processor.setMacroTable(table);
        table->set("hn", "Hà Nội");
        type(processor, "hn ");
        expectText(processor.takeCommit(), "Hà Nội ", "macro added after sharing");
    }

    // Dictionary.
    {
        lotus::Dictionary dictionary;
        dictionary.loadFromText("Xin\r\nchào\n\nthế giới\n");
        expect(dictionary.size() == 3, "dictionary size");
        expect(dictionary.hasWord(U"xin"), "dictionary word lowercased");
        expect(dictionary.hasWordOrPrefix(U"thế"), "dictionary prefix");
        expect(!dictionary.hasWord(U"thế"), "prefix is not a word");
        dictionary.loadFromWords({"một", "hai"});
        expect(dictionary.size() == 2 && dictionary.hasWord(U"một") && !dictionary.hasWord(U"xin"), "loadFromWords replaces the words");
    }

    // Custom input method, charsets, time format.
    {
        const std::unordered_map<std::string, std::string> definition = {{"z", "DauSac"}, {"q", "A_Â"}};
        lotus::KeyProcessor                                 processor(lotus::parseInputMethod(definition), false);
        processor.setOptions(lotus::Options{});
        type(processor, "aqz");
        expectText(processor.preedit(), "ấ", "custom input method");
        expect(lotus::encode(lotus::Charset::VniWindows, U"ệ") == "e\xC3\xA4", "VNI Windows encoding");
        expect(lotus::encode(lotus::Charset::NcrDecimal, U"ă") == "&#259;", "NCR decimal encoding");
        expect(lotus::KeyProcessor::formatTime("").empty(), "empty time format");
        expect(lotus::KeyProcessor::formatTime("%Y").size() == 4, "year format");
    }

    if (failures == 0) {
        std::printf("engine api: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
