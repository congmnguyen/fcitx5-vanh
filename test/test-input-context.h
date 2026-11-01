// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include <unistd.h>

/**
 * @brief Headless Mock InputContext for Fcitx5-Lotus Integration Testing.
 *
 * Architecture & Rationale:
 * Fcitx5 input method engines interact with client applications (text editors,
 * browsers, terminals) through an abstract `fcitx::InputContext` interface. In a
 * live desktop environment, concrete implementations (like X11, Wayland, or D-Bus
 * frontends) handle IPC and render UI.
 *
 * In headless integration tests, we need to verify engine behavior (commits,
 * backspaces, forwarded keys, preedit updates) without running a display server
 * (X11/Wayland) or D-Bus daemon. `TestInputContext` subclasses `fcitx::InputContext`
 * directly, intercepting all engine output callbacks and recording them into
 * in-memory vectors for synchronous, deterministic assertions.
 *
 * Example — Writing a new integration test:
 * @code
 * int main() {
 *     // 1. Sandbox filesystem paths to /tmp/fcitx5-vanh-my-test
 *     configureTestPaths("fcitx5-vanh-my-test");
 *
 *     // 2. Initialize headless Fcitx instance and Lotus engine
 *     TestInstance testInstance;
 *     fcitx::LotusEngine engine(&testInstance.instance);
 *
 *     // 3. Set engine mode and input method
 *     fcitx::RawConfig config;
 *     config.setValueByPath("Mode", "Preedit"); // or "Surrounding", "Smooth", "Off"
 *     config.setValueByPath("InputMethod", "Telex");
 *     engine.setConfig(config);
 *
 *     // 4. Create mock context with client capabilities and activate
 *     auto context = std::make_unique<TestInputContext>(&testInstance.instance);
 *     context->setCapabilityFlags(fcitx::CapabilityFlag::Preedit);
 *     context->focusIn();
 *     fcitx::InputMethodEntry entry("vanh", "Vanh", "vi", "vanh");
 *     fcitx::InputContextEvent in(context.get(), fcitx::EventType::InputContextFocusIn);
 *     engine.activate(entry, in);
 *
 *     // 5. Send keystrokes to the engine
 *     fcitx::KeyEvent event(context.get(), fcitx::Key(FcitxKey_a), false);
 *     engine.keyEvent(entry, event);
 *
 *     // 6. Assert on recorded outputs
 *     // - Commits:        context->commits()
 *     // - Deletions:      context->deletes()
 *     // - Forwarded keys: context->forwarded()
 *     // - Client preedit: context->inputPanel().clientPreedit().toString()
 *     if (!event.accepted() || context->inputPanel().clientPreedit().toString() != "a") {
 *         return 1; // Test failed
 *     }
 *     return 0; // Test passed
 * }
 * @endcode
 *
 * CMake Registration (test/CMakeLists.txt):
 * @code
 * add_lotus_headless_test(my_test my-test.cpp "integration;myfeature")
 * @endcode
 */
class TestInputContext final : public fcitx::InputContext {
  public:
    explicit TestInputContext(fcitx::Instance* instance, const std::string& programName = "test") : InputContext(instance->inputContextManager(), programName) {
        // Fcitx5 lifecycle contract: notifies the InputContextManager that
        // this context is initialized and ready to receive events.
        created();
    }

    ~TestInputContext() override {
        // Fcitx5 lifecycle contract: detaches context and invalidates event slots.
        destroy();
    }

    TestInputContext(const TestInputContext&)            = delete;
    TestInputContext& operator=(const TestInputContext&) = delete;
    TestInputContext(TestInputContext&&)                 = delete;
    TestInputContext& operator=(TestInputContext&&)      = delete;

    /**
     * @brief Overrides the frontend name reported to the engine.
     * @param name Frontend name, e.g. "dbus" to exercise the deferred commit path.
     */
    void setFrontendName(std::string name) {
        frontendName_ = std::move(name);
    }

    [[nodiscard]] const char* frontend() const override {
        return frontendName_.c_str();
    }

    // Called by the engine when text is committed to the client application.
    void commitStringImpl(const std::string& text) override {
        commits_.push_back(text);
        order_.push_back('c');
    }

    // Called when the engine requests surrounding text deletion (e.g. backspacing
    // prior characters during composition or word replacement).
    void deleteSurroundingTextImpl(int offset, unsigned int size) override {
        deletes_.emplace_back(offset, size);
    }

    // Called when a key event is not consumed by the engine and forwarded
    // back to the client application (passthrough mode).
    void forwardKeyImpl(const fcitx::ForwardKeyEvent& event) override {
        forwarded_.push_back(event);
        order_.push_back('f');
    }

