// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Case mapping restricted to the letters the engine knows (ASCII, Latin-1, Vietnamese), like the
//! unicode tables Go's bamboo relied on for these code points.

use crate::data::case::{LETTERS, LOWER_UPPER, UPPER_LOWER};

fn lookup(table: &[(char, char)], key: char) -> char {
    match table.binary_search_by(|entry| entry.0.cmp(&key)) {
        Ok(index) => table[index].1,
        Err(_) => key,
    }
}

pub fn to_lower(c: char) -> char {
    if c.is_ascii() {
        c.to_ascii_lowercase()
    } else {
        lookup(&UPPER_LOWER, c)
    }
}

pub fn to_upper(c: char) -> char {
    if c.is_ascii() {
        c.to_ascii_uppercase()
    } else {
        lookup(&LOWER_UPPER, c)
    }
}

pub fn is_upper(c: char) -> bool {
    to_lower(c) != c
}

pub fn is_lower(c: char) -> bool {
    to_upper(c) != c
}

pub fn is_letter(c: char) -> bool {
    if c.is_ascii() {
        c.is_ascii_alphabetic()
    } else {
        LETTERS.binary_search(&c).is_ok()
    }
}

pub fn lower(text: &[char]) -> Vec<char> {
    text.iter().map(|&c| to_lower(c)).collect()
}

pub fn upper(text: &[char]) -> Vec<char> {
    text.iter().map(|&c| to_upper(c)).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_vietnamese_letters() {
        assert_eq!(to_lower('Ữ'), 'ữ');
        assert_eq!(to_upper('đ'), 'Đ');
        assert_eq!(to_upper('ß'), 'ß');
        assert!(is_upper('Ơ') && !is_upper('ơ') && is_lower('ơ'));
        assert!(is_letter('ỹ') && is_letter('ß') && !is_letter('1') && !is_letter('€'));
        assert_eq!(to_lower('Ÿ'), 'ÿ');
        assert_eq!(to_lower('Σ'), 'Σ');
    }

    #[test]
    fn tables_are_sorted_and_inverse() {
        assert!(LOWER_UPPER.windows(2).all(|w| w[0].0 < w[1].0));
        assert!(UPPER_LOWER.windows(2).all(|w| w[0].0 < w[1].0));
        assert!(LETTERS.windows(2).all(|w| w[0] < w[1]));
        for &(lower, upper) in LOWER_UPPER.iter() {
            assert_eq!(to_lower(upper), lower);
        }
    }
}
