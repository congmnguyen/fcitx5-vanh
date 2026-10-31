// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Thin `syslog()` wrapper (port of server/lotus-logger.{h,cpp}).
//!
//! Messages go to the local syslog daemon (journald/rsyslog), facility `LOG_DAEMON`, option
//! `LOG_PID`, ident [`crate::names::SYSLOG_IDENT`]. The minimum level is Info, as in the C++
//! logger (nothing ever lowered it). Unit tests capture the messages instead; the replay helper
//! used by the C++/Rust wire test switches to stderr.

use std::ffi::{c_char, c_int, CString};
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};

/// Log level, ordered like the C++ `LogLevel`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Level {
    Debug,
    Info,
    Warn,
    Error,
}

const LOG_PID: c_int = 0x01;
const LOG_DAEMON: c_int = 3 << 3;
const LOG_ERR: c_int = 3;
const LOG_WARNING: c_int = 4;
const LOG_INFO: c_int = 6;
const LOG_DEBUG: c_int = 7;

extern "C" {
    fn openlog(ident: *const c_char, option: c_int, facility: c_int);
    fn syslog(priority: c_int, format: *const c_char, ...);
}

static MIN_LEVEL: AtomicU8 = AtomicU8::new(Level::Info as u8);
static OPENED: AtomicBool = AtomicBool::new(false);
static TO_STDERR: AtomicBool = AtomicBool::new(false);

/// Sends the log to stderr instead of syslog (test helpers only).
pub fn use_stderr() {
    TO_STDERR.store(true, Ordering::Release);
}

/// Logs one message at `level`, if enabled.
pub fn log(level: Level, message: &str) {
    if (level as u8) < MIN_LEVEL.load(Ordering::Acquire) {
        return;
    }
    #[cfg(test)]
    {
        capture::push(level, message);
    }
    #[cfg(not(test))]
    {
        if TO_STDERR.load(Ordering::Acquire) {
            eprintln!("{level:?}: {message}");
            return;
        }
        write_syslog(level, message);
    }
}

#[cfg_attr(test, allow(dead_code))]
fn write_syslog(level: Level, message: &str) {
    if !OPENED.swap(true, Ordering::AcqRel) {
        // SAFETY: the ident is a 'static C string, as openlog() requires.
        unsafe { openlog(crate::names::SYSLOG_IDENT.as_ptr(), LOG_PID, LOG_DAEMON) };
    }
    let priority = match level {
        Level::Debug => LOG_DEBUG,
        Level::Info => LOG_INFO,
        Level::Warn => LOG_WARNING,
        Level::Error => LOG_ERR,
    };
    // Interior NULs would truncate the C string; keep the rest of the message visible.
    let text = CString::new(message.replace('\0', "\\0")).unwrap_or_default();
    // SAFETY: the message is data passed through "%s", never a format string.
    unsafe { syslog(priority, c"%s".as_ptr(), text.as_ptr()) };
}

pub fn info(message: &str) {
    log(Level::Info, message);
}

pub fn warn(message: &str) {
    log(Level::Warn, message);
}

pub fn error(message: &str) {
    log(Level::Error, message);
}

#[cfg(test)]
pub mod capture {
    use super::Level;
    use std::cell::RefCell;

    thread_local! {
        static LINES: RefCell<Vec<(Level, String)>> = const { RefCell::new(Vec::new()) };
    }

    pub fn push(level: Level, message: &str) {
        LINES.with(|lines| lines.borrow_mut().push((level, message.to_owned())));
    }

    /// Takes the messages logged by the current test thread so far.
    pub fn take() -> Vec<(Level, String)> {
        LINES.with(|lines| std::mem::take(&mut *lines.borrow_mut()))
    }
}
