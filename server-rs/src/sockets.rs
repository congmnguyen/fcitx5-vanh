// SPDX-FileCopyrightText: 2026 LotusInputMethod contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The two listening sockets: filesystem `AF_UNIX` sockets in a directory only the server user
//! can write, `<SOCKET_DIR>/<namespace>/{kb,mouse}.sock`.
//!
//! The former abstract sockets could be bound first by any local user (the abstract namespace
//! has no permissions), who then received the addon's requests. Here:
//!
//! * `/run/vanh` belongs to the uinput proxy user, mode 0755 (tmpfiles.d, systemd
//!   `RuntimeDirectory=`); the server creates or checks `<namespace>/` below it: a real directory
//!   (not a symlink) owned by the server's effective uid, forced to mode 0755;
//! * a leftover file at a socket path is removed only when it is a socket owned by the server's
//!   uid; anything else (a regular file, a symlink, another uid's socket) makes the bind fail;
//! * the socket file is mode 0666: connecting needs write permission on it, the server cannot
//!   give the file to the target user (no CAP_CHOWN) and every connection is checked against the
//!   target uid (`SO_PEERCRED`, [`crate::auth`]) before anything is read from or sent to it.

use crate::log;
use crate::names;
use crate::sys::{self, UnixAddr};
use std::ffi::OsStr;
use std::io;
use std::os::fd::{AsFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, FileTypeExt, MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};

/// `listen()` backlog of both sockets.
const LISTEN_BACKLOG: i32 = 5;
/// Mode of the per-user socket directory.
pub const SOCKET_DIR_MODE: u32 = 0o755;
/// Mode of the socket files (see the module documentation).
pub const SOCKET_FILE_MODE: u32 = 0o666;

/// Base directory: `VANH_SOCKET_DIR` when set and non-empty (tests), `/run/vanh` otherwise.
pub fn base_dir() -> PathBuf {
    match std::env::var_os(names::SOCKET_DIR_ENV) {
        Some(value) if !value.is_empty() => PathBuf::from(value),
        _ => PathBuf::from(names::SOCKET_DIR),
    }
}

/// A namespace usable as one path component: non-empty, no `/` or NUL, not `.` or `..`.
pub fn is_valid_namespace(namespace: &[u8]) -> bool {
    !namespace.is_empty() && namespace != b"." && namespace != b".." && !namespace.iter().any(|&byte| byte == b'/' || byte == 0)
}

/// `<base>/<namespace>`, or `None` for an invalid namespace.
pub fn socket_dir(base: &Path, namespace: &[u8]) -> Option<PathBuf> {
    is_valid_namespace(namespace).then(|| base.join(OsStr::from_bytes(namespace)))
}

fn refused(what: String) -> io::Error {
    io::Error::new(io::ErrorKind::PermissionDenied, what)
}

/// Creates `dir` (mode 0755) when missing, then checks that it is a directory, not a symlink,
/// owned by `owner`, and sets its mode to 0755. The parent must exist.
pub fn prepare_socket_dir(dir: &Path, owner: u32) -> io::Result<()> {
    match std::fs::DirBuilder::new().mode(SOCKET_DIR_MODE).create(dir) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
        Err(error) => return Err(error),
    }
    let meta = std::fs::symlink_metadata(dir)?;
    if !meta.file_type().is_dir() {
        return Err(refused(format!("{} is not a directory", dir.display())));
    }
    if meta.uid() != owner {
        return Err(refused(format!("{} belongs to UID {}, not {owner}", dir.display(), meta.uid())));
    }
    if meta.mode() & 0o7777 != SOCKET_DIR_MODE {
        std::fs::set_permissions(dir, std::fs::Permissions::from_mode(SOCKET_DIR_MODE))?;
    }
    Ok(())
}

/// Removes a leftover socket file at `path` (a previous instance that crashed). Nothing there is
/// fine; a socket owned by `owner` is removed; anything else is refused and left in place.
pub fn remove_stale_socket(path: &Path, owner: u32) -> io::Result<()> {
    let meta = match std::fs::symlink_metadata(path) {
        Ok(meta) => meta,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error),
    };
    if !meta.file_type().is_socket() {
        return Err(refused(format!("{} exists and is not a socket", path.display())));
    }
    if meta.uid() != owner {
        return Err(refused(format!("socket {} belongs to UID {}, not {owner}", path.display(), meta.uid())));
    }
    std::fs::remove_file(path)
}

/// A bound, listening socket; its file is removed when dropped (if it is still the same inode).
pub struct Listener {
    pub fd: OwnedFd,
    pub path: PathBuf,
    dev_ino: (u64, u64),
}

