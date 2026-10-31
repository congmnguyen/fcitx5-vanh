// SPDX-License-Identifier: GPL-3.0-or-later
//
// The addon only talks to a uinput server socket owned by the uinput proxy user: the SO_PEERCRED
// uid of the peer decides, never its executable or argv[0] (which any process can choose). This
// test checks the uid resolution, the peer check on socketpairs with real credentials, the refusal
// of a listening socket owned by an unexpected uid, and the socket path rules.
#include "common-fd-guard.h"
#include "kb-socket-listener.h"
#include "lotus-kb-sender.h"
#include "lotus-utils.h"
#include "test-input-context.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

namespace {
    int  failures = 0;

    void check(const char* name, bool condition) {
        if (!condition) {
            std::printf("FAIL %s\n", name);
            ++failures;
        }
    }

    void setServerUid(uid_t uid) {
        setenv(VANH_SERVER_UID_ENV, std::to_string(uid).c_str(), 1);
    }
} // namespace

int main() {
    configureTestPaths("fcitx5-vanh-server-peer-auth");
    const uid_t self  = getuid();
    const uid_t other = self + 1;

    // User lookup.
    check("root resolves to uid 0", lookupUserUid("root") == std::optional<uid_t>(0));
    check("an unknown user does not resolve", !lookupUserUid("no-such-user-vanh-test").has_value());
    check("an empty name does not resolve", !lookupUserUid("").has_value());

    // Expected server uid: the override, else the proxy user.
    setServerUid(other);
    check("the override names the server uid", expectedServerUid() == std::optional<uid_t>(other));
    setenv(VANH_SERVER_UID_ENV, "12abc", 1);
    check("a malformed override refuses every server", !expectedServerUid().has_value());
    unsetenv(VANH_SERVER_UID_ENV);
    check("without override the proxy user is expected", expectedServerUid() == lookupUserUid(LOTUS_UINPUT_PROXY_USER));

    // Peer check on a socketpair: the credentials are this process's, whatever its argv[0].
    {
        int fds[2] = {-1, -1};
        check("socketpair", socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) == 0);
        FdGuard left(fds[0]);
        FdGuard right(fds[1]);
        setServerUid(self);
        check("a peer of the expected uid is accepted", verifyServerPeer(left.get(), "test socket"));
        setServerUid(other);
        check("a peer of another uid is refused", !verifyServerPeer(left.get(), "test socket"));
        setenv(VANH_SERVER_UID_ENV, "x", 1);
        check("an unresolvable server user refuses the peer", !verifyServerPeer(left.get(), "test socket"));
    }
    check("a descriptor without credentials is refused", !verifyServerPeer(-1, "test socket"));

    // A listening socket squatted by an unexpected uid: KbSender never connects to it.
    {
        setServerUid(self);
        KbSocketListener listener;
        check("listener binds", listener.valid());
        setServerUid(other);
        KbSender squatted(buildSocketPath(VANH_KB_SOCKET_FILE));
        check("the sender refuses a socket of another uid", !squatted.ensureConnected());
        check("nothing is sent to a socket of another uid", !squatted.send(KbMsg{.op = KbOp::Backspace, .count = 1, .preDelay = 0, .interval = 1, .postDelay = 0}));
        KbMsg leaked{};
        check("the squatter receives nothing", !listener.tryReceive(leaked));
        setServerUid(self);
        KbSender genuine(buildSocketPath(VANH_KB_SOCKET_FILE));
        check("the sender connects to the expected uid", genuine.ensureConnected());
    }

    // Socket paths.
    {
        setenv(VANH_SOCKET_DIR_ENV, "/run/vanh-test", 1);
        setenv(VANH_SOCKET_NAMESPACE_ENV, "alice", 1);
        check("keyboard socket path", buildSocketPath(VANH_KB_SOCKET_FILE) == "/run/vanh-test/alice/kb.sock");
        check("mouse socket path", buildSocketPath(VANH_MOUSE_SOCKET_FILE) == "/run/vanh-test/alice/mouse.sock");
        unsetenv(VANH_SOCKET_DIR_ENV);
        check("default socket directory", buildSocketPath(VANH_KB_SOCKET_FILE) == "/run/vanh/alice/kb.sock");
        for (const char* bad : {"..", ".", "a/b", "../alice"}) {
            setenv(VANH_SOCKET_NAMESPACE_ENV, bad, 1);
            check("a namespace that is not one path component gives no path", buildSocketPath(VANH_KB_SOCKET_FILE).empty());
        }
        setenv(VANH_SOCKET_NAMESPACE_ENV, std::string(120, 'n').c_str(), 1);
        check("a path longer than sun_path gives no path (never truncated)", buildSocketPath(VANH_KB_SOCKET_FILE).empty());
        check("connecting to an empty path fails", connectServerSocket("", false, "test socket") < 0);
    }

    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("vanh server peer auth: all checks passed\n");
    return 0;
}
