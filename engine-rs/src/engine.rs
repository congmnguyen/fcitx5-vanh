// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The composition engine, 1:1 with bamboo's bamboo.go (BambooEngine).

use crate::case_map;
use crate::composition::{
    break_composition, extract_last_word, extract_last_word_with_punctuation_marks, find_last_appending_trans, flatten,
    Composition,
};
use crate::engine_utils::{self, extract_last_syllable, find_target, matches_u_oh_tail, TargetRule};
use crate::rebuild::{rebuild_composition_from_text, rewrite_horn_u_as_w2u};
use crate::rule::{is_same, make_appending_trans, new_trans, InputMethod, Rule, TransRef, Transformation};
use crate::types::{BracketTransformMode, EffectType, EngineFlags, Mark, Mode, W2uMode};
use crate::utils;
use std::rc::Rc;

const BRACKETS: [char; 4] = ['[', ']', '{', '}'];

pub struct Engine {
    composition: Composition,
    input_method: InputMethod,
    flags: EngineFlags,
    w2u_mode: Option<W2uMode>,
    bracket_transform_mode: Option<BracketTransformMode>,
    effect_keys_plain: Vec<char>,
    effect_keys_with_brackets: Vec<char>,
}

impl Engine {
    pub fn new(input_method: InputMethod, flags: EngineFlags) -> Engine {
        let effect_keys_plain = input_method.keys.clone();
        let mut effect_keys_with_brackets = effect_keys_plain.clone();
        effect_keys_with_brackets.extend(BRACKETS);
        Engine {
            composition: Vec::new(),
            input_method,
            flags,
            w2u_mode: None,
            bracket_transform_mode: None,
            effect_keys_plain,
            effect_keys_with_brackets,
        }
    }

    pub fn input_method(&self) -> &InputMethod {
        &self.input_method
    }

    pub fn set_flag(&mut self, flags: EngineFlags) {
        self.flags = flags;
    }

    pub fn flag(&self) -> EngineFlags {
        self.flags
    }

    pub fn set_w2u_mode(&mut self, mode: Option<W2uMode>) {
        self.w2u_mode = mode;
    }

    pub fn set_bracket_transform_mode(&mut self, mode: Option<BracketTransformMode>) {
        self.bracket_transform_mode = mode;
    }

    pub fn composition(&self) -> &Composition {
        &self.composition
    }

    fn is_bracket_enabled(&self) -> bool {
        match self.bracket_transform_mode {
            // The mode that follows the bracket transform engine flag.
            None => self.flags.has(EngineFlags::BRACKET_TRANSFORM),
            Some(BracketTransformMode::Everywhere) => true,
            Some(BracketTransformMode::NonStart) => !self.composition.is_empty(),
            Some(BracketTransformMode::Disabled) => false,
        }
    }

    fn effect_keys(&self) -> &[char] {
        if self.is_bracket_enabled() {
            &self.effect_keys_with_brackets
        } else {
            &self.effect_keys_plain
        }
    }

    pub fn is_valid(&self, input_is_full_complete: bool) -> bool {
        let split = extract_last_word(&self.composition, self.effect_keys());
        engine_utils::is_valid(&split.last, input_is_full_complete)
    }

    pub fn processed_string(&self, mode: Mode) -> Vec<char> {
        if mode.has(Mode::FULL_TEXT) {
            return flatten(&self.composition, mode);
        }
        if mode.has(Mode::PUNCTUATION) {
            let part = extract_last_word_with_punctuation_marks(&self.composition).last;
            return flatten(&part, Mode::VIETNAMESE);
        }
        let part = extract_last_word(&self.composition, self.effect_keys()).last;
        flatten(&part, mode)
    }

    fn applicable_rules(&self, key: char) -> Vec<Rule> {
        let lower_key = case_map::to_lower(key);
        self.input_method.rules.iter().filter(|r| r.key == lower_key).cloned().collect()
    }

    fn find_target_by_key(&self, composition: &[TransRef], key: char) -> TargetRule {
        find_target(composition, &self.applicable_rules(key), self.flags)
    }

