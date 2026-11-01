// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Rebuilds a composition from text already on screen (surrounding text), so typing can resume
//! inside a word.

use crate::case_map;
use crate::composition::Composition;
use crate::engine_utils::find_tone_target;
use crate::rule::{make_appending_trans, new_trans, Rule, Transformation};
use crate::types::{EffectType, Mark, Tone, W2uMode};
use crate::utils;
use std::rc::Rc;

pub fn rebuild_composition_from_text(text: &[char], std_style: bool) -> Composition {
    let mut result = Vec::new();
    // A word break symbol is a word of its own, the letters between two of them are one word.
    let is_break = utils::is_word_break_symbol;
    for word in text.chunk_by(|&a, &b| !is_break(a) && !is_break(b)) {
        // Go tests len(word), the UTF-8 length; every word break symbol is ASCII, so the code
        // point count of the single symbol word is the same.
        if word.len() == 1 && is_break(word[0]) {
            result.push(make_appending_trans(word[0], case_map::is_upper(word[0])));
            continue;
        }
        let mut composition = Vec::new();
        for &c in word {
            let lower = case_map::to_lower(c);
            let tone = utils::find_tone_from_char(lower);
            let mark = utils::find_mark_from_char(lower).filter(|&m| m != Mark::None);
            let mut root = lower;
            if tone != Tone::None {
                root = utils::add_tone_to_char(root, Tone::None);
            }
            if mark.is_some() {
                root = utils::add_mark_to_char(root, Mark::None);
            }
            let append = make_appending_trans(root, case_map::is_upper(c));
            composition.push(append.clone());
            let Some(mark) = mark else {
                continue;
            };
            let mut rule = Rule {
                key: '\0',
                effect_type: EffectType::MarkTransformation,
                effect_on: root,
                result: utils::add_mark_to_toneless_char(root, mark),
                ..Rule::default()
            };
            rule.set_mark(mark);
            let target = Some(append);
            composition.push(new_trans(Transformation { rule, target, is_upper_case: false }));
        }
        let last_tone = word
            .iter()
            .map(|&c| utils::find_tone_from_char(case_map::to_lower(c)))
            .rfind(|&t| t != Tone::None)
            .unwrap_or(Tone::None);
        if last_tone != Tone::None {
            if let Some(target) = find_tone_target(&composition, std_style) {
                let effect_type = EffectType::ToneTransformation;
                let mut rule = Rule { key: '\0', effect_type, ..Rule::default() };
                rule.set_tone(last_tone);
                let target = Some(target);
                composition.push(new_trans(Transformation { rule, target, is_upper_case: false }));
            }
        }
        result.extend(composition);
    }
    result
}

/// Turns the "ư" of a rebuilt word back into the single "w" key that W2U typing produces, so
/// that undoing it behaves like the typed word.
pub fn rewrite_horn_u_as_w2u(composition: &mut Composition, w2u_mode: W2uMode) {
    if w2u_mode == W2uMode::Disabled {
        return;
    }
    let mut i = 0;
    while i < composition.len() {
        let horn_target = {
            let mark = composition[i].borrow();
            mark.target.clone().filter(|target| {
                mark.rule.effect_type == EffectType::MarkTransformation
                    && mark.rule.mark() == Mark::Horn
                    && mark.rule.key == '\0'
                    && target.borrow().rule.effect_on == 'u'
                    && i != 0
                    && Rc::ptr_eq(&composition[i - 1], target)
            })
        };
        let Some(target) = horn_target else {
            i += 1;
            continue;
        };
        // "ươ" comes from "uow"/"uwow", never from a lone "w".
        let before_o = i + 1 < composition.len() && {
            let next = composition[i + 1].borrow();
            next.rule.effect_type == EffectType::Appending && next.rule.effect_on == 'o'
        };
        // Non-Start never turns a word initial "w" into "ư".
        let word_start =
            i == 1 || utils::is_word_break_symbol(composition[i - 2].borrow().rule.result);
        if before_o || (w2u_mode == W2uMode::NonStart && word_start) {
            i += 1;
            continue;
        }
        let mut t = target.borrow_mut();
        t.rule.key = 'w';
        t.rule.effect_on = 'ư';
        t.rule.result = 'ư';
        drop(t);
        composition.remove(i);
    }
}
