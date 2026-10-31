// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Composition helpers (slicing, syllable parts, word splits), 1:1 with bamboo's
//! composition.go and flattener.go.

use crate::case_map;
use crate::rule::{is_same, make_appending_trans, new_trans, Rule, TransRef, Transformation};
use crate::types::{EffectType, Mark, Mode, Tone};
use crate::utils;
use std::rc::Rc;

pub type Composition = Vec<TransRef>;

/// Onset, rime vowels and coda of a syllable.
#[derive(Default)]
pub struct CvcParts {
    pub first_consonant: Composition,
    pub vowel: Composition,
    pub last_consonant: Composition,
}

/// A composition split before its last word.
#[derive(Default)]
pub struct WordSplit {
    pub previous: Composition,
    pub last: Composition,
}

pub fn slice(source: &[TransRef], begin: usize, end: usize) -> Composition {
    if end <= begin || begin >= source.len() {
        return Vec::new();
    }
    source[begin..end.min(source.len())].to_vec()
}

pub fn find_last_appending_trans(composition: &[TransRef]) -> Option<TransRef> {
    composition.iter().rev().find(|t| t.borrow().rule.effect_type == EffectType::Appending).cloned()
}

pub fn filter_appending_composition(composition: &[TransRef]) -> Composition {
    composition.iter().filter(|t| t.borrow().rule.effect_type == EffectType::Appending).cloned().collect()
}

pub fn find_root_target(target: &TransRef) -> TransRef {
    let mut current = target.clone();
    loop {
        let next = current.borrow().target.clone();
        match next {
            Some(next) => current = next,
            None => return current,
        }
    }
}

pub fn get_last_tone_transformation(composition: &[TransRef]) -> Option<TransRef> {
    composition
        .iter()
        .rev()
        .find(|t| {
            let t = t.borrow();
            t.rule.effect_type == EffectType::ToneTransformation && t.target.is_some()
        })
        .cloned()
}

pub fn is_free(composition: &[TransRef], trans: &TransRef, effect_type: EffectType) -> bool {
    !composition.iter().any(|node| {
        let node = node.borrow();
        is_same(node.target.as_ref(), Some(trans)) && node.rule.effect_type == effect_type
    })
}

pub fn is_in(target: &TransRef, list: &[TransRef]) -> bool {
    list.iter().any(|node| Rc::ptr_eq(node, target))
}

pub fn break_composition(composition: &[TransRef]) -> Composition {
    composition
        .iter()
        .filter_map(|t| {
            let t = t.borrow();
            (t.rule.key != '\0').then(|| make_appending_trans(t.rule.key, t.is_upper_case))
        })
        .collect()
}

pub fn generate_appending_trans(rules: &[Rule], lower_key: char, is_upper_case: bool) -> TransRef {
    for rule in rules {
        if rule.key != lower_key || rule.effect_type != EffectType::Appending {
            continue;
        }
        let mut rule = rule.clone();
        let upper = is_upper_case || case_map::is_upper(rule.effect_on);
        rule.effect_on = case_map::to_lower(rule.effect_on);
        rule.result = rule.effect_on;
        return new_trans(Transformation { rule, target: None, is_upper_case: upper });
    }
    make_appending_trans(lower_key, is_upper_case)
}

fn is_plain_vowel(trans: &TransRef) -> bool {
    let t = trans.borrow();
    t.target.is_none() && utils::is_vowel(t.rule.result)
}

fn is_plain_consonant(trans: &TransRef) -> bool {
    let t = trans.borrow();
    t.target.is_none() && !utils::is_vowel(t.rule.result)
}

fn result_of(trans: &TransRef) -> char {
    trans.borrow().rule.result
}

pub fn extract_cvc_appending_trans(composition: &[TransRef]) -> CvcParts {
    let n = composition.len();
    if n == 0 {
        return CvcParts::default();
    }
    let mut lc_start = n;
    while lc_start > 0 && !is_plain_vowel(&composition[lc_start - 1]) {
        lc_start -= 1;
    }
    let mut vo_start = lc_start;
    while vo_start > 0 && !is_plain_consonant(&composition[vo_start - 1]) {
        vo_start -= 1;
    }
    let mut first_consonant = slice(composition, 0, vo_start);
    let mut vowel = slice(composition, vo_start, lc_start);
    let mut last_consonant = slice(composition, lc_start, n);

    if !last_consonant.is_empty() && vowel.is_empty() && first_consonant.is_empty() {
        first_consonant = std::mem::take(&mut last_consonant);
    }

    if first_consonant.len() == 1 && !vowel.is_empty() {
        let fc = result_of(&first_consonant[0]);
        let v0 = result_of(&vowel[0]);
        let gi = fc == 'g' && v0 == 'i' && vowel.len() > 1 && (result_of(&vowel[1]) != 'e' || last_consonant.is_empty());
        let qu = fc == 'q' && v0 == 'u';
        if gi || qu {
            first_consonant = slice(composition, 0, vo_start + 1);
            vowel = slice(composition, vo_start + 1, lc_start);
        }
    }
    CvcParts { first_consonant, vowel, last_consonant }
}

