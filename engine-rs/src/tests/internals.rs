// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Ported from test/lotus-rules-parser-test.cpp, lotus-spelling-test.cpp, lotus-flattener-test.cpp,
//! lotus-engine-utils-test.cpp, lotus-rebuild-test.cpp and lotus-data-test.cpp.

use crate::composition::{
    extract_last_word, extract_last_word_with_punctuation_marks, flatten, slice, Composition,
};
use crate::data::{
    self, FIRST_CONSONANT_SEQS, INPUT_METHOD_DEFINITIONS, LAST_CONSONANT_SEQS, VOWEL_SEQS,
};
use crate::engine::Engine;
use crate::engine_utils::*;
use crate::input_method::cached_input_method;
use crate::rebuild::rebuild_composition_from_text;
use crate::rule::{
    is_same, make_appending_trans, new_trans, Effect, Rule, TransRef, Transformation,
};
use crate::rules_parser::*;
use crate::spelling::{is_valid_cvc, is_valid_vc, lookup};
use crate::test_support::{chars, string};
use crate::types::{EffectType, EngineFlags, InputMethodId, Mark, Mode, Tone, W2uMode};
use std::rc::Rc;

const VN: Mode = Mode::VIETNAMESE;

fn app(c: char) -> TransRef {
    make_appending_trans(c, false)
}

fn effect(target: &TransRef, effect_type: EffectType, effect: Effect) -> TransRef {
    new_trans(Transformation {
        rule: Rule { effect_type, effect, ..Rule::default() },
        target: Some(target.clone()),
        is_upper_case: false,
    })
}

fn rule_effect(target: &TransRef, rule: &Rule) -> TransRef {
    new_trans(Transformation {
        rule: rule.clone(),
        target: Some(target.clone()),
        is_upper_case: false,
    })
}

fn appending(key: char, effect_on: char, upper: bool) -> TransRef {
    new_trans(Transformation {
        rule: Rule {
            key,
            effect_on,
            result: effect_on,
            effect_type: EffectType::Appending,
            ..Rule::default()
        },
        target: None,
        is_upper_case: upper,
    })
}

#[track_caller]
fn canvas(composition: &[TransRef], mode: Mode, expected: &str) {
    assert_eq!(string(&flatten(composition, mode)), expected);
}

fn tone_rule(key: char, tone: Tone) -> Rule {
    let mut r = Rule { key, effect_type: EffectType::ToneTransformation, ..Rule::default() };
    r.set_tone(tone);
    r
}

fn mark_rule(key: char, effect_on: char, result: char, mark: Mark) -> Rule {
    let mut r = Rule {
        key,
        effect_type: EffectType::MarkTransformation,
        effect_on,
        result,
        ..Rule::default()
    };
    r.set_mark(mark);
    r
}

fn rules_for(key: char) -> Vec<Rule> {
    cached_input_method(InputMethodId::Telex)
        .rules
        .iter()
        .filter(|r| r.key == key)
        .cloned()
        .collect()
}

fn same(a: &Option<TransRef>, b: &TransRef) -> bool {
    is_same(a.as_ref(), Some(b))
}

