// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
// SPDX-FileCopyrightText: 2026 LotusInputMethod contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Hand-written libc / Linux kernel interface.
//!
//! The crate has no dependency (offline Debian builds), so the few libc calls, kernel structs,
//! ioctl numbers and per-architecture constants the server needs are declared here. Apart from
//! the libinput/libudev bindings (`libinput.rs`) and `syslog` (`log.rs`), this is the only module
//! with `unsafe` code. Every function exported from it is safe: it takes borrowed descriptors and
//! slices, checks the return value and turns `-1`/`errno` into `io::Error`.
//!
//! Layouts follow the Linux UAPI headers (`<linux/uinput.h>`, `<linux/input.h>`, `<sys/un.h>`,
//! `<bits/socket.h>`) and glibc/musl (`struct passwd`, `cpu_set_t`).

#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_int, c_short, c_uint, c_ulong, c_void, CStr, CString};
use std::io;
use std::mem::{size_of, MaybeUninit};
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};

// Per-architecture constants. Values from the kernel's arch/*/include/uapi/asm headers.

#[cfg(not(target_os = "linux"))]
compile_error!("fcitx5-vanh-server only supports Linux");

#[cfg(any(
    target_arch = "mips",
    target_arch = "mips64",
    target_arch = "mips32r6",
    target_arch = "mips64r6"
))]
mod arch {
    use std::ffi::c_int;
    pub const O_NONBLOCK: c_int = 0x80;
    pub const O_CLOEXEC: c_int = 0o2000000;
    pub const SOL_SOCKET: c_int = 0xffff;
    pub const SO_PEERCRED: c_int = 18;
    pub const IOC_NONE: u32 = 1;
    pub const IOC_READ: u32 = 2;
    pub const IOC_WRITE: u32 = 4;
    pub const IOC_SIZEBITS: u32 = 13;
}

#[cfg(any(target_arch = "powerpc", target_arch = "powerpc64"))]
mod arch {
    use std::ffi::c_int;
    pub const O_NONBLOCK: c_int = 0o4000;
    pub const O_CLOEXEC: c_int = 0o2000000;
    pub const SOL_SOCKET: c_int = 1;
    pub const SO_PEERCRED: c_int = 21;
    pub const IOC_NONE: u32 = 1;
    pub const IOC_READ: u32 = 2;
    pub const IOC_WRITE: u32 = 4;
    pub const IOC_SIZEBITS: u32 = 13;
}

#[cfg(any(target_arch = "sparc", target_arch = "sparc64"))]
mod arch {
    use std::ffi::c_int;
    pub const O_NONBLOCK: c_int = 0x4000;
    pub const O_CLOEXEC: c_int = 0x400000;
    pub const SOL_SOCKET: c_int = 0xffff;
    pub const SO_PEERCRED: c_int = 0x40;
    pub const IOC_NONE: u32 = 1;
    pub const IOC_READ: u32 = 2;
    pub const IOC_WRITE: u32 = 4;
    pub const IOC_SIZEBITS: u32 = 13;
}

// asm-generic: x86, x86_64, arm, aarch64, riscv, loongarch, s390x, ...
#[cfg(not(any(
    target_arch = "mips",
    target_arch = "mips64",
    target_arch = "mips32r6",
    target_arch = "mips64r6",
    target_arch = "powerpc",
    target_arch = "powerpc64",
    target_arch = "sparc",
    target_arch = "sparc64"
)))]
mod arch {
    use std::ffi::c_int;
    pub const O_NONBLOCK: c_int = 0o4000;
    pub const O_CLOEXEC: c_int = 0o2000000;
    pub const SOL_SOCKET: c_int = 1;
    pub const SO_PEERCRED: c_int = 17;
    pub const IOC_NONE: u32 = 0;
    pub const IOC_READ: u32 = 2;
    pub const IOC_WRITE: u32 = 1;
    pub const IOC_SIZEBITS: u32 = 14;
}

pub use arch::O_NONBLOCK;
use arch::{IOC_NONE, IOC_READ, IOC_SIZEBITS, IOC_WRITE, O_CLOEXEC, SOL_SOCKET, SO_PEERCRED};

const AF_UNIX: c_int = 1;
const SOCK_SEQPACKET: c_int = 5;
const SOCK_NONBLOCK: c_int = O_NONBLOCK;
const SOCK_CLOEXEC: c_int = O_CLOEXEC;
const MSG_DONTWAIT: c_int = 0x40;
const MSG_NOSIGNAL: c_int = 0x4000;
const PRIO_PROCESS: c_int = 0;
const R_OK: c_int = 4;
const ERANGE: c_int = 34;
const ENAMETOOLONG: i32 = 36;
const EINVAL: i32 = 22;
const SIGINT: c_int = 2;
const SIGTERM: c_int = 15;
const SIG_ERR: usize = usize::MAX;

