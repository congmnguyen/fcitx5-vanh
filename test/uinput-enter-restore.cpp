// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file uinput-enter-restore.cpp
 * @brief Enter and the revert key in the uinput modes, against a simulated client buffer.
 *
 * An English word last on a line must be restored before Enter reaches the app: Enter is held
 * back while the uinput Backspaces delete the shown word, and is sent (forwardKey) after the
 * restored word is committed, never before it. The revert key right after a word break switches
 * the word to its other reading through the same replacement.
 *
 * Keys: '\n' = Enter, '\b' = the revert key (Shift+BackSpace).
 */

#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"
#include "utf8.h"

#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>
#include <systemd/sd-event.h>

#include <array>
#include <chrono>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <memory>
#include <string>
#include <string_view>

namespace {

    using lotus::utf8::decode;
    using lotus::utf8::encode;

    struct Client {
        fcitx::LotusEngine&             engine;
        const fcitx::InputMethodEntry&  entry;
        TestInputContext&               context;
        KbSocketListener&               listener;
        std::u32string                  text          = {};
        size_t                          commitsSeen   = 0;
        size_t                          forwardedSeen = 0;
        /// Fast typing: the server's bursts wait until `lag` more keys were typed.
        int                             lag = 0;
        /// The dbus frontend in a GTK4 app: forwarded keys are dropped (fcitx5-gtk4), only keys
        /// the uinput server types (KbOp::Return) reach the app.
        bool                            gtk4 = false;
        fcitx::EventLoop*               loop = nullptr;
        /// A web editor (Apple Notes in Brave) that reports its text but drops deleteSurroundingText
        /// and resets the input method after every key.
        bool                            notes = false;
        /// Time between two keys, the event loop running meanwhile.
        int                             keyMs = 0;

        /** Lets the event loop run the 0 ms timers the dbus frontend defers its commits to. */
        void pump() {
            if (loop == nullptr) {
                return;
            }
            // One non-blocking pass of the loop (sd-event): an exited fcitx loop cannot run again.
            auto* event = static_cast<sd_event*>(loop->nativeHandle());
            while (sd_event_run(event, 0) > 0) {}
        }
        std::vector<KbMsg>              pending       = {};
        std::vector<int>                pendingAge    = {};

        /** Applies what the engine sent to the app since the last call. */
        size_t orderSeen = 0;

        void sync() {
            pump();
            for (; orderSeen < context.order().size(); ++orderSeen) {
                if (context.order()[orderSeen] == 'c') {
                    if (std::getenv("TRACE") != nullptr) {
                        std::printf("  commit '%s'\n", context.commits()[commitsSeen].c_str());
                    }
                    text += decode(context.commits()[commitsSeen++]);
                    continue;
                }
                const auto& f = context.forwarded()[forwardedSeen++];
                if (!gtk4 && !f.isRelease() && f.rawKey().sym() == FcitxKey_Return) {
                    text.push_back(U'\n');
                }
            }
        }

