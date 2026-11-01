// SPDX-FileCopyrightText: 2026 Duc Vu <ducvd89@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Types whole words in Surrounding Text mode against a simulated client text: every commit and
// deleteSurroundingText is applied to the text, and a key the engine lets through is appended as
// the client would. Guards the W2U undo: "ww" must leave "w", not "ưw".
#include "lotus-engine.h"
#include "test-input-context.h"

#include <fcitx-utils/event.h>
#include <fcitx-utils/utf8.h>
#include <systemd/sd-event.h>

#include <array>
#include <chrono>
#include <thread>
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

            // '\n' is Enter, '\b' the revert key (Shift+BackSpace).
            const fcitx::Key key = ch == '\n' ? fcitx::Key(FcitxKey_Return) :
                ch == '\b'                    ? fcitx::Key(FcitxKey_BackSpace, fcitx::KeyState::Shift) :
                                                 fcitx::Key(static_cast<fcitx::KeySym>(ch));
            fcitx::KeyEvent event(context, key, false);
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
            if (!event.accepted() && ch != '\b') {
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
    configureTestPaths("fcitx5-vanh-surrounding-text-typing");
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
    fcitx::InputMethodEntry  entry("vanh", "Vanh", "vi", "vanh");
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
    // Context (Vikey look-back, word pairs, English mode): the words around decide.
    constexpr std::array<Case, 26> contextCases{
        // A word glued to code: the keys of the word are still known after the glue character.
        Case{.keys = "file ~/.config/fcitx5/profile ", .want = "file ~/.config/fcitx5/profile "},
        Case{.keys = "max_tokens = 4 ", .want = "max_tokens = 4 "},
        Case{.keys = "video call this ", .want = "video call this "},
        Case{.keys = "this makes sense ", .want = "this makes sense "},
        Case{.keys = "this error ", .want = "this error "},
        Case{.keys = "error this ", .want = "error this "},
        Case{.keys = "the bus ", .want = "the bus "},
        Case{.keys = "cho con bus ", .want = "cho con bú "},
        Case{.keys = "tooi ddi hocj ", .want = "tôi đi học "},
        Case{.keys = "minhf sex focus vaof cais issue ddos ", .want = "mình sẽ focus vào cái issue đó "},
        Case{.keys = "fix error nayf ddi ", .want = "fix error này đi "},
        Case{.keys = "user cos ", .want = "user có "},
        Case{.keys = "test xong roofi ", .want = "test xong rồi "},
        Case{.keys = "it has been ", .want = "it has been "},
        Case{.keys = "office its nhaats ", .want = "office ít nhất "},
        Case{.keys = "the style of rust ", .want = "the style of rust "},
        Case{.keys = "this is a test ", .want = "this is a test "},
        Case{.keys = "car release ", .want = "cả release "},
        Case{.keys = "xoas car release ", .want = "xoá cả release "},
        Case{.keys = "sex focus ", .want = "sẽ focus "},
        Case{.keys = "hocj it been fpt ", .want = "học it bên fpt "},
        // Enter ends the word like a space: the last English word on a line is restored too.
        Case{.keys = "the poller is\n", .want = "the poller is\n"},
        Case{.keys = "tooi ddi hocj\n", .want = "tôi đi học\n"},
        Case{.keys = "docs: fix\n", .want = "docs: fix\n"},
        // The revert key right after a word break switches the word to its other reading.
        Case{.keys = "dduwowcj host \brieeng", .want = "được hót riêng"},
        Case{.keys = "theo max \bticket", .want = "theo max ticket"},
    };
    for (const Case& test : contextCases) {
        const std::string got = typer.type(test.keys);
        if (got != test.want) {
            std::cerr << "Context typing '" << test.keys << "': expected '" << test.want << "', got '" << got << "'\n";
            ++failures;
        }
    }
    // Without the auto restore (AutoNonVnRestore and SpellCheck off), only the English restore
    // brings a glued word back, and it needs the keys typed: a word tracked across the glue
    // character made the next key rebuild the engine from the text ("pròile", "tokén").
    config.setValueByPath("AutoNonVnRestore", "False");
    config.setValueByPath("SpellCheck", "False");
    engine.setConfig(config);
    constexpr std::array<Case, 3> gluedCases{
        Case{.keys = "file ~/.config/fcitx5/profile ", .want = "file ~/.config/fcitx5/profile "},
        Case{.keys = "max_tokens = 4 ", .want = "max_tokens = 4 "},
        Case{.keys = "src/main.rs ", .want = "src/main.rs "},
    };
    for (const Case& test : gluedCases) {
        const std::string got = typer.type(test.keys);
        if (got != test.want) {
            std::cerr << "Glued typing without auto restore '" << test.keys << "': expected '" << test.want << "', got '" << got << "'\n";
            ++failures;
        }
    }

    // GNOME Shell relays native Wayland apps and merges edits that reach it less than a frame
    // apart ("được" -> "đươcợc"). Keys typed at once, Enter included: no edit may follow another
    // closer than 30 ms, and the app still ends up with the right text and Enter.
    auto shell = std::make_unique<TestInputContext>(&testInstance.instance, "gnome-shell");
    shell->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
    shell->focusIn();
    fcitx::InputContextEvent shellFocus(shell.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, shellFocus);
    shell->surroundingText().setText("", 0, 0);
    shell->updateSurroundingText();
    for (const char ch : std::string_view("dduwowcj nguwowif vieetj\n")) {
        const fcitx::Key key = ch == '\n' ? fcitx::Key(FcitxKey_Return) : fcitx::Key(static_cast<fcitx::KeySym>(ch));
        fcitx::KeyEvent  event(shell.get(), key, false);
        engine.keyEvent(entry, event);
        if (!event.accepted()) {
            shell->recordPassed(key.toString());
        }
    }
    auto* loop = static_cast<sd_event*>(testInstance.instance.eventLoop().nativeHandle());
    for (int i = 0; i < 200; ++i) {
        while (sd_event_run(loop, 0) > 0) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // An edit is a deletion and the commit after it, or a commit alone.
    std::string                           shown;
    std::chrono::steady_clock::time_point last{};
    char                                  previous = 0;
    for (const auto& edit : shell->edits()) {
        const bool starts = edit.kind == 'd' || (edit.kind == 'c' && previous != 'd');
        if (starts && last != std::chrono::steady_clock::time_point{} && edit.at - last < std::chrono::milliseconds(29)) {
            std::cerr << "GNOME Shell: an edit " << std::chrono::duration_cast<std::chrono::milliseconds>(edit.at - last).count() << " ms after the previous one\n";
            ++failures;
        }
        if (starts) {
            last = edit.at;
        }
        previous = edit.kind;
        if (edit.kind == 'd') {
            for (unsigned int n = 0; n < edit.size; ++n) {
                const auto start = fcitx_utf8_get_nth_char(shown.c_str(), fcitx_utf8_strlen(shown.c_str()) - 1);
                shown.erase(static_cast<size_t>(start - shown.c_str()));
            }
        } else if (edit.kind == 'c') {
            shown += edit.text;
        } else {
            shown += edit.text == "Return" ? "\n" : edit.text;
        }
    }
    if (shown != "được người việt\n") {
        std::cerr << "GNOME Shell typing: expected 'được người việt\\n', got '" << shown << "'\n";
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