pub const POLLIN: c_short = 0x1;
pub const POLLERR: c_short = 0x8;
pub const POLLHUP: c_short = 0x10;

/// Generic `_IOC()` encoding with the architecture's direction bits.
const fn ioc(dir: u32, kind: u8, nr: u32, size: usize) -> c_ulong {
    ((dir << (16 + IOC_SIZEBITS)) | ((size as u32) << 16) | ((kind as u32) << 8) | nr) as c_ulong
}

// Kernel / libc structs.

/// `struct sockaddr_un`.
#[repr(C)]
#[derive(Clone, Copy)]
struct sockaddr_un {
    sun_family: u16,
    sun_path: [u8; SUN_PATH_LEN],
}
const SUN_PATH_LEN: usize = 108;

/// `struct ucred`.
#[repr(C)]
#[derive(Default)]
struct ucred {
    pid: i32,
    uid: u32,
    gid: u32,
}

/// `struct pollfd`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct PollFd {
    pub fd: c_int,
    pub events: c_short,
    pub revents: c_short,
}

/// `struct passwd` (same layout in glibc and musl).
#[repr(C)]
struct passwd {
    pw_name: *mut c_char,
    pw_passwd: *mut c_char,
    pw_uid: u32,
    pw_gid: u32,
    pw_gecos: *mut c_char,
    pw_dir: *mut c_char,
    pw_shell: *mut c_char,
}

/// `struct input_event` as userspace writes it: on every ABI the two time fields are
/// `__kernel_ulong_t` wide (32-bit time64 ABIs included); the kernel ignores them on uinput writes.
#[repr(C)]
struct input_event {
    time: [c_ulong; 2],
    kind: u16,
    code: u16,
    value: i32,
}

/// `struct input_id`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InputId {
    pub bustype: u16,
    pub vendor: u16,
    pub product: u16,
    pub version: u16,
}

/// Length of the uinput device name field (`UINPUT_MAX_NAME_SIZE`).
pub const UINPUT_MAX_NAME_SIZE: usize = 80;

/// `struct uinput_setup`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UinputSetup {
    pub id: InputId,
    pub name: [u8; UINPUT_MAX_NAME_SIZE],
    pub ff_effects_max: u32,
}

/// `struct uinput_user_dev`, the pre-4.5 kernel interface `UI_DEV_SETUP` replaced.
#[repr(C)]
struct uinput_user_dev {
    name: [u8; UINPUT_MAX_NAME_SIZE],
    id: InputId,
    ff_effects_max: u32,
    absmax: [i32; 64],
    absmin: [i32; 64],
    absfuzz: [i32; 64],
    absflat: [i32; 64],
}

const _: () = assert!(size_of::<UinputSetup>() == 92);
const _: () = assert!(size_of::<uinput_user_dev>() == 1116);
const _: () = assert!(size_of::<input_event>() == 2 * size_of::<c_ulong>() + 8);
const _: () = assert!(size_of::<ucred>() == 12);

// ioctl numbers of <linux/uinput.h> and <linux/input.h>.
const UI_DEV_CREATE: c_ulong = ioc(IOC_NONE, b'U', 1, 0);
const UI_DEV_DESTROY: c_ulong = ioc(IOC_NONE, b'U', 2, 0);
const UI_DEV_SETUP: c_ulong = ioc(IOC_WRITE, b'U', 3, size_of::<UinputSetup>());
const UI_SET_EVBIT: c_ulong = ioc(IOC_WRITE, b'U', 100, size_of::<c_int>());
const UI_SET_KEYBIT: c_ulong = ioc(IOC_WRITE, b'U', 101, size_of::<c_int>());
const fn ui_get_sysname_request(len: usize) -> c_ulong {
    ioc(IOC_READ, b'U', 44, len)
}
const fn eviocgbit(ev: u32, len: usize) -> c_ulong {
    ioc(IOC_READ, b'E', 0x20 + ev, len)
}
const fn eviocgkey(len: usize) -> c_ulong {
    ioc(IOC_READ, b'E', 0x18, len)
}

// `cpu_set_t`: 1024 bits stored as unsigned longs.
const CPU_SETSIZE: usize = 1024;
const CPU_WORD_BITS: usize = c_ulong::BITS as usize;
type CpuSet = [c_ulong; CPU_SETSIZE / CPU_WORD_BITS];

