// SPDX-FileCopyrightText: 2026 LotusInputMethod contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The server is Rust, the addon C++: this test drives the server's own socket directory, stale
// socket, authorization, decoding and planning code (server-rs/examples/kb_replay.rs) with the
// addon's real KbSender, socket paths and server peer check, so a layout or naming drift between
// the two sides fails here.
//
//  - a regular file at a socket path is never removed: the server refuses to start;
//  - a stale socket file of a previous run is replaced;
//  - the socket directory is 0755 and the socket files 0666;
//  - the mouse socket: the addon's connectServerSocket() accepts the server (same uid as the one
//    VANH_SERVER_UID names) and receives the 'C' click byte; with another expected uid it refuses;
//  - the keyboard socket: four KbSender requests come out as the expected key transcript.
#include "common-fd-guard.h"
#include "lotus-kb-sender.h"
#include "lotus-utils.h"
#include "test-input-context.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

    /// Starts kb_replay expecting @p messages keyboard requests; its stdout is returned in @p out.
    pid_t startReplay(const char* messages, int& out) {
        std::array<int, 2> pipeFds{};
        if (pipe(pipeFds.data()) != 0) {
            return -1;
        }
        const pid_t child = fork();
        if (child == 0) {
            dup2(pipeFds[1], STDOUT_FILENO);
            close(pipeFds[0]);
            close(pipeFds[1]);
            execl(LOTUS_SERVER_KB_REPLAY, LOTUS_SERVER_KB_REPLAY, messages, static_cast<char*>(nullptr));
            _exit(127);
        }
        close(pipeFds[1]);
        out = pipeFds[0];
        return child;
    }

    unsigned modeOf(const std::string& path) {
        struct stat st{};
        return lstat(path.c_str(), &st) == 0 ? static_cast<unsigned>(st.st_mode) : 0U;
    }

    /// Binds and closes a socket at @p path, leaving its file behind like a crashed server.
    bool leaveStaleSocket(const std::string& path) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.data(), path.size());
        FdGuard sock(socket(AF_UNIX, SOCK_SEQPACKET, 0));
        return sock.isValid() && bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1)) == 0;
    }

} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-server-wire-compat");
    const std::string kbPath    = buildSocketPath(VANH_KB_SOCKET_FILE);
    const std::string mousePath = buildSocketPath(VANH_MOUSE_SOCKET_FILE);
    check("socket paths are built", !kbPath.empty() && !mousePath.empty());
    check("both sockets share the per-user directory",
          std::filesystem::path(kbPath).parent_path() == std::filesystem::path(mousePath).parent_path() &&
              std::filesystem::path(kbPath).filename() == "kb.sock" && std::filesystem::path(mousePath).filename() == "mouse.sock");
    const std::string socketDir = std::filesystem::path(kbPath).parent_path().string();
    std::filesystem::create_directories(socketDir);

    // A regular file where the mouse socket goes: the server must neither delete it nor start.
    {
        std::ofstream(mousePath) << "precious";
        int         refusedOut = -1;
        const pid_t refused    = startReplay("0", refusedOut);
        int         status     = 0;
        waitpid(refused, &status, 0);
        close(refusedOut);
        check("server refuses to replace a non-socket file", WIFEXITED(status) && WEXITSTATUS(status) == 2);
        std::ifstream kept(mousePath);
        std::string   content;
        kept >> content;
        check("the non-socket file is left untouched", content == "precious");
        std::filesystem::remove(mousePath);
    }

    // Stale sockets of a crashed run are replaced.
    check("stale keyboard socket left behind", leaveStaleSocket(kbPath));
    check("stale mouse socket left behind", leaveStaleSocket(mousePath));

    int         outFd = -1;
    const pid_t child = startReplay("4", outFd);
    if (child < 0) {
        std::printf("FAIL start replay server\n");
        return 1;
    }
    std::array<int, 1> out{outFd};

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

    check("socket directory is a 0755 directory", S_ISDIR(modeOf(socketDir)) && (modeOf(socketDir) & 07777U) == 0755U);
    check("keyboard socket file is a 0666 socket", S_ISSOCK(modeOf(kbPath)) && (modeOf(kbPath) & 07777U) == 0666U);
    check("mouse socket file is a 0666 socket", S_ISSOCK(modeOf(mousePath)) && (modeOf(mousePath) & 07777U) == 0666U);

    // The addon refuses a server that is not the uinput proxy user (here: any other uid)...
    {
        const std::string ownUid = std::getenv(VANH_SERVER_UID_ENV);
        setenv(VANH_SERVER_UID_ENV, std::to_string(getuid() + 1).c_str(), 1);
        FdGuard refused(connectServerSocket(mousePath, true, "mouse socket"));
        check("a server socket of another uid is refused", !refused.isValid());
        setenv(VANH_SERVER_UID_ENV, ownUid.c_str(), 1);
    }
    // ...and connects the way LotusEngine::setupMouseWatcher() does to the expected one.
    {
        FdGuard sock(connectServerSocket(mousePath, true, "mouse socket"));
        check("mouse socket connects", sock.isValid());
        pollfd pfd{.fd = sock.get(), .events = POLLIN, .revents = 0};
        char   byte = 0;
        check("mouse click byte arrives", sock.isValid() && poll(&pfd, 1, 5000) == 1 && recv(sock.get(), &byte, 1, 0) == 1 && byte == 'C');
    }

    KbSender sender(kbPath);
    check("backspace request", sender.send(KbMsg{.op = KbOp::Backspace, .count = 2, .preDelay = 0, .interval = 6, .postDelay = 4}));
    check("select request", sender.send(KbMsg{.op = KbOp::Select, .count = 3, .preDelay = 0, .interval = 7, .postDelay = 0}));
    check("cancel request", sender.send(KbMsg{.op = KbOp::Cancel, .count = 0, .preDelay = 0, .interval = 0, .postDelay = 0}));
    check("delayed backspace request", sender.send(KbMsg{.op = KbOp::Backspace, .count = 1, .preDelay = 3, .interval = 0, .postDelay = 0}));

    while (readWithTimeout(out[0], transcript, 5000)) {
    }
    int status = 0;
    waitpid(child, &status, 0);
    check("replay server exits cleanly", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    check("the server removes its socket files on exit", modeOf(kbPath) == 0 && modeOf(mousePath) == 0);

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