    // Called when inline preedit text or server candidate window changes.
    void updatePreeditImpl() override {
        ++preeditUpdates_;
    }

    // Verification helpers: inspect recorded engine actions
    void resetPreeditUpdateCount() {
        preeditUpdates_ = 0;
    }
    [[nodiscard]] unsigned int preeditUpdates() const {
        return preeditUpdates_;
    }
    [[nodiscard]] const std::vector<std::string>& commits() const {
        return commits_;
    }
    [[nodiscard]] const auto& deletes() const {
        return deletes_;
    }
    [[nodiscard]] const auto& forwarded() const {
        return forwarded_;
    }
    /** @brief The order of commits ('c') and forwarded keys ('f'), as the client would see them. */
    [[nodiscard]] const std::string& order() const {
        return order_;
    }

  private:
    std::string                               frontendName_{"test"};
    std::vector<std::string>                  commits_;
    std::vector<std::pair<int, unsigned int>> deletes_;
    std::vector<fcitx::ForwardKeyEvent>       forwarded_;
    std::string                               order_;
    unsigned int                              preeditUpdates_ = 0;
};

/**
 * @brief Sandboxes environment paths to isolate tests from host user configuration.
 *
 * Fcitx5 engines and Bamboo read/write user configuration, dictionaries, and cache
 * under $HOME, $XDG_CONFIG_HOME, $XDG_DATA_HOME, and $XDG_CACHE_HOME.
 * Calling `configureTestPaths` redirects all these paths to an ephemeral subdirectory
 * in the system temporary folder (`/tmp/<name>`).
 *
 * Guarantees:
 * 1. Clean slate: Previous runs or local user configs will not contaminate test results.
 * 2. Host safety: Tests will NEVER overwrite developer's personal fcitx5 settings.
 * 3. Parallel-safe: Each test target passes a unique test name for isolated folders.
 * 4. Socket isolation: the uinput server sockets live in a fresh private directory
 *    (`VANH_SOCKET_DIR`, a short mkdtemp() path so `<dir>/<namespace>/mouse.sock` fits in
 *    sockaddr_un), never in /run/vanh, and the addon accepts a server socket owned by the test's
 *    own uid (`VANH_SERVER_UID`) instead of the uinput proxy user.
 */
inline void configureTestPaths(const char* name) {
    const auto      root = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "config/fcitx5/conf", ec);
    std::filesystem::create_directories(root / "data", ec);
    std::filesystem::create_directories(root / "cache", ec);
#ifdef VANH_TEST_DICTIONARY
    // The dictionary of the source tree, not whatever the host has installed.
    std::filesystem::create_directories(root / "data/fcitx5/vanh", ec);
    std::filesystem::copy_file(VANH_TEST_DICTIONARY, root / "data/fcitx5/vanh/vietnamese.cm.dict", std::filesystem::copy_options::overwrite_existing, ec);
#endif

    setenv("HOME", root.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);

    static std::filesystem::path s_socketDir;
    if (s_socketDir.empty()) {
        std::string pattern = (std::filesystem::temp_directory_path() / "vanh-XXXXXX").string();
        if (mkdtemp(pattern.data()) != nullptr) {
            s_socketDir = pattern;
        }
    }
    setenv("VANH_SOCKET_DIR", s_socketDir.empty() ? (root / "run").c_str() : s_socketDir.c_str(), 1);
    setenv("VANH_SERVER_UID", std::to_string(getuid()).c_str(), 1);

    static std::filesystem::path s_cleanupPath;
    s_cleanupPath                = root;
    static const bool registered = [] {
        std::atexit([] {
            std::error_code err;
            std::filesystem::remove_all(s_cleanupPath, err);
            if (!s_socketDir.empty()) {
                std::filesystem::remove_all(s_socketDir, err);
            }
        });
        return true;
    }();
    (void)registered;
}

/**
 * @brief Headless Fcitx5 instance environment isolator.
 *
 * Instantiates a minimal, headless `fcitx::Instance` with:
 * - `--disable=all`: disables loading external modules (UI, D-Bus, Wayland/X11 frontends),
 *   allowing the instance to initialize in pure headless/CI containers without hanging.
 * - `registerDefaultLoader(nullptr)`: prevents dynamic plugin loader from scanning
 *   system directories (`/usr/lib/fcitx5`) for host-installed addons.
 */
struct TestInstance {
    TestInstance() : instance(2, argv.data()) {
        instance.addonManager().registerDefaultLoader(nullptr);
        instance.initialize();
    }
    std::array<char, 64> program{"vanh-headless-test"};
    std::array<char, 16> disableAll{"--disable=all"};
    std::array<char*, 3> argv{program.data(), disableAll.data(), nullptr};
    fcitx::Instance      instance;
};