extern "C" {
    fn socket(domain: c_int, kind: c_int, protocol: c_int) -> c_int;
    fn bind(fd: c_int, addr: *const sockaddr_un, len: u32) -> c_int;
    fn connect(fd: c_int, addr: *const sockaddr_un, len: u32) -> c_int;
    fn listen(fd: c_int, backlog: c_int) -> c_int;
    fn accept4(fd: c_int, addr: *mut c_void, len: *mut u32, flags: c_int) -> c_int;
    fn recv(fd: c_int, buf: *mut c_void, len: usize, flags: c_int) -> isize;
    fn send(fd: c_int, buf: *const c_void, len: usize, flags: c_int) -> isize;
    fn getsockopt(fd: c_int, level: c_int, name: c_int, value: *mut c_void, len: *mut u32)
        -> c_int;
    fn poll(fds: *mut PollFd, nfds: c_ulong, timeout: c_int) -> c_int;
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn write(fd: c_int, buf: *const c_void, len: usize) -> isize;
    fn pipe2(fds: *mut c_int, flags: c_int) -> c_int;
    fn signal(signum: c_int, handler: usize) -> usize;
    fn setpriority(which: c_int, who: c_uint, prio: c_int) -> c_int;
    fn sched_setaffinity(pid: c_int, size: usize, mask: *const CpuSet) -> c_int;
    fn getuid() -> u32;
    fn geteuid() -> u32;
    fn capget(header: *mut CapHeader, data: *mut CapData) -> c_int;
    fn capset(header: *mut CapHeader, data: *const CapData) -> c_int;
    fn prctl(option: c_int, ...) -> c_int;
    fn getpwuid_r(
        uid: u32,
        pwd: *mut passwd,
        buf: *mut c_char,
        len: usize,
        result: *mut *mut passwd,
    ) -> c_int;
    fn getpwnam_r(
        name: *const c_char,
        pwd: *mut passwd,
        buf: *mut c_char,
        len: usize,
        result: *mut *mut passwd,
    ) -> c_int;
    fn access(path: *const c_char, mode: c_int) -> c_int;
    fn strerror(errnum: c_int) -> *const c_char;
}

fn cvt(ret: c_int) -> io::Result<c_int> {
    if ret < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(ret)
    }
}

fn cvt_size(ret: isize) -> io::Result<usize> {
    if ret < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(ret as usize)
    }
}

/// `strerror()` text of an errno value, as the C++ server logged it.
pub fn strerror_text(errno: i32) -> String {
    // SAFETY: strerror() returns a pointer to a NUL-terminated static or thread-local string; the
    // server is single-threaded and copies it immediately.
    unsafe {
        let text = strerror(errno);
        if text.is_null() {
            return format!("Unknown error {errno}");
        }
        CStr::from_ptr(text).to_string_lossy().into_owned()
    }
}

/// `strerror()` text of an I/O error (falls back to Rust's description for non-OS errors).
pub fn error_text(error: &io::Error) -> String {
    match error.raw_os_error() {
        Some(errno) => strerror_text(errno),
        None => error.to_string(),
    }
}

// Filesystem AF_UNIX sockets.

/// Longest socket path: `sun_path` minus the terminating NUL.
pub const MAX_SOCKET_PATH: usize = SUN_PATH_LEN - 1;

/// A filesystem AF_UNIX address together with the length `bind()`/`connect()` need.
#[derive(Clone, Copy)]
pub struct UnixAddr {
    addr: sockaddr_un,
    len: u32,
}

impl UnixAddr {
    /// Builds the address of `path`. Fails with `ENAMETOOLONG` for a path longer than
    /// [`MAX_SOCKET_PATH`] (never truncated: a truncated path would name another file) and with
    /// `EINVAL` for an empty path or one with a NUL byte.
    pub fn new(path: &std::path::Path) -> io::Result<Self> {
        use std::os::unix::ffi::OsStrExt;
        let bytes = path.as_os_str().as_bytes();
        if bytes.is_empty() || bytes.contains(&0) {
            return Err(io::Error::from_raw_os_error(EINVAL));
        }
        if bytes.len() > MAX_SOCKET_PATH {
            return Err(io::Error::from_raw_os_error(ENAMETOOLONG));
        }
        let mut addr = sockaddr_un { sun_family: AF_UNIX as u16, sun_path: [0; SUN_PATH_LEN] };
        addr.sun_path[..bytes.len()].copy_from_slice(bytes);
        let len = (std::mem::offset_of!(sockaddr_un, sun_path) + bytes.len() + 1) as u32;
        Ok(UnixAddr { addr, len })
    }

    /// The path the address designates.
    pub fn path(&self) -> &[u8] {
        let path_len = self.len as usize - std::mem::offset_of!(sockaddr_un, sun_path) - 1;
        &self.addr.sun_path[..path_len]
    }

    /// Length passed to `bind()`/`connect()`.
    pub fn socklen(&self) -> u32 {
        self.len
    }
}

