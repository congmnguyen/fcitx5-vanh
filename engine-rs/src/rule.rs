// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Rules, transformations and input methods, 1:1 with bamboo's Rule, Transformation and
//! InputMethod.

use crate::types::{EffectType, Mark, Tone};
use std::cell::RefCell;
use std::rc::Rc;

/// What a rule does: a mark or a tone, depending on its effect type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Effect {
    Mark(Mark),
    Tone(Tone),
}

/// One transformation rule. A key of `'\0'` marks a virtual rule that no key produced.
#[derive(Clone, Debug, PartialEq)]
pub struct Rule {
    pub key: char,
    pub effect: Effect,
    pub effect_type: EffectType,
    pub effect_on: char,
    pub result: char,
    pub appended_rules: Vec<Rule>,
}

impl Default for Rule {
    fn default() -> Self {
        Rule {
            key: '\0',
            effect: Effect::Mark(Mark::None),
            effect_type: EffectType::Appending,
            effect_on: '\0',
            result: '\0',
            appended_rules: Vec::new(),
        }
    }
}

impl Rule {
    /// The mark of the rule, `Mark::None` for a tone rule.
    pub fn mark(&self) -> Mark {
        match self.effect {
            Effect::Mark(mark) => mark,
            Effect::Tone(_) => Mark::None,
        }
    }

    /// The tone of the rule, `Tone::None` for a mark rule.
    pub fn tone(&self) -> Tone {
        match self.effect {
            Effect::Tone(tone) => tone,
            Effect::Mark(_) => Tone::None,
        }
    }

    pub fn set_mark(&mut self, mark: Mark) {
        self.effect = Effect::Mark(mark);
    }

    pub fn set_tone(&mut self, tone: Tone) {
        self.effect = Effect::Tone(tone);
    }
}

/// Parsed input method.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct InputMethod {
    /// Rules of every definition line, in key order.
    pub rules: Vec<Rule>,
    /// Keys whose line mentions "uo".
    pub super_keys: Vec<char>,
    /// Keys that produce tone rules.
    pub tone_keys: Vec<char>,
    /// Keys that produce appending rules.
    pub appending_keys: Vec<char>,
    /// Every key of the definition.
    pub keys: Vec<char>,
}

/// A node of a composition. Nodes are compared by identity (`Rc::ptr_eq`), and a few steps of the
/// engine change a node in place, so they live behind `Rc<RefCell<..>>`.
#[derive(Clone, Debug, Default)]
pub struct Transformation {
    pub rule: Rule,
    /// Node the effect applies to, `None` for an appending node.
    pub target: Option<TransRef>,
    /// The key was typed with a capital letter.
    pub is_upper_case: bool,
}

pub type TransRef = Rc<RefCell<Transformation>>;

pub fn new_trans(trans: Transformation) -> TransRef {
    Rc::new(RefCell::new(trans))
}

/// A plain appending node of a key.
pub fn make_appending_trans(key: char, is_upper_case: bool) -> TransRef {
    let effect_type = EffectType::Appending;
    let rule = Rule { key, effect_on: key, result: key, effect_type, ..Rule::default() };
    new_trans(Transformation { rule, target: None, is_upper_case })
}

/// Node identity, Go's pointer comparison (two missing nodes are the same).
pub fn is_same(first: Option<&TransRef>, second: Option<&TransRef>) -> bool {
    match (first, second) {
        (Some(a), Some(b)) => Rc::ptr_eq(a, b),
        (None, None) => true,
        _ => false,
    }
}
