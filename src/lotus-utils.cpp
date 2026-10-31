/*
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
#include "lotus-utils.h"
#include "common-fd-guard.h"
#include "lotus-log.h"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <vector>

std::atomic<fcitx::LotusMode> g_realMode{fcitx::LotusMode::Smooth};
std::atomic<bool>             g_mouseClicked{false};
std::atomic<bool>             g_isDeleting{false};
std::atomic<bool>             g_stopFlagMonitor{false};
std::atomic<unsigned int>     g_realTextLen{0};
std::atomic<int>              g_mouseSocketFd{-1};

namespace fcitx {
    FCITX_DEFINE_LOG_CATEGORY(lotusLog, "vanh", fcitx::LogLevel::NoLog);
} // namespace fcitx

void setRealMode(fcitx::LotusMode mode) {
    g_realMode.store(mode, std::memory_order_release);
}

namespace {
    std::string currentUserName() {
        struct passwd  pwd{};
        struct passwd* result  = nullptr;
        long           bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufSize == -1) {
            bufSize = 16384;
        }
        std::vector<char> buf(bufSize);
        const int         res = getpwuid_r(getuid(), &pwd, buf.data(), bufSize, &result);
        return res == 0 && result != nullptr ? std::string(result->pw_name) : std::string("unknown");
    }
} // namespace

std::string buildSocketPath(std::string_view fileName) {
    const std::string ns = lotusSocketNamespace(currentUserName());
    if (!lotusIsValidSocketNamespace(ns)) {
        return {};
    }
    std::string path = lotusSocketBaseDir();
    path += '/';
    path += ns;
    path += '/';
    path += fileName;
    if (path.size() >= UNIX_PATH_MAX) {
        return {};
    }
    return path;
}

std::optional<uid_t> lookupUserUid(const char* name) {
    if (name == nullptr || *name == '\0') {
        return std::nullopt;
    }
    struct passwd  pwd{};
    struct passwd* result  = nullptr;
    long           bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufSize == -1) {
        bufSize = 16384;
    }
    std::vector<char> buf(bufSize);
    int               res = 0;
    while ((res = getpwnam_r(name, &pwd, buf.data(), buf.size(), &result)) == ERANGE && buf.size() < (1U << 20)) {
        buf.resize(buf.size() * 2);
    }
    if (res != 0 || result == nullptr) {
        return std::nullopt;
    }
    return result->pw_uid;
}

std::optional<uid_t> expectedServerUid() {
    const char* override_value = std::getenv(VANH_SERVER_UID_ENV);
    if (override_value != nullptr && *override_value != '\0') {
        const std::string_view text(override_value);
        uid_t                  uid = 0;
        const auto [end, ec]       = std::from_chars(text.data(), text.data() + text.size(), uid);
        if (ec != std::errc{} || end != text.data() + text.size()) {
            return std::nullopt;
        }
        return uid;
    }
    return lookupUserUid(LOTUS_UINPUT_PROXY_USER);
}

bool verifyServerPeer(int fd, std::string_view label) {
    struct ucred cred{};
    socklen_t    credLen = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0) {
        LOTUS_ERROR("Failed to get the peer credentials of the " + std::string(label) + ": " + std::string(std::strerror(errno)));
        return false;
    }
    const auto expected = expectedServerUid();
    if (!expected) {
        LOTUS_ERROR("Refusing the " + std::string(label) + ": the uinput server user " LOTUS_UINPUT_PROXY_USER " does not exist");
        return false;
    }
    if (cred.uid != *expected) {
        LOTUS_ERROR("Refusing the " + std::string(label) + ": its owner has UID " + std::to_string(cred.uid) + " (PID " + std::to_string(cred.pid) +
                    "), not the uinput server user " LOTUS_UINPUT_PROXY_USER " (UID " + std::to_string(*expected) + ")");
        return false;
    }
    return true;
}

int connectServerSocket(const std::string& path, bool nonBlocking, std::string_view label) {
    sockaddr_un address{};
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        LOTUS_ERROR("Invalid path of the " + std::string(label) + ": " + path);
        return -1;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);

    FdGuard    sock(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | (nonBlocking ? SOCK_NONBLOCK : 0), 0));
    if (!sock.isValid()) {
        LOTUS_ERROR("Failed to create the " + std::string(label) + ": " + std::string(std::strerror(errno)));
        return -1;
    }
    if (connect(sock.get(), reinterpret_cast<const sockaddr*>(&address), length) != 0) {
        LOTUS_ERROR("Failed to connect to the " + std::string(label) + " " + path + ": " + std::string(std::strerror(errno)));
        return -1;
    }
    if (!verifyServerPeer(sock.get(), label)) {
        return -1;
    }
    return sock.release();
}

std::string lotusConfigDir() {
#if LOTUS_USE_MODERN_FCITX_API
    return (fcitx::StandardPaths::global().userDirectory(fcitx::StandardPathsType::Config) / "fcitx5" / "conf").string();
#else
    return fcitx::StandardPath::global().userDirectory(fcitx::StandardPath::Type::Config) + "/fcitx5/conf";
#endif
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool isBackspace(uint32_t sym) {
    return sym == 65288 || sym == 8 || sym == FcitxKey_BackSpace;
}

uint32_t engineKeyValue(uint32_t sym) {
    const uint32_t codePoint = fcitx::Key::keySymToUnicode(static_cast<fcitx::KeySym>(sym));
    return codePoint >= 0x20U ? codePoint : sym;
}

bool isUinputMode(fcitx::LotusMode mode) {
    // Constant set, so unlike the configurable shortcuts elsewhere a static
    // table stays correct.
    static const auto modes = std::to_array<fcitx::LotusMode>({
        fcitx::LotusMode::Smooth,
        fcitx::LotusMode::SuperSmooth,
        fcitx::LotusMode::Select,
    });
    return std::ranges::find(modes, mode) != modes.end();
}

bool isChromiumProgram(std::string_view program) {
    static const auto names = std::to_array<std::string_view>({
        "chrome",
        "chromium",
        "brave",
        "edge",
        "vivaldi",
        "opera",
        "cromite",
        "helium",
        "thorium",
        "slimjet",
        "yandex",
    });
    return std::ranges::any_of(names, [program](std::string_view name) { return program.find(name) != std::string_view::npos; });
}

DiffPart compareAndSplitU32(std::u32string_view oldText, std::u32string_view newText) {
    auto [itA, itB] = std::ranges::mismatch(oldText, newText);
    size_t offset   = std::distance(oldText.begin(), itA);
    return {.deletedPart = oldText.substr(offset), .addedPart = newText.substr(offset)};
}

std::string getFrontendName(fcitx::InputContext* ic) {
    if (ic == nullptr) {
        return "unknown";
    }
    return ic->frontend();
}

std::string getEnv(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    return ((value != nullptr) && ((*value) != 0)) ? value : "";
}