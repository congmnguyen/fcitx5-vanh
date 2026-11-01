// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The $TIME / $DATE macros, like Go's formatTime: the strftime-like format goes through Go's
//! strftimeReplacer table (giving a Go reference layout), which is then rendered with the C
//! library's strftime in local time.

use std::ffi::{c_char, c_long, c_void};

extern "C" {
    // time_t is `long` in the classic glibc ABI on every Linux target this addon is built for.
    fn time(out: *mut c_long) -> c_long;
    fn localtime_r(timer: *const c_long, result: *mut c_void) -> *mut c_void;
    fn strftime(out: *mut c_char, max: usize, format: *const c_char, tm: *const c_void) -> usize;
}

/// Go's strftimeReplacer, applied left to right (no pattern is a prefix of another one).
const REPLACEMENTS: [(&str, &str); 18] = [
    ("%H", "15"),
    ("%I", "03"),
    ("%M", "04"),
    ("%S", "05"),
    ("%p", "PM"),
    ("%P", "pm"),
    ("%d", "02"),
    ("%m", "01"),
    ("%Y", "2006"),
    ("%y", "06"),
    ("%b", "Jan"),
    ("%B", "January"),
    ("%a", "Mon"),
    ("%A", "Monday"),
    ("%D", "01/02/06"),
    ("%F", "2006-01-02"),
    ("%T", "15:04:05"),
    ("%R", "15:04"),
];

/// Go reference layout tokens, longest first so "2006" wins over "06" and "January" over "Jan".
const TOKENS: [(&str, &str); 14] = [
    ("2006", "%Y"),
    ("January", "%B"),
    ("Jan", "%b"),
    ("Monday", "%A"),
    ("Mon", "%a"),
    ("15", "%H"),
    ("03", "%I"),
    ("04", "%M"),
    ("05", "%S"),
    ("02", "%d"),
    ("01", "%m"),
    ("06", "%y"),
    ("PM", "%p"),
    ("pm", "%P"),
];

fn replace_tokens(input: &[u8], table: &[(&str, &str)]) -> Vec<u8> {
    let mut out = Vec::with_capacity(input.len());
    let mut index = 0;
    while index < input.len() {
        match table.iter().find(|(from, _)| input[index..].starts_with(from.as_bytes())) {
            Some((from, to)) => {
                out.extend_from_slice(to.as_bytes());
                index += from.len();
            }
            None => {
                out.push(input[index]);
                index += 1;
            }
        }
    }
    out
}

pub fn strftime_pattern(layout: &[u8]) -> Vec<u8> {
    replace_tokens(layout, &TOKENS)
}

/// The pattern handed to strftime for a configured format, None when nothing is rendered.
pub fn pattern_for(format: &[u8]) -> Option<Vec<u8>> {
    let layout = replace_tokens(format, &REPLACEMENTS);
    if layout.is_empty() {
        return None;
    }
    // Go renders the reference layout "15:04:05 02/01/2006" when the format still holds a '%'
    // that the replacer did not know.
    if layout == format && format.contains(&b'%') {
        Some(strftime_pattern(b"15:04:05 02/01/2006"))
    } else {
        Some(strftime_pattern(&layout))
    }
}

/// Formats the current local time; empty for an empty format.
pub fn format_time(format: &[u8]) -> Vec<u8> {
    let Some(mut pattern) = pattern_for(format) else {
        return Vec::new();
    };
    // c_str() semantics: the pattern ends at its first NUL.
    if let Some(nul) = pattern.iter().position(|&b| b == 0) {
        pattern.truncate(nul);
    }
    pattern.push(0);
    // Room for any libc's struct tm, kept opaque.
    let mut tm = [0u64; 16];
    let mut buffer = [0u8; 256];
    // SAFETY: plain libc calls with valid pointers; tm is larger than struct tm and aligned for
    // it.
    unsafe {
        let now = time(std::ptr::null_mut());
        if localtime_r(&now, tm.as_mut_ptr().cast()).is_null() {
            return Vec::new();
        }
        let out = buffer.as_mut_ptr().cast();
        let written = strftime(out, buffer.len(), pattern.as_ptr().cast(), tm.as_ptr().cast());
        buffer[..written].to_vec()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Ported from test/lotus-key-processor-test.cpp (formatTime cases).
    #[test]
    fn layouts() {
        assert_eq!(pattern_for(b"%H:%M").unwrap(), b"%H:%M");
        assert_eq!(pattern_for(b"%d/%m/%Y").unwrap(), b"%d/%m/%Y");
        assert_eq!(pattern_for(b"%I:%M %p").unwrap(), b"%I:%M %p");
        assert_eq!(pattern_for(b"%T").unwrap(), b"%H:%M:%S");
        // An unknown placeholder keeps its '%': the Go reference layout is rendered instead.
        assert_eq!(pattern_for(b"%Q").unwrap(), b"%H:%M:%S %d/%m/%Y");
        // A literal Go token is rendered too, like Go did.
        assert_eq!(pattern_for(b"15h").unwrap(), b"%Hh");
        assert!(pattern_for(b"").is_none());
    }

    #[test]
    fn formats_the_current_time() {
        let text = String::from_utf8(format_time(b"%H:%M")).unwrap();
        assert_eq!(text.len(), 5);
        assert_eq!(text.as_bytes()[2], b':');
        assert!(format_time(b"").is_empty());
        assert_eq!(format_time(b"%Y").len(), 4);
    }
}