    pub fn can_process_key(&self, key: char) -> bool {
        if utils::can_process_key(key, &self.input_method.keys) {
            return true;
        }
        self.is_bracket_enabled() && BRACKETS.contains(&case_map::to_lower(key))
    }

    fn generate_transformations(&self, composition: &[TransRef], lower_key: char, is_upper_case: bool) -> Composition {
        let rules = self.applicable_rules(lower_key);
        let mut transformations =
            engine_utils::generate_transformations(composition, &rules, self.flags, lower_key, is_upper_case);
        if transformations.is_empty() {
            // No applicable rule can be applied, so the key falls back to an appending node.
            transformations = engine_utils::generate_fallback_transformations(&rules, lower_key, is_upper_case);
        }

        if self.is_bracket_enabled() {
            if let Some(last) = composition.last() {
                let l = last.borrow();
                let open_stroke = lower_key == '[' || lower_key == '{';
                let close_stroke = lower_key == ']' || lower_key == '}';
                let open_result = l.rule.result == 'ơ' || l.rule.result == 'Ơ';
                let stroke_result = l.rule.result == 'ư' || l.rule.result == 'Ư';
                let open_bracket = open_stroke && (l.rule.key == '[' || l.rule.key == '{') && open_result;
                let close_bracket = close_stroke && (l.rule.key == ']' || l.rule.key == '}') && stroke_result;
                if open_bracket || close_bracket {
                    let mut rule = Rule { effect_type: EffectType::MarkTransformation, ..Rule::default() };
                    rule.set_mark(Mark::Raw);
                    return vec![new_trans(Transformation { rule, target: Some(last.clone()), is_upper_case: false })];
                }
            }
            if let Some(first) = transformations.first() {
                let mut f = first.borrow_mut();
                if (lower_key == '[' || lower_key == '{') && (f.rule.result == '[' || f.rule.result == '{') {
                    f.rule.result = 'ơ';
                    f.rule.effect_on = 'ơ';
                    if lower_key == '{' {
                        f.is_upper_case = true;
                    }
                }
                if (lower_key == ']' || lower_key == '}') && (f.rule.result == ']' || f.rule.result == '}') {
                    f.rule.result = 'ư';
                    f.rule.effect_on = 'ư';
                    if lower_key == '}' {
                        f.is_upper_case = true;
                    }
                }
            }
        }

        // Keep the old W2U logic for backward compatibility.
        let can_apply_w2u = match self.w2u_mode {
            None => self.flags.has(EngineFlags::W2U),
            Some(mode) => mode == W2uMode::Everywhere || (mode == W2uMode::NonStart && !composition.is_empty()),
        };
        if can_apply_w2u && lower_key == 'w' {
            if let Some(first) = transformations.first() {
                let mut f = first.borrow_mut();
                match f.rule.result {
                    'w' => {
                        f.rule.result = 'ư';
                        f.rule.effect_on = 'ư';
                    }
                    'W' => {
                        f.rule.result = 'Ư';
                        f.rule.effect_on = 'Ư';
                    }
                    _ => {}
                }
            }
        }

        let mut new_composition = composition.to_vec();
        new_composition.extend(transformations.iter().cloned());

        // The "uwo" typing shortcut is a virtual Mark::Horn rule that targets an existing node.
        if let Some(virtual_trans) = self.apply_uow_shortcut(&new_composition) {
            transformations.push(virtual_trans);
        }

        // Sometimes the tone of a previous state must move to fit the new state:
        // chuyr -> chuỷ, then chuyrene -> chuyển.
        let mut refresh_source = composition.to_vec();
        refresh_source.extend(transformations.iter().cloned());
        let refreshed = self.refresh_last_tone_target(&refresh_source);
        transformations.extend(refreshed);
        transformations
    }

    fn new_composition(&self, composition: &[TransRef], key: char, is_upper_case: bool) -> Composition {
        let split = extract_last_syllable(composition);
        let mut syllable = split.last;
        let produced = self.generate_transformations(&syllable, key, is_upper_case);
        syllable.extend(produced);
        let mut result = split.previous;
        result.extend(syllable);
        result
    }

