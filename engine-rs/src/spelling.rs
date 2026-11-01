// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Syllable spell checking, 1:1 with bamboo's spelling.go.

use crate::data::{FIRST_CONSONANT_SEQS, LAST_CONSONANT_SEQS, VC_MATRIX, VOWEL_SEQS};
use crate::types::Mark;
use crate::utils;

/// Indexes of the rows that hold a sequence matching `input`.
pub fn lookup(
    sequences: &[&[&str]],
    input: &[char],
    input_is_full: bool,
    input_is_complete: bool,
) -> Vec<usize> {
    let mut result = Vec::new();
    for (index, row) in sequences.iter().enumerate() {
        for token in row.iter() {
            let length = token.chars().count();
            if length < input.len() || (input_is_full && length > input.len()) {
                continue;
            }
            let is_match = input.iter().zip(token.chars()).all(|(&c, t)| {
                c == t
                    || (!input_is_complete && utils::add_mark_to_toneless_char(t, Mark::None) == c)
            });
            if is_match {
                result.push(index);
                break;
            }
        }
    }
    result
}

pub fn is_valid_vc(vowel_indexes: &[usize], last_consonant_indexes: &[usize]) -> bool {
    vowel_indexes
        .iter()
        .filter_map(|&v| VC_MATRIX.get(v))
        .any(|allowed| allowed.iter().any(|&a| last_consonant_indexes.contains(&(a as usize))))
}

pub fn is_valid_cvc(
    first_consonant: &[char],
    vowel: &[char],
    last_consonant: &[char],
    input_is_full_complete: bool,
) -> bool {
    let full = input_is_full_complete;
    let mut first_indexes = Vec::new();
    let mut vowel_indexes = Vec::new();
    let mut last_indexes = Vec::new();
    if !first_consonant.is_empty() {
        let is_full = full || !vowel.is_empty();
        first_indexes = lookup(FIRST_CONSONANT_SEQS, first_consonant, is_full, true);
        if first_indexes.is_empty() {
            return false;
        }
    }
    if !vowel.is_empty() {
        let is_full = full || !last_consonant.is_empty();
        vowel_indexes = lookup(VOWEL_SEQS, vowel, is_full, full);
        if vowel_indexes.is_empty() {
            return false;
        }
    }
    if !last_consonant.is_empty() {
        last_indexes = lookup(LAST_CONSONANT_SEQS, last_consonant, full, true);
        if last_indexes.is_empty() {
            return false;
        }
    }
    if vowel_indexes.is_empty() {
        return !first_indexes.is_empty();
    }
    last_indexes.is_empty() || is_valid_vc(&vowel_indexes, &last_indexes)
}
