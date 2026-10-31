// SPDX-FileCopyrightText: 2026 LotusInputMethod contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Test helper of the C++/Rust wire test (test/server-wire-compat.cpp), not installed.
//!
//! `kb_replay <allowed-exe> <messages>` binds the server's two abstract sockets for the current
//! user (honouring VANH_SOCKET_NAMESPACE) with the server's own socket, authorization, decoding
//! and planning code, prints `ready`, answers each authorized mouse client with one `C` byte,
//! replays `<messages>` keyboard requests into a recorder (physical Shift released, paced keys
//! drained before the next non-Cancel request) and prints the key transcript. No uinput, no libinput. Exit 0 on success,
//! 2 on timeout or error.

use lotus_server::auth::{authorize_peer, AllowedExecutable};
use lotus_server::planner::{Planner, Recorder};
use lotus_server::protocol::{self, KbOp, KB_MSG_SIZE};
use lotus_server::server::{bind_server_socket, current_username, listen_server_socket, socket_name, socket_namespace};
use lotus_server::shift::ShiftState;
use lotus_server::sys::{self, AbstractAddr, PollFd, POLLHUP, POLLIN};
use lotus_server::{log, names};
use std::io::Write;
use std::os::fd::{AsFd, AsRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::time::{Duration, Instant};

fn main() {
    log::use_stderr();
    let args: Vec<std::ffi::OsString> = std::env::args_os().collect();
    if args.len() != 3 {
        eprintln!("usage: kb_replay <allowed-exe> <messages>");
        std::process::exit(2);
    }
    let allowed = AllowedExecutable { path: args[1].as_bytes().to_vec(), alt_prefix: Vec::new() };
    let wanted: usize = args[2].to_str().and_then(|text| text.parse().ok()).unwrap_or(0);
    std::process::exit(match replay(&allowed, wanted) {
        Some(transcript) => {
            print!("{transcript}");
            0
        }
        None => 2,
    });
}

fn replay(allowed: &AllowedExecutable, wanted: usize) -> Option<String> {
    let namespace = socket_namespace(&current_username());
    let kb_server = bind_server_socket(&AbstractAddr::new(&socket_name(&namespace, names::KB_SOCKET_SUFFIX)), "keyboard socket")?;
    let mouse_server = bind_server_socket(&AbstractAddr::new(&socket_name(&namespace, names::MOUSE_SOCKET_SUFFIX)), "mouse socket")?;
    if !listen_server_socket(&kb_server, "keyboard socket") || !listen_server_socket(&mouse_server, "mouse socket") {
        return None;
    }
    println!("ready");
    std::io::stdout().flush().ok()?;

    let uid = sys::current_uid();
    let deadline = Instant::now() + Duration::from_secs(10);
    let mut planner = Planner::default();
    let mut out = Recorder::default();
    let mut kb_client: Option<OwnedFd> = None;
    let mut mouse_clients: Vec<OwnedFd> = Vec::new();
    let mut received = 0;

    while received < wanted {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            eprintln!("kb_replay: timed out after {received} of {wanted} messages");
            return None;
        }
        let mut fds = [
            PollFd { fd: kb_server.as_raw_fd(), events: POLLIN, revents: 0 },
            PollFd { fd: mouse_server.as_raw_fd(), events: POLLIN, revents: 0 },
            PollFd { fd: kb_client.as_ref().map_or(-1, |fd| fd.as_raw_fd()), events: POLLIN, revents: 0 },
        ];
        sys::poll_fds(&mut fds, i32::try_from(left.as_millis()).unwrap_or(i32::MAX)).ok()?;
        if fds[0].revents & POLLIN != 0 {
            if let Ok(candidate) = sys::accept_nonblocking(kb_server.as_fd()) {
                if authorize_peer(candidate.as_fd(), uid, "keyboard socket", allowed).is_some() {
                    kb_client = Some(candidate);
                    continue;
                }
            }
        }
        if fds[1].revents & POLLIN != 0 {
            if let Ok(candidate) = sys::accept_nonblocking(mouse_server.as_fd()) {
                if authorize_peer(candidate.as_fd(), uid, "mouse socket", allowed).is_some() {
                    sys::send_nonblocking(candidate.as_fd(), b"C").ok()?;
                    mouse_clients.push(candidate);
                }
            }
        }
        if fds[2].revents & (POLLIN | POLLHUP) != 0 {
            let client = kb_client.as_ref()?;
            let mut buf = [0u8; KB_MSG_SIZE];
            match sys::recv_packet(client.as_fd(), &mut buf) {
                Ok(0) | Err(_) => {
                    planner.disconnected(&mut out);
                    kb_client = None;
                }
                Ok(len) => {
                    received += 1;
                    match protocol::decode(&buf, len) {
                        None => out.events.push(lotus_server::planner::Event::Sleep(u32::MAX)),
                        Some(msg) => {
                            // Let the previous burst finish first, except for a Cancel, which
                            // is meant to cut it short.
                            if msg.op != KbOp::Cancel {
                                drain(&mut planner, &mut out);
                            }
                            planner.handle(&msg, ShiftState::Released, &mut out);
                        }
                    }
                }
            }
        }
    }
    drain(&mut planner, &mut out);
    Some(out.transcript())
}

fn drain(planner: &mut Planner, out: &mut Recorder) {
    while planner.has_pending() {
        planner.tick(out);
    }
}
