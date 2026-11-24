// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file first-run-mode.cpp
 * @brief Verifies first-run mode auto-selection when the uinput server is running.
 */
#include "kb-socket-listener.h"
#include "lotus-engine.h"
#include "test-input-context.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace {
    int  failures = 0;

    void check(const char* name, bool condition) {
        if (!condition) {
            std::printf("FAIL %s\n", name);
            ++failures;
        }
    }
} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-first-run-mode");
    const std::filesystem::path conf =
        std::filesystem::path(std::getenv("XDG_CONFIG_HOME")) / "fcitx5/conf/vanh.conf";

    // 1. Fresh install without a running uinput server stays on Preedit.
    {
        std::filesystem::remove(conf);
        TestInstance       testInstance;
        fcitx::LotusEngine engine(&testInstance.instance);
        check("fresh run without uinput server keeps Preedit",
            engine.config().mode.value() == fcitx::LotusMode::Preedit);
    }

    // 2. Fresh install with a running uinput server defaults to Uinput (Smooth) and persists it.
    {
        std::filesystem::remove(conf);
        KbSocketListener listener;
        if (!listener.valid()) {
            return 1;
        }
        {
            TestInstance       testInstance;
            fcitx::LotusEngine engine(&testInstance.instance);
            check("fresh run with uinput server defaults to Uinput (Smooth)",
                engine.config().mode.value() == fcitx::LotusMode::Smooth);
            check("first-run mode selection is saved to vanh.conf", std::filesystem::exists(conf));
        }

        // 3. Explicit user choice of Preedit in an existing vanh.conf is never overridden.
        {
            TestInstance       testInstance;
            fcitx::LotusEngine engine(&testInstance.instance);
            fcitx::RawConfig   raw;
            raw.setValueByPath("Mode", "Preedit");
            engine.setConfig(raw);
        }
        {
            TestInstance       testInstance;
            fcitx::LotusEngine engine(&testInstance.instance);
            check("existing vanh.conf with Preedit is respected even when server is up",
                engine.config().mode.value() == fcitx::LotusMode::Preedit);
        }
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("vanh first-run mode: all checks passed\n");
    return 0;
}
