// SPDX-License-Identifier: GPL-3.0-or-later
//
// A replacement dropped by the engine must tell the uinput server to stop pacing
// its burst, and keys typed while the burst tail still holds Shift must replay
// without the phantom Shift.
#include "fcitx-utils/keysym.h"
#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

    bool press(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, fcitx::KeySym symbol, bool requireAccepted) {
        fcitx::KeyEvent event(&context, fcitx::Key(symbol), false);
        engine.keyEvent(entry, event);
        if (event.accepted() == requireAccepted)
            return true;
        reportFailure("process key " + std::to_string(symbol), "accepted=" + std::to_string(static_cast<int>(requireAccepted)),
                      "accepted=" + std::to_string(static_cast<int>(event.accepted())) + ", commits=" + std::to_string(context.commits().size()), "unexpected key outcome");
        return false;
    }

    bool pressKey(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, fcitx::Key key, bool requireAccepted) {
        fcitx::KeyEvent event(&context, key, false);
        engine.keyEvent(entry, event);
        if (event.accepted() == requireAccepted)
            return true;
        reportFailure("process key " + std::to_string(key.sym()), "accepted=" + std::to_string(static_cast<int>(requireAccepted)),
                      "accepted=" + std::to_string(static_cast<int>(event.accepted())) + ", commits=" + std::to_string(context.commits().size()), "unexpected key outcome");
        return false;
    }

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

    std::string commitList(const TestInputContext& context) {
        std::string actual = "commits=";
        for (const auto& commit : context.commits())
            actual += "['" + commit + "']";
        return actual;
    }

    bool expectMsg(const KbMsg& msg, KbOp op, size_t count, const char* step, const char* meaning) {
        if (msg.op == op && msg.count == count)
            return true;
        reportFailure(step, "op=" + std::to_string(static_cast<int>(op)) + ", count=" + std::to_string(count),
                      "op=" + std::to_string(static_cast<int>(msg.op)) + ", count=" + std::to_string(msg.count), meaning);
        return false;
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-select-cancel");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Uinput (Select)");
    config.setValueByPath("InputMethod", "Telex");
    engine.setConfig(config);

    KbSocketListener listener;
    if (!listener.valid())
        return 1;

    fcitx::InputMethodEntry entry("vanh", "Vanh", "vi", "vanh");
    auto                    context = std::make_unique<TestInputContext>(&testInstance.instance);
    gainFocus(engine, entry, *context);

    // A focus-out in the middle of a Select burst severs the echo path: the app
    // consumes the injected keys while text-input is disabled, so the replacement
    // must be dropped and the pending burst cancelled at focus-out time.
    if (!press(engine, entry, *context, FcitxKey_a, false) || !press(engine, entry, *context, FcitxKey_t, false) || !press(engine, entry, *context, FcitxKey_s, true))
        return 1;
    KbMsg msg{};
    if (!listener.receive(msg, "the replacement request did not arrive"))
        return 1;
    if (!expectMsg(msg, KbOp::Select, 2, "receive replacement request", "the replacement never started"))
        return 1;

    loseFocus(engine, entry, *context);
    if (!listener.receive(msg, "the cancel request at focus-out did not arrive"))
        return 1;
    if (!expectMsg(msg, KbOp::Cancel, 0, "receive cancel request", "focus out must cancel the pending burst"))
        return 1;
    if (!context->commits().empty()) {
        reportFailure("verify no commit after the cancel", "no commits", commitList(*context), "the cancelled replacement must commit nothing");
        return 1;
    }

    // Typing resumes right after the bounce: the first key commits the stashed
    // replacement, then a new word triggers a fresh replacement that completes
    // through its echoes.
    gainFocus(engine, entry, *context);
    if (!press(engine, entry, *context, FcitxKey_Escape, false))
        return 1;
    const std::vector<std::string> afterBounce{"át"};
    if (context->commits() != afterBounce) {
        reportFailure("verify commit after the bounce", "commits=['át']", commitList(*context), "the stashed replacement was not committed once focus returned");
        return 1;
    }
    if (!press(engine, entry, *context, FcitxKey_a, false) || !press(engine, entry, *context, FcitxKey_t, false) || !press(engine, entry, *context, FcitxKey_s, true))
        return 1;
    if (!listener.receive(msg, "the replacement request after the bounce did not arrive"))
        return 1;
    if (!expectMsg(msg, KbOp::Select, 2, "receive replacement request", "typing must resume with a fresh select request"))
        return 1;
    if (!press(engine, entry, *context, FcitxKey_Left, false) || !press(engine, entry, *context, FcitxKey_Left, false) || !press(engine, entry, *context, FcitxKey_Pause, true))
        return 1;
    const std::vector<std::string> afterFresh{"át", "át"};
    if (context->commits() != afterFresh) {
        reportFailure("verify commit of the fresh replacement", "commits=['át']['át']", commitList(*context), "the fresh replacement did not commit normally");
        return 1;
    }

    // A burst abandoned past its ack deadline (echoes lost without any focus
    // change) is still cancelled on the next key, and the pending text is
    // committed exactly once. The 'a' re-composes "át" into "ất" (the Telex
    // aa -> â rule) and triggers a fresh replacement; Escape both triggers the
    // overdue check and cleans the word state.
    const size_t commitsBeforeTimeout = context->commits().size();
    if (!press(engine, entry, *context, FcitxKey_a, true))
        return 1;
    if (!listener.receive(msg, "the second replacement request did not arrive"))
        return 1;
    if (!expectMsg(msg, KbOp::Select, 2, "receive replacement request", "the second replacement never started"))
        return 1;

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!press(engine, entry, *context, FcitxKey_Escape, false))
        return 1;
    if (!listener.receive(msg, "the cancel request after the ack timeout did not arrive"))
        return 1;
    if (!expectMsg(msg, KbOp::Cancel, 0, "receive cancel request", "the ack timeout must cancel the abandoned burst"))
        return 1;
    if (context->commits().size() != commitsBeforeTimeout + 1) {
        reportFailure("verify commit after the ack timeout", "one pending commit", commitList(*context), "the abandoned replacement text was not committed exactly once");
        return 1;
    }

    // Escape cleaned the word state, so the next syllable starts from a clean
    // buffer. While a Select burst is in flight the server holds LEFTSHIFT until
    // the burst tail, so keys typed near the tail carry a phantom Shift state
    // that must replay unshifted.
    if (!press(engine, entry, *context, FcitxKey_e, false) || !press(engine, entry, *context, FcitxKey_s, true))
        return 1;
    if (!listener.receive(msg, "the third replacement request did not arrive"))
        return 1;
    if (!expectMsg(msg, KbOp::Select, 1, "receive replacement request", "the third replacement never started"))
        return 1;

    if (!press(engine, entry, *context, FcitxKey_Left, false) || !pressKey(engine, entry, *context, fcitx::Key(FcitxKey_N, fcitx::KeyState::Shift), true) ||
        !press(engine, entry, *context, FcitxKey_Pause, true))
        return 1;
    if (context->commits().empty() || context->commits().back() != "én") {
        reportFailure("verify commit after the phantom Shift", "commits ending with ['én']", commitList(*context), "the phantom Shift was not stripped from the buffered key");
        return 1;
    }

    std::printf("vanh select cancel: all checks passed\n");
    return 0;
}