impl Drop for Listener {
    fn drop(&mut self) {
        if let Ok(meta) = std::fs::symlink_metadata(&self.path) {
            if meta.file_type().is_socket() && (meta.dev(), meta.ino()) == self.dev_ino {
                let _ = std::fs::remove_file(&self.path);
            }
        }
    }
}

fn listen_at(path: &Path, owner: u32) -> io::Result<Listener> {
    let addr = UnixAddr::new(path)?;
    remove_stale_socket(path, owner)?;
    let fd = sys::seqpacket_socket(true)?;
    sys::bind_unix(fd.as_fd(), &addr)?;
    let meta = std::fs::symlink_metadata(path)?;
    let listener = Listener { fd, path: path.to_path_buf(), dev_ino: (meta.dev(), meta.ino()) };
    // Before listen(): until then a connect() is refused anyway.
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(SOCKET_FILE_MODE))?;
    sys::listen_socket(listener.fd.as_fd(), LISTEN_BACKLOG)?;
    Ok(listener)
}

/// Binds and listens on `path` (removing a stale socket of `owner` first), logging the reason on
/// failure.
pub fn listen(path: &Path, owner: u32, label: &str) -> Option<Listener> {
    match listen_at(path, owner) {
        Ok(listener) => Some(listener),
        Err(error) => {
            log::error(&format!("Failed to bind {label} at {}: {}", path.display(), sys::error_text(&error)));
            None
        }
    }
}

/// The keyboard and mouse sockets of `namespace` under `base`, owned by `owner`.
pub struct ServerSockets {
    pub kb: Listener,
    pub mouse: Listener,
}

