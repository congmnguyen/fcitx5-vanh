// SPDX-License-Identifier: GPL-3.0-or-later
//
// Backspace inside a Preedit composition must edit the composition, not commit it: the engine is
// addressed by keysym, so the addon must hand over FcitxKey_BackSpace, never the Unicode 0x08 the
// keysym maps to (0x08 is an unknown key for the engine, which then commits the whole preedit).
#include "lotus-engine.h"
#include "test-input-context.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

    void reportFailure(const std::string& step, const std::string& expected, const std::string& actual, const std::string& meaning) {
        std::fprintf(stderr, "Step: %s\nExpected: %s\nActual: %s\nMeaning: %s\n", step.c_str(), expected.c_str(), actual.c_str(), meaning.c_str());
    }

    bool send(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, fcitx::KeySym symbol, bool requireAccepted) {
        fcitx::KeyEvent event(&context, fcitx::Key(symbol), false);
        engine.keyEvent(entry, event);
        if (event.accepted() != requireAccepted) {
            reportFailure("process key " + std::to_string(symbol), "accepted=" + std::to_string(static_cast<int>(requireAccepted)),
                          "accepted=" + std::to_string(static_cast<int>(event.accepted())) + ", commits=" + std::to_string(context.commits().size()),
                          "Preedit mode turned Backspace into a commit instead of editing the composition");
            return false;
        }
        return true;
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-preedit-backspace");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Preedit");
    config.setValueByPath("InputMethod", "Telex");
    engine.setConfig(config);

    fcitx::InputMethodEntry entry("vanh", "Vanh", "vi", "vanh");
    auto                    context = std::make_unique<TestInputContext>(&testInstance.instance);
    context->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
    context->focusIn();
    fcitx::InputContextEvent focus(context.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, focus);

    // "lo" stays a composition.
    if (!send(engine, entry, *context, FcitxKey_l, true) || !send(engine, entry, *context, FcitxKey_o, true))
        return 1;
    if (context->inputPanel().clientPreedit().toString() != "lo") {
        reportFailure("compose", "client preedit=lo", "client preedit=" + context->inputPanel().clientPreedit().toString(), "the test cannot compose");
        return 1;
    }

    // Backspace removes the last rune, is swallowed, and commits nothing.
    if (!send(engine, entry, *context, FcitxKey_BackSpace, true))
        return 1;
    if (!context->commits().empty()) {
        std::string actual = "commits=";
        for (const auto& commit : context->commits())
            actual += "['" + commit + "']";
        reportFailure("backspace inside a composition", "no commits", actual, "backspace committed the remaining preedit instead of editing it");
        return 1;
    }
    if (context->inputPanel().clientPreedit().toString() != "l") {
        reportFailure("backspace edits the composition", "client preedit=l", "client preedit=" + context->inputPanel().clientPreedit().toString(),
                      "backspace must shorten the composition");
        return 1;
    }

    // Backspace on the last rune ends the composition without committing text.
    if (!send(engine, entry, *context, FcitxKey_BackSpace, true))
        return 1;
    if (!context->commits().empty() || !context->inputPanel().clientPreedit().toString().empty()) {
        reportFailure("backspace on the last rune", "no commits, empty preedit", "commits=" + std::to_string(context->commits().size()),
                      "backspace on the last rune must only end the composition");
        return 1;
    }

    // With nothing composed, Backspace belongs to the application.
    if (!send(engine, entry, *context, FcitxKey_BackSpace, false))
        return 1;

    std::printf("vanh preedit backspace: all checks passed\n");
    return 0;
}