    fn apply_uow_shortcut(&self, syllable: &[TransRef]) -> Option<TransRef> {
        let text = flatten(syllable, Mode::TONE_LESS | Mode::LOWER_CASE);
        let &super_key = self.input_method.super_keys.first()?;
        if !matches_u_oh_tail(&text) {
            return None;
        }
        let found = self.find_target_by_key(syllable, super_key);
        let target = found.target?;
        let mut rule = found.rule;
        rule.key = '\0'; // A virtual rule must not appear in the raw string.
        Some(new_trans(Transformation { rule, target: Some(target), is_upper_case: false }))
    }

    fn refresh_last_tone_target(&self, syllable: &[TransRef]) -> Composition {
        if self.flags.has(EngineFlags::FREE_TONE_MARKING) && engine_utils::is_valid(syllable, false) {
            return engine_utils::refresh_last_tone_target(syllable, self.flags.has(EngineFlags::STD_TONE_STYLE));
        }
        Vec::new()
    }

    pub fn process_string(&mut self, text: &[char], mode: Mode) {
        for &key in text {
            self.process_key(key, mode);
        }
    }

    pub fn process_key(&mut self, key: char, mode: Mode) {
        let lower_key = case_map::to_lower(key);
        let is_upper_case = case_map::is_upper(key);
        if mode.has(Mode::ENGLISH) || !self.can_process_key(lower_key) {
            let trans = make_appending_trans(lower_key, is_upper_case);
            if mode.has(Mode::IN_REVERSE_ORDER) {
                self.composition.insert(0, trans);
            } else {
                self.composition.push(trans);
            }
            return;
        }
        self.composition = self.new_composition(&self.composition, lower_key, is_upper_case);
    }

    pub fn restore_last_word(&mut self, to_vietnamese: bool) {
        // Go splits the last word with the raw input method keys here, so the brackets are not
        // part of the word, unlike in is_valid / processed_string / can_process_key.
        let split = extract_last_word(&self.composition, &self.input_method.keys);
        if split.last.is_empty() {
            return;
        }
        let mut result = split.previous;
        if !to_vietnamese {
            result.extend(break_composition(&split.last));
        } else {
            let mut new_comp = Vec::new();
            for trans in &split.last {
                let (key, upper) = {
                    let t = trans.borrow();
                    (t.rule.key, t.is_upper_case)
                };
                new_comp = self.new_composition(&new_comp, key, upper);
            }
            result.extend(new_comp);
        }
        self.composition = result;
    }

    pub fn reset(&mut self) {
        self.composition.clear();
    }

    pub fn remove_last_char(&mut self, refresh_last_tone_target: bool) {
        let Some(last_appending) = find_last_appending_trans(&self.composition) else {
            return;
        };
        if !self.can_process_key(last_appending.borrow().rule.key) {
            self.composition.pop();
            return;
        }
        // Go also splits the last word with the raw input method keys here.
        let split = extract_last_word(&self.composition, &self.input_method.keys);
        let mut new_comb: Composition = split
            .last
            .iter()
            .filter(|t| !(is_same(t.borrow().target.as_ref(), Some(&last_appending)) || Rc::ptr_eq(t, &last_appending)))
            .cloned()
            .collect();
        if refresh_last_tone_target {
            let refreshed = self.refresh_last_tone_target(&new_comb);
            new_comb.extend(refreshed);
        }
        let mut result = split.previous;
        result.extend(new_comb);
        self.composition = result;
    }

    pub fn rebuild_from_text(&mut self, text: &[char]) {
        self.reset();
        self.composition = rebuild_composition_from_text(text, self.flags.has(EngineFlags::STD_TONE_STYLE));
        let resolved = match self.w2u_mode {
            Some(mode) => mode,
            None if self.flags.has(EngineFlags::W2U) => W2uMode::Everywhere,
            None => W2uMode::Disabled,
        };
        rewrite_horn_u_as_w2u(&mut self.composition, resolved);
    }
}
