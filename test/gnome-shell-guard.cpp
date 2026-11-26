// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file gnome-shell-guard.cpp
 * @brief Verifies GNOME Shell < 47 keeps gnome-shell contexts on uinput in auto surrounding mode.
 */
#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"

#include <cstdlib>
#include <iostream>
#include <memory>

int main() {
    configureTestPaths("fcitx5-vanh-gnome-shell-guard");
    setenv("VANH_GNOME_SHELL_VERSION", "46.0", 1);

    KbSocketListener listener;
    if (!listener.valid()) {
        return 1;
    }

    TestInstance       testInstance;
    fcitx::LotusEngine engine(&testInstance.instance);

    fcitx::RawConfig config;
    config.setValueByPath("Mode", "Uinput (Smooth)");
    config.setValueByPath("InputMethod", "Telex");
    config.setValueByPath("AutoSurroundingText", "True");
    config.setValueByPath("EnableMacro", "False");
    engine.setConfig(config);

    fcitx::InputMethodEntry entry("vanh", "Vanh", "vi", "vanh");
    int                     failures = 0;

    // On GNOME Shell 46, a gnome-shell context must stay on uinput even when the app reports
    // surrounding text, so Mutter's delete_surrounding assertion can never abort the session.
    {
        auto shell = std::make_unique<TestInputContext>(&testInstance.instance, "gnome-shell");
        shell->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
        shell->focusIn();
        fcitx::InputContextEvent focus(shell.get(), fcitx::EventType::InputContextFocusIn);
        engine.activate(entry, focus);
        shell->surroundingText().setText("d", 1, 1);
        shell->updateSurroundingText();

        fcitx::KeyEvent first(shell.get(), fcitx::Key(FcitxKey_d), false);
        engine.keyEvent(entry, first);
        fcitx::KeyEvent second(shell.get(), fcitx::Key(FcitxKey_d), false);
        engine.keyEvent(entry, second);

        KbMsg msg{};
        if (!shell->deletes().empty()) {
            std::cerr << "FAIL GNOME 46 gnome-shell: sent deleteSurroundingText ("
                      << shell->deletes().size() << ")\n";
            ++failures;
        }
        if (!listener.receive(msg, "GNOME 46 gnome-shell must replace through uinput")) {
            ++failures;
        }
        // Complete the replacement burst so g_isDeleting clears before the next check.
        fcitx::KeyEvent anchor(shell.get(), fcitx::Key(FcitxKey_Pause), false);
        engine.keyEvent(entry, anchor);
    }

    // A non-gnome-shell context (direct GTK/Qt IM module) does not pass through Mutter and still
    // uses surrounding text when the app supports it.
    {
        auto direct = std::make_unique<TestInputContext>(&testInstance.instance, "brave");
        direct->setCapabilityFlags(fcitx::CapabilityFlag::SurroundingText);
        direct->focusIn();
        fcitx::InputContextEvent focus(direct.get(), fcitx::EventType::InputContextFocusIn);
        engine.activate(entry, focus);
        direct->surroundingText().setText("", 0, 0);
        direct->updateSurroundingText();

        fcitx::KeyEvent first(direct.get(), fcitx::Key(FcitxKey_d), false);
        engine.keyEvent(entry, first);
        direct->surroundingText().setText("d", 1, 1);
        direct->updateSurroundingText();
        fcitx::KeyEvent second(direct.get(), fcitx::Key(FcitxKey_d), false);
        engine.keyEvent(entry, second);

        if (direct->deletes().empty()) {
            std::cerr << "FAIL GNOME 46 direct IM: expected surrounding text replacement\n";
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}
