/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file common-executable-path.h
 * @brief Peer executable path checks of the addon (the Rust server has its own copy,
 *        server-rs/src/auth.rs, tested against the same cases).
 */

#ifndef _COMMON_EXECUTABLE_PATH_H_
#define _COMMON_EXECUTABLE_PATH_H_

#include <string_view>

#include "common-export.h"

/**
 * @brief Checks whether a peer executable may act as the expected program.
 * @param exePath Peer executable path as reported by readlink(/proc/<pid>/exe).
 * @param expectedPath Configured executable path (e.g. LOTUS_FCITX5_EXECUTABLE).
 * @param altPrefix Configured extra prefix (e.g. LOTUS_ALT_EXECUTABLE_PREFIX), may be empty.
 * @return True when the peer path is accepted.
 */
// LOTUS_EXPORT: also read by the headless tests through lotus_test_core.
bool LOTUS_EXPORT isAllowedExecutablePath(std::string_view exePath, std::string_view expectedPath, std::string_view altPrefix);

#endif // _COMMON_EXECUTABLE_PATH_H_