/// Creates a close-on-exec `SOCK_SEQPACKET` AF_UNIX socket.
pub fn seqpacket_socket(nonblocking: bool) -> io::Result<OwnedFd> {
    let flags = SOCK_SEQPACKET | SOCK_CLOEXEC | if nonblocking { SOCK_NONBLOCK } else { 0 };
    // SAFETY: plain syscall; on success the returned descriptor is new and owned by nobody else.
    let fd = cvt(unsafe { socket(AF_UNIX, flags, 0) })?;
    // SAFETY: fd is a fresh, valid descriptor.
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

/// `bind()` to a filesystem address (creates the socket file).
pub fn bind_unix(fd: BorrowedFd<'_>, addr: &UnixAddr) -> io::Result<()> {
    // SAFETY: addr points to a valid sockaddr_un of at least `len` bytes.
    cvt(unsafe { bind(fd.as_raw_fd(), &addr.addr, addr.len) }).map(drop)
}

/// `connect()` to a filesystem address (used by the tests).
pub fn connect_unix(fd: BorrowedFd<'_>, addr: &UnixAddr) -> io::Result<()> {
    // SAFETY: as in bind_unix.
    cvt(unsafe { connect(fd.as_raw_fd(), &addr.addr, addr.len) }).map(drop)
}

/// `listen()`.
pub fn listen_socket(fd: BorrowedFd<'_>, backlog: i32) -> io::Result<()> {
    // SAFETY: plain syscall on a borrowed descriptor.
    cvt(unsafe { listen(fd.as_raw_fd(), backlog) }).map(drop)
}

/// `accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)`.
pub fn accept_nonblocking(fd: BorrowedFd<'_>) -> io::Result<OwnedFd> {
    // SAFETY: null address/length pointers are allowed by accept4().
    let client = cvt(unsafe {
        accept4(
            fd.as_raw_fd(),
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            SOCK_NONBLOCK | SOCK_CLOEXEC,
        )
    })?;
    // SAFETY: client is a fresh, valid descriptor.
    Ok(unsafe { OwnedFd::from_raw_fd(client) })
}

/// `recv(flags = 0)`; `Ok(0)` means the peer closed the connection.
pub fn recv_packet(fd: BorrowedFd<'_>, buf: &mut [u8]) -> io::Result<usize> {
    // SAFETY: buf is a valid writable buffer of buf.len() bytes.
    cvt_size(unsafe { recv(fd.as_raw_fd(), buf.as_mut_ptr().cast(), buf.len(), 0) })
}

/// `send(MSG_NOSIGNAL | MSG_DONTWAIT)`.
pub fn send_nonblocking(fd: BorrowedFd<'_>, buf: &[u8]) -> io::Result<usize> {
    // SAFETY: buf is a valid readable buffer of buf.len() bytes.
    let flags = MSG_NOSIGNAL | MSG_DONTWAIT;
    cvt_size(unsafe { send(fd.as_raw_fd(), buf.as_ptr().cast(), buf.len(), flags) })
}

/// `send(MSG_NOSIGNAL)`, blocking (used by the tests and the replay helper).
pub fn send_packet(fd: BorrowedFd<'_>, buf: &[u8]) -> io::Result<usize> {
    // SAFETY: buf is a valid readable buffer of buf.len() bytes.
    cvt_size(unsafe { send(fd.as_raw_fd(), buf.as_ptr().cast(), buf.len(), MSG_NOSIGNAL) })
}

/// Credentials of the peer of a connected AF_UNIX socket (`SO_PEERCRED`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PeerCred {
    pub pid: i32,
    pub uid: u32,
    pub gid: u32,
}

/// Reads `SO_PEERCRED`.
pub fn peer_credentials(fd: BorrowedFd<'_>) -> io::Result<PeerCred> {
    let mut cred = ucred::default();
    let mut len = size_of::<ucred>() as u32;
    // SAFETY: cred is a writable ucred and len holds its size.
    cvt(unsafe {
        let value = (&mut cred as *mut ucred).cast();
        getsockopt(fd.as_raw_fd(), SOL_SOCKET, SO_PEERCRED, value, &mut len)
    })?;
    Ok(PeerCred { pid: cred.pid, uid: cred.uid, gid: cred.gid })
}

/// `poll()`; returns the number of ready descriptors (0 on timeout). `timeout_ms < 0` waits
/// forever.
pub fn poll_fds(fds: &mut [PollFd], timeout_ms: i32) -> io::Result<usize> {
    // SAFETY: fds is a valid array of fds.len() pollfd structs.
    cvt(unsafe { poll(fds.as_mut_ptr(), fds.len() as c_ulong, timeout_ms) })
        .map(|ready| ready as usize)
}

// uinput and evdev ioctls.

/// `UI_SET_EVBIT`.
pub fn ui_set_evbit(fd: BorrowedFd<'_>, kind: u16) -> io::Result<()> {
    // SAFETY: UI_SET_EVBIT takes its argument by value.
    cvt(unsafe { ioctl(fd.as_raw_fd(), UI_SET_EVBIT, c_int::from(kind)) }).map(drop)
}

/// `UI_SET_KEYBIT`.
pub fn ui_set_keybit(fd: BorrowedFd<'_>, code: u16) -> io::Result<()> {
    // SAFETY: UI_SET_KEYBIT takes its argument by value.
    cvt(unsafe { ioctl(fd.as_raw_fd(), UI_SET_KEYBIT, c_int::from(code)) }).map(drop)
}

