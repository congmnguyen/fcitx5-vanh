// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Quang Minh <minhnbnt@gmail.com>
// SPDX-FileCopyrightText: 2026 LotusInputMethod contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Setup and event loop (port of `main()` of server/lotus-server.cpp and of
//! server/lotus-server-socket.cpp).
//!
//! Setup order: resolve the target user, raise the priority and pin the CPUs (the only use of
//! CAP_SYS_NICE), drop every capability and set no_new_privs / non-dumpable, then open uinput, the
//! sockets and libinput with the plain rights of the proxy user (ACL on /dev/uinput, group
//! `input`, owner of `/run/vanh`).
//!
//! Exit codes: 0 after SIGTERM/SIGINT, 1 when setup fails (unknown user, privilege drop, uinput,
//! sockets, libinput) or the loop breaks (libinput dispatch or poll failure).

use crate::auth::authorize_peer;
use crate::libinput::{EventKind, Libinput};
use crate::log::{self, Level};
use crate::names;
use crate::planner::{Output, Planner};
use crate::protocol::{self, KB_MSG_SIZE};
use crate::shift::{EvdevKeyboard, ShiftTracker};
use crate::sockets;
use crate::sys::{self, PollFd, POLLERR, POLLHUP, POLLIN};
use crate::uinput::{self, UinputDevice};
use std::ffi::OsString;
use std::os::fd::{AsFd, AsRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::time::{Duration, Instant};

/// CPUs the server pins itself to (the performance cores of common hybrid laptops).
const PINNED_CPUS: [usize; 4] = [0, 1, 2, 3];
/// Nice value of the server.
const PRIORITY: i32 = -10;

/// The user named by `-u <user>` (exactly three arguments), the current user otherwise.
pub fn target_user(args: &[OsString]) -> Vec<u8> {
    if args.len() == 3 && args[1] == names::USER_FLAG {
        args[2].as_bytes().to_vec()
    } else {
        current_username()
    }
}

/// Login name of the real uid, `unknown` when it has none.
pub fn current_username() -> Vec<u8> {
    sys::user_name_of(sys::current_uid()).unwrap_or_else(|| b"unknown".to_vec())
}

/// User segment of the socket paths: `VANH_SOCKET_NAMESPACE` when set and non-empty, the
/// user name otherwise (common/common-socket-name.h).
pub fn socket_namespace(username: &[u8]) -> Vec<u8> {
    match std::env::var_os(names::SOCKET_NAMESPACE_ENV) {
        Some(value) if !value.is_empty() => value.as_bytes().to_vec(),
        _ => username.to_vec(),
    }
}

/// Drops every capability (CAP_SYS_NICE was only needed for the two calls above), forbids gaining
/// privileges through execve() and makes the process non-dumpable (no core file, no ptrace by
/// other processes of the proxy user). Failing to drop a capability is fatal.
pub fn drop_privileges() -> bool {
    if let Err(error) = sys::drop_all_capabilities() {
        log::error(&format!("Failed to drop capabilities: {}", sys::error_text(&error)));
        return false;
    }
    if let Err(error) = sys::set_no_new_privs() {
        log::error(&format!("Failed to set no_new_privs: {}", sys::error_text(&error)));
        return false;
    }
    if let Err(error) = sys::set_dumpable(false) {
        log::warn(&format!("Failed to make the process non-dumpable: {}", sys::error_text(&error)));
    }
    match sys::capabilities() {
        Ok(caps) if caps == sys::CapSets::default() => {
            log::info("Dropped all capabilities");
            true
        }
        Ok(caps) => {
            log::error(&format!(
                "Capabilities left after the drop: effective {:#x}, permitted {:#x}",
                caps.effective, caps.permitted
            ));
            false
        }
        Err(error) => {
            let text = sys::error_text(&error);
            log::error(&format!("Failed to read the capabilities back: {text}"));
            false
        }
    }
}

/// Creates the virtual keyboard and waits (up to 1 s) for udev to make its node readable.
fn create_uinput_device() -> Option<UinputDevice> {
    let device = match UinputDevice::create(&uinput::lotus_keyboard()) {
        Ok(device) => device,
        Err(error) => {
            log::error(&format!("Failed to create the uinput device: {}", sys::error_text(&error)));
            return None;
        }
    };
    match device.devnode() {
        Some(node) => {
            let mut waited = 0;
            while waited < 1000 && !sys::is_readable(node) {
                std::thread::sleep(Duration::from_millis(10));
                waited += 10;
            }
            log::info(&format!("Uinput device ready: {}", node.display()));
        }
        None => {
            std::thread::sleep(Duration::from_millis(1000));
            log::info("Uinput device ready (device node unknown)");
        }
    }
    Some(device)
}

/// The plan's effects on the real device.
struct DeviceOutput<'a> {
    device: &'a mut UinputDevice,
}

