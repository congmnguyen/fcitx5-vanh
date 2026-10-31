// SPDX-License-Identifier: GPL-3.0-or-later
//
// A Select replacement must finish on its Pause anchor even when some Shift+Left
// echoes are lost, and a stray anchor after the burst must not commit again.
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
        if (event.accepted() == requireAccepted)
            return true;
        reportFailure("process key " + std::to_string(symbol), "accepted=" + std::to_string(static_cast<int>(requireAccepted)),
                      "accepted=" + std::to_string(static_cast<int>(event.accepted())), "unexpected key outcome");
        return false;
    }

    std::string commitList(const TestInputContext& context) {
        std::string actual = "commits=";
        for (const auto& commit : context.commits())
            actual += "['" + commit + "']";
        return actual;
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-select-anchor-completion");
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
    context->focusIn();
    fcitx::InputContextEvent focus(context.get(), fcitx::EventType::InputContextFocusIn);
    engine.activate(entry, focus);

    // "at" plus the tone-mark key requests a 2-character selection.
    if (!send(engine, entry, *context, FcitxKey_a, false) || !send(engine, entry, *context, FcitxKey_t, false) || !send(engine, entry, *context, FcitxKey_s, true))
        return 1;
    KbMsg msg{};
    if (!listener.receive(msg, "the Telex replacement request did not arrive"))
        return 1;
    if (msg.op != KbOp::Select || msg.count != 2) {
        reportFailure("receive replacement request", "op=select, count=2", "op=" + std::to_string(static_cast<int>(msg.op)) + ", count=" + std::to_string(msg.count),
                      "the selection did not match the deleted part");
        return 1;
    }

    // One echo is lost; the anchor still closes the replacement.
    if (!send(engine, entry, *context, FcitxKey_Left, false))
        return 1;
    if (!send(engine, entry, *context, FcitxKey_Pause, true))
        return 1;
    const std::vector<std::string> afterAnchor{"át"};
    if (context->commits() != afterAnchor) {
        reportFailure("commit on the anchor", "commits=['át']", commitList(*context), "the replacement was not committed when the anchor arrived with echoes missing");
        return 1;
    }

    // A stray anchor after the burst is forwarded and must not commit again.
    if (!send(engine, entry, *context, FcitxKey_Pause, false))
        return 1;
    if (context->commits() != afterAnchor) {
        reportFailure("no second commit on a stray anchor", "commits=['át']", commitList(*context), "a stray anchor committed the replacement twice");
        return 1;
    }

    std::printf("vanh select anchor completion: all checks passed\n");
    return 0;
}
