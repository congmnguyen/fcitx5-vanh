// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Quang Minh <minhnbnt@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! fcitx5-lotus-server: the privileged helper of the fcitx5-lotus (Vanh) addon, a Rust port of
//! the former C++ server/.
//!
//! It runs as the uinput proxy user (rw ACL on /dev/uinput, group `input`) with ambient
//! CAP_SYS_NICE (priority boost) and CAP_SYS_PTRACE (reading `/proc/<pid>/exe` of the user's
//! fcitx5), one instance per user (`-u <user>`), and:
//!
//! * injects BackSpace / Shift+Left / Delete / Pause bursts through a virtual keyboard on request
//!   of the addon (abstract socket `lotussocket-<user>-kbSocket`), refusing a Shift+Left selection
//!   while a physical Shift is held;
//! * tells the addon about mouse clicks (abstract socket `lotussocket-<user>-mouseSocket`, one
//!   byte `C` per button press), so it can reset its word buffer.
//!
//! Module map: [`names`] (every external name), [`protocol`] (wire format), [`planner`] (the
//! injection state machine), [`shift`] (physical Shift tracking), [`auth`] (peer checks),
//! [`uinput`] (virtual keyboard), [`server`] (event loop). `unsafe` code is confined to
//! [`sys`], [`libinput`] and [`log`].

pub mod auth;
pub mod libinput;
pub mod log;
pub mod names;
pub mod planner;
pub mod protocol;
pub mod server;
pub mod shift;
pub mod sys;
pub mod uinput;
