// SPDX-FileCopyrightText: 2026 Duc Vu <ducvd89@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Types whole words in Surrounding Text mode against a simulated client text: every commit and
// deleteSurroundingText is applied to the text, and a key the engine lets through is appended as
// the client would. Guards the W2U undo: "ww" must leave "w", not "ưw".
#include "lotus-engine.h"
#include "test-input-context.h"

#include <fcitx-utils/utf8.h>

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

    struct Typer {
        fcitx::LotusEngine*      engine;
        fcitx::InputMethodEntry* entry;
        TestInputContext*        context;
        std::string              text;
        bool                     stale = false; ///< App never refreshes its surrounding text (fast typing)

        void                     typeKey(char ch) {
            if (!stale || text.empty()) {
                const size_t cursor = fcitx_utf8_strlen(text.c_str());
                context->surroundingText().setText(text, cursor, cursor);
                context->updateSurroundingText();
            }
            const size_t    commitsBefore = context->commits().size();
            const size_t    deletesBefore = context->deletes().size();

            fcitx::KeyEvent event(context, fcitx::Key(static_cast<fcitx::KeySym>(ch)), false);
            engine->keyEvent(*entry, event);

            for (size_t i = deletesBefore; i < context->deletes().size(); ++i) {
                const auto [offset, size] = context->deletes()[i];
                const size_t length       = fcitx_utf8_strlen(text.c_str());
                const size_t start        = length + offset;
                const char*  begin        = fcitx_utf8_get_nth_char(text.c_str(), start);
                const char*  end          = fcitx_utf8_get_nth_char(begin, size);
                text.erase(static_cast<size_t>(begin - text.c_str()), static_cast<size_t>(end - begin));
            }
            for (size_t i = commitsBefore; i < context->commits().size(); ++i) {
                text += context->commits()[i];
            }
            if (!event.accepted()) {
                text += ch;
            }
        }

        std::string type(std::string_view keys) {
            text.clear();
            fcitx::InputContextEvent resetEvent(context, fcitx::EventType::InputContextReset);
            engine->reset(*entry, resetEvent);
            for (const char ch : keys) {
                typeKey(ch);
            }
            return text;
        }
    };

} // namespace

int main() {
    configureTestPaths("fcitx5-lotus-surrounding-text-typing");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);

    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Surrounding Text");
    config.setValueByPath("InputMethod", "Telex");
    config.setValueByPath("W2U", "Everywhere");
    config.setValueByPath("EnableMacro", "False");
    engine.setConfig(config);

    auto context = std::make_unique<TestInputContext>(&testInstance.instance);
    context->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
    context->focusIn();
    fcitx::InputMethodEntry  entry("lotus", "Lotus", "vi", "lotus");
    fcitx::InputContextEvent focus(context.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, focus);

    struct Case {
        std::string_view keys;
        std::string_view want;
    };
    constexpr std::array<Case, 10> cases{
        Case{.keys = "w", .want = "ư"},           Case{.keys = "ww", .want = "w"},          Case{.keys = "tw", .want = "tư"},       Case{.keys = "tww", .want = "tw"},
        Case{.keys = "tuwf", .want = "từ"},       Case{.keys = "duwowng", .want = "dương"}, Case{.keys = "cuwngs", .want = "cứng"}, Case{.keys = "vieetj", .want = "việt"},
        Case{.keys = "tieengs", .want = "tiếng"}, Case{.keys = "abc.", .want = "abc."},
    };

    Typer typer{.engine = &engine, .entry = &entry, .context = context.get(), .text = {}};
    int   failures = 0;
    for (const Case& test : cases) {
        const std::string got = typer.type(test.keys);
        if (got != test.want) {
            std::cerr << "Surrounding Text typing '" << test.keys << "': expected '" << test.want << "', got '" << got << "'\n";
            ++failures;
        }
    }
    // Fast typing: the app's surrounding text lags behind; the engine must rely on what it wrote.
    typer.stale = true;
    constexpr std::array<Case, 4> staleCases{
        Case{.keys = "neen", .want = "nên"},
        Case{.keys = "tieengs vieetj", .want = "tiếng việt"},
        Case{.keys = "phoos hoir", .want = "phố hỏi"},
        Case{.keys = "hangf aos", .want = "hàng áo"},
    };
    for (const Case& test : staleCases) {
        const std::string got = typer.type(test.keys);
        if (got != test.want) {
            std::cerr << "Stale surrounding typing '" << test.keys << "': expected '" << test.want << "', got '" << got << "'\n";
            ++failures;
        }
    }
    // English restore (Vikey word lists): an English word goes back to the keys at the word break,
    // a common Vietnamese word stays.
    typer.stale = false;
    constexpr std::array<Case, 10> englishCases{
        Case{.keys = "error ", .want = "error "}, Case{.keys = "issue ", .want = "issue "},
        Case{.keys = "more ", .want = "more "}, Case{.keys = "user ", .want = "user "},   Case{.keys = "test ", .want = "test "},
        Case{.keys = "cos ", .want = "có "},     Case{.keys = "ddi ", .want = "đi "},       Case{.keys = "tieengs ", .want = "tiếng "},
        Case{.keys = "khoong ", .want = "không "}, Case{.keys = "vieetj ", .want = "việt "},
    };
    for (const Case& test : englishCases) {
        const std::string got = typer.type(test.keys);
        if (got != test.want) {
            std::cerr << "English restore typing '" << test.keys << "': expected '" << test.want << "', got '" << got << "'\n";
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
