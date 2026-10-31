// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Quang Minh <minhnbnt@gmail.com>
// SPDX-FileCopyrightText: 2026 LotusInputMethod contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Setup and event loop (port of `main()` of server/lotus-server.cpp and of
//! server/lotus-server-socket.cpp, server/lotus-abstract-socket.cpp).
//!
//! Exit codes: 0 after SIGTERM/SIGINT, 1 when setup fails (unknown user, uinput, sockets,
//! libinput) or the loop breaks (libinput dispatch or poll failure).

use crate::auth::{authorize_peer, AllowedExecutable};
use crate::libinput::{EventKind, Libinput};
use crate::log::{self, Level};
use crate::names;
use crate::planner::{Output, Planner};
use crate::protocol::{self, KB_MSG_SIZE};
use crate::shift::{EvdevKeyboard, ShiftTracker};
use crate::sys::{self, AbstractAddr, PollFd, POLLERR, POLLHUP, POLLIN};
use crate::uinput::{self, UinputDevice};
use std::ffi::OsString;
use std::os::fd::{AsFd, AsRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::time::{Duration, Instant};

/// `listen()` backlog of both sockets.
const LISTEN_BACKLOG: i32 = 5;
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

/// User segment of the socket names: `VANH_SOCKET_NAMESPACE` when set and non-empty, the
/// user name otherwise (common/common-socket-name.h).
pub fn socket_namespace(username: &[u8]) -> Vec<u8> {
    match std::env::var_os(names::SOCKET_NAMESPACE_ENV) {
        Some(value) if !value.is_empty() => value.as_bytes().to_vec(),
        _ => username.to_vec(),
    }
}

/// `vanhsocket-<namespace>-<suffix>`, the name the addon's `buildSocketPath()` builds.
pub fn socket_name(namespace: &[u8], suffix: &str) -> Vec<u8> {
    let mut name = names::SOCKET_PREFIX.as_bytes().to_vec();
    name.extend_from_slice(namespace);
    name.push(b'-');
    name.extend_from_slice(suffix.as_bytes());
    name
}

/// Creates and binds a non-blocking server socket, logging the reason on failure.
pub fn bind_server_socket(addr: &AbstractAddr, label: &str) -> Option<OwnedFd> {
    let bound = sys::seqpacket_socket(true).and_then(|fd| sys::bind_abstract(fd.as_fd(), addr).map(|()| fd));
    match bound {
        Ok(fd) => Some(fd),
        Err(error) => {
            log::error(&format!("Failed to bind {label}: {}", sys::error_text(&error)));
            None
        }
    }
}

/// Marks a bound socket as listening, logging the reason on failure.
pub fn listen_server_socket(fd: &OwnedFd, label: &str) -> bool {
    match sys::listen_socket(fd.as_fd(), LISTEN_BACKLOG) {
        Ok(()) => true,
        Err(error) => {
            log::error(&format!("Failed to listen on {label}: {}", sys::error_text(&error)));
            false
        }
    }
}

fn boost_process_priority() {
    if sys::set_process_priority(PRIORITY).is_err() {
        log::error("Failed to boost process priority");
    }
}

fn pin_to_pcore() {
    if sys::pin_to_cpus(&PINNED_CPUS).is_err() {
        log::error("Failed to pin process to core");
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

    boost_process_priority();
    pin_to_pcore();

    let namespace = socket_namespace(&user);
    let kb_addr = AbstractAddr::new(&socket_name(&namespace, names::KB_SOCKET_SUFFIX));
    let mouse_addr = AbstractAddr::new(&socket_name(&namespace, names::MOUSE_SOCKET_SUFFIX));

    let Some(mut device) = create_uinput_device() else {
        log::error("Failed to initialize uinput device");
        return 1;
    };
    let mut tracker = ShiftTracker::<EvdevKeyboard>::default();

    let Some(kb_server) = bind_server_socket(&kb_addr, "keyboard socket") else {
        return 1;
    };
    let Some(mouse_server) = bind_server_socket(&mouse_addr, "mouse socket") else {
        return 1;
    };
    if !listen_server_socket(&kb_server, "keyboard socket") || !listen_server_socket(&mouse_server, "mouse socket") {
        return 1;
    }

    let Some(mut input) = Libinput::new(names::SEAT) else {
        log::error("Failed to create libinput/udev context");
        return 1;
    };

    let wake = match sys::install_termination_handlers() {
        Ok(fd) => Some(fd),
        Err(error) => {
            log::error(&format!("Failed to install the signal handlers: {}", sys::error_text(&error)));
            None
        }
    };

    let allowed = AllowedExecutable::configured();
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
            Some(interval) => ms_until(*next_tick.get_or_insert_with(|| Instant::now() + Duration::from_millis(u64::from(interval)))),
        };
        fds[KB_CLIENT].fd = kb_client.as_ref().map_or(-1, |fd| fd.as_raw_fd());
        for fd in &mut fds {
            fd.revents = 0;
        }

        if let Err(error) = sys::poll_fds(&mut fds, timeout) {
            if error.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            log::error(&format!("Failed to poll the server descriptors: {}", sys::error_text(&error)));
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

        let mut kb_client_replaced = false;
        if fds[KB_SERVER].revents & POLLIN != 0 {
            if let Ok(candidate) = sys::accept_nonblocking(kb_server.as_fd()) {
                if let Some(pid) = authorize_peer(candidate.as_fd(), expected_uid, "keyboard socket", &allowed) {
                    log::info(&format!("Fcitx5 connected to keyboard socket (PID: {pid})"));
                    kb_client = Some(candidate);
                    // The revents of the client slot belong to the replaced descriptor.
                    kb_client_replaced = true;
                }
            }
        }

        if !kb_client_replaced && fds[KB_CLIENT].revents & (POLLIN | POLLHUP | POLLERR) != 0 {
            if let Some(client) = kb_client.as_ref() {
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
                            let shift = tracker.shift_state();
                            planner.handle(&msg, shift, &mut out);
                            next_tick = None;
                        }
                    },
                }
            }
        }

        if fds[MOUSE_SERVER].revents & POLLIN != 0 {
            if let Ok(candidate) = sys::accept_nonblocking(mouse_server.as_fd()) {
                if authorize_peer(candidate.as_fd(), expected_uid, "mouse socket", &allowed).is_some() {
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
                    if let Some(client) = mouse_client.as_ref() {
                        if !matches!(sys::send_nonblocking(client.as_fd(), b"C"), Ok(sent) if sent > 0) {
                            log::warn("Failed to send to mouse flag client, closing connection");
                            mouse_client = None;
                        }
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
    fn socket_names_match_the_addon() {
        assert_eq!(socket_name(b"alice", names::KB_SOCKET_SUFFIX), b"vanhsocket-alice-kbSocket");
        assert_eq!(socket_name(b"alice", names::MOUSE_SOCKET_SUFFIX), b"vanhsocket-alice-mouseSocket");
    }

    /// Port of test/server-socket-init.cpp: a socket that fails bind()/listen() is reported.
    #[test]
    fn server_socket_init() {
        let addr = AbstractAddr::new(format!("vanh-server-socket-init-{}", std::process::id()).as_bytes());
        let first = bind_server_socket(&addr, "test socket").expect("bind fresh address");
        assert!(listen_server_socket(&first, "test socket"), "listen on bound socket");
        capture::take();
        assert!(bind_server_socket(&addr, "test socket").is_none(), "rebind same address fails");
        assert_eq!(capture::take(), vec![(Level::Error, "Failed to bind test socket: Address already in use".to_owned())]);
        let not_a_socket: OwnedFd = std::fs::File::open("/dev/null").unwrap().into();
        assert!(!listen_server_socket(&not_a_socket, "test socket"), "listen on a non-socket fails");
        assert_eq!(capture::take(), vec![(Level::Error, "Failed to listen on test socket: Socket operation on non-socket".to_owned())]);
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
