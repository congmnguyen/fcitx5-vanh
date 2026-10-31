// SPDX-License-Identifier: GPL-3.0-or-later
//
// The addon sends the number of characters to delete; the uinput server emits exactly that many
// BackSpace keys and then an anchor KEY_PAUSE. The anchor is a real key, so the kernel orders it after
// the whole burst, but no app, terminal or WM binds it: when it lands in an app (the app is out of
// the addon's key path) it does nothing. A standard app never sees it because the addon swallows it.
#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"

#include <memory>
#include <string>
#include <vector>

namespace {

    bool send(fcitx::LotusEngine& engine, const fcitx::InputMethodEntry& entry, TestInputContext& context, fcitx::KeySym symbol, bool requireAccepted) {
        fcitx::KeyEvent event(&context, fcitx::Key(symbol), false);
        engine.keyEvent(entry, event);
        if (event.accepted() != requireAccepted) {
            reportFailure("process key " + std::to_string(symbol), "accepted=" + std::to_string(static_cast<int>(requireAccepted)),
                          "accepted=" + std::to_string(static_cast<int>(event.accepted())) + ", commits=" + std::to_string(context.commits().size()),
                          "the anchor protocol test saw an unexpected key outcome");
            return false;
        }
        return true;
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-anchor-key-protocol");
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    fcitx::RawConfig   config;
    config.setValueByPath("Mode", "Uinput (Smooth)");
    config.setValueByPath("InputMethod", "Telex");
    engine.setConfig(config);

    KbSocketListener listener;
    if (!listener.valid())
        return 1;

    fcitx::InputMethodEntry entry("vanh", "Vanh", "vi", "vanh");
    auto                    context = std::make_unique<TestInputContext>(&testInstance.instance);
    context->focusIn();
    fcitx::InputContextEvent focus(context.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, focus);

    if (!send(engine, entry, *context, FcitxKey_a, false) || !send(engine, entry, *context, FcitxKey_s, true))
        return 1;

    KbMsg msg{};
    if (!listener.receive(msg, "the Telex replacement request did not arrive"))
        return 1;
    if (msg.op != KbOp::Backspace) {
        reportFailure("replacement op", "op=Backspace", "op=" + std::to_string(static_cast<int>(msg.op)), "the replacement no longer uses the backspace burst");
        return 1;
    }
    if (msg.count != 1) {
        reportFailure("count sent down", "count=1 (characters to delete, anchor excluded)", "count=" + std::to_string(msg.count),
                      "the addon must send the word size so the server emits the burst and appends the anchor");
        return 1;
    }

    // The burst backspace is intermediate; the trailing anchor is the final ack.
    if (!send(engine, entry, *context, FcitxKey_BackSpace, false))
        return 1;
    if (!context->commits().empty()) {
        reportFailure("wait for the anchor", "no commit before the anchor", "commits=" + std::to_string(context->commits().size()),
                      "the replacement committed before the anchor key arrived");
        return 1;
    }
    if (!send(engine, entry, *context, FcitxKey_Pause, true))
        return 1;
    const std::vector<std::string> expected{"á"};
    if (context->commits() != expected) {
        std::string actual = "commits=";
        for (const auto& commit : context->commits())
            actual += "['" + commit + "']";
        reportFailure("commit on the anchor", "commits=['á']", actual, "the anchor echo did not finish the replacement");
        return 1;
    }

    // A later anchor must not reach the app: it is a no-op key, forwarded and never committed.
    if (!send(engine, entry, *context, FcitxKey_Pause, false))
        return 1;
    if (context->commits() != expected) {
        reportFailure("anchor leak", "commits unchanged", "commits=" + std::to_string(context->commits().size()), "the anchor key produced text");
        return 1;
    }

    std::printf("vanh anchor key protocol: all checks passed\n");
    return 0;
}
