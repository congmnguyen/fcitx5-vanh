// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Rebuilds a composition from text already on screen (surrounding text), so typing can resume
//! inside a word.

use crate::case_map;
use crate::composition::Composition;
use crate::engine_utils::find_tone_target;
use crate::rule::{new_trans, Rule, TransRef, Transformation};
use crate::types::{EffectType, Mark, Tone, W2uMode};
use crate::utils;
use std::rc::Rc;

pub fn rebuild_composition_from_text(text: &[char], std_style: bool) -> Composition {
    split_text_into_words(text).iter().flat_map(|word| process_word_to_composition(word, std_style)).collect()
}

pub fn split_text_into_words(text: &[char]) -> Vec<Vec<char>> {
    let mut words = Vec::new();
    let mut current = Vec::new();
    for &c in text {
        if utils::is_word_break_symbol(c) {
            if !current.is_empty() {
                words.push(std::mem::take(&mut current));
            }
            words.push(vec![c]);
        } else {
            current.push(c);
        }
    }
    if !current.is_empty() {
        words.push(current);
    }
    words
}

fn appending(c: char, is_upper_case: bool) -> TransRef {
    new_trans(Transformation {
        rule: Rule { key: c, effect_on: c, result: c, effect_type: EffectType::Appending, ..Rule::default() },
        target: None,
        is_upper_case,
    })
}

pub fn process_word_to_composition(word: &[char], std_style: bool) -> Composition {
    // Go tests len(word), the UTF-8 length; every word break symbol is ASCII, so the code point
    // count of the single symbol word is the same.
    if word.len() == 1 && utils::is_word_break_symbol(word[0]) {
        return vec![appending(word[0], case_map::is_upper(word[0]))];
    }
    let mut composition = Vec::new();
    for &c in word {
        let lower = case_map::to_lower(c);
        let is_upper_case = case_map::is_upper(c);
        let tone = utils::find_tone_from_char(lower);
        let mark = utils::find_mark_from_char(lower).filter(|&m| m != Mark::None);
        let mut root = lower;
        if tone != Tone::None {
            root = utils::add_tone_to_char(root, Tone::None);
        }
        if mark.is_some() {
            root = utils::add_mark_to_char(root, Mark::None);
        }
        let append = appending(root, is_upper_case);
        composition.push(append.clone());
        if let Some(mark) = mark {
            let mut rule = Rule {
                key: '\0',
                effect_type: EffectType::MarkTransformation,
                effect_on: root,
                result: utils::add_mark_to_toneless_char(root, mark),
                ..Rule::default()
            };
            rule.set_mark(mark);
            composition.push(new_trans(Transformation { rule, target: Some(append), is_upper_case: false }));
        }
    }

    let last_tone = word
        .iter()
        .map(|&c| utils::find_tone_from_char(case_map::to_lower(c)))
        .filter(|&t| t != Tone::None)
        .last()
        .unwrap_or(Tone::None);
    if last_tone != Tone::None {
        if let Some(target) = find_tone_target(&composition, std_style) {
            let mut rule = Rule { key: '\0', effect_type: EffectType::ToneTransformation, ..Rule::default() };
            rule.set_tone(last_tone);
            composition.push(new_trans(Transformation { rule, target: Some(target), is_upper_case: false }));
        }
    }
    composition
}

/// Turns the "ư" of a rebuilt word back into the single "w" key that W2U typing produces, so
/// that undoing it behaves like the typed word.
pub fn rewrite_horn_u_as_w2u(composition: &mut Composition, w2u_mode: W2uMode) {
    if w2u_mode == W2uMode::Disabled {
        return;
    }
    let mut i = 0;
    while i < composition.len() {
        let skip = {
            let mark = composition[i].borrow();
            let target = match &mark.target {
                Some(target) => target.clone(),
                None => {
                    i += 1;
                    continue;
                }
            };
            mark.rule.effect_type != EffectType::MarkTransformation
                || mark.rule.mark() != Mark::Horn
                || mark.rule.key != '\0'
                || target.borrow().rule.effect_on != 'u'
                || i == 0
                || !Rc::ptr_eq(&composition[i - 1], &target)
        };
        if skip {
            i += 1;
            continue;
        }
        // "ươ" comes from "uow"/"uwow", never from a lone "w".
        let before_o = i + 1 < composition.len() && {
            let next = composition[i + 1].borrow();
            next.rule.effect_type == EffectType::Appending && next.rule.effect_on == 'o'
        };
        // Non-Start never turns a word initial "w" into "ư".
        let word_start = i == 1 || utils::is_word_break_symbol(composition[i - 2].borrow().rule.result);
        if before_o || (w2u_mode == W2uMode::NonStart && word_start) {
            i += 1;
            continue;
        }
        let target = composition[i].borrow().target.clone().expect("checked above");
        {
            let mut t = target.borrow_mut();
            t.rule.key = 'w';
            t.rule.effect_on = 'ư';
            t.rule.result = 'ư';
        }
        composition.remove(i);
    }
}