/// `UI_DEV_SETUP`, falling back to writing `struct uinput_user_dev` on kernels older than 4.5
/// (which answer `EINVAL`), like libevdev does.
pub fn ui_dev_setup(fd: BorrowedFd<'_>, setup: &UinputSetup) -> io::Result<()> {
    // SAFETY: setup is a valid uinput_setup, read by the kernel only.
    match cvt(unsafe { ioctl(fd.as_raw_fd(), UI_DEV_SETUP, setup as *const UinputSetup) }) {
        Ok(_) => Ok(()),
        Err(error) if error.raw_os_error() == Some(EINVAL) => {
            let legacy = uinput_user_dev {
                name: setup.name,
                id: setup.id,
                ff_effects_max: setup.ff_effects_max,
                absmax: [0; 64],
                absmin: [0; 64],
                absfuzz: [0; 64],
                absflat: [0; 64],
            };
            let bytes = (&legacy as *const uinput_user_dev).cast();
            write_all_once(fd, bytes, size_of::<uinput_user_dev>())
        }
        Err(error) => Err(error),
    }
}

/// `UI_DEV_CREATE`.
pub fn ui_dev_create(fd: BorrowedFd<'_>) -> io::Result<()> {
    // SAFETY: UI_DEV_CREATE has no argument.
    cvt(unsafe { ioctl(fd.as_raw_fd(), UI_DEV_CREATE) }).map(drop)
}

/// `UI_DEV_DESTROY`.
pub fn ui_dev_destroy(fd: BorrowedFd<'_>) -> io::Result<()> {
    // SAFETY: UI_DEV_DESTROY has no argument.
    cvt(unsafe { ioctl(fd.as_raw_fd(), UI_DEV_DESTROY) }).map(drop)
}

/// `UI_GET_SYSNAME`: sysfs name of the created device (e.g. `input23`).
pub fn ui_get_sysname(fd: BorrowedFd<'_>) -> io::Result<Vec<u8>> {
    let mut buf = [0u8; 64];
    // SAFETY: the kernel writes at most buf.len() bytes, the size encoded in the request.
    cvt(unsafe { ioctl(fd.as_raw_fd(), ui_get_sysname_request(buf.len()), buf.as_mut_ptr()) })?;
    let end = buf.iter().position(|&byte| byte == 0).unwrap_or(buf.len());
    Ok(buf[..end].to_vec())
}

/// Writes one `struct input_event` (time left zero, the kernel stamps it).
pub fn write_input_event(fd: BorrowedFd<'_>, kind: u16, code: u16, value: i32) -> io::Result<()> {
    let event = input_event { time: [0; 2], kind, code, value };
    write_all_once(fd, (&event as *const input_event).cast(), size_of::<input_event>())
}

fn write_all_once(fd: BorrowedFd<'_>, data: *const c_void, len: usize) -> io::Result<()> {
    // SAFETY: callers pass a pointer to a live value of `len` bytes.
    let written = cvt_size(unsafe { write(fd.as_raw_fd(), data, len) })?;
    if written == len {
        Ok(())
    } else {
        Err(io::Error::from(io::ErrorKind::WriteZero))
    }
}

/// `EVIOCGBIT(ev, len)`: capability bitmap of one event type (`ev = 0`: the event types).
pub fn evdev_get_bits(fd: BorrowedFd<'_>, ev: u16, bits: &mut [u8]) -> io::Result<()> {
    // SAFETY: the kernel writes at most bits.len() bytes, the size encoded in the request.
    let request = eviocgbit(u32::from(ev), bits.len());
    cvt(unsafe { ioctl(fd.as_raw_fd(), request, bits.as_mut_ptr()) }).map(drop)
}

/// `EVIOCGKEY(len)`: bitmap of the keys currently down.
pub fn evdev_get_key_state(fd: BorrowedFd<'_>, bits: &mut [u8]) -> io::Result<()> {
    // SAFETY: the kernel writes at most bits.len() bytes, the size encoded in the request.
    cvt(unsafe { ioctl(fd.as_raw_fd(), eviocgkey(bits.len()), bits.as_mut_ptr()) }).map(drop)
}

// Raw open/close for the libinput interface callbacks.

/// `open(path, flags)`; returns the descriptor or `-errno`, the libinput `open_restricted`
/// contract.
pub fn open_raw(path: &CStr, flags: i32) -> RawFd {
    // SAFETY: path is NUL-terminated; the caller (libinput) takes ownership of the descriptor.
    let fd = unsafe { open(path.as_ptr(), flags) };
    if fd < 0 {
        -io::Error::last_os_error().raw_os_error().unwrap_or(EINVAL)
    } else {
        fd
    }
}

/// `close(fd)` of a descriptor handed out by [`open_raw`] (libinput `close_restricted`).
pub fn close_raw(fd: RawFd) {
    // SAFETY: libinput hands back a descriptor it got from open_raw and no longer uses.
    unsafe {
        close(fd);
    }
}