        bool press(const fcitx::Key& key, char32_t typed) {
            if (notes) {
                const std::string shown = encode(text);
                const auto        cursor = static_cast<unsigned int>(text.size());
                context.surroundingText().setText(shown, cursor, cursor);
                context.updateSurroundingText();
            }
            fcitx::KeyEvent event(&context, key, false);
            engine.keyEvent(entry, event);
            sync();
            if (notes) {
                fcitx::InputContextEvent reset(&context, fcitx::EventType::InputContextReset);
                engine.reset(entry, reset);
            }
            if (!event.accepted() && typed != 0) {
                text.push_back(typed);
            }
            // The uinput server: delete, then the anchor that completes the replacement.
            KbMsg msg{};
            while (listener.tryReceive(msg)) {
                pending.push_back(msg);
                pendingAge.push_back(0);
            }
            for (int& age : pendingAge) {
                ++age;
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(keyMs);
            while (std::chrono::steady_clock::now() < until) {
                sync();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return drain(false);
        }

        bool drain(bool all) {
            // A Return request is typed by the server at once (no burst to pace), before the next
            // key a person could type; only the backspace bursts lag.
            while (!pending.empty() && (all || pendingAge.front() > lag || pending.front().op == KbOp::Return)) {
                const KbMsg msg = pending.front();
                pending.erase(pending.begin());
                pendingAge.erase(pendingAge.begin());
                if (std::getenv("TRACE") != nullptr) {
                    std::printf("  request op=%d count=%zu\n", static_cast<int>(msg.op), msg.count);
                }
                if (msg.op == KbOp::Return) {
                    // The server types Enter: it reaches the app through the IM, which must let it pass.
                    for (size_t i = 0; i < msg.count; ++i) {
                        fcitx::KeyEvent enter(&context, fcitx::Key(FcitxKey_Return), false);
                        engine.keyEvent(entry, enter);
                        sync();
                        if (enter.accepted()) {
                            std::printf("  the Enter the server typed was swallowed on '%s'\n", encode(text).c_str());
                            return false;
                        }
                        text.push_back(U'\n');
                    }
                    fcitx::KeyEvent anchor(&context, fcitx::Key(FcitxKey_Pause), false);
                    engine.keyEvent(entry, anchor);
                    sync();
                    KbMsg more{};
                    while (listener.tryReceive(more)) {
                        pending.push_back(more);
                        pendingAge.push_back(0);
                    }
                    continue;
                }
                if (msg.op != KbOp::Backspace || msg.count > text.size()) {
                    std::printf("  unexpected request op=%d count=%zu on '%s'\n", static_cast<int>(msg.op), msg.count, encode(text).c_str());
                    return false;
                }
                if (std::getenv("TRACE") != nullptr) {
                    std::printf("  burst %zu on '%s'\n", msg.count, encode(text).c_str());
                }
                text.resize(text.size() - msg.count);
                for (size_t i = 0; i < msg.count; ++i) {
                    fcitx::KeyEvent bs(&context, fcitx::Key(FcitxKey_BackSpace), false);
                    engine.keyEvent(entry, bs);
                }
                fcitx::KeyEvent anchor(&context, fcitx::Key(FcitxKey_Pause), false);
                engine.keyEvent(entry, anchor);
                sync();
                KbMsg more{};
                while (listener.tryReceive(more)) {
                    pending.push_back(more);
                    pendingAge.push_back(0);
                }
            }
            return true;
        }

        bool type(std::string_view keys) {
            text.clear();
            fcitx::InputContextEvent resetEvent(&context, fcitx::EventType::InputContextReset);
            engine.reset(entry, resetEvent);
            KbMsg drained{};
            while (listener.tryReceive(drained)) {}
            commitsSeen   = context.commits().size();
            forwardedSeen = context.forwarded().size();
            orderSeen     = context.order().size();
            for (const char ch : keys) {
                bool ok = true;
                if (ch == '\n') {
                    ok = press(fcitx::Key(FcitxKey_Return), U'\n');
                } else if (ch == '\b') {
                    // Shift is still down: the backspaces must wait for its release, or the app
                    // gets Shift+BackSpace (Messenger ignores it).
                    const size_t before = pending.size();
                    ok = press(fcitx::Key(FcitxKey_BackSpace, fcitx::KeyState::Shift), 0);
                    // Ctrl goes down and up again meanwhile: Shift is still held.
                    fcitx::KeyEvent ctrlDown(&context, fcitx::Key(FcitxKey_Control_L, fcitx::KeyState::Shift), false);
                    engine.keyEvent(entry, ctrlDown);
                    fcitx::KeyEvent ctrlUp(&context,
                        fcitx::Key(FcitxKey_Control_L, fcitx::KeyStates{fcitx::KeyState::Shift, fcitx::KeyState::Ctrl}), true);
                    engine.keyEvent(entry, ctrlUp);
                    sync();
                    KbMsg early{};
                    while (listener.tryReceive(early)) {
                        pending.push_back(early);
                        pendingAge.push_back(0);
                    }
                    if (ok && pending.size() != before) {
                        std::printf("  the revert deleted with Shift held on '%s'\n", encode(text).c_str());
                        return false;
                    }
                    fcitx::KeyEvent up(&context, fcitx::Key(FcitxKey_Shift_L, fcitx::KeyState::Shift), true);
                    engine.keyEvent(entry, up);
                    sync();
                    KbMsg msg{};
                    while (listener.tryReceive(msg)) {
                        pending.push_back(msg);
                        pendingAge.push_back(0);
                    }
                } else {
                    ok = press(fcitx::Key(static_cast<fcitx::KeySym>(static_cast<unsigned char>(ch))), static_cast<char32_t>(ch));
                }
                if (!ok) {
                    return false;
                }
            }
            return drain(true);
        }
    };

} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-uinput-enter-restore");
    if (std::getenv("TRACE") != nullptr) {
        fcitx::Log::setLogRule("vanh=5");
    }
    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);
    KbSocketListener   listener;
    if (!listener.valid())
        return 1;
    const fcitx::InputMethodEntry entry("vanh", "Vanh", "vi", "vanh");