#[test]
fn rules_parser() {
    let rules = parse_rules('z', &chars("XoaDauThanh"));
    assert_eq!(rules.len(), 1);
    assert_eq!(rules[0].effect_type, EffectType::ToneTransformation);
    assert_eq!(rules[0].effect, Effect::Tone(Tone::None));
    let tilde = parse_rules('x', &chars("DauNga"));
    assert_eq!(tilde.len(), 1);
    assert_eq!(tilde[0].tone(), Tone::Tilde);

    let check_mark = |rules: &[Rule], index: usize, key: char, mark: Mark, effect_on: char| {
        let r = &rules[index];
        assert!(
            r.key == key
                && r.effect_type == EffectType::MarkTransformation
                && r.mark() == mark
                && r.effect_on == effect_on,
            "{index}"
        );
    };
    let dash = parse_toneless_rules('d', &chars("D_Đ"));
    assert_eq!(dash.len(), 2);
    check_mark(&dash, 0, 'd', Mark::Dash, 'd');
    let appending_rule = parse_toneless_rules('{', &chars("_Ư"));
    assert_eq!(appending_rule.len(), 1);
    assert!(
        appending_rule[0].effect_type == EffectType::Appending
            && appending_rule[0].effect_on == 'Ư'
    );
    let word = parse_toneless_rules('w', &chars("UOA_ƯƠĂ"));
    assert_eq!(word.len(), 33);
    check_mark(&word, 0, 'w', Mark::Horn, 'u');
    check_mark(&word, 7, 'w', Mark::Horn, 'o');
    check_mark(&word, 20, 'w', Mark::Breve, 'a');
    let with_appending = parse_toneless_rules('w', &chars("UOA_ƯƠĂ__Ư"));
    assert_eq!(with_appending.len(), 34);
    check_mark(&with_appending, 20, 'w', Mark::Breve, 'a');
    assert!(
        with_appending[33].key == 'w'
            && with_appending[33].effect_type == EffectType::Appending
            && with_appending[33].effect_on == 'ư'
    );

    let lower = parse_toneless_rules('[', &chars("__ươ"));
    assert_eq!(lower.len(), 1);
    assert_eq!(lower[0].appended_rules.len(), 1);
    assert!(lower[0].appended_rules[0].key == '[' && lower[0].appended_rules[0].effect_on == 'ơ');
    let upper = parse_toneless_rules('{', &chars("__ƯƠ"));
    assert!(upper.len() == 1 && upper[0].appended_rules[0].effect_on == 'Ơ');

    let m = match_dsl(&chars("a_â")).unwrap();
    assert!(m.effective_ons == chars("a") && m.results == chars("â") && m.appended.is_empty());
    let m = match_dsl(&chars("UOA_ƯƠĂ")).unwrap();
    assert!(m.effective_ons == chars("UOA") && m.results == chars("ƯƠĂ") && m.appended.is_empty());
    let m = match_dsl(&chars("uoa_ươă__ư")).unwrap();
    assert!(m.results == chars("ươă") && m.appended == chars("__ư"));
    let m = match_dsl(&chars("AEO_ÂÊÔ")).unwrap();
    assert!(m.effective_ons == chars("AEO") && m.results == chars("ÂÊÔ"));
    let m = match_dsl(&chars("D_Đ")).unwrap();
    assert!(m.effective_ons == chars("D") && m.results == chars("Đ"));
    assert!(
        match_dsl(&chars("__ă")).is_none()
            && match_dsl(&[]).is_none()
            && match_dsl(&chars("abc")).is_none()
    );
    assert_eq!(match_appending_letters(&chars("__ă")), Some(chars("ă")));
    assert_eq!(match_appending_letters(&chars("_ă")), Some(chars("ă")));
    assert_eq!(match_appending_letters(&chars("abc")), None);
    // Letters of the second half of the Vietnamese range (the case table had two literals).
    assert_eq!(match_appending_letters(&chars("_ểỹ")), Some(chars("ểỹ")));

    let telex = parse_input_method(INPUT_METHOD_DEFINITIONS[InputMethodId::Telex as usize]);
    assert_eq!(telex.keys.len(), 11);
    assert!(!telex.rules.is_empty());
    assert!(std::ptr::eq(
        cached_input_method(InputMethodId::Telex),
        cached_input_method(InputMethodId::Telex)
    ));
    assert_eq!(*cached_input_method(InputMethodId::Telex), telex);
    assert!(cached_input_method(InputMethodId::Custom).rules.is_empty());
    assert_eq!(telex.super_keys, vec!['w']);
}

