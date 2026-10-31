/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoàng Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/*
 * Header-only, shared by the addon and the tests. The Rust server builds the same names from
 * server-rs/src/names.rs (keep both in sync).
 */

#ifndef _COMMON_SOCKET_NAME_H_
#define _COMMON_SOCKET_NAME_H_

#include <cstdlib>
#include <string>
#include <string_view>

/**
 * @brief Environment variable that replaces the user segment of the abstract socket names.
 */
inline constexpr const char* LOTUS_SOCKET_NAMESPACE_ENV = "LOTUS_SOCKET_NAMESPACE";

/**
 * @brief Resolves the user segment of the abstract socket name.
 * @param username Default segment, the name of the current user.
 * @return Value of LOTUS_SOCKET_NAMESPACE_ENV when set and non-empty, @p username otherwise.
 */
inline std::string lotusSocketNamespace(std::string_view username) {
    const char* override_value = std::getenv(LOTUS_SOCKET_NAMESPACE_ENV);
    if (override_value != nullptr && *override_value != '\0') {
        return override_value;
    }
    return std::string(username);
}

#endif // _COMMON_SOCKET_NAME_H_
