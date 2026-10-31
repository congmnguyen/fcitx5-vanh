/*
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file lotus-monitor.h
 * @brief Input monitoring and timing utilities for fcitx5-vanh.
 */

#ifndef _FCITX5_LOTUS_MONITOR_H_
#define _FCITX5_LOTUS_MONITOR_H_

#include <string>

bool authenticateMouseSocketPeer(int sock, std::string& out_exe_path);
#endif // _FCITX5_LOTUS_MONITOR_H_
