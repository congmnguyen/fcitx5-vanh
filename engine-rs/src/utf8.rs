// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! UTF-8 decoding with Go's semantics: an invalid sequence yields one U+FFFD per byte, like
//! ranging over a Go string (`String::from_utf8_lossy` replaces a whole maximal subpart instead).

pub const REPLACEMENT: char = '\u{FFFD}';

fn is_continuation(byte: u8) -> bool {
    byte & 0xC0 == 0x80
}

/// Decodes a whole byte string.
pub fn decode(input: &[u8]) -> Vec<char> {
    let mut out = Vec::with_capacity(input.len());
    let mut i = 0;
    while i < input.len() {
        let b0 = input[i] as u32;
        // `n` continuation bytes follow.
        let cont = |n: usize| {
            input.len() - i > n && input[i + 1..=i + n].iter().all(|&b| is_continuation(b))
        };
        let trail = |offset: usize| (input[i + offset] & 0x3F) as u32;
        let mut c = REPLACEMENT;
        let mut len = 1;
        if b0 < 0x80 {
            c = b0 as u8 as char;
        } else if (0xC2..=0xDF).contains(&b0) && cont(1) {
            c = char::from_u32(((b0 & 0x1F) << 6) | trail(1)).unwrap_or(REPLACEMENT);
            len = 2;
        } else if (0xE0..=0xEF).contains(&b0) && cont(2) {
            let code_point = ((b0 & 0x0F) << 12) | (trail(1) << 6) | trail(2);
            if code_point >= 0x800 && !(0xD800..=0xDFFF).contains(&code_point) {
                c = char::from_u32(code_point).unwrap_or(REPLACEMENT);
                len = 3;
            }
        } else if (0xF0..=0xF4).contains(&b0) && cont(3) {
            let code_point = ((b0 & 0x07) << 18) | (trail(1) << 12) | (trail(2) << 6) | trail(3);
            if (0x10000..=0x10FFFF).contains(&code_point) {
                c = char::from_u32(code_point).unwrap_or(REPLACEMENT);
                len = 4;
            }
        }
        out.push(c);
        i += len;
    }
    out
}

/// Encodes code points as UTF-8.
pub fn encode(input: &[char]) -> String {
    input.iter().collect()
}

/// A code point from the C ABI; values that are no Unicode scalar become U+FFFD, like a UTF-8
/// round trip (Go's `string(rune)`) would make them.
pub fn from_u32(value: u32) -> char {
    char::from_u32(value).unwrap_or(REPLACEMENT)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chars(s: &str) -> Vec<char> {
        s.chars().collect()
    }

    #[test]
    fn decodes_valid_text() {
        assert_eq!(decode("aà ư𝄞".as_bytes()), chars("aà ư𝄞"));
        assert_eq!(encode(&chars("tiếng Việt")), "tiếng Việt");
    }

    #[test]
    fn invalid_bytes_become_one_replacement_each() {
        // A truncated three byte sequence: one U+FFFD per byte, like Go.
        assert_eq!(decode(b"\xE1\xBA"), vec![REPLACEMENT, REPLACEMENT]);
        assert_eq!(decode(b"a\xFFb"), vec!['a', REPLACEMENT, 'b']);
        // Overlong encodings and surrogate halves are invalid.
        assert_eq!(decode(b"\xC0\xAF"), vec![REPLACEMENT, REPLACEMENT]);
        assert_eq!(decode(b"\xE0\x80\xAF"), vec![REPLACEMENT, REPLACEMENT, REPLACEMENT]);
        assert_eq!(decode(b"\xED\xA0\x80"), vec![REPLACEMENT, REPLACEMENT, REPLACEMENT]);
        // Beyond U+10FFFF.
        assert_eq!(decode(b"\xF4\x90\x80\x80"), vec![REPLACEMENT; 4]);
        assert_eq!(decode(b"\xF0\x9F\x98\x80"), vec!['\u{1F600}']);
    }

    #[test]
    fn invalid_scalars_from_the_abi() {
        assert_eq!(from_u32(0xD800), REPLACEMENT);
        assert_eq!(from_u32(0x110000), REPLACEMENT);
        assert_eq!(from_u32(0x1EA1), 'ạ');
    }
}
