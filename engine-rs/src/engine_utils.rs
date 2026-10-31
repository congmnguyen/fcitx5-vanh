// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Rule lookup, syllable validation and undo helpers, 1:1 with bamboo's bamboo_utils.go.

use crate::case_map;
use crate::composition::{
    extract_cvc_trans, extract_last_word, filter_appending_composition, find_last_appending_trans, find_root_target,
    flatten, generate_appending_trans, get_last_tone_transformation, is_free, slice, Composition, WordSplit,
};
use crate::rule::{is_same, make_appending_trans, new_trans, Rule, TransRef, Transformation};
use crate::spelling::is_valid_cvc;
use crate::types::{EffectType, EngineFlags, Mark, Mode, Tone};
use crate::utils;

fn with(composition: &[TransRef], extra: TransRef) -> Composition {
    let mut out = Vec::with_capacity(composition.len() + 1);
    out.extend_from_slice(composition);
    out.push(extra);
    out
}

fn effect(target: Option<TransRef>, rule: Rule) -> TransRef {
    new_trans(Transformation { rule, target, is_upper_case: false })
}

fn virtual_rule(effect_type: EffectType, mark: Option<Mark>, tone: Option<Tone>) -> Rule {
    let mut rule = Rule { key: '\0', effect_type, ..Rule::default() };
    if let Some(mark) = mark {
        rule.set_mark(mark);
    }
    if let Some(tone) = tone {
        rule.set_tone(tone);
    }
    rule
}

pub fn is_valid(composition: &[TransRef], input_is_full_complete: bool) -> bool {
    if composition.len() <= 1 {
        return true;
    }
    // Last tone checking.
    for trans in composition.iter().rev() {
        let t = trans.borrow();
        if t.rule.effect_type == EffectType::ToneTransformation {
            if !has_valid_tone(composition, t.rule.tone()) {
                return false;
            }
            break;
        }
    }
    // Spell checking.
    let cvc = extract_cvc_trans(composition);
    let mode = Mode::VIETNAMESE | Mode::LOWER_CASE | Mode::TONE_LESS;
    is_valid_cvc(
        &flatten(&cvc.first_consonant, mode),
        &flatten(&cvc.vowel, mode),
        &flatten(&cvc.last_consonant, mode),
        input_is_full_complete,
    )
}

pub fn right_most_vowels(composition: &[TransRef]) -> Composition {
    extract_cvc_trans(composition).vowel
}

fn plain_lower(composition: &[TransRef]) -> Vec<char> {
    flatten(composition, Mode::ENGLISH | Mode::LOWER_CASE | Mode::TONE_LESS | Mode::MARK_LESS)
}

pub fn find_tone_target(composition: &[TransRef], std_style: bool) -> Option<TransRef> {
    if composition.is_empty() {
        return None;
    }
    let cvc = extract_cvc_trans(composition);
    let vowels = filter_appending_composition(&cvc.vowel);
    match vowels.len() {
        1 => Some(vowels[0].clone()),
        2 if std_style => {
            let mut target = None;
            for trans in &cvc.vowel {
                let t = trans.borrow();
                if t.rule.result == 'ơ' || t.rule.result == 'ê' {
                    target = Some(t.target.clone().unwrap_or_else(|| trans.clone()));
                }
            }
            target.or_else(|| Some(if cvc.last_consonant.is_empty() { vowels[0].clone() } else { vowels[1].clone() }))
        }
        2 => {
            if !cvc.last_consonant.is_empty() {
                return Some(vowels[1].clone());
            }
            let flattened: String = plain_lower(&vowels).into_iter().collect();
            if matches!(flattened.as_str(), "oa" | "oe" | "uy" | "ue" | "uo") {
                Some(vowels[1].clone())
            } else {
                Some(vowels[0].clone())
            }
        }
        3 => {
            if plain_lower(&vowels) == ['u', 'y', 'e'] {
                Some(vowels[2].clone())
            } else {
                Some(vowels[1].clone())
            }
        }
        _ => None,
    }
}

pub fn has_valid_tone(composition: &[TransRef], tone: Tone) -> bool {
    if matches!(tone, Tone::None | Tone::Acute | Tone::Dot) {
        return true;
    }
    let cvc = extract_cvc_trans(composition);
    if cvc.last_consonant.is_empty() {
        return true;
    }
    let last: String = flatten(&cvc.last_consonant, Mode::ENGLISH | Mode::LOWER_CASE).into_iter().collect();
    // These last consonants only go with the acute and dot tones.
    !matches!(last.as_str(), "c" | "k" | "p" | "t" | "ch")
}

