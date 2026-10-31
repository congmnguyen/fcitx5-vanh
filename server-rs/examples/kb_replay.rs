// SPDX-FileCopyrightText: 2026 LotusInputMethod contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Test helper of the C++/Rust wire test (test/server-wire-compat.cpp), not installed.
//!
//! `kb_replay <messages>` opens the server's two sockets for the current user (honouring
//! VANH_SOCKET_DIR and VANH_SOCKET_NAMESPACE) with the server's own socket directory, stale socket,
//! authorization, decoding and planning code, prints `ready`, answers each authorized mouse
//! client with one `C` byte,
//! replays `<messages>` keyboard requests into a recorder (physical Shift released, paced keys
//! drained before the next non-Cancel request) and prints the key transcript. No uinput, no libinput. Exit 0 on success,
//! 2 on timeout or error.

use lotus_server::auth::authorize_peer;
use lotus_server::planner::{Planner, Recorder};
use lotus_server::protocol::{self, KbOp, KB_MSG_SIZE};
use lotus_server::server::{current_username, socket_namespace};
use lotus_server::shift::ShiftState;
use lotus_server::sys::{self, PollFd, POLLHUP, POLLIN};
use lotus_server::{log, sockets};
use std::io::Write;
use std::os::fd::{AsFd, AsRawFd, OwnedFd};
use std::time::{Duration, Instant};

fn main() {
    log::use_stderr();
    let args: Vec<std::ffi::OsString> = std::env::args_os().collect();
    if args.len() != 2 {
        eprintln!("usage: kb_replay <messages>");
        std::process::exit(2);
    }
    let wanted: usize = args[1].to_str().and_then(|text| text.parse().ok()).unwrap_or(0);
    std::process::exit(match replay(wanted) {
        Some(transcript) => {
            print!("{transcript}");
            0
        }
        None => 2,
    });
}

fn replay(wanted: usize) -> Option<String> {
    let namespace = socket_namespace(&current_username());
    let server_sockets = sockets::open_server_sockets(&sockets::base_dir(), &namespace, sys::effective_uid())?;
    let (kb_server, mouse_server) = (&server_sockets.kb.fd, &server_sockets.mouse.fd);
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
                if authorize_peer(candidate.as_fd(), uid, "keyboard socket").is_some() {
                    kb_client = Some(candidate);
                    continue;
                }
            }
        }
        if fds[1].revents & POLLIN != 0 {
            if let Ok(candidate) = sys::accept_nonblocking(mouse_server.as_fd()) {
                if authorize_peer(candidate.as_fd(), uid, "mouse socket").is_some() {
                    // A client that already hung up (the test's refused connection) is dropped,
                    // as the server does.
                    if sys::send_nonblocking(candidate.as_fd(), b"C").is_ok() {
                        mouse_clients.push(candidate);
                    }
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
