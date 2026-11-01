/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoàng Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/*
 * Header-only, shared by the addon and the tests. The Rust server builds the same paths from
 * server-rs/src/names.rs and server-rs/src/sockets.rs (keep both in sync).
 *
 * The sockets are filesystem sockets <dir>/<namespace>/{kb,mouse}.sock: <dir> is /run/vanh,
 * writable only by the uinput proxy user, so no other user can bind them first.
 */

#ifndef _COMMON_SOCKET_NAME_H_
#define _COMMON_SOCKET_NAME_H_

#include <cstdlib>
#include <string>
#include <string_view>

/**
 * @brief Environment variable that replaces the user segment of the socket paths.
 */
inline constexpr const char* VANH_SOCKET_NAMESPACE_ENV = "VANH_SOCKET_NAMESPACE";

/**
 * @brief Environment variable that replaces the socket directory; for the tests only.
 */
inline constexpr const char* VANH_SOCKET_DIR_ENV = "VANH_SOCKET_DIR";

/**
 * @brief Directory of the per-user socket directories (tmpfiles.d, systemd RuntimeDirectory=).
 */
inline constexpr const char* VANH_DEFAULT_SOCKET_DIR = "/run/vanh";

/**
 * @brief File name of the keyboard request socket.
 */
inline constexpr std::string_view VANH_KB_SOCKET_FILE = "kb.sock";

/**
 * @brief File name of the mouse click notification socket.
 */
inline constexpr std::string_view VANH_MOUSE_SOCKET_FILE = "mouse.sock";

/**
 * @brief Resolves the user segment of the socket paths.
 * @param username Default segment, the name of the current user.
 * @return Value of VANH_SOCKET_NAMESPACE_ENV when set and non-empty, @p username otherwise.
 */
inline std::string lotusSocketNamespace(std::string_view username) {
    const char* override_value = std::getenv(VANH_SOCKET_NAMESPACE_ENV);
    if (override_value != nullptr && *override_value != '\0') {
        return override_value;
    }
    return std::string(username);
}

/**
 * @brief Resolves the socket directory.
 * @return Value of VANH_SOCKET_DIR_ENV when set and non-empty, VANH_DEFAULT_SOCKET_DIR otherwise.
 */
inline std::string lotusSocketBaseDir() {
    const char* override_value = std::getenv(VANH_SOCKET_DIR_ENV);
    if (override_value != nullptr && *override_value != '\0') {
        return override_value;
    }
    return VANH_DEFAULT_SOCKET_DIR;
}

/**
 * @brief Checks that a namespace is one path component (server-rs is_valid_namespace()).
 * @param ns Namespace to check.
 * @return True when non-empty, without '/' or NUL, and neither "." nor "..".
 */
inline bool lotusIsValidSocketNamespace(std::string_view ns) {
    return !ns.empty() && ns != "." && ns != ".." &&
        ns.find_first_of(std::string_view("/\0", 2)) == std::string_view::npos;
}

#endif // _COMMON_SOCKET_NAME_H_
