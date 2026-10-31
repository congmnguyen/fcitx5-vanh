// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Macro table (trigger word -> expansion), like Go's `map[string]string`.

use crate::case_map;
use crate::utf8;
use std::collections::HashMap;

#[derive(Clone, Debug, Default)]
pub struct MacroTable {
    table: HashMap<Vec<char>, Vec<char>>,
}

impl MacroTable {
    pub fn is_empty(&self) -> bool {
        self.table.is_empty()
    }

    /// The expansion of a trigger word, looked up in lowercase (the keys are stored as given).
    pub fn get(&self, key: &[char]) -> Option<&[char]> {
        if key.is_empty() || self.is_empty() {
            return None;
        }
        self.table.get(&case_map::lower(key)).map(Vec::as_slice)
    }

    pub fn set(&mut self, key: &[u8], value: &[u8]) {
        self.table.insert(utf8::decode(key), utf8::decode(value));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chars(s: &str) -> Vec<char> {
        s.chars().collect()
    }

    // Ported from test/lotus-macro-dictionary-test.cpp.
    #[test]
    fn get_and_set() {
        let mut table = MacroTable::default();
        assert!(table.is_empty());
        assert_eq!(table.get(&chars("vn")), None);
        table.set(b"vn", "Việt Nam".as_bytes());
        table.set(b"ko", b"kh\xc3\xb4ng");
        assert!(!table.is_empty());
        assert_eq!(table.get(&chars("VN")), Some(chars("Việt Nam").as_slice()));
        assert_eq!(table.get(&chars("ko")), Some(chars("không").as_slice()));
        assert_eq!(table.get(&chars("")), None);
        assert_eq!(table.get(&chars("x")), None);
        // Keys are stored as given, so an uppercase key never matches.
        table.set(b"TP", b"th\xc3\xa0nh ph\xe1\xbb\x91");
        assert_eq!(table.get(&chars("TP")), None);
        table.set(b"vn", b"VN");
        assert_eq!(table.get(&chars("vn")), Some(chars("VN").as_slice()));
    }
}