// Process: users, priority, affinity, file access.

/// Real uid of the process.
pub fn current_uid() -> u32 {
    // SAFETY: getuid() cannot fail.
    unsafe { getuid() }
}

/// Effective uid of the process (the owner of the files it creates).
pub fn effective_uid() -> u32 {
    // SAFETY: geteuid() cannot fail.
    unsafe { geteuid() }
}

/// Runs a `getpw*_r` lookup, growing the buffer on `ERANGE` (the C++ server gave up on long
/// entries with its fixed 1 KiB buffer).
fn lookup_passwd<T>(
    call: impl Fn(*mut passwd, *mut c_char, usize, *mut *mut passwd) -> c_int,
    extract: impl FnOnce(&passwd) -> T,
) -> Option<T> {
    let mut size = 1024usize;
    loop {
        let mut buf = vec![0 as c_char; size];
        let mut pwd = MaybeUninit::<passwd>::zeroed();
        let mut result: *mut passwd = std::ptr::null_mut();
        let rc = call(pwd.as_mut_ptr(), buf.as_mut_ptr(), buf.len(), &mut result);
        if rc == ERANGE && size < (1 << 20) {
            size *= 2;
            continue;
        }
        if rc != 0 || result.is_null() {
            return None;
        }
        // SAFETY: on success result points at pwd, fully initialised, whose strings live in buf
        // (still alive here).
        return Some(extract(unsafe { &*result }));
    }
}

/// Login name of `uid`.
pub fn user_name_of(uid: u32) -> Option<Vec<u8>> {
    lookup_passwd(
        // SAFETY: all pointers are valid for the duration of the call (see lookup_passwd).
        |pwd, buf, len, result| unsafe { getpwuid_r(uid, pwd, buf, len, result) },
        // SAFETY: pw_name is a NUL-terminated string inside the lookup buffer.
        |pwd| unsafe { CStr::from_ptr(pwd.pw_name) }.to_bytes().to_vec(),
    )
}

/// Uid of the user called `name`.
pub fn uid_of_user(name: &[u8]) -> Option<u32> {
    let name = CString::new(name).ok()?;
    lookup_passwd(
        // SAFETY: all pointers are valid for the duration of the call (see lookup_passwd).
        |pwd, buf, len, result| unsafe { getpwnam_r(name.as_ptr(), pwd, buf, len, result) },
        |pwd| pwd.pw_uid,
    )
}

/// `setpriority(PRIO_PROCESS, 0, prio)`.
pub fn set_process_priority(prio: i32) -> io::Result<()> {
    // SAFETY: plain syscall.
    cvt(unsafe { setpriority(PRIO_PROCESS, 0, prio) }).map(drop)
}

/// `sched_setaffinity(0, ...)` to the given CPUs.
pub fn pin_to_cpus(cpus: &[usize]) -> io::Result<()> {
    let mut set: CpuSet = [0; CPU_SETSIZE / CPU_WORD_BITS];
    for &cpu in cpus.iter().filter(|&&cpu| cpu < CPU_SETSIZE) {
        set[cpu / CPU_WORD_BITS] |= 1 << (cpu % CPU_WORD_BITS);
    }
    // SAFETY: set is a valid cpu_set_t of the size passed.
    cvt(unsafe { sched_setaffinity(0, size_of::<CpuSet>(), &set) }).map(drop)
}

/// `access(path, R_OK) == 0`.
pub fn is_readable(path: &std::path::Path) -> bool {
    use std::os::unix::ffi::OsStrExt;
    let Ok(path) = CString::new(path.as_os_str().as_bytes()) else {
        return false;
    };
    // SAFETY: path is NUL-terminated.
    unsafe { access(path.as_ptr(), R_OK) == 0 }
}

// Capabilities and process hardening.

/// `struct __user_cap_header_struct`.
#[repr(C)]
struct CapHeader {
    version: u32,
    pid: c_int,
}

/// `struct __user_cap_data_struct`; version 3 uses two of them (capabilities 0-31 and 32-63).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct CapData {
    effective: u32,
    permitted: u32,
    inheritable: u32,
}

const LINUX_CAPABILITY_VERSION_3: u32 = 0x2008_0522;
const PR_GET_DUMPABLE: c_int = 3;
const PR_SET_DUMPABLE: c_int = 4;
const PR_SET_NO_NEW_PRIVS: c_int = 38;
const PR_GET_NO_NEW_PRIVS: c_int = 39;
const PR_CAP_AMBIENT: c_int = 47;
const PR_CAP_AMBIENT_IS_SET: c_ulong = 1;
const PR_CAP_AMBIENT_CLEAR_ALL: c_ulong = 4;
/// `CAP_SYS_NICE`.
pub const CAP_SYS_NICE: u32 = 23;
/// `CAP_SYS_PTRACE`.
pub const CAP_SYS_PTRACE: u32 = 19;