#[test]
fn spelling() {
    let contains = |v: Vec<usize>, i: usize| assert!(v.contains(&i), "{v:?} {i}");
    contains(lookup(FIRST_CONSONANT_SEQS, &chars("ng"), true, true), 0);
    contains(lookup(VOWEL_SEQS, &chars("a"), false, true), 1);
    contains(lookup(VOWEL_SEQS, &chars("uyê"), true, true), 1);
    contains(lookup(VOWEL_SEQS, &chars("oă"), true, true), 3);
    contains(lookup(VOWEL_SEQS, &chars("uơ"), true, true), 4);
    contains(lookup(LAST_CONSONANT_SEQS, &chars("c"), false, true), 0);
    contains(lookup(LAST_CONSONANT_SEQS, &chars("c"), true, true), 1);
    contains(lookup(LAST_CONSONANT_SEQS, &chars("ch"), true, true), 0);
    assert!(lookup(FIRST_CONSONANT_SEQS, &chars("zzz"), true, true).is_empty());
    contains(lookup(VOWEL_SEQS, &chars("a"), true, false), 2);
    assert!(is_valid_vc(&[1], &[1]));
    assert!(is_valid_vc(&[1], &[0]));
    assert!(!is_valid_vc(&[1], &[4]));
    for (fc, v, lc, want) in [
        ("ng", "", "", true),
        ("q", "", "", false),
        ("zzz", "a", "", false),
        ("", "a", "", true),
        ("", "uyê", "", true),
        ("", "oă", "", true),
        ("", "uơ", "", true),
        ("", "a", "ch", true),
        ("x", "a", "c", true),
        ("ng", "a", "ch", true),
        ("b", "zzz", "", false),
        ("", "a", "zzz", false),
        ("", "", "", false),
    ] {
        assert_eq!(is_valid_cvc(&chars(fc), &chars(v), &chars(lc), true), want, "{fc}|{v}|{lc}");
    }
}

#[test]
fn flattener() {
    canvas(&[app('a')], VN, "a");
    let a = app('a');
    let hat = effect(&a, EffectType::MarkTransformation, Effect::Mark(Mark::Hat));
    let acute = effect(&a, EffectType::ToneTransformation, Effect::Tone(Tone::Acute));
    canvas(&[a.clone(), hat.clone()], VN, "â");
    canvas(&[a.clone(), hat.clone(), acute.clone()], VN, "ấ");
    canvas(&[a.clone(), hat.clone(), acute.clone()], VN | Mode::TONE_LESS, "â");
    canvas(&[a.clone(), hat, acute], VN | Mode::TONE_LESS | Mode::MARK_LESS, "a");
    canvas(&[appending('w', 'a', true)], VN, "A");
    canvas(&[appending('w', 'A', true)], VN | Mode::LOWER_CASE, "a");
    let horn = appending('w', 'ư', false);
    canvas(
        &[horn.clone(), effect(&horn, EffectType::MarkTransformation, Effect::Mark(Mark::Raw))],
        VN,
        "w",
    );
    canvas(
        &[horn.clone(), effect(&horn, EffectType::MarkTransformation, Effect::Mark(Mark::Horn))],
        Mode::ENGLISH,
        "w",
    );
    canvas(&[appending('\0', 'x', false)], VN, "");
    canvas(&[appending('\0', 'x', false)], Mode::ENGLISH, "");

    let three: Composition = vec![app('a'), app('b'), app('c')];
    assert_eq!(slice(&three, 1, 3).len(), 2);
    assert!(slice(&three, 4, 6).is_empty());
    assert!(slice(&three, 2, 1).is_empty());
    let space = app(' ');
    let parts = extract_last_word(&[app('a'), space.clone(), app('b')], &[]);
    assert!(parts.previous.len() == 2 && parts.last.len() == 1);
    let single = extract_last_word(&three, &[]);
    assert!(single.previous.is_empty() && single.last.len() == 3);
    let trailing = extract_last_word(&[app('a'), app('b'), space.clone()], &[]);
    assert!(trailing.previous.len() == 3 && trailing.last.is_empty());
    let two = extract_last_word(&[app('a'), space.clone(), app('b'), space.clone()], &[]);
    assert!(two.previous.len() == 4 && two.last.is_empty());
    let empty = extract_last_word(&[], &[]);
    assert!(empty.previous.is_empty() && empty.last.is_empty());
    let marks = extract_last_word_with_punctuation_marks(&[app('a'), space, app('b')]);
    assert!(marks.previous.len() == 2 && marks.last.len() == 1);
}