/// Prepares `<base>/<namespace>/` and listens on both sockets in it, logging the reason on failure.
pub fn open_server_sockets(base: &Path, namespace: &[u8], owner: u32) -> Option<ServerSockets> {
    let Some(dir) = socket_dir(base, namespace) else {
        log::error(&format!("Invalid socket namespace: {}", String::from_utf8_lossy(namespace)));
        return None;
    };
    if let Err(error) = prepare_socket_dir(&dir, owner) {
        log::error(&format!("Failed to prepare the socket directory {}: {}", dir.display(), sys::error_text(&error)));
        return None;
    }
    let kb = listen(&dir.join(names::KB_SOCKET_FILE), owner, "keyboard socket")?;
    let mouse = listen(&dir.join(names::MOUSE_SOCKET_FILE), owner, "mouse socket")?;
    Some(ServerSockets { kb, mouse })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::log::capture;
    use std::sync::atomic::{AtomicUsize, Ordering};

    /// A fresh, private directory under the system temp dir, removed on drop.
    struct TempDir(PathBuf);

    impl TempDir {
        fn new() -> Self {
            static NEXT: AtomicUsize = AtomicUsize::new(0);
            let path = std::env::temp_dir().join(format!("vanh-srv-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
            let _ = std::fs::remove_dir_all(&path);
            std::fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
            TempDir(path)
        }
    }

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn mode_of(path: &Path) -> u32 {
        std::fs::symlink_metadata(path).unwrap().mode() & 0o7777
    }

    fn me() -> u32 {
        sys::effective_uid()
    }

    #[test]
    fn namespaces_are_single_path_components() {
        assert!(is_valid_namespace(b"alice"));
        assert!(is_valid_namespace(b"ctest-0123456789ab-kb_sender"));
        for bad in [&b""[..], b".", b"..", b"a/b", b"/", b"nul\0"] {
            assert!(!is_valid_namespace(bad), "{bad:?}");
        }
        assert_eq!(socket_dir(Path::new("/run/vanh"), b"alice"), Some(PathBuf::from("/run/vanh/alice")));
        assert_eq!(socket_dir(Path::new("/run/vanh"), b"../etc"), None);
    }

    #[test]
    fn socket_dir_is_created_with_mode_0755() {
        let tmp = TempDir::new();
        let dir = tmp.0.join("alice");
        prepare_socket_dir(&dir, me()).unwrap();
        assert_eq!(mode_of(&dir), 0o755);
        // An existing directory of ours with a loose mode is tightened, not refused.
        std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o777)).unwrap();
        prepare_socket_dir(&dir, me()).unwrap();
        assert_eq!(mode_of(&dir), 0o755);
    }

    #[test]
    fn socket_dir_of_another_owner_symlink_or_file_is_refused() {
        let tmp = TempDir::new();
        let dir = tmp.0.join("alice");
        prepare_socket_dir(&dir, me()).unwrap();
        let error = prepare_socket_dir(&dir, me().wrapping_add(1)).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);

        let link = tmp.0.join("bob");
        std::os::unix::fs::symlink(&dir, &link).unwrap();
        assert!(prepare_socket_dir(&link, me()).is_err(), "a symlink is not followed");

        let file = tmp.0.join("carol");
        std::fs::write(&file, b"x").unwrap();
        assert!(prepare_socket_dir(&file, me()).is_err());
        assert!(prepare_socket_dir(&tmp.0.join("missing/parent"), me()).is_err(), "the parent is never created");
    }

    #[test]
    fn stale_socket_of_ours_is_removed() {
        let tmp = TempDir::new();
        let path = tmp.0.join("kb.sock");
        remove_stale_socket(&path, me()).expect("nothing there is fine");
        drop(std::os::unix::net::UnixListener::bind(&path).unwrap());
        assert!(path.exists(), "a closed listener leaves its file behind");
        remove_stale_socket(&path, me()).unwrap();
        assert!(std::fs::symlink_metadata(&path).is_err());
    }

    #[test]
    fn stale_socket_of_another_uid_is_kept() {
        let tmp = TempDir::new();
        let path = tmp.0.join("kb.sock");
        drop(std::os::unix::net::UnixListener::bind(&path).unwrap());
        let error = remove_stale_socket(&path, me().wrapping_add(1)).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
        assert!(std::fs::symlink_metadata(&path).unwrap().file_type().is_socket(), "left in place");
    }

    #[test]
    fn non_socket_at_the_socket_path_is_kept() {
        let tmp = TempDir::new();
        let file = tmp.0.join("kb.sock");
        std::fs::write(&file, b"precious").unwrap();
        assert!(remove_stale_socket(&file, me()).is_err());
        assert_eq!(std::fs::read(&file).unwrap(), b"precious");

        let target = tmp.0.join("target");
        std::fs::write(&target, b"precious").unwrap();
        let link = tmp.0.join("mouse.sock");
        std::os::unix::fs::symlink(&target, &link).unwrap();
        assert!(remove_stale_socket(&link, me()).is_err());
        assert!(std::fs::symlink_metadata(&link).unwrap().file_type().is_symlink());

        capture::take();
        assert!(listen(&file, me(), "test socket").is_none());
        let lines = capture::take();
        assert!(lines.last().unwrap().1.ends_with("exists and is not a socket"), "{lines:?}");
        assert_eq!(std::fs::read(&file).unwrap(), b"precious");
    }

    #[test]
    fn sockets_are_mode_0666_and_removed_on_drop() {
        let tmp = TempDir::new();
        let sockets = open_server_sockets(&tmp.0, b"alice", me()).expect("open both sockets");
        let dir = tmp.0.join("alice");
        assert_eq!(sockets.kb.path, dir.join("kb.sock"));
        assert_eq!(sockets.mouse.path, dir.join("mouse.sock"));
        for path in [&sockets.kb.path, &sockets.mouse.path] {
            assert!(std::fs::symlink_metadata(path).unwrap().file_type().is_socket());
            assert_eq!(mode_of(path), 0o666);
        }
        assert_eq!(mode_of(&dir), 0o755);
        let client = std::os::unix::net::UnixStream::connect(&sockets.kb.path);
        // SOCK_SEQPACKET listener: a stream connect is refused with EPROTOTYPE, which still proves
        // the path is bound.
        assert_eq!(client.err().and_then(|e| e.raw_os_error()), Some(91));
        let (kb, mouse) = (sockets.kb.path.clone(), sockets.mouse.path.clone());
        drop(sockets);
        assert!(std::fs::symlink_metadata(&kb).is_err());
        assert!(std::fs::symlink_metadata(&mouse).is_err());
    }

    #[test]
    fn second_server_replaces_the_stale_socket_of_the_first() {
        let tmp = TempDir::new();
        let first = open_server_sockets(&tmp.0, b"alice", me()).unwrap();
        // A crashed instance leaves its files; the next one must start.
        std::mem::forget(first);
        let second = open_server_sockets(&tmp.0, b"alice", me());
        assert!(second.is_some());
    }

    #[test]
    fn invalid_namespace_and_long_paths_are_reported() {
        let tmp = TempDir::new();
        capture::take();
        assert!(open_server_sockets(&tmp.0, b"../x", me()).is_none());
        assert_eq!(capture::take().last().unwrap().1, "Invalid socket namespace: ../x");
        let long = "n".repeat(120);
        assert!(open_server_sockets(&tmp.0, long.as_bytes(), me()).is_none());
        assert!(capture::take().last().unwrap().1.ends_with("File name too long"));
    }
}
