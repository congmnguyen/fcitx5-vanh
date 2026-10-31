// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Quang Minh <minhnbnt@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Peer authorization of the server sockets (`authorizePeer()` of server/lotus-server.cpp and
//! `isAllowedExecutablePath()` of common/common-executable-path.cpp).
//!
//! A client is accepted when its `SO_PEERCRED` uid is the target user's and its executable
//! (`/proc/<pid>/exe`, readable for another user's process thanks to CAP_SYS_PTRACE) is the
//! configured fcitx5 binary.

use crate::log;
use crate::names;
use crate::sys;
use std::os::fd::BorrowedFd;
use std::os::unix::ffi::OsStrExt;

/// Executables allowed to connect.
#[derive(Clone, Debug)]
pub struct AllowedExecutable {
    /// Exact path (`LOTUS_FCITX5_EXECUTABLE`).
    pub path: Vec<u8>,
    /// Extra prefix whose `<prefix>…/bin/<name>` is accepted too (`LOTUS_ALT_EXECUTABLE_PREFIX`).
    pub alt_prefix: Vec<u8>,
}

impl AllowedExecutable {
    /// The paths configured at build time.
    pub fn configured() -> Self {
        AllowedExecutable { path: names::FCITX5_EXECUTABLE.as_bytes().to_vec(), alt_prefix: names::ALT_EXECUTABLE_PREFIX.as_bytes().to_vec() }
    }

    pub fn allows(&self, exe: &[u8]) -> bool {
        is_allowed_executable_path(exe, &self.path, &self.alt_prefix)
    }
}

/// Whether a peer executable may act as the expected program: the exact path, or, with a
/// non-empty `alt_prefix`, any `<alt_prefix>…/bin/<basename of expected>`.
pub fn is_allowed_executable_path(exe: &[u8], expected: &[u8], alt_prefix: &[u8]) -> bool {
    if exe.is_empty() || expected.is_empty() {
        return false;
    }
    if exe == expected {
        return true;
    }
    if alt_prefix.is_empty() || exe.len() <= alt_prefix.len() || !exe.starts_with(alt_prefix) {
        return false;
    }
    const BIN_DIR: &[u8] = b"/bin/";
    let name = match expected.iter().rposition(|&byte| byte == b'/') {
        Some(slash) => &expected[slash + 1..],
        None => expected,
    };
    if name.is_empty() || exe.len() < BIN_DIR.len() + name.len() {
        return false;
    }
    let name_at = exe.len() - name.len();
    &exe[name_at..] == name && &exe[name_at - BIN_DIR.len()..name_at] == BIN_DIR
}