#[test]
fn engine_utils() {
    let w_rules = rules_for('w');
    let o_rules = rules_for('o');
    assert_eq!(w_rules.len(), 33);
    assert_eq!(o_rules.len(), 13);
    assert!(
        w_rules[0].mark() == Mark::Horn && w_rules[0].effect_on == 'u' && w_rules[0].result == 'ư'
    );
    assert!(
        w_rules[7].mark() == Mark::Horn && w_rules[7].effect_on == 'o' && w_rules[7].result == 'ơ'
    );
    assert!(
        o_rules[0].mark() == Mark::Hat && o_rules[0].effect_on == 'o' && o_rules[0].result == 'ô'
    );
    assert!(
        o_rules[7].mark() == Mark::Hat && o_rules[7].effect_on == 'ơ' && o_rules[7].result == 'ô'
    );
    let std = EngineFlags::STD;

    let ba = vec![app('b'), app('a')];
    assert!(is_valid(&ba, false) && is_valid(&ba, true));
    let a = app('a');
    let ac = vec![
        a.clone(),
        effect(&a, EffectType::ToneTransformation, Effect::Tone(Tone::Grave)),
        app('c'),
    ];
    assert!(!is_valid(&ac, false));
    assert!(is_valid(&[], false));

    let (a, o, n) = (app('a'), app('o'), app('n'));
    assert!(find_tone_target(&[], false).is_none());
    assert!(same(&find_tone_target(&[app('b'), a.clone()], false), &a));
    assert!(same(&find_tone_target(&[o.clone(), a.clone()], false), &a));
    assert!(same(&find_tone_target(&[o.clone(), a.clone()], true), &o));
    assert!(same(&find_tone_target(&[o.clone(), a.clone(), n.clone()], false), &a));

    let (a, c, h, n) = (app('a'), app('c'), app('h'), app('n'));
    let grave = effect(&a, EffectType::ToneTransformation, Effect::Tone(Tone::Grave));
    let dot = effect(&a, EffectType::ToneTransformation, Effect::Tone(Tone::Dot));
    let a_grave = vec![a.clone(), grave.clone()];
    let plus =
        |extra: &[TransRef]| -> Composition { a_grave.iter().chain(extra).cloned().collect() };
    assert!(has_valid_tone(&a_grave, Tone::Acute));
    assert!(has_valid_tone(&[a.clone(), dot, c.clone()], Tone::Dot));
    assert!(has_valid_tone(&a_grave, Tone::Grave));
    assert!(!has_valid_tone(&plus(&[c.clone()]), Tone::Grave));
    assert!(!has_valid_tone(&plus(&[c.clone(), h.clone()]), Tone::Grave));
    assert!(has_valid_tone(&plus(&[n.clone()]), Tone::Grave));

    let a = app('a');
    let t_rule = tone_rule('f', Tone::Grave);
    let found = find_target(&[a.clone()], std::slice::from_ref(&t_rule), std);
    assert!(same(&found.target, &a));
    assert!(
        found.rule.effect_type == EffectType::ToneTransformation
            && found.rule.tone() == Tone::Grave
            && found.rule.key == 'f'
    );
    let m_rule = mark_rule('w', 'a', 'ă', Mark::Breve);
    let found = find_target(&[a.clone()], &[m_rule], std);
    assert!(same(&found.target, &a));
    assert!(
        found.rule.effect_type == EffectType::MarkTransformation
            && found.rule.mark() == Mark::Breve
            && found.rule.effect_on == 'a'
    );
    let none = find_target(&[app('b')], &[t_rule], EngineFlags(0));
    assert!(none.target.is_none());
    assert!(
        none.rule.key == '\0'
            && none.rule.mark() == Mark::None
            && none.rule.effect_type == EffectType::Appending
    );

    let (u, o) = (app('u'), app('o'));
    let uo = vec![u.clone(), o.clone()];
    let applied = generate_transformations(&uo, &w_rules, std, 'w', false);
    assert_eq!(applied.len(), 1);
    assert!(
        applied[0].borrow().rule.mark() == Mark::Horn && applied[0].borrow().rule.effect_on == 'o'
    );
    assert!(same(&applied[0].borrow().target, &o));
    canvas(&[uo.clone(), applied].concat(), VN, "uơ");
    let double_key = appending('w', 'ư', false);
    let doubled = generate_transformations(&[double_key.clone()], &w_rules, std, 'w', false);
    assert_eq!(doubled.len(), 1);
    assert!(
        doubled[0].borrow().rule.mark() == Mark::Raw
            && same(&doubled[0].borrow().target, &double_key)
    );
    canvas(&[vec![double_key], doubled].concat(), VN, "w");
    let horned =
        vec![u.clone(), rule_effect(&u, &w_rules[0]), o.clone(), rule_effect(&o, &w_rules[7])];
    canvas(&horned, VN, "ươ");
    assert!(matches_uh_o(&flatten(&horned, VN | Mode::TONE_LESS | Mode::LOWER_CASE)));
    let hatted = generate_transformations(&horned, &o_rules, std, 'o', false);
    assert_eq!(hatted.len(), 2);
    assert!(hatted[0].borrow().rule.mark() == Mark::None && same(&hatted[0].borrow().target, &u));
    assert!(hatted[1].borrow().rule.mark() == Mark::Hat && same(&hatted[1].borrow().target, &o));
    canvas(&[horned, hatted].concat(), VN, "uô");

    let a = app('a');
    let a_grave =
        vec![a.clone(), effect(&a, EffectType::ToneTransformation, Effect::Tone(Tone::Grave))];
    let undo = generate_undo_transformations(&a_grave, &[tone_rule('f', Tone::Grave)], std);
    assert_eq!(undo.len(), 1);
    assert!(undo[0].borrow().rule.tone() == Tone::None && same(&undo[0].borrow().target, &a));
    canvas(&[a_grave, undo].concat(), VN, "a");
    let a_hat = vec![a.clone(), rule_effect(&a, &mark_rule('a', 'a', 'â', Mark::Hat))];
    let hat_rule = mark_rule('a', 'â', 'a', Mark::Hat);
    let undo = generate_undo_transformations(&a_hat, std::slice::from_ref(&hat_rule), std);
    assert_eq!(undo.len(), 1);
    assert!(undo[0].borrow().rule.mark() == Mark::None && same(&undo[0].borrow().target, &a));
    canvas(&[a_hat, undo].concat(), VN, "a");
    assert!(generate_undo_transformations(&[a.clone()], &[hat_rule], std).is_empty());

    let plain = generate_fallback_transformations(&[], 'z', false);
    assert_eq!(plain.len(), 1);
    canvas(&plain, VN, "z");
    let mut appending_rule = Rule {
        key: 'w',
        effect_type: EffectType::Appending,
        effect_on: 'ư',
        result: 'ư',
        ..Rule::default()
    };
    appending_rule.appended_rules.push(Rule {
        key: 'w',
        effect_type: EffectType::Appending,
        effect_on: 'ơ',
        result: 'ơ',
        ..Rule::default()
    });
    let with_appended = generate_fallback_transformations(&[appending_rule], 'w', false);
    assert_eq!(with_appended.len(), 2);
    assert!(
        with_appended[1].borrow().rule.key == '\0'
            && with_appended[1].borrow().rule.effect_on == 'ơ'
    );
    canvas(&with_appended, VN, "ư");
    let upper = Rule {
        key: 'w',
        effect_type: EffectType::Appending,
        effect_on: 'Ư',
        result: 'Ư',
        ..Rule::default()
    };
    canvas(&generate_fallback_transformations(&[upper], 'w', false), VN, "Ư");

    let (o, a) = (app('o'), app('a'));
    assert_eq!(right_most_vowels(&[app('u'), app('o')]).len(), 2);
    assert!(right_most_vowels(&[]).is_empty());
    assert!(right_most_vowels(&[app('b')]).is_empty());
    let grave_on_o = effect(&o, EffectType::ToneTransformation, Effect::Tone(Tone::Grave));
    let grave_on_a = effect(&a, EffectType::ToneTransformation, Effect::Tone(Tone::Grave));
    let base = vec![o.clone(), a.clone(), grave_on_o];
    let moved = refresh_last_tone_target(&base, false);
    assert_eq!(moved.len(), 2);
    assert!(same(&moved[0].borrow().target, &o) && moved[0].borrow().rule.tone() == Tone::None);
    assert!(same(&moved[1].borrow().target, &a) && moved[1].borrow().rule.tone() == Tone::Grave);
    canvas(&[base, moved].concat(), VN, "oà");
    assert!(refresh_last_tone_target(&[o, a, grave_on_a], false).is_empty());

    let (b, a, space) = (app('b'), app('a'), app(' '));
    let split = extract_last_syllable(&[b.clone(), a.clone(), space.clone(), app('b'), app('a')]);
    assert!(split.previous.len() == 3 && split.last.len() == 2);
    let split = extract_last_syllable(&[b.clone(), a.clone(), app('.'), app('b'), app('a')]);
    assert!(split.previous.len() == 3 && split.last.len() == 2);
    let broken = vec![b, a, space, app('b'), app('b')];
    let split = extract_last_syllable(&broken);
    assert!(
        split.previous.len() == 4
            && split.last.len() == 1
            && Rc::ptr_eq(&split.last[0], &broken[4])
    );

    for (text, want) in [
        ("uơ", false),
        ("uơn", true),
        ("uơ1", false),
        ("muơn", true),
        ("ưong", true),
        ("ưo", false),
        ("ưo ", false),
        ("xưa", false),
        ("", false),
    ] {
        assert_eq!(matches_u_oh_tail(&chars(text)), want, "{text}");
    }
    for (text, want) in
        [("ưo", true), ("ươ", true), ("xươn", true), ("uơ", false), ("ư", false), ("", false)]
    {
        assert_eq!(matches_uh_o(&chars(text)), want, "{text}");
    }
}

