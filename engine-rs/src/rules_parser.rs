// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Parser of input method definition lines, 1:1 with bamboo's rules_parser.go. The Go regular
//! expressions are matched by hand.

use crate::case_map;
use crate::rule::{InputMethod, Rule};
use crate::types::{EffectType, Mark, Tone};
use crate::utf8;
use crate::utils;

/// Tone order used when expanding a mark rule, 1:1 with Go's ParseToneLessRule.
const TONE_ORDER: [Tone; 6] = [Tone::None, Tone::Dot, Tone::Acute, Tone::Grave, Tone::Hook, Tone::Tilde];

/// Definition lines naming a tone, 1:1 with Go's tones map.
const TONE_NAMES: [(&str, Tone); 6] = [
    ("XoaDauThanh", Tone::None),
    ("DauSac", Tone::Acute),
    ("DauHuyen", Tone::Grave),
    ("DauNga", Tone::Tilde),
    ("DauNang", Tone::Dot),
    ("DauHoi", Tone::Hook),
];

/// Groups of Go's `([a-zA-Z]+)_(\p{L}+)([_\p{L}]*)`.
#[derive(Debug, PartialEq)]
pub struct DslMatch {
    pub effective_ons: Vec<char>,
    pub results: Vec<char>,
    pub appended: Vec<char>,
}

/// Leftmost match of `([a-zA-Z]+)_(\p{L}+)([_\p{L}]*)` (letters as the case map knows them).
pub fn match_dsl(line: &[char]) -> Option<DslMatch> {
    for start in 0..line.len() {
        let mut index = start;
        while index < line.len() && utils::is_alpha(line[index]) {
            index += 1;
        }
        let ascii_end = index;
        if ascii_end == start || index >= line.len() || line[index] != '_' {
            continue;
        }
        index += 1;
        let letters_start = index;
        while index < line.len() && case_map::is_letter(line[index]) {
            index += 1;
        }
        let letters_end = index;
        if letters_end == letters_start {
            continue;
        }
        while index < line.len() && (line[index] == '_' || case_map::is_letter(line[index])) {
            index += 1;
        }
        return Some(DslMatch {
            effective_ons: line[start..ascii_end].to_vec(),
            results: line[letters_start..letters_end].to_vec(),
            appended: line[letters_end..index].to_vec(),
        });
    }
    None
}

/// Group 1 of the leftmost match of `_?_(\p{L}+)`.
pub fn match_appending_letters(value: &[char]) -> Option<Vec<char>> {
    for start in 0..value.len() {
        // `_?` is greedy: the one underscore alternative is tried first, like RE2.
        for optional_underscore in [true, false] {
            let mut index = start;
            if optional_underscore {
                if index >= value.len() || value[index] != '_' {
                    continue;
                }
                index += 1;
            }
            if index >= value.len() || value[index] != '_' {
                continue;
            }
            index += 1;
            let letters_start = index;
            while index < value.len() && case_map::is_letter(value[index]) {
                index += 1;
            }
            if index == letters_start {
                continue;
            }
            return Some(value[letters_start..index].to_vec());
        }
    }
    None
}

pub fn tone_from_name(name: &[char]) -> Option<Tone> {
    TONE_NAMES.iter().find(|(text, _)| text.chars().eq(name.iter().copied())).map(|&(_, tone)| tone)
}

pub fn parse_rules(key: char, line: &[char]) -> Vec<Rule> {
    if let Some(tone) = tone_from_name(line) {
        let mut rule = Rule { key, effect_type: EffectType::ToneTransformation, ..Rule::default() };
        rule.set_tone(tone);
        return vec![rule];
    }
    parse_toneless_rules(key, line)
}

pub fn parse_toneless_rules(key: char, line: &[char]) -> Vec<Rule> {
    let mut rules = Vec::new();
    if match_dsl(line).is_some() {
        // Go matches the original line but captures the groups from the lowercased one.
        let Some(parts) = match_dsl(&case_map::lower(line)) else {
            return rules;
        };
        for (&effective_on, &result) in parts.effective_ons.iter().zip(&parts.results) {
            let Some(effect) = utils::find_mark_from_char(result) else {
                continue;
            };
            rules.extend(parse_toneless_rule(key, effective_on, result, effect));
        }
        if let Some(appending) = get_appending_rule(key, &parts.appended) {
            rules.push(appending);
        }
    } else if let Some(appending) = get_appending_rule(key, line) {
        rules.push(appending);
    }
    rules
}

pub fn parse_toneless_rule(key: char, effective_on: char, result: char, effect: Mark) -> Vec<Rule> {
    let mut rules = Vec::new();
    let mark_rule = |effect_on: char, mark: Mark, result: char| {
        let mut rule = Rule { key, effect_type: EffectType::MarkTransformation, effect_on, result, ..Rule::default() };
        rule.set_mark(mark);
        rule
    };
    for c in utils::get_mark_family(effective_on) {
        if c == result {
            rules.push(mark_rule(result, Mark::None, effective_on));
        } else if utils::is_vowel(c) {
            for tone in TONE_ORDER {
                rules.push(mark_rule(utils::add_tone_to_char(c, tone), effect, utils::add_tone_to_char(result, tone)));
            }
        } else {
            rules.push(mark_rule(c, effect, result));
        }
    }
    rules
}

pub fn get_appending_rule(key: char, value: &[char]) -> Option<Rule> {
    let letters = match_appending_letters(value)?;
    let (&first, rest) = letters.split_first()?;
    let appending = |c: char| Rule { key, effect_type: EffectType::Appending, effect_on: c, result: c, ..Rule::default() };
    let mut rule = appending(first);
    rule.appended_rules = rest.iter().map(|&c| appending(c)).collect();
    Some(rule)
}

/// Parses an input method definition (key -> definition line), like Go's ParseInputMethod.
///
/// Go iterates a map here, so the order of the keys is random; the keys are sorted (byte-wise,
/// like the C++ port did) to keep the result deterministic.
pub fn parse_input_method<K: AsRef<[u8]>, V: AsRef<[u8]>>(definition: &[(K, V)]) -> InputMethod {
    let mut entries: Vec<(&[u8], &[u8])> = definition.iter().map(|(k, v)| (k.as_ref(), v.as_ref())).collect();
    entries.sort();
    let mut method = InputMethod::default();
    for (key, value) in entries {
        let keys = utf8::decode(key);
        let Some(&first) = keys.first() else {
            continue;
        };
        let line = utf8::decode(value);
        method.rules.extend(parse_rules(first, &line));
        let lowered = case_map::lower(&line);
        if lowered.windows(2).any(|w| w == ['u', 'o']) {
            method.super_keys.push(first);
        }
        method.keys.push(first);
    }
    for rule in &method.rules {
        if rule.effect_type == EffectType::Appending {
            method.appending_keys.push(rule.key);
        }
        if rule.effect_type == EffectType::ToneTransformation {
            method.tone_keys.push(rule.key);
        }
    }
    method
}