    struct Case {
        std::string_view    keys;
        std::u32string_view want;
    };
    constexpr std::array<Case, 7> cases{
        Case{.keys = "the poller is\n", .want = U"the poller is\n"},
        Case{.keys = "status line as\nnext", .want = U"status line as\nnext"},
        Case{.keys = "tooi ddi hocj\n", .want = U"tôi đi học\n"},
        Case{.keys = "git push\n", .want = U"git push\n"},
        Case{.keys = "\n\n", .want = U"\n\n"},
        Case{.keys = "dduwowcj host \brieeng", .want = U"được hót riêng"},
        Case{.keys = "theo max \bticket", .want = U"theo max ticket"},
    };

    int failures = 0;
    fcitx::RawConfig config;
    config.setValueByPath("Mode", "Uinput (Smooth)");
    config.setValueByPath("InputMethod", "Telex");
    config.setValueByPath("DefaultBackspaceInterval", "1");
    config.setValueByPath("EnableMacro", "False");
    engine.setConfig(config);
    for (const bool gtk4 : {false, true}) {
    for (const Case& test : cases) {
        auto ctx = std::make_unique<TestInputContext>(&testInstance.instance);
        if (gtk4) {
            ctx->setFrontendName("dbus");
        }
        ctx->focusIn();
        fcitx::InputContextEvent focusEvent(ctx.get(), fcitx::EventType::InputContextFocusIn);
        engine.activate(entry, focusEvent);
        Client client{.engine = engine, .entry = entry, .context = *ctx, .listener = listener, .gtk4 = gtk4, .loop = gtk4 ? &testInstance.instance.eventLoop() : nullptr};
        if (!client.type(test.keys)) {
            std::printf("FAIL%s '%s': bad replacement request\n", gtk4 ? " (gtk4)" : "", std::string(test.keys).c_str());
            ++failures;
            continue;
        }
        if (client.text != test.want) {
            std::printf("FAIL%s '%s': expected '%s', got '%s'\n", gtk4 ? " (gtk4)" : "", std::string(test.keys).c_str(), encode(test.want).c_str(), encode(client.text).c_str());
            ++failures;
        }
    }
    // Fast typing: random lines typed while the replacements lag one to three keys behind must
    // come out exactly as when every replacement completes before the next key (nothing dropped,
    // nothing doubled, Enter after the restored word).
    const std::array<std::string_view, 14> words{"the", "is", "as", "docs", "host", "max", "tooi", "dduwowcj", "vieejt", "cos", "diff", "git", "has", "mono"};
    uint64_t                               seed = 20261101;
    auto                                   next = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    const int rounds = std::getenv("VANH_FUZZ_SCALE") != nullptr ? 300 * std::atoi(std::getenv("VANH_FUZZ_SCALE")) : 300;
    for (int round = 0; round < rounds; ++round) {
        std::string keys;
        for (int w = 0, n = 2 + static_cast<int>(next() % 6); w < n; ++w) {
            keys += words[next() % words.size()];
            keys += (next() % 4 == 0 && std::getenv("NO_ENTER") == nullptr) ? '\n' : ' ';
        }
        std::u32string results[2];
        for (int lag : {0, 1 + static_cast<int>(next() % 3)}) {
            auto ctx = std::make_unique<TestInputContext>(&testInstance.instance);
            if (gtk4) {
                ctx->setFrontendName("dbus");
            }
            ctx->focusIn();
            fcitx::InputContextEvent focusEvent(ctx.get(), fcitx::EventType::InputContextFocusIn);
            engine.activate(entry, focusEvent);
            Client client{.engine = engine, .entry = entry, .context = *ctx, .listener = listener, .lag = lag, .gtk4 = gtk4, .loop = gtk4 ? &testInstance.instance.eventLoop() : nullptr};
            if (!client.type(keys)) {
                std::printf("FAIL%s fast typing round %d (lag %d): bad replacement request typing '%s'\n", gtk4 ? " (gtk4)" : "", round, lag, keys.c_str());
                ++failures;
            }
            results[lag == 0 ? 0 : 1] = client.text;
        }
        if (results[0] != results[1]) {
            std::printf("FAIL%s fast typing round %d: '%s' slow '%s' fast '%s'\n", gtk4 ? " (gtk4)" : "", round, keys.c_str(), encode(results[0]).c_str(), encode(results[1]).c_str());
            ++failures;
        }
    }
    }

    // Apple Notes on the web drops the deletions of the surrounding text path: the word typed when
    // that shows is repaired and the rest goes through the uinput server.
    config.setValueByPath("AutoSurroundingText", "True");
    engine.setConfig(config);
    {
        // Not "gnome-shell": its pacing of the surrounding edits is tested in surrounding-text-typing.
        auto ctx = std::make_unique<TestInputContext>(&testInstance.instance, "brave");
        ctx->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
        ctx->focusIn();
        fcitx::InputContextEvent focusEvent(ctx.get(), fcitx::EventType::InputContextFocusIn);
        engine.activate(entry, focusEvent);
        Client client{.engine = engine, .entry = entry, .context = *ctx, .listener = listener, .notes = true};
        if (!client.type("dduwowcj dduwowcj vieetj ") || client.text != U"được được việt ") {
            std::printf("FAIL Apple Notes: expected 'được được việt ', got '%s'\n", encode(client.text).c_str());
            ++failures;
        }
    }