fn rebuilt(text: &str) -> String {
    string(&flatten(&rebuild_composition_from_text(&chars(text), true), VN))
}

#[test]
fn rebuild() {
    assert_eq!(rebuilt("goo"), "goo");
    for text in [
        "chào",
        "việt",
        "google",
        "đường",
        "người",
        "as",
        "được",
        "những",
        "ước",
        "ươi",
        "Việt",
        "OO",
        "DD",
        "Nội",
    ] {
        assert_eq!(rebuilt(text), text);
    }
    for text in [
        "vãi",
        "vãi.",
        "vãi ",
        "vãi. ",
        "chào.",
        "chào. ",
        "chào, ",
        "chào. Xin",
        "vãi, ",
        "vãi! ",
        "vãi? ",
        "vãi; ",
        "vãi: ",
    ] {
        assert_eq!(rebuilt(text), text);
    }
    let telex = || Engine::new(cached_input_method(InputMethodId::Telex).clone(), EngineFlags::STD);
    let mut e = telex();
    e.rebuild_from_text(&chars("go"));
    assert_eq!(string(&e.processed_string(VN)), "go");
    e.process_key('s', VN);
    assert_eq!(string(&e.processed_string(VN)), "gó");

    let mut e = telex();
    for (text, key, want) in [("tư", 'w', "tw"), ("tư", 'f', "từ"), ("dươ", 'w', "duow")] {
        e.rebuild_from_text(&chars(text));
        e.process_key(key, VN);
        assert_eq!(string(&e.processed_string(VN)), want, "{text}+{key}");
    }
    e.set_w2u_mode(Some(W2uMode::NonStart));
    e.rebuild_from_text(&chars("ư"));
    e.process_key('w', VN);
    assert_eq!(string(&e.processed_string(VN)), "uw");

    let mut e = telex();
    e.rebuild_from_text(&chars("goo"));
    assert_eq!(string(&e.processed_string(VN)), "goo");

    let full = VN | Mode::FULL_TEXT;
    let mut e = telex();
    for text in ["vãi", "vãi.", "vãi ", "vãi. ", "chào.", "chào. "] {
        e.rebuild_from_text(&chars(text));
        assert_eq!(string(&e.processed_string(full)), text);
    }
    for text in ["chào", "việt", "đường", "người", "chào. Xin"] {
        let mut rebuilt_engine = telex();
        rebuilt_engine.rebuild_from_text(&chars(text));
        let mut typed = telex();
        typed.process_string(&chars(text), full);
        assert_eq!(string(&rebuilt_engine.processed_string(full)), text);
        assert_eq!(rebuilt_engine.processed_string(full), typed.processed_string(full));
    }
}

