// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Quang Minh <minhnbnt@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Peer authorization of the server sockets.
//!
//! A client is accepted when its `SO_PEERCRED` uid (set by the kernel, not spoofable) is the
//! target user's. The server used to also compare `/proc/<pid>/exe` with the fcitx5 binary, which
//! needed CAP_SYS_PTRACE for the whole life of the process; that check is gone on purpose: any
//! process already running as the target user can do worse to that user than send Backspace
//! requests, so the uid is the real boundary.

use crate::log;
use crate::sys;
use std::os::fd::BorrowedFd;

/// Authorizes a freshly accepted client. Returns its pid, or `None` (after logging why).
pub fn authorize_peer(fd: BorrowedFd<'_>, expected_uid: u32, label: &str) -> Option<i32> {
    let Ok(cred) = sys::peer_credentials(fd) else {
        log::warn(&format!("Failed to get peer credentials for {label}"));
        return None;
    };
    if cred.uid != expected_uid {
        log::warn(&format!("Unauthorized UID connection attempt to {label} from UID: {} (PID: {})", cred.uid, cred.pid));
        return None;
    }
    Some(cred.pid)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::log::capture;
    use std::os::fd::AsFd;

    #[test]
    fn peer_with_the_right_uid_is_accepted() {
        let (left, _right) = std::os::unix::net::UnixStream::pair().unwrap();
        assert_eq!(authorize_peer(left.as_fd(), sys::current_uid(), "test socket"), Some(std::process::id() as i32));
    }

    #[test]
    fn peer_with_another_uid_is_rejected() {
        let (left, _right) = std::os::unix::net::UnixStream::pair().unwrap();
        let other = sys::current_uid().wrapping_add(1);
        capture::take();
        assert_eq!(authorize_peer(left.as_fd(), other, "keyboard socket"), None);
        let lines = capture::take();
        assert_eq!(
            lines.last().unwrap().1,
            format!("Unauthorized UID connection attempt to keyboard socket from UID: {} (PID: {})", sys::current_uid(), std::process::id())
        );
    }

    #[test]
    fn non_socket_has_no_credentials() {
        let file = std::fs::File::open("/dev/null").unwrap();
        capture::take();
        assert_eq!(authorize_peer(file.as_fd(), sys::current_uid(), "keyboard socket"), None);
        assert_eq!(capture::take().last().unwrap().1, "Failed to get peer credentials for keyboard socket");
    }
}
