// SPDX-License-Identifier: GPL-3.0-or-later
//
// Uinput modes must keep the composed word across a focus bounce: Brave's ibus input context on
// Wayland (iCloud Notes) reports focus-out/focus-in on every key, and the focus-out reset used to
// destroy the word before activate() could recognise the bounce.
#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

    bool send(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, fcitx::KeySym symbol, bool requireAccepted) {
        fcitx::KeyEvent event(&context, fcitx::Key(symbol), false);
        engine.keyEvent(entry, event);
        if (event.accepted() != requireAccepted) {
            reportFailure("process key " + std::to_string(symbol), "accepted=" + std::to_string(static_cast<int>(requireAccepted)),
                          "accepted=" + std::to_string(static_cast<int>(event.accepted())) + ", commits=" + std::to_string(context.commits().size()),
                          "uinput focus-bounce handling accepted or rejected the key unexpectedly");
            return false;
        }
        return true;
    }

    // The order a frontend reports a focus loss: deactivate(), then reset(FocusOut).
    void loseFocus(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context) {
        fcitx::InputContextEvent out(&context, fcitx::EventType::InputContextFocusOut);
        engine.deactivate(entry, out);
        engine.reset(entry, out);
        context.focusOut();
    }

    void gainFocus(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context) {
        context.focusIn();
        fcitx::InputContextEvent in(&context, fcitx::EventType::InputContextFocusIn);
        engine.activate(entry, in);
    }

    // Back-to-back focus bounces with no sleep: Brave's ibus IC re-reports focus-out/focus-in
    // on (nearly) every key, sometimes several times per key.
    void bounceFocus(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, int times) {
        for (int i = 0; i < times; ++i) {
            loseFocus(engine, entry, context);
            gainFocus(engine, entry, context);
        }
    }

    // Reads the replacement request and acknowledges it with the n+1 backspaces the addon expects.
    bool completeReplacement(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, KbSocketListener& listener, const char* meaning) {
        KbMsg msg{};
        if (!listener.receive(msg, meaning))
            return false;
        if (msg.op != KbOp::Backspace || msg.count == 0) {
            reportFailure("receive replacement request", "op=backspace, count > 0", "op=" + std::to_string(static_cast<int>(msg.op)) + ", count=" + std::to_string(msg.count),
                          meaning);
            return false;
        }
        for (std::size_t i = 0; i < msg.count; ++i) {
            if (!send(engine, entry, context, FcitxKey_BackSpace, false))
                return false;
        }
        return send(engine, entry, context, FcitxKey_Pause, true);
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-focus-bounce-word-survival");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Uinput (Smooth)");
    config.setValueByPath("InputMethod", "Telex");
    engine.setConfig(config);
    if (engine.config().mode.value() != fcitx::LotusMode::Smooth) {
        reportFailure("configure Smooth", "mode=Smooth", "configured mode differs", "the test needs uinput mode");
        return 1;
    }

    KbSocketListener listener;
    if (!listener.valid())
        return 1;

    fcitx::InputMethodEntry entry("vanh", "Vanh", "vi", "vanh");
    auto                    context = std::make_unique<TestInputContext>(&testInstance.instance);
    // #610 fidelity: Brave on Wayland drives Lotus through an ibus context.
    // Note: engine branches on frontend == "dbus" (lotus-engine.cpp:394,735) and on
    // surroundingText().isValid() (:393-394,431,743). Uinput Smooth mode does not use
    // surrounding text, so leaving capabilities unset (surrvalid=false) matches the
    // Brave uinput path; "ibus" vs "test" takes the same branch here by design.
    context->setFrontendName("ibus");
    gainFocus(engine, entry, *context);

    // Per-key bounce: Brave's ibus IC reports focus-out/focus-in on nearly every key, so bounce
    // after EACH of 't' and 'u' (multiple back-to-back rounds, no sleep) before typing 'w'.
    if (!send(engine, entry, *context, FcitxKey_t, false))
        return 1;
    bounceFocus(engine, entry, *context, 2);
    if (!send(engine, entry, *context, FcitxKey_u, false))
        return 1;
    bounceFocus(engine, entry, *context, 3);

    if (!send(engine, entry, *context, FcitxKey_w, true))
        return 1;
    if (!completeReplacement(engine, entry, *context, listener, "a focus bounce must not drop the composed word: typing after it still needs the replacement request"))
        return 1;

    const std::vector<std::string> expected{"ư"};
    if (context->commits() != expected) {
        std::string actual = "commits=";
        for (const auto& commit : context->commits())
            actual += "['" + commit + "']";
        reportFailure("verify commit after a bounce", "commits=['ư']", actual, "the bounced word was replaced from scratch instead of keeping 'tu'");
        return 1;
    }

    // Real focus move A -> B: lose A, wait past the 100ms bounce window
    // (lotus-engine.cpp:412), then gain B. No gainFocus(A) here: A's deferred reset
    // stays pending until A is re-entered, which is the true handoff.
    loseFocus(engine, entry, *context);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // Two-context isolation: a second ibus context composes 'a' + 's' independently; nothing may
    // leak across contexts and contextA must still hold only the bounced word.
    auto contextB = std::make_unique<TestInputContext>(&testInstance.instance);
    contextB->setFrontendName("ibus");
    gainFocus(engine, entry, *contextB);

    if (!send(engine, entry, *contextB, FcitxKey_a, false) || !send(engine, entry, *contextB, FcitxKey_s, true))
        return 1;
    if (!completeReplacement(engine, entry, *contextB, listener, "a second context must replace its own word independently"))
        return 1;

    if (context->commits() != expected) {
        std::string actual = "commits=";
        for (const auto& commit : context->commits())
            actual += "['" + commit + "']";
        reportFailure("cross-context isolation", "contextA commits=['ư']", actual, "typing on contextB leaked into contextA");
        return 1;
    }
    const std::vector<std::string> expectedB{"á"};
    if (contextB->commits() != expectedB) {
        std::string actual = "commits=";
        for (const auto& commit : contextB->commits())
            actual += "['" + commit + "']";
        reportFailure("independent word on contextB", "contextB commits=['á']", actual, "contextB must compose its own word without contextA state");
        return 1;
    }
    // Return B -> A: A's deferred reset flushes on re-entry (elapsed >100ms), so the next
    // word on A starts from scratch.
    loseFocus(engine, entry, *contextB);
    gainFocus(engine, entry, *context);
    // Same-context fresh word after the real move: 'a' + 's' must replace only that fresh word.
    if (!send(engine, entry, *context, FcitxKey_a, false) || !send(engine, entry, *context, FcitxKey_s, true))
        return 1;
    if (!completeReplacement(engine, entry, *context, listener, "the word after a real focus move must be replaced from scratch"))
        return 1;

    const std::vector<std::string> expectedAfterMove{"ư", "á"};
    if (context->commits() != expectedAfterMove) {
        std::string actual = "commits=";
        for (const auto& commit : context->commits())
            actual += "['" + commit + "']";
        reportFailure("next word after a real focus move", "commits=['ư']['á']", actual, "a real focus move must still drop the word buffer");
        return 1;
    }

    std::printf("vanh focus bounce word survival: all checks passed\n");
    return 0;
}
