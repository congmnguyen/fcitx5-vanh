// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Character helpers of the engine (vowels, marks, tones, word breaks), 1:1 with bamboo's utils.go.

use crate::case_map;
use crate::data::{mark_family, PUNCTUATION_MARKS, VOWELS};
use crate::types::{Mark, Tone};

pub fn find_vowel_position(c: char) -> Option<usize> {
    VOWELS.iter().position(|&v| v == c)
}

pub fn is_vowel(c: char) -> bool {
    find_vowel_position(c).is_some()
}

/// The mark family row of a character, `_` included.
pub fn find_mark_family(c: char) -> Option<&'static [char]> {
    mark_family(c)
}

/// The mark family of a character without the `_` placeholders.
pub fn get_mark_family(c: char) -> Vec<char> {
    match find_mark_family(c) {
        Some(family) => family.iter().copied().filter(|&m| m != '_').collect(),
        None => Vec::new(),
    }
}

pub fn find_mark_position(c: char) -> Option<usize> {
    find_mark_family(c)?.iter().position(|&m| m == c)
}

pub fn find_mark_from_char(c: char) -> Option<Mark> {
    find_mark_position(c).map(Mark::from_index)
}

pub fn add_mark_to_toneless_char(c: char, mark: Mark) -> char {
    match find_mark_family(c) {
        Some(family) => match family.get(mark as usize) {
            Some(&m) if m != '_' => m,
            _ => c,
        },
        None => c,
    }
}

pub fn add_tone_to_char(c: char, tone: Tone) -> char {
    match find_vowel_position(c) {
        // Go indexes the vowel table without a bound check; the target always stays inside it.
        Some(position) => VOWELS.get(position + tone as usize - position % 6).copied().unwrap_or(c),
        None => c,
    }
}

pub fn add_mark_to_char(c: char, mark: Mark) -> char {
    let tone = find_tone_from_char(c);
    let bare = add_mark_to_toneless_char(add_tone_to_char(c, Tone::None), mark);
    add_tone_to_char(bare, tone)
}

pub fn find_tone_from_char(c: char) -> Tone {
    match find_vowel_position(c) {
        Some(position) => Tone::from_index(position % 6),
        None => Tone::None,
    }
}

pub fn is_punctuation_mark(c: char) -> bool {
    PUNCTUATION_MARKS.contains(&c)
}

pub fn is_word_break_symbol(c: char) -> bool {
    is_punctuation_mark(c) || c.is_ascii_digit()
}

pub fn is_alpha(c: char) -> bool {
    c.is_ascii_alphabetic()
}

pub fn is_space(c: char) -> bool {
    c == ' '
}

pub fn in_key_list(keys: &[char], key: char) -> bool {
    keys.contains(&key)
}

pub fn can_process_key(lower_key: char, effect_keys: &[char]) -> bool {
    if is_alpha(lower_key) || in_key_list(effect_keys, lower_key) {
        return true;
    }
    if is_word_break_symbol(lower_key) {
        return false;
    }
    is_vietnamese_rune(lower_key)
}

pub fn is_vietnamese_rune(lower_key: char) -> bool {
    if lower_key.is_ascii() {
        return false;
    }
    if find_tone_from_char(lower_key) != Tone::None {
        return true;
    }
    lower_key != add_mark_to_toneless_char(lower_key, Mark::None)
}

pub fn has_any_vietnamese_rune(word: &[char]) -> bool {
    word.iter().any(|&c| is_vietnamese_rune(case_map::to_lower(c)))
}

pub fn has_any_vietnamese_vowel(word: &[char]) -> bool {
    word.iter().any(|&c| is_vowel(case_map::to_lower(c)))
}

#[cfg(test)]
mod tests {
    use super::*;

    // Ported from test/lotus-utils-test.cpp.
    #[test]
    fn tones_and_marks() {
        assert_eq!(find_tone_from_char('á'), Tone::Acute);
        assert_eq!(find_tone_from_char('ọ'), Tone::Dot);
        assert_eq!(find_tone_from_char('ừ'), Tone::Grave);
        assert_eq!(find_tone_from_char('a'), Tone::None);
        assert_eq!(find_tone_from_char('b'), Tone::None);
        assert_eq!(find_tone_from_char('ỷ'), Tone::Hook);
        assert_eq!(add_tone_to_char('ư', Tone::Tilde), 'ữ');
        assert_eq!(add_tone_to_char('b', Tone::Acute), 'b');
        assert_eq!(add_mark_to_char('ầ', Mark::Breve), 'ằ');
        assert_eq!(add_mark_to_char('ợ', Mark::None), 'ọ');
        assert_eq!(add_mark_to_char('d', Mark::Dash), 'đ');
        assert_eq!(add_mark_to_char('e', Mark::Horn), 'e');
        assert!(is_vowel('y') && is_vowel('ỵ') && !is_vowel('đ') && !is_vowel('Á'));
    }

    #[test]
    fn word_breaks_and_vietnamese_runes() {
        assert!(is_word_break_symbol('3') && is_word_break_symbol(' ') && is_word_break_symbol('|'));
        assert!(!is_word_break_symbol('a') && !is_word_break_symbol('\t'));
        assert!(is_vietnamese_rune('đ') && is_vietnamese_rune('á') && is_vietnamese_rune('ơ'));
        assert!(!is_vietnamese_rune('a') && !is_vietnamese_rune('ç'));
        assert!(has_any_vietnamese_rune(&['T', 'Ư']));
        assert!(has_any_vietnamese_vowel(&['B', 'Ô']));
        assert!(can_process_key('w', &[]) && can_process_key('1', &['1']) && !can_process_key('1', &[]));
        assert!(can_process_key('ơ', &[]) && !can_process_key('€', &[]));
        assert_eq!(get_mark_family('ơ'), vec!['o', 'ô', 'ơ']);
        assert_eq!(find_mark_from_char('ư'), Some(Mark::Horn));
        assert_eq!(find_mark_from_char('x'), None);
    }

    #[test]
    fn vowel_table_shape() {
        // Six tones per vowel, the tone-less form first.
        for chunk in VOWELS.chunks(6) {
            assert_eq!(find_tone_from_char(chunk[0]), Tone::None);
            for (i, &v) in chunk.iter().enumerate() {
                assert_eq!(add_tone_to_char(chunk[0], Tone::from_index(i)), v);
            }
        }
    }
}
