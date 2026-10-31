// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file replacement-delays.cpp
 * @brief Headless regression test: the global default delays reach the uinput
 *        server through KbMsg (interval = DefaultBackspaceInterval,
 *        postDelay = DefaultPostDelay) and DefaultCommitInterval delays a
 *        replacement that follows a recent commit.
 */

#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"

#include <cstddef>
#include <memory>
#include <string>

namespace {

    bool receiveBackspaceRequest(KbSocketListener& listener, KbMsg& msg, const char* meaning, const char* timeoutExpected = "request within 5000 ms") {
        if (!listener.receive(msg, meaning, timeoutExpected))
            return false;
        if (msg.op != KbOp::Backspace || msg.count <= 0) {
            reportFailure("receive replacement request", "op=backspace, count > 0", "op=" + std::to_string(static_cast<int>(msg.op)) + ", count=" + std::to_string(msg.count),
                          meaning);
            return false;
        }
        return true;
    }

    bool send(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, fcitx::KeySym symbol, bool requireAccepted) {
        fcitx::KeyEvent event(&context, fcitx::Key(symbol), false);
        engine.keyEvent(entry, event);
        if (event.accepted() != requireAccepted) {
            reportFailure("process key " + std::to_string(symbol), "accepted=" + std::to_string(static_cast<int>(requireAccepted)),
                          "accepted=" + std::to_string(static_cast<int>(event.accepted())) + ", commits=" + std::to_string(context.commits().size()),
                          "Smooth replacement handling accepted or rejected the key unexpectedly");
            return false;
        }
        return true;
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-replacement-delays");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Uinput (Smooth)");
    config.setValueByPath("InputMethod", "Telex");
    config.setValueByPath("DefaultBackspaceInterval", "17");
    config.setValueByPath("DefaultPostDelay", "33");
    config.setValueByPath("DefaultCommitInterval", "1000");
    engine.setConfig(config);
    if (engine.config().mode.value() != fcitx::LotusMode::Smooth || *engine.config().defaultBackspaceInterval != 17 || *engine.config().defaultPostDelay != 33 ||
        *engine.config().defaultCommitInterval != 1000) {
        reportFailure("configure Smooth/Telex and default delays", "mode=Smooth, intervals 17/33/1000", "configured values differ",
                      "the delay wiring cannot be exercised with the wrong configuration");
        return 1;
    }

    KbSocketListener listener;
    if (!listener.valid())
        return 1;
    auto context = std::make_unique<TestInputContext>(&testInstance.instance);
    context->focusIn();
    fcitx::InputMethodEntry  entry("vanh", "Vanh", "vi", "vanh");
    fcitx::InputContextEvent focus(context.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, focus);
    context->resetPreeditUpdateCount();

    // Telex a, s changes the real Bamboo preedit a -> á; Smooth mode replaces
    // the old character through the kbSocket transport.
    if (!send(engine, entry, *context, FcitxKey_a, false) || !send(engine, entry, *context, FcitxKey_s, true))
        return 1;

    KbMsg first{};
    if (!receiveBackspaceRequest(listener, first, "the initial Telex replacement request did not arrive"))
        return 1;
    if (first.interval != 17 || first.postDelay != 33 || first.preDelay != 0) {
        reportFailure("default delays on the first request", "interval=17, postDelay=33, preDelay=0",
                      "interval=" + std::to_string(first.interval) + ", postDelay=" + std::to_string(first.postDelay) + ", preDelay=" + std::to_string(first.preDelay),
                      "DefaultBackspaceInterval/DefaultPostDelay must be forwarded and no commit happened before");
        return 1;
    }

    if (!send(engine, entry, *context, FcitxKey_x, true) || !context->commits().empty()) {
        reportFailure("buffer key x before deletion completes", "no immediate commits", "commits=" + std::to_string(context->commits().size()),
                      "buffered key x was emitted before the first replacement completed");
        return 1;
    }
    for (size_t i = 0; i < first.count; ++i) {
        if (!send(engine, entry, *context, FcitxKey_BackSpace, false))
            return 1;
    }
    if (!send(engine, entry, *context, FcitxKey_Pause, true))
        return 1;

    KbMsg second{};
    if (!receiveBackspaceRequest(listener, second, "buffered key was not replayed after deletion", "buffered x replay starts another replacement request within 5000 ms"))
        return 1;
    if (second.preDelay < 900 || second.preDelay > 1000) {
        reportFailure("commit cooldown on the repeated request", "preDelay in [900, 1000]", "preDelay=" + std::to_string(second.preDelay),
                      "DefaultCommitInterval=1000 must delay a replacement that follows the previous commit");
        return 1;
    }
    for (size_t i = 0; i < second.count; ++i) {
        if (!send(engine, entry, *context, FcitxKey_BackSpace, false))
            return 1;
    }
    if (!send(engine, entry, *context, FcitxKey_Pause, true))
        return 1;
    return 0;
}