pub fn extract_cvc_trans(composition: &[TransRef]) -> CvcParts {
    let n = composition.len();
    if n == 0 {
        return CvcParts::default();
    }
    let appending_list: Composition = composition.iter().filter(|t| t.borrow().target.is_none()).cloned().collect();
    let parts = extract_cvc_appending_trans(&appending_list);
    if appending_list.len() == n {
        return parts;
    }
    let mut fc = parts.first_consonant.clone();
    let mut vo = parts.vowel.clone();
    let mut lc = parts.last_consonant.clone();
    for trans in composition {
        let target = match &trans.borrow().target {
            Some(target) => target.clone(),
            None => continue,
        };
        if is_in(&target, &parts.vowel) {
            vo.push(trans.clone());
        } else if is_in(&target, &parts.first_consonant) {
            fc.push(trans.clone());
        } else if is_in(&target, &parts.last_consonant) {
            lc.push(trans.clone());
        }
    }
    CvcParts { first_consonant: fc, vowel: vo, last_consonant: lc }
}

pub fn extract_last_word(composition: &[TransRef], effect_keys: &[char]) -> WordSplit {
    let n = composition.len();
    let mut nodes = Vec::new();
    let text = render(composition, Mode::VIETNAMESE | Mode::LOWER_CASE | Mode::TONE_LESS | Mode::MARK_LESS, Some(&mut nodes));
    for k in (0..nodes.len()).rev() {
        let i = nodes[k];
        let c = text[k];
        if !utils::is_alpha(c) && !utils::in_key_list(effect_keys, c) {
            if i == n - 1 {
                return WordSplit { previous: composition.to_vec(), last: Vec::new() };
            }
            return WordSplit { previous: slice(composition, 0, i + 1), last: slice(composition, i + 1, n) };
        }
    }
    WordSplit { previous: Vec::new(), last: composition.to_vec() }
}

pub fn extract_last_word_with_punctuation_marks(composition: &[TransRef]) -> WordSplit {
    let n = composition.len();
    let mut nodes = Vec::new();
    let text = render(composition, Mode::ENGLISH, Some(&mut nodes));
    for k in (0..nodes.len()).rev() {
        let i = nodes[k];
        if utils::is_space(text[k]) {
            if i == n - 1 {
                return WordSplit { previous: composition.to_vec(), last: Vec::new() };
            }
            return WordSplit { previous: slice(composition, 0, i + 1), last: slice(composition, i + 1, n) };
        }
    }
    WordSplit { previous: Vec::new(), last: composition.to_vec() }
}

/// Renders a composition; `nodes` receives the composition index of every rendered character.
pub fn render(composition: &[TransRef], mode: Mode, mut nodes: Option<&mut Vec<usize>>) -> Vec<char> {
    let is_english = mode.has(Mode::ENGLISH);
    let mut effects: Vec<std::cell::Ref<'_, Transformation>> = Vec::new();
    let mut appending: Vec<(usize, &TransRef)> = Vec::with_capacity(composition.len());
    for (index, trans) in composition.iter().enumerate() {
        let t = trans.borrow();
        if is_english || t.rule.effect_type == EffectType::Appending {
            if t.rule.key == '\0' {
                continue;
            }
            appending.push((index, trans));
        } else if t.target.is_some() {
            effects.push(t);
        }
    }

    let mut canvas = Vec::with_capacity(appending.len());
    for (node_index, appending_trans) in appending {
        let a = appending_trans.borrow();
        let mut c;
        if is_english {
            c = a.rule.key;
        } else {
            c = a.rule.effect_on;
            for effect in &effects {
                if !is_same(effect.target.as_ref(), Some(appending_trans)) {
                    continue;
                }
                match effect.rule.effect_type {
                    EffectType::MarkTransformation => {
                        if effect.rule.mark() == Mark::Raw {
                            c = a.rule.key;
                        } else {
                            c = utils::add_mark_to_char(c, effect.rule.mark());
                        }
                    }
                    EffectType::ToneTransformation => c = utils::add_tone_to_char(c, effect.rule.tone()),
                    _ => {}
                }
            }
        }
        if mode.has(Mode::TONE_LESS) {
            c = utils::add_tone_to_char(c, Tone::None);
        }
        if mode.has(Mode::MARK_LESS) {
            c = utils::add_mark_to_char(c, Mark::None);
        }
        if mode.has(Mode::LOWER_CASE) {
            c = case_map::to_lower(c);
        } else if a.is_upper_case {
            c = case_map::to_upper(c);
        }
        canvas.push(c);
        if let Some(nodes) = nodes.as_deref_mut() {
            nodes.push(node_index);
        }
    }
    canvas
}

pub fn flatten(composition: &[TransRef], mode: Mode) -> Vec<char> {
    render(composition, mode, None)
}
