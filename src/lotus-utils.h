/*
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file lotus-utils.h
 * @brief Utility functions and global state for fcitx5-vanh.
 */

#ifndef _FCITX5_LOTUS_UTILS_H_
#define _FCITX5_LOTUS_UTILS_H_

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "common-export.h"
#include "common-socket-name.h"
#include "lotus-mode.h"

/**
 * @brief Maximum length of Unix socket paths.
*/
#define UNIX_PATH_MAX sizeof(((struct sockaddr_un*)0)->sun_path)

// Forward declaration: the header only passes InputContext pointers around.
namespace fcitx {
    class InputContext;
}

// Global state variables for input processing
// LOTUS_EXPORT: read by the headless tests through lotus_test_core.
extern LOTUS_EXPORT std::atomic<fcitx::LotusMode> g_realMode;        ///< Current active input mode
extern std::atomic<bool>                          g_mouseClicked;    ///< Mouse click detection flag
extern std::atomic<bool>                          g_isDeleting;      ///< Deletion in progress flag
extern std::atomic<bool>                          g_stopFlagMonitor; ///< Signal to stop monitor threads
extern std::atomic<unsigned int>                  g_realTextLen;     ///< Current text length
extern std::atomic<int>                           g_mouseSocketFd;   ///< Mouse socket file descriptor

/**
 * @brief Current input mode, read with acquire ordering.
 */
inline fcitx::LotusMode currentMode() {
    return g_realMode.load(std::memory_order_acquire);
}

/**
 * @brief Stores the active input mode with release ordering.
 * @param mode Mode to store.
 */
void setRealMode(fcitx::LotusMode mode);

struct DiffPart {
    std::u32string_view deletedPart;
    std::u32string_view addedPart;
};

/**
 * @brief Builds the path of one uinput server socket of the current user.
 *
 * `<dir>/<namespace>/<fileName>`: dir is /run/vanh (VANH_SOCKET_DIR in the tests), namespace the
 * user name (VANH_SOCKET_NAMESPACE in the tests).
 * @param fileName VANH_KB_SOCKET_FILE or VANH_MOUSE_SOCKET_FILE.
 * @return The path, or an empty string when the namespace is not a single path component or the
 *         path does not fit in sockaddr_un (never truncated: that would name another file).
 */
std::string LOTUS_EXPORT buildSocketPath(std::string_view fileName);

/**
 * @brief Environment variable that replaces the expected uid of the uinput server; tests only.
 */
inline constexpr const char* VANH_SERVER_UID_ENV = "VANH_SERVER_UID";

/**
 * @brief Looks a user up by name.
 * @param name User name.
 * @return Its uid, or nullopt when there is no such user.
 */
std::optional<uid_t> LOTUS_EXPORT lookupUserUid(const char* name);

/**
 * @brief The uid the uinput server must run as.
 *
 * The uid of LOTUS_UINPUT_PROXY_USER (resolved on every call, so a user created after fcitx5
 * started is found), or the decimal value of VANH_SERVER_UID when set (tests).
 * @return The uid, or nullopt when the user does not exist or the override is malformed.
 */
std::optional<uid_t> LOTUS_EXPORT expectedServerUid();

/**
 * @brief Checks that the peer of a connected socket is the uinput server (SO_PEERCRED uid).
 *
 * Logs the reason of a refusal.
 * @param fd Connected AF_UNIX socket.
 * @param label Socket name for the log ("keyboard socket", "mouse socket").
 * @return True when the peer uid is expectedServerUid().
 */
bool LOTUS_EXPORT verifyServerPeer(int fd, std::string_view label);

/**
 * @brief Connects a SOCK_SEQPACKET socket to a uinput server socket and authenticates the server.
 *
 * Logs the reason of a failure.
 * @param path Socket path from buildSocketPath().
 * @param nonBlocking True for a SOCK_NONBLOCK socket.
 * @param label Socket name for the log.
 * @return Connected close-on-exec descriptor owned by the caller, or -1.
 */
int LOTUS_EXPORT connectServerSocket(const std::string& path, bool nonBlocking, std::string_view label);

/**
 * @brief Returns the fcitx5 configuration directory of the current user.
 * @return Path of the fcitx5 configuration directory.
 */
std::string lotusConfigDir();

/**
 * @brief Gets current time in milliseconds.
 * @return Timestamp in milliseconds.
 */
int64_t nowMs();

/**
 * @brief Checks if key symbol is a backspace.
 * @param sym Key symbol to check.
 * @return True if backspace.
 */
bool isBackspace(uint32_t sym);

/**
 * @brief Key value handed to the engine for a key symbol.
 *
 * The Go engine took key symbols (BackSpace, Space and Tab are symbols, not code points), so a symbol
 * with a Unicode mapping is passed as its code point and every other symbol as-is.
 * @param sym Key symbol of the event.
 * @return Key value for lotus::KeyProcessor::processKey.
 */
uint32_t engineKeyValue(uint32_t sym);

/**
 * @brief Whether a mode delivers text through the uinput (fake backspace) path.
 * @param mode Mode to check.
 * @return True for Uinput, Smooth, SuperSmooth and Select.
 */
bool isUinputMode(fcitx::LotusMode mode);

/**
 * @brief Reports whether a program name belongs to a Chromium based browser.
 * @param program Program name reported by fcitx
 * @return True for Chromium-based
 */
bool isChromiumProgram(std::string_view program);

/**
 * @brief Compares two texts and computes diff.
 * @param A First text.
 * @param B Second text.
 * @return Pairs of delete part and add part.
 */
DiffPart compareAndSplitU32(std::u32string_view oldText, std::u32string_view newText);

/**
 * @brief Get the frontend name from the input context.
 * @param ic Input context.
 * @return Frontend name.
 */
std::string getFrontendName(fcitx::InputContext* ic);

/**
 * @brief Key event entry for replay buffer.
 */
struct KeyEntry {
    uint32_t sym;   ///< Key symbol
    uint32_t state; ///< Key state (modifiers)
};

/**
 * @brief get environement variable.
 * @param name Name of the variable.
 * @return Value of the variable.
 */
std::string LOTUS_EXPORT getEnv(const std::string& name);

#endif // _FCITX5_LOTUS_UTILS_H_
