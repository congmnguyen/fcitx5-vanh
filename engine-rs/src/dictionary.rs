// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Custom word list for the spell check, like Go's sorted `[]string` dictionary.

use crate::case_map;
use crate::utf8;

#[derive(Clone, Debug, Default)]
pub struct Dictionary {
    // Sorted (code point order, which is UTF-8 byte order) and without duplicates.
    words: Vec<Vec<char>>,
}

impl Dictionary {
    fn assign(&mut self, mut words: Vec<Vec<char>>) {
        words.retain(|w| !w.is_empty());
        words.sort();
        words.dedup();
        self.words = words;
    }

    pub fn load_from_words<S: AsRef<[u8]>>(&mut self, words: &[S]) {
        self.assign(words.iter().map(|w| utf8::decode(w.as_ref())).collect());
    }

    /// One word per line ('\n', an optional '\r' before it), lowercased.
    pub fn load_from_text(&mut self, whole_file: &[u8]) {
        let words = whole_file
            .split(|&b| b == b'\n')
            .map(|line| line.strip_suffix(b"\r").unwrap_or(line))
            .filter(|line| !line.is_empty())
            .map(|line| case_map::lower(&utf8::decode(line)))
            .collect();
        self.assign(words);
    }

    fn lower_bound(&self, word: &[char]) -> Option<&Vec<char>> {
        let index = self.words.partition_point(|stored| stored.as_slice() < word);
        self.words.get(index)
    }

    pub fn has_word(&self, word: &[char]) -> bool {
        !word.is_empty() && self.lower_bound(word).is_some_and(|found| found.as_slice() == word)
    }

    pub fn has_word_or_prefix(&self, word: &[char]) -> bool {
        !word.is_empty() && self.lower_bound(word).is_some_and(|found| found.starts_with(word))
    }

    pub fn words(&self) -> &[Vec<char>] {
        &self.words
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
    fn lookup() {
        let mut dictionary = Dictionary::default();
        dictionary.load_from_words(&["việt", "nam", "người", "nam", ""]);
        assert_eq!(dictionary.words().len(), 3);
        assert!(dictionary.has_word(&chars("nam")));
        assert!(dictionary.has_word(&chars("người")));
        assert!(!dictionary.has_word(&chars("ngườ")));
        assert!(!dictionary.has_word(&chars("")));
        assert!(dictionary.has_word_or_prefix(&chars("ngườ")));
        assert!(dictionary.has_word_or_prefix(&chars("việt")));
        assert!(!dictionary.has_word_or_prefix(&chars("việtn")));
        assert!(!dictionary.has_word_or_prefix(&chars("")));
        assert!(!dictionary.has_word_or_prefix(&chars("x")));
    }

    #[test]
    fn load_from_text_lowercases_and_skips_empty_lines() {
        let mut dictionary = Dictionary::default();
        dictionary.load_from_text("Việt\r\n\nNAM\nviệt\n\n".as_bytes());
        assert_eq!(dictionary.words(), &[chars("nam"), chars("việt")]);
        let mut empty = Dictionary::default();
        empty.load_from_text(b"");
        assert!(empty.words().is_empty());
    }
}