/// Effective, permitted and inheritable sets of the calling thread, as 64-bit masks.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CapSets {
    pub effective: u64,
    pub permitted: u64,
    pub inheritable: u64,
}

/// `capget()` of the calling thread.
pub fn capabilities() -> io::Result<CapSets> {
    let mut header = CapHeader { version: LINUX_CAPABILITY_VERSION_3, pid: 0 };
    let mut data = [CapData::default(); 2];
    // SAFETY: header is valid and data holds the two structs version 3 writes.
    cvt(unsafe { capget(&mut header, data.as_mut_ptr()) })?;
    let join = |low: u32, high: u32| u64::from(low) | (u64::from(high) << 32);
    Ok(CapSets {
        effective: join(data[0].effective, data[1].effective),
        permitted: join(data[0].permitted, data[1].permitted),
        inheritable: join(data[0].inheritable, data[1].inheritable),
    })
}

/// Whether `cap` is in the ambient set of the calling thread.
pub fn ambient_capability_is_set(cap: u32) -> io::Result<bool> {
    // SAFETY: plain prctl() with integer arguments.
    let cap = c_ulong::from(cap);
    cvt(unsafe { prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, cap, 0 as c_ulong, 0 as c_ulong) })
        .map(|set| set == 1)
}

/// Empties the ambient set, then the effective, permitted and inheritable sets of the calling
/// thread (lowering capabilities never needs a privilege). The server is single-threaded when it
/// calls this, so the whole process ends up without capabilities.
pub fn drop_all_capabilities() -> io::Result<()> {
    // SAFETY: plain prctl() with integer arguments.
    let zero = 0 as c_ulong;
    cvt(unsafe { prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, zero, zero, zero) })?;
    let mut header = CapHeader { version: LINUX_CAPABILITY_VERSION_3, pid: 0 };
    let data = [CapData::default(); 2];
    // SAFETY: header is valid and data holds the two structs version 3 reads.
    cvt(unsafe { capset(&mut header, data.as_ptr()) }).map(drop)
}

/// `PR_SET_NO_NEW_PRIVS`: no later `execve()` can gain privileges (setuid bits, file caps).
pub fn set_no_new_privs() -> io::Result<()> {
    // SAFETY: plain prctl() with integer arguments.
    let zero = 0 as c_ulong;
    cvt(unsafe { prctl(PR_SET_NO_NEW_PRIVS, 1 as c_ulong, zero, zero, zero) }).map(drop)
}

/// `PR_GET_NO_NEW_PRIVS`.
pub fn no_new_privs() -> io::Result<bool> {
    // SAFETY: plain prctl() with integer arguments.
    let zero = 0 as c_ulong;
    cvt(unsafe { prctl(PR_GET_NO_NEW_PRIVS, zero, zero, zero, zero) }).map(|set| set == 1)
}

/// `PR_SET_DUMPABLE`: with `false`, no core dump and no ptrace attach by other processes of the
/// same uid.
pub fn set_dumpable(dumpable: bool) -> io::Result<()> {
    // SAFETY: plain prctl() with integer arguments.
    let zero = 0 as c_ulong;
    cvt(unsafe { prctl(PR_SET_DUMPABLE, c_ulong::from(dumpable), zero, zero, zero) }).map(drop)
}

/// Serializes the tests that flip the process-wide dumpable flag.
#[cfg(test)]
pub(crate) static DUMPABLE_TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// `PR_GET_DUMPABLE`.
pub fn dumpable() -> io::Result<bool> {
    // SAFETY: plain prctl() with integer arguments.
    cvt(unsafe { prctl(PR_GET_DUMPABLE, 0 as c_ulong, 0 as c_ulong, 0 as c_ulong, 0 as c_ulong) })
        .map(|value| value == 1)
}

// ---------------------------------------------------------------------------------------------
// SIGTERM / SIGINT: self-pipe, so a signal that lands between the "running?" check and poll()
// still wakes the loop up (the C++ server could sleep through it until the next event).
// ---------------------------------------------------------------------------------------------

static RUNNING: AtomicBool = AtomicBool::new(true);
static WAKE_FD: AtomicI32 = AtomicI32::new(-1);

extern "C" fn on_termination_signal(_signum: c_int) {
    RUNNING.store(false, Ordering::Release);
    let fd = WAKE_FD.load(Ordering::Acquire);
    if fd >= 0 {
        // SAFETY: write() is async-signal-safe; the pipe is non-blocking and never closed.
        unsafe {
            write(fd, b"x".as_ptr().cast(), 1);
        }
    }
}