/// A target node and the rule to apply to it.
#[derive(Default)]
pub struct TargetRule {
    pub target: Option<TransRef>,
    pub rule: Rule,
}

pub fn find_mark_target(composition: &[TransRef], rules: &[Rule]) -> TargetRule {
    let text = flatten(composition, Mode::VIETNAMESE);
    for trans in composition.iter().rev() {
        let result = trans.borrow().rule.result;
        for rule in rules {
            if rule.effect_type != EffectType::MarkTransformation {
                continue;
            }
            if result != rule.effect_on || rule.mark() == Mark::None {
                continue;
            }
            let target = find_root_target(trans);
            let with_effect = with(composition, effect(Some(target.clone()), rule.clone()));
            if text == flatten(&with_effect, Mode::VIETNAMESE) {
                continue;
            }
            if is_valid(&with_effect, false) {
                return TargetRule { target: Some(target), rule: rule.clone() };
            }
        }
    }
    TargetRule::default()
}

fn tone_target(composition: &[TransRef], tone: Tone, flags: EngineFlags) -> Option<TransRef> {
    if flags.has(EngineFlags::FREE_TONE_MARKING) {
        if has_valid_tone(composition, tone) {
            return find_tone_target(composition, flags.has(EngineFlags::STD_TONE_STYLE));
        }
        None
    } else {
        find_last_appending_trans(composition).filter(|last| utils::is_vowel(last.borrow().rule.effect_on))
    }
}

pub fn find_target(composition: &[TransRef], applicable_rules: &[Rule], flags: EngineFlags) -> TargetRule {
    let text = flatten(composition, Mode::VIETNAMESE);
    // Find the tone target first.
    for rule in applicable_rules {
        if rule.effect_type != EffectType::ToneTransformation {
            continue;
        }
        let Some(target) = tone_target(composition, rule.tone(), flags) else {
            continue;
        };
        let with_effect = with(composition, effect(Some(target.clone()), rule.clone()));
        if text == flatten(&with_effect, Mode::VIETNAMESE) {
            continue;
        }
        let mut target = Some(target);
        if let Some(t) = &target {
            if rule.tone() == Tone::None
                && is_free(composition, t, EffectType::ToneTransformation)
                && utils::find_tone_from_char(t.borrow().rule.result) == Tone::None
            {
                target = None;
            }
        }
        return TargetRule { target, rule: rule.clone() };
    }
    find_mark_target(composition, applicable_rules)
}

pub fn generate_undo_transformations(composition: &[TransRef], rules: &[Rule], flags: EngineFlags) -> Composition {
    let mut transformations = Vec::new();
    let mode = Mode::VIETNAMESE | Mode::TONE_LESS | Mode::LOWER_CASE;
    let text = flatten(composition, mode);
    for rule in rules {
        match rule.effect_type {
            EffectType::ToneTransformation => {
                let Some(target) = tone_target(composition, rule.tone(), flags) else {
                    continue;
                };
                transformations.push(effect(Some(target), virtual_rule(EffectType::ToneTransformation, None, Some(Tone::None))));
            }
            EffectType::MarkTransformation => {
                for trans in composition.iter().rev() {
                    if trans.borrow().rule.result != rule.effect_on {
                        continue;
                    }
                    let undo = effect(Some(find_root_target(trans)), virtual_rule(EffectType::MarkTransformation, Some(Mark::None), None));
                    let with_effect = with(composition, undo.clone());
                    if text == flatten(&with_effect, mode) {
                        continue;
                    }
                    transformations.push(undo);
                }
            }
            _ => {}
        }
    }
    transformations
}