/// Authorizes a freshly accepted client. Returns its pid, or `None` (after logging why).
pub fn authorize_peer(fd: BorrowedFd<'_>, expected_uid: u32, label: &str, allowed: &AllowedExecutable) -> Option<i32> {
    let Ok(cred) = sys::peer_credentials(fd) else {
        log::warn(&format!("Failed to get peer credentials for {label}"));
        return None;
    };
    if cred.uid != expected_uid {
        log::warn(&format!("Unauthorized UID connection attempt to {label} from UID: {}", cred.uid));
        return None;
    }
    let exe = match std::fs::read_link(format!("/proc/{}/exe", cred.pid)) {
        Ok(exe) if !exe.as_os_str().is_empty() => exe,
        _ => {
            log::warn(&format!("Failed to resolve the peer executable for {label}"));
            return None;
        }
    };
    let exe = exe.as_os_str().as_bytes();
    if !allowed.allows(exe) {
        log::warn(&format!("Unauthorized executable connection attempt to {label} from: {}", String::from_utf8_lossy(exe)));
        return None;
    }
    Some(cred.pid)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::log::capture;
    use std::os::fd::AsFd;

    const FCITX5: &[u8] = b"/usr/bin/fcitx5";
    const SERVER: &[u8] = b"/usr/bin/fcitx5-lotus-server";
    const NIX: &[u8] = b"/nix/store/";

    /// Same cases as test/executable-path-match.cpp, which keeps testing the C++ copy the addon uses.
    #[test]
    fn executable_path_rules() {
        let store_peer: &[u8] = b"/nix/store/abc123-fcitx5-5.1.13/bin/fcitx5";
        let cases: &[(&[u8], &[u8], &[u8], bool)] = &[
            (FCITX5, FCITX5, b"", true),
            (FCITX5, FCITX5, NIX, true),
            (store_peer, FCITX5, NIX, true),
            (store_peer, FCITX5, b"", false),
            (b"/nix/store/abc123-fcitx5-5.1.13/other/fcitx5", FCITX5, NIX, false),
            (b"/nix/store/abc123-fcitx5-5.1.13/bin/fcitx6", FCITX5, NIX, false),
            (b"/nix/storeabc/bin/fcitx5", FCITX5, NIX, false),
            (b"/usr/libexec/fcitx5", FCITX5, NIX, false),
            (b"", FCITX5, NIX, false),
            (FCITX5, b"", NIX, false),
            (b"/nix/store/abc-lotus/bin/fcitx5-lotus-server", SERVER, NIX, true),
            (b"/nix/store/abc-lotus/bin/fcitx5", SERVER, NIX, false),
            (b"/nix/store/", FCITX5, NIX, false),
            (b"/nix/store/bin/fcitx5", FCITX5, NIX, true),
            (b"/nix/store/x/bin/fcitx5 (deleted)", FCITX5, NIX, false),
            (b"/usr/bin/fcitx5 (deleted)", FCITX5, b"", false),
            (b"/nix/store/x/bin/sh", b"/", NIX, false),
        ];
        for &(exe, expected, prefix, want) in cases {
            assert_eq!(
                is_allowed_executable_path(exe, expected, prefix),
                want,
                "exe={} expected={} prefix={}",
                String::from_utf8_lossy(exe),
                String::from_utf8_lossy(expected),
                String::from_utf8_lossy(prefix)
            );
        }
    }

    fn own_exe() -> Vec<u8> {
        std::fs::read_link("/proc/self/exe").unwrap().as_os_str().as_bytes().to_vec()
    }

    #[test]
    fn peer_with_the_right_uid_and_executable_is_accepted() {
        let (left, _right) = std::os::unix::net::UnixStream::pair().unwrap();
        let allowed = AllowedExecutable { path: own_exe(), alt_prefix: Vec::new() };
        assert_eq!(authorize_peer(left.as_fd(), sys::current_uid(), "test socket", &allowed), Some(std::process::id() as i32));
    }

    #[test]
    fn peer_with_another_uid_is_rejected() {
        let (left, _right) = std::os::unix::net::UnixStream::pair().unwrap();
        let allowed = AllowedExecutable { path: own_exe(), alt_prefix: Vec::new() };
        let other = sys::current_uid().wrapping_add(1);
        capture::take();
        assert_eq!(authorize_peer(left.as_fd(), other, "keyboard socket", &allowed), None);
        let lines = capture::take();
        assert_eq!(lines.last().unwrap().1, format!("Unauthorized UID connection attempt to keyboard socket from UID: {}", sys::current_uid()));
    }

    #[test]
    fn peer_with_another_executable_is_rejected() {
        let (left, _right) = std::os::unix::net::UnixStream::pair().unwrap();
        let allowed = AllowedExecutable { path: FCITX5.to_vec(), alt_prefix: NIX.to_vec() };
        capture::take();
        assert_eq!(authorize_peer(left.as_fd(), sys::current_uid(), "mouse socket", &allowed), None);
        let lines = capture::take();
        assert!(lines.last().unwrap().1.starts_with("Unauthorized executable connection attempt to mouse socket from: /"));
    }

    #[test]
    fn non_socket_has_no_credentials() {
        let file = std::fs::File::open("/dev/null").unwrap();
        capture::take();
        assert_eq!(authorize_peer(file.as_fd(), sys::current_uid(), "keyboard socket", &AllowedExecutable::configured()), None);
        assert_eq!(capture::take().last().unwrap().1, "Failed to get peer credentials for keyboard socket");
    }
}