/// Installs the SIGTERM/SIGINT handlers. Returns the read end of the wake-up pipe, to be polled.
pub fn install_termination_handlers() -> io::Result<OwnedFd> {
    let mut fds = [-1 as c_int; 2];
    // SAFETY: fds is a writable array of two ints.
    cvt(unsafe { pipe2(fds.as_mut_ptr(), O_NONBLOCK | O_CLOEXEC) })?;
    // The write end stays open for the life of the process (the handler may run at any time).
    WAKE_FD.store(fds[1], Ordering::Release);
    for signum in [SIGTERM, SIGINT] {
        // SAFETY: the handler only touches atomics and calls write().
        if unsafe { signal(signum, on_termination_signal as extern "C" fn(c_int) as usize) }
            == SIG_ERR
        {
            return Err(io::Error::last_os_error());
        }
    }
    // SAFETY: fds[0] is a fresh, valid descriptor.
    Ok(unsafe { OwnedFd::from_raw_fd(fds[0]) })
}

/// False once SIGTERM or SIGINT arrived.
pub fn running() -> bool {
    RUNNING.load(Ordering::Acquire)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
    #[test]
    fn ioctl_numbers_match_the_uapi_headers() {
        // Values printed by a C program including <linux/uinput.h> and <linux/input.h> on x86_64.
        assert_eq!(UI_DEV_CREATE, 0x5501);
        assert_eq!(UI_DEV_DESTROY, 0x5502);
        assert_eq!(UI_DEV_SETUP, 0x405c5503);
        assert_eq!(UI_SET_EVBIT, 0x40045564);
        assert_eq!(UI_SET_KEYBIT, 0x40045565);
        assert_eq!(ui_get_sysname_request(64), 0x8040552c);
        assert_eq!(eviocgbit(1, 96), 0x80604521);
        assert_eq!(eviocgkey(96), 0x80604518);
    }

    #[test]
    fn unix_address_layout() {
        let addr = UnixAddr::new(std::path::Path::new("/run/vanh/alice/kb.sock")).unwrap();
        assert_eq!(addr.path(), b"/run/vanh/alice/kb.sock");
        assert_eq!(addr.socklen() as usize, 2 + 23 + 1);
        assert_eq!(addr.addr.sun_path[0], b'/');
        assert_eq!(addr.addr.sun_path[23], 0);
    }

    #[test]
    fn unix_address_is_never_truncated() {
        let longest = format!("/{}", "a".repeat(MAX_SOCKET_PATH - 1));
        assert_eq!(
            UnixAddr::new(std::path::Path::new(&longest)).unwrap().socklen() as usize,
            size_of::<sockaddr_un>()
        );
        let too_long = format!("/{}", "a".repeat(MAX_SOCKET_PATH));
        assert_eq!(
            UnixAddr::new(std::path::Path::new(&too_long)).err().and_then(|e| e.raw_os_error()),
            Some(ENAMETOOLONG)
        );
        assert_eq!(
            UnixAddr::new(std::path::Path::new("")).err().and_then(|e| e.raw_os_error()),
            Some(EINVAL)
        );
    }

    /// Runs in its own thread: capabilities and no_new_privs are per thread, so the other tests of
    /// the process keep theirs.
    #[test]
    fn dropping_privileges_empties_every_set() {
        std::thread::spawn(|| {
            drop_all_capabilities().unwrap();
            assert_eq!(capabilities().unwrap(), CapSets::default());
            assert!(!ambient_capability_is_set(CAP_SYS_NICE).unwrap());
            assert!(!ambient_capability_is_set(CAP_SYS_PTRACE).unwrap());
            // Lowering is idempotent.
            drop_all_capabilities().unwrap();
            set_no_new_privs().unwrap();
            assert!(no_new_privs().unwrap());
        })
        .join()
        .unwrap();
    }

    #[test]
    fn dumpable_flag_round_trips() {
        // Process wide: restore it so a failing test can still dump core.
        let _guard = DUMPABLE_TEST_LOCK.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let before = dumpable().unwrap();
        set_dumpable(false).unwrap();
        assert!(!dumpable().unwrap());
        set_dumpable(before).unwrap();
        assert_eq!(dumpable().unwrap(), before);
    }

    #[test]
    fn peer_credentials_of_a_socketpair() {
        use std::os::fd::AsFd;
        let (left, _right) = std::os::unix::net::UnixStream::pair().unwrap();
        let cred = peer_credentials(left.as_fd()).unwrap();
        assert_eq!(cred.pid, std::process::id() as i32);
        assert_eq!(cred.uid, current_uid());
    }

    #[test]
    fn current_user_resolves_both_ways() {
        let uid = current_uid();
        if let Some(name) = user_name_of(uid) {
            assert_eq!(uid_of_user(&name), Some(uid));
        }
        assert_eq!(uid_of_user(b"no-such-user-\xff-vanh"), None);
        assert_eq!(uid_of_user(b"nul\0inside"), None);
    }

    #[test]
    fn strerror_matches_libc() {
        assert_eq!(strerror_text(98), "Address already in use");
        assert_eq!(error_text(&io::Error::from_raw_os_error(9)), "Bad file descriptor");
    }
}
