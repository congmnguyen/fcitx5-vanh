// SPDX-FileCopyrightText: 2026 LotusInputMethod contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The server is Rust, the addon C++: this test drives the server's own socket, authorization,
// decoding and planning code (server-rs/examples/kb_replay.rs) with the addon's real KbSender and
// socket names, so a layout or naming drift between the two sides fails here.
//
//  - the mouse socket: an authorized client receives the 'C' click byte;
//  - the keyboard socket: four KbSender requests come out as the expected key transcript.
#include "common-fd-guard.h"
#include "lotus-kb-sender.h"
#include "lotus-utils.h"
#include "test-input-context.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

    int  failures = 0;

    void check(const char* name, bool condition) {
        if (!condition) {
            std::printf("FAIL %s\n", name);
            ++failures;
        }
    }

    bool readWithTimeout(int fd, std::string& out, int timeoutMs) {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, timeoutMs) <= 0) {
            return false;
        }
        std::array<char, 4096> buf{};
        const ssize_t          n = read(fd, buf.data(), buf.size());
        if (n <= 0) {
            return false;
        }
        out.append(buf.data(), static_cast<std::size_t>(n));
        return true;
    }

    std::string selfExecutable() {
        std::array<char, 4096> path{};
        const ssize_t          n = readlink("/proc/self/exe", path.data(), path.size() - 1);
        return n > 0 ? std::string(path.data(), static_cast<std::size_t>(n)) : std::string{};
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-lotus-server-wire-compat");
    const std::string self = selfExecutable();
    check("own executable resolves", !self.empty());

    std::array<int, 2> out{};
    if (pipe(out.data()) != 0) {
        std::printf("FAIL pipe\n");
        return 1;
    }
    const pid_t child = fork();
    if (child == 0) {
        dup2(out[1], STDOUT_FILENO);
        close(out[0]);
        close(out[1]);
        execl(LOTUS_SERVER_KB_REPLAY, LOTUS_SERVER_KB_REPLAY, self.c_str(), "4", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(out[1]);

    std::string transcript;
    while (transcript.find('\n') == std::string::npos) {
        if (!readWithTimeout(out[0], transcript, 5000)) {
            std::printf("FAIL replay server did not start\n");
            kill(child, SIGTERM);
            return 1;
        }
    }
    check("replay server is ready", transcript == "ready\n");
    transcript.clear();

    // Mouse socket, addressed the way makeAbstractUnixSocket() does for LotusEngine::setupMouseWatcher().
    {
        const std::string name = buildSocketPath("mouseSocket");
        sockaddr_un       addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(&addr.sun_path[1], name.data(), name.size());
        const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1);
        FdGuard    sock(socket(AF_UNIX, SOCK_SEQPACKET, 0));
        check("mouse socket connects", sock.isValid() && connect(sock.get(), reinterpret_cast<sockaddr*>(&addr), len) == 0);
        pollfd pfd{.fd = sock.get(), .events = POLLIN, .revents = 0};
        char   byte = 0;
        check("mouse click byte arrives", poll(&pfd, 1, 5000) == 1 && recv(sock.get(), &byte, 1, 0) == 1 && byte == 'C');
    }

    KbSender sender(buildSocketPath("kbSocket"));
    check("backspace request", sender.send(KbMsg{.op = KbOp::Backspace, .count = 2, .preDelay = 0, .interval = 6, .postDelay = 4}));
    check("select request", sender.send(KbMsg{.op = KbOp::Select, .count = 3, .preDelay = 0, .interval = 7, .postDelay = 0}));
    check("cancel request", sender.send(KbMsg{.op = KbOp::Cancel, .count = 0, .preDelay = 0, .interval = 0, .postDelay = 0}));
    check("delayed backspace request", sender.send(KbMsg{.op = KbOp::Backspace, .count = 1, .preDelay = 3, .interval = 0, .postDelay = 0}));

    while (readWithTimeout(out[0], transcript, 5000)) {
    }
    int status = 0;
    waitpid(child, &status, 0);
    check("replay server exits cleanly", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    const std::string expected = "tap 14\ntap 14\nsleep 4\ntap 119\n"                          // BackSpace x2, post-delay, Pause
                                 "down 42\nsleep 7\ntap 105\n"                                // Shift down, first Left
                                 "sleep 5\ntap 105\nsleep 5\ntap 105\nup 42\ntap 111\n"       // Cancel completes the selection, Delete
                                 "sleep 3\ntap 14\ntap 119\n";                                // pre-delay, BackSpace, Pause
    if (transcript != expected) {
        std::printf("FAIL key transcript\n--- expected\n%s--- actual\n%s---\n", expected.c_str(), transcript.c_str());
        ++failures;
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("server wire compat: all checks passed\n");
    return 0;
}