impl Output for DeviceOutput<'_> {
    fn tap(&mut self, code: u16) {
        self.device.tap(code);
    }
    fn key(&mut self, code: u16, pressed: bool) {
        self.device.key(code, pressed);
    }
    fn sleep_ms(&mut self, ms: u32) {
        std::thread::sleep(Duration::from_millis(u64::from(ms)));
    }
    fn log(&mut self, level: Level, message: &str) {
        log::log(level, message);
    }
}

fn devnode_of(sysname: Option<String>) -> String {
    sysname.map(|name| format!("{}{name}", names::INPUT_DEV_DIR)).unwrap_or_default()
}

/// Milliseconds until `due`, rounded up so the tick never fires early.
fn ms_until(due: Instant) -> i32 {
    let left = due.saturating_duration_since(Instant::now());
    let ms = left.as_micros().div_ceil(1000);
    i32::try_from(ms).unwrap_or(i32::MAX)
}

// Poll slots: keyboard server, libinput, mouse server, keyboard client, signal wake-up pipe.
const KB_SERVER: usize = 0;
const MOUSE_SERVER: usize = 2;
const KB_CLIENT: usize = 3;

/// Runs the server; returns the process exit code.
pub fn run(args: &[OsString]) -> i32 {
    let user = target_user(args);
    let user_text = String::from_utf8_lossy(&user).into_owned();
    log::info(&format!("Target user: {user_text}"));

    let Some(expected_uid) = sys::uid_of_user(&user) else {
        log::error(&format!("Failed to find UID for target user: {user_text}"));
        return 1;
    };

    if sys::set_process_priority(PRIORITY).is_err() {
        log::error("Failed to boost process priority");
    }
    if sys::pin_to_cpus(&PINNED_CPUS).is_err() {
        log::error("Failed to pin process to core");
    }
    if !drop_privileges() {
        return 1;
    }

    let namespace = socket_namespace(&user);

    let Some(mut device) = create_uinput_device() else {
        log::error("Failed to initialize uinput device");
        return 1;
    };
    let mut tracker = ShiftTracker::<EvdevKeyboard>::default();

    let base = sockets::base_dir();
    let Some(server_sockets) =
        sockets::open_server_sockets(&base, &namespace, sys::effective_uid())
    else {
        return 1;
    };
    log::info(&format!(
        "Listening on {} and {}",
        server_sockets.kb.path.display(),
        server_sockets.mouse.path.display()
    ));
    let kb_server = &server_sockets.kb.fd;
    let mouse_server = &server_sockets.mouse.fd;

    let Some(mut input) = Libinput::new(names::SEAT) else {
        log::error("Failed to create libinput/udev context");
        return 1;
    };

    let wake = match sys::install_termination_handlers() {
        Ok(fd) => Some(fd),
        Err(error) => {
            let text = sys::error_text(&error);
            log::error(&format!("Failed to install the signal handlers: {text}"));
            None
        }
    };

    let mut planner = Planner::default();
    let mut out = DeviceOutput { device: &mut device };
    let mut kb_client: Option<OwnedFd> = None;
    let mut mouse_client: Option<OwnedFd> = None;
    let mut next_tick: Option<Instant> = None;
    let mut exit_code = 0;

    let slot = |fd: i32| PollFd { fd, events: POLLIN, revents: 0 };
    let mut fds = [
        slot(kb_server.as_raw_fd()),
        slot(input.fd()),
        slot(mouse_server.as_raw_fd()),
        slot(-1),
        slot(wake.as_ref().map_or(-1, |fd| fd.as_raw_fd())),
    ];

    while sys::running() {
        // Paced keys: one per interval, measured from the previous key or request. Unlike the
        // C++ loop, other wake-ups (mouse motion, typing) do not restart the wait, so a moving
        // mouse can no longer stall a burst.
        let timeout = match planner.tick_interval() {
            None => {
                next_tick = None;
                -1
            }
            Some(interval) => ms_until(*next_tick.get_or_insert_with(|| {
                Instant::now() + Duration::from_millis(u64::from(interval))
            })),
        };
        fds[KB_CLIENT].fd = kb_client.as_ref().map_or(-1, |fd| fd.as_raw_fd());
        for fd in &mut fds {
            fd.revents = 0;
        }

        if let Err(error) = sys::poll_fds(&mut fds, timeout) {
            if error.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            let text = sys::error_text(&error);
            log::error(&format!("Failed to poll the server descriptors: {text}"));
            exit_code = 1;
            break;
        }

        if next_tick.is_some_and(|due| Instant::now() >= due) {
            planner.tick(&mut out);
            next_tick = None;
        }

        if let Err(rc) = input.dispatch() {
            log::error(&format!("Failed to dispatch libinput events: {}", sys::strerror_text(-rc)));
            exit_code = 1;
            break;
        }

        if fds[KB_SERVER].revents & POLLIN != 0 {
            if let Ok(candidate) = sys::accept_nonblocking(kb_server.as_fd()) {
                let peer = authorize_peer(candidate.as_fd(), expected_uid, "keyboard socket");
                if let Some(pid) = peer {
                    log::info(&format!("Fcitx5 connected to keyboard socket (PID: {pid})"));
                    kb_client = Some(candidate);
                    // The revents of the client slot belong to the replaced descriptor.
                    fds[KB_CLIENT].revents = 0;
                }
            }
        }

        let kb_ready = fds[KB_CLIENT].revents & (POLLIN | POLLHUP | POLLERR) != 0;
        if let Some(client) = kb_client.as_ref().filter(|_| kb_ready) {
            let mut buf = [0u8; KB_MSG_SIZE];
            match sys::recv_packet(client.as_fd(), &mut buf) {
                Ok(0) | Err(_) => {
                    log::warn("Keyboard client disconnected or connection error");
                    planner.disconnected(&mut out);
                    kb_client = None;
                }
                Ok(len) => match protocol::decode(&buf, len) {
                    None => log::warn(&format!("Malformed keyboard message ({len} bytes)")),
                    Some(msg) => {
                        planner.handle(&msg, tracker.shift_state(), &mut out);
                        next_tick = None;
                    }
                },
            }
        }

        if fds[MOUSE_SERVER].revents & POLLIN != 0 {
            if let Ok(candidate) = sys::accept_nonblocking(mouse_server.as_fd()) {
                let peer = authorize_peer(candidate.as_fd(), expected_uid, "mouse socket");
                if peer.is_some() {
                    log::info("New mouse flag client connected");
                    mouse_client = Some(candidate);
                }
            }
        }

        // libinput_dispatch() may have consumed the fd in an iteration where its revents were 0,
        // so always drain the queue.
        while let Some(event) = input.next_event() {
            match event.kind() {
                EventKind::PointerButton { pressed: true } => {
                    let sent = mouse_client
                        .as_ref()
                        .map(|client| sys::send_nonblocking(client.as_fd(), b"C"));
                    if matches!(sent, Some(Err(_) | Ok(0))) {
                        log::warn("Failed to send to mouse flag client, closing connection");
                        mouse_client = None;
                    }
                }
                EventKind::DeviceAdded => {
                    let device = event.device();
                    let name = device.name();
                    log::info(&format!("Device added: {}", name.as_deref().unwrap_or("")));
                    if device.is_keyboard() && name.as_deref() != Some(names::UINPUT_NAME) {
                        tracker.track(&devnode_of(device.sysname()), EvdevKeyboard::probe);
                    }
                    device.enable_tapping();
                }
                EventKind::DeviceRemoved => tracker.forget(&devnode_of(event.device().sysname())),
                EventKind::PointerButton { pressed: false } | EventKind::Other => {}
            }
        }
    }

    planner.shutdown(&mut out);
    log::info("Terminating server...");
    exit_code
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::log::capture;

    fn os(args: &[&str]) -> Vec<OsString> {
        args.iter().map(OsString::from).collect()
    }

    #[test]
    fn cli_matches_the_cxx_server() {
        assert_eq!(target_user(&os(&["fcitx5-vanh-server", "-u", "alice"])), b"alice");
        let current = current_username();
        assert_eq!(target_user(&os(&["fcitx5-vanh-server"])), current);
        assert_eq!(target_user(&os(&["fcitx5-vanh-server", "-u"])), current, "flag without a user");
        assert_eq!(target_user(&os(&["fcitx5-vanh-server", "--user", "alice"])), current);
        assert_eq!(target_user(&os(&["fcitx5-vanh-server", "-u", "alice", "extra"])), current);
    }

    #[test]
    fn privileges_are_dropped() {
        let _guard =
            sys::DUMPABLE_TEST_LOCK.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        // Per thread (capabilities, no_new_privs): keep the other tests' threads untouched.
        std::thread::spawn(|| {
            capture::take();
            assert!(drop_privileges());
            assert_eq!(capture::take().last().unwrap().1, "Dropped all capabilities");
            assert_eq!(sys::capabilities().unwrap(), sys::CapSets::default());
            assert!(sys::no_new_privs().unwrap());
        })
        .join()
        .unwrap();
        // Process wide; restored for the rest of the test run.
        sys::set_dumpable(true).unwrap();
    }

    #[test]
    fn deadline_rounding() {
        assert_eq!(ms_until(Instant::now() - Duration::from_millis(5)), 0);
        let soon = ms_until(Instant::now() + Duration::from_micros(1500));
        assert!((1..=2).contains(&soon), "{soon}");
    }

    #[test]
    fn devnodes() {
        assert_eq!(devnode_of(Some("event7".to_owned())), "/dev/input/event7");
        assert_eq!(devnode_of(None), "");
    }
}