#[test]
fn data_tables() {
    // From test/lotus-data-test.cpp.
    assert_eq!(data::VOWELS.len(), 72);
    assert_eq!(data::PUNCTUATION_MARKS.len(), 33);
    assert!(data::PUNCTUATION_MARKS.iter().all(char::is_ascii));
    assert_eq!(data::FIRST_CONSONANT_SEQS[0].len(), 29);
    assert_eq!(data::VOWEL_SEQS.len(), data::VC_MATRIX.len());
    assert_eq!(data::LAST_CONSONANT_SEQS.len(), 5);
    let sizes: Vec<usize> = INPUT_METHOD_DEFINITIONS.iter().map(|d| d.len()).collect();
    assert_eq!(sizes, vec![11, 10, 21, 31, 11, 15, 10, 0]);
    for definition in INPUT_METHOD_DEFINITIONS.iter() {
        assert!(definition.windows(2).all(|w| w[0].0 < w[1].0));
    }
    for (c, family) in [('a', &data::FAMILY_A), ('ơ', &data::FAMILY_O), ('đ', &data::FAMILY_D)] {
        assert!(std::ptr::eq(data::mark_family(c).unwrap(), &family[..]));
    }
    assert!(data::mark_family('b').is_none());

    use crate::data::{case, charset::TABLES};
    use crate::types::Charset;
    let lookup =
        |charset: Charset, c: char| TABLES[charset as usize].iter().find(|e| e.0 == c).map(|e| e.1);
    assert_eq!(TABLES.iter().map(|t| t.len()).sum::<usize>(), 2112);
    assert_eq!(lookup(Charset::Tcvn3Abc, 'đ'), Some("\u{AE}"));
    assert_eq!(lookup(Charset::UnicodeCStringHex, 'đ'), Some("\\x111"));
    assert_eq!(lookup(Charset::NcrHex, 'ơ'), Some("&#x1A1;"));
    assert_eq!(lookup(Charset::NcrDecimal, 'đ'), Some("&#273;"));
    assert_eq!(data::VOWELS[6], 'ă');
    assert_eq!(data::FIRST_CONSONANT_SEQS[0][4], "đ");
    assert_eq!(data::VOWEL_SEQS[5].len(), 30);
    assert_eq!(data::VC_MATRIX[7], &[4]);
    assert_eq!(case::LOWER_UPPER.len(), 82);
    assert_eq!(case::LETTERS.len(), 165);
    assert!(
        case::LETTERS.binary_search(&'đ').is_ok() && case::LETTERS.binary_search(&'Z').is_err()
    );
}