pub fn generate_transformations(
    composition: &[TransRef],
    applicable_rules: &[Rule],
    flags: EngineFlags,
    lower_key: char,
    is_upper_case: bool,
) -> Composition {
    let mut transformations = Vec::new();
    // Double typing an effect key undoes it and its effects, e.g. w + w -> w.
    if let Some(last) = composition.last() {
        let last_rule = &last.borrow().rule;
        if last_rule.effect_type == EffectType::Appending && last_rule.key == lower_key && last_rule.key != last_rule.result {
            transformations.push(effect(Some(last.clone()), virtual_rule(EffectType::MarkTransformation, Some(Mark::Raw), None)));
            return transformations;
        }
    }
    // A target may be applied by many different transformations, e.g. o + o + w -> ơ.
    let found = find_target(composition, applicable_rules, flags);
    if let Some(target) = found.target {
        let is_mark = found.rule.effect_type == EffectType::MarkTransformation;
        transformations.push(new_trans(Transformation { rule: found.rule, target: Some(target), is_upper_case }));
        if !is_mark {
            return transformations;
        }
        let mut new_composition = composition.to_vec();
        new_composition.extend(transformations.iter().cloned());
        if is_valid(&new_composition, true) {
            return transformations;
        }
        // The uow typing shortcut: a virtual Mark::Horn rule that targets 'u' or 'o'.
        let virtual_found = find_target(&new_composition, applicable_rules, flags);
        if let Some(target) = virtual_found.target {
            let mut rule = virtual_found.rule;
            rule.key = '\0';
            transformations.push(new_trans(Transformation { rule, target: Some(target), is_upper_case: false }));
        }
    } else {
        // ươ/ưo(i/c/ng) + o -> uô.
        if matches_uh_o(&flatten(composition, Mode::VIETNAMESE | Mode::TONE_LESS | Mode::LOWER_CASE)) {
            let vowels = filter_appending_composition(&right_most_vowels(composition));
            if let Some(first_vowel) = vowels.first() {
                let clear_effect =
                    effect(Some(first_vowel.clone()), virtual_rule(EffectType::MarkTransformation, Some(Mark::None), None));
                let with_effect = with(composition, clear_effect.clone());
                let new_found = find_target(&with_effect, applicable_rules, flags);
                if let Some(target) = new_found.target {
                    if !is_same(Some(&target), Some(first_vowel)) {
                        transformations.push(clear_effect);
                        transformations.push(new_trans(Transformation { rule: new_found.rule, target: Some(target), is_upper_case }));
                        return transformations;
                    }
                }
            }
        }
        let undo = generate_undo_transformations(composition, applicable_rules, flags);
        if !undo.is_empty() {
            // An effect key that finds no target undoes its effects, e.g. ươ + w -> uow.
            transformations.extend(undo);
            transformations.push(make_appending_trans(lower_key, is_upper_case));
        }
    }
    transformations
}

pub fn generate_fallback_transformations(applicable_rules: &[Rule], lower_key: char, is_upper_case: bool) -> Composition {
    let trans = generate_appending_trans(applicable_rules, lower_key, is_upper_case);
    let appended = trans.borrow().rule.appended_rules.clone();
    let mut transformations = vec![trans];
    for appended_rule in appended {
        let mut rule = appended_rule;
        let upper = is_upper_case || case_map::is_upper(rule.effect_on);
        rule.key = '\0';
        rule.effect_on = case_map::to_lower(rule.effect_on);
        rule.result = rule.effect_on;
        transformations.push(new_trans(Transformation { rule, target: None, is_upper_case: upper }));
    }
    transformations
}

pub fn refresh_last_tone_target(composition: &[TransRef], std_style: bool) -> Composition {
    let mut transformations = Vec::new();
    // Go compares the vowels with nil, which only happens when the composition holds no vowel.
    let vowels = right_most_vowels(composition);
    let Some(last_tone) = get_last_tone_transformation(composition) else {
        return transformations;
    };
    if vowels.is_empty() {
        return transformations;
    }
    let new_target = find_tone_target(composition, std_style);
    let last = last_tone.borrow();
    if !is_same(last.target.as_ref(), new_target.as_ref()) {
        transformations.push(effect(last.target.clone(), virtual_rule(EffectType::ToneTransformation, None, Some(Tone::None))));
        let mut rule = last.rule.clone();
        rule.key = '\0';
        transformations.push(effect(new_target, rule));
    }
    transformations
}

pub fn extract_last_syllable(composition: &[TransRef]) -> WordSplit {
    let split = extract_last_word(composition, &[]);
    let mut previous = split.previous;
    let last = split.last;
    let mut anchor = 0;
    for i in 0..last.len() {
        if !is_valid(&slice(&last, anchor, i + 1), false) {
            anchor = i;
        }
    }
    if anchor > 0 {
        previous.extend_from_slice(&last[..anchor]);
    }
    WordSplit { previous, last: slice(&last, anchor, last.len()) }
}

fn contains(text: &[char], needle: &[char]) -> bool {
    text.windows(needle.len()).any(|w| w == needle)
}

pub fn matches_u_oh_tail(text: &[char]) -> bool {
    [['u', 'ơ'], ['ư', 'o']].iter().any(|head| {
        (0..text.len().saturating_sub(1)).any(|p| text[p..p + 2] == *head && p + 2 < text.len() && case_map::is_letter(text[p + 2]))
    })
}

pub fn matches_uh_o(text: &[char]) -> bool {
    contains(text, &['ư', 'o']) || contains(text, &['ư', 'ơ'])
}
