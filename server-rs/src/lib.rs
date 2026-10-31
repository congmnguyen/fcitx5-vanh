// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Quang Minh <minhnbnt@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! fcitx5-vanh-server: the privileged helper of the fcitx5-vanh addon, a Rust port of
//! the former C++ server/.
//!
//! It runs as the uinput proxy user (rw ACL on /dev/uinput, group `input`), one instance per
//! user (`-u <user>`). It starts with ambient CAP_SYS_NICE for its priority boost and CPU pinning
//! and drops every capability right after (see [`server::drop_privileges`]). It:
//!
//! * injects BackSpace / Shift+Left / Delete / Pause bursts through a virtual keyboard on request
//!   of the addon (socket `/run/vanh/<user>/kb.sock`), refusing a Shift+Left selection while a
//!   physical Shift is held;
//! * tells the addon about mouse clicks (socket `/run/vanh/<user>/mouse.sock`, one byte `C` per
//!   button press), so it can reset its word buffer.
//!
//! Only connections whose `SO_PEERCRED` uid is the target user's are served ([`auth`]); the
//! addon in turn only talks to a server whose uid is the proxy user's.
//!
//! Module map: [`names`] (every external name), [`protocol`] (wire format), [`planner`] (the
//! injection state machine), [`shift`] (physical Shift tracking), [`auth`] (peer checks),
//! [`sockets`] (socket directory and files), [`uinput`] (virtual keyboard), [`server`] (event
//! loop). `unsafe` code is confined to [`sys`], [`libinput`] and [`log`].

pub mod auth;
pub mod libinput;
pub mod log;
pub mod names;
pub mod planner;
pub mod protocol;
pub mod server;
pub mod shift;
pub mod sockets;
pub mod sys;
pub mod uinput;