    // Messenger on facebook.com through GNOME Shell keeps reporting an empty field, so GNOME Shell
    // drops every deletion (here: none is applied): once the app has shown none of the edits for
    // a while, the field goes through the uinput server. Keys 60 ms apart.
    {
        auto ctx = std::make_unique<TestInputContext>(&testInstance.instance, "gnome-shell");
        ctx->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
        ctx->focusIn();
        fcitx::InputContextEvent focusEvent(ctx.get(), fcitx::EventType::InputContextFocusIn);
        engine.activate(entry, focusEvent);
        ctx->surroundingText().setText("", 0, 0);
        ctx->updateSurroundingText();
        Client client{.engine = engine, .entry = entry, .context = *ctx, .listener = listener,
            .loop = &testInstance.instance.eventLoop(), .keyMs = 60};
        bool ok = client.type("khi naof ");
        // the held edit gives up waiting for the app after 200 ms: the server gets its burst
        KbMsg msg{};
        for (int i = 0; i < 400 && client.pending.empty(); ++i) {
            client.sync();
            while (listener.tryReceive(msg)) {
                client.pending.push_back(msg);
                client.pendingAge.push_back(0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ok = ok && client.drain(true);
        if (!ok || client.text != U"khi nào ") {
            std::printf("FAIL Messenger: expected 'khi nào ', got '%s'\n", encode(client.text).c_str());
            ++failures;
        }
    }

    // The same field, clicked while the edit waits: the cursor may have moved. No uinput
    // backspaces (they would delete at the new cursor), and no protocol edit the app cannot
    // be seen to take (GNOME Shell would drop its deletion, "nao" became "naoào "): the edit
    // is dropped. Unless the app shows what it deletes by then: it goes through the protocol.
    {
        // the server's mouse socket, which the engine connects to on activation
        FdGuard mouse(socket(AF_UNIX, SOCK_SEQPACKET, 0));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string mousePath = buildSocketPath(VANH_MOUSE_SOCKET_FILE);
        std::memcpy(address.sun_path, mousePath.data(), mousePath.size());
        unlink(mousePath.c_str());
        if (bind(mouse.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 || listen(mouse.get(), 1) < 0) {
            std::printf("FAIL Messenger, click: mouse socket\n");
            return 1;
        }
        FdGuard watcher;
        for (const bool reports : {false, true}) {
            auto ctx = std::make_unique<TestInputContext>(&testInstance.instance, "gnome-shell");
            ctx->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
            ctx->focusIn();
            fcitx::InputContextEvent focusEvent(ctx.get(), fcitx::EventType::InputContextFocusIn);
            engine.activate(entry, focusEvent);
            ctx->surroundingText().setText("", 0, 0);
            ctx->updateSurroundingText();
            if (!watcher.isValid()) {
                watcher = FdGuard(accept(mouse.get(), nullptr, nullptr));
            }
            Client client{.engine = engine, .entry = entry, .context = *ctx, .listener = listener,
                .loop = &testInstance.instance.eventLoop(), .keyMs = 60};
            const bool ok = client.type("khi naof ");
            const size_t edits = ctx->edits().size();
            if (reports) {
                ctx->surroundingText().setText("khi nao", 7, 7);
                ctx->updateSurroundingText();
            }
            if (send(watcher.get(), "C", 1, 0) != 1) {
                std::printf("FAIL Messenger, click: send click\n");
                return 1;
            }
            KbMsg msg{};
            for (int i = 0; i < 400; ++i) {
                client.sync();
                while (listener.tryReceive(msg)) {
                    client.pending.push_back(msg);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto& sent = ctx->edits();
            const bool expected = reports
                ? sent.size() == edits + 2 && sent[edits].kind == 'd' && sent[edits].size == 2 && sent[edits + 1].text == "ào "
                : sent.size() == edits;
            if (!ok || !client.pending.empty() || !expected) {
                std::printf("FAIL Messenger, click (app %s): %zu uinput message(s), %zu protocol edit(s)\n",
                    reports ? "reports" : "silent", client.pending.size(), sent.size() - edits);
                for (size_t i = edits; i < sent.size(); ++i) {
                    std::printf("  %c %u '%s'\n", sent[i].kind, sent[i].size, sent[i].text.c_str());
                }
                ++failures;
            }
        }
        unlink(mousePath.c_str());
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("vanh uinput enter restore: all checks passed\n");
    return 0;
}
