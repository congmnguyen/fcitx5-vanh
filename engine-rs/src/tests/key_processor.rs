// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Ported from test/lotus-key-processor-test.cpp (bamboo's fcitxbambooengine_test.go) and
//! test/numeric-macro-case-preservation.cpp.

use crate::dictionary::Dictionary;
use crate::encoder::encode;
use crate::input_method::cached_input_method;
use crate::key_processor::{determine_macro_case, KeyProcessor, MacroCase, Shared, KEY_BACKSPACE, KEY_SPACE, KEY_TAB};
use crate::macro_table::MacroTable;
use crate::test_support::{chars, from_hex, read_golden, string};
use crate::time_format::format_time;
use crate::types::{key_state, BracketTransformMode, Charset, EngineFlags, InputMethodId, W2uMode};
use std::sync::{Arc, RwLock};

fn shared<T>(value: T) -> Shared<T> {
    Arc::new(RwLock::new(value))
}

fn dictionary(words: &[&str]) -> Shared<Dictionary> {
    let mut d = Dictionary::default();
    d.load_from_words(words);
    shared(d)
}

fn macros(entries: &[(&str, &str)]) -> Shared<MacroTable> {
    let mut t = MacroTable::default();
    for (k, v) in entries {
        t.set(k.as_bytes(), v.as_bytes());
    }
    shared(t)
}

fn test_engine(dict: &[&str], spell_check_with_dicts: bool) -> KeyProcessor {
    let mut p = KeyProcessor::new(cached_input_method(InputMethodId::Telex).clone(), spell_check_with_dicts);
    p.set_macro_table(Some(macros(&[])));
    p.set_dictionary(Some(dictionary(dict)));
    p
}

fn opt_engine(flags: EngineFlags, dict: &[&str], spell_check_with_dicts: bool) -> KeyProcessor {
    let mut p = test_engine(dict, spell_check_with_dicts);
    p.engine().set_flag(flags);
    p
}

fn without(flag: EngineFlags) -> EngineFlags {
    let mut flags = EngineFlags::STD;
    flags.remove(flag);
    flags
}

fn type_keys(p: &mut KeyProcessor, keys: &str) {
    for c in keys.chars() {
        p.process_key(c as u32, 0);
    }
}

fn preedit(p: &KeyProcessor) -> String {
    string(p.preedit())
}

fn commit(p: &mut KeyProcessor) -> String {
    string(&p.take_commit())
}

fn time_shape(text: &str) -> bool {
    let b = text.as_bytes();
    b.len() == 5 && b[2] == b':' && [0, 1, 3, 4].iter().all(|&i| b[i].is_ascii_digit())
}

#[test]
fn macro_case() {
    for (input, want) in [
        ("abc", MacroCase::AllSmall),
        ("ABC", MacroCase::AllCapital),
        ("aBc", MacroCase::NoChange),
        ("123", MacroCase::NoChange),
        ("", MacroCase::NoChange),
        ("áộ", MacroCase::AllSmall),
        ("ÁỘ", MacroCase::AllCapital),
    ] {
        assert_eq!(determine_macro_case(&chars(input)), want, "{input}");
    }
}

#[test]
fn time_formats() {
    assert!(format_time(b"").is_empty());
    assert_eq!(format_time(b"no placeholders"), b"no placeholders");
    assert!(time_shape(std::str::from_utf8(&format_time(b"%H:%M")).unwrap()));
    let date = String::from_utf8(format_time(b"%d/%m/%Y")).unwrap();
    assert!(date.len() == 10 && date.as_bytes()[2] == b'/' && date.as_bytes()[5] == b'/');
    let fallback = format_time(b"%Q");
    assert!(!fallback.is_empty() && fallback != b"%Q");
}

#[test]
fn expand_macro() {
    let mut p = test_engine(&[], false);
    assert_eq!(string(&p.expand_macro(&chars("VN"), &chars("Việt Nam"))), "Việt Nam");
    p.options_mut().auto_capitalize_macro = true;
    assert_eq!(string(&p.expand_macro(&chars("vn"), &chars("Việt Nam"))), "việt nam");
    assert_eq!(string(&p.expand_macro(&chars("VN"), &chars("việt nam"))), "VIỆT NAM");
    assert_eq!(string(&p.expand_macro(&chars("Vn"), &chars("Việt Nam"))), "Việt Nam");
    p.options_mut().auto_capitalize_macro = false;
    p.options_mut().time_format = b"%H:%M".to_vec();
    assert!(p.expand_macro(&chars("now"), &chars("giờ $TIME")).iter().any(char::is_ascii_digit));
    p.options_mut().time_format.clear();
    assert_eq!(string(&p.expand_macro(&chars("now"), &chars("giờ $TIME"))), "giờ $TIME");
    p.options_mut().date_format = b"%d/%m/%Y".to_vec();
    assert!(p.expand_macro(&chars("now"), &chars("$DATE")).contains(&'/'));
}

#[test]
fn update_preedit_clears_both() {
    let mut p = test_engine(&[], false);
    type_keys(&mut p, "chao");
    p.process_key(KEY_SPACE, 0);
    type_keys(&mut p, "aw");
    assert_eq!(preedit(&p), "ă");
    p.update_preedit(&[]);
    assert!(p.preedit().is_empty());
    assert!(p.take_commit().is_empty());
}

#[test]
fn charsets() {
    assert_eq!(encode(Charset::Unicode, &chars("tôi")), "tôi");
    assert_ne!(encode(Charset::Tcvn3Abc, &chars("tôi")), "tôi");
    let mut p = test_engine(&[], false);
    type_keys(&mut p, "dd");
    assert_eq!(preedit(&p), "đ");
    p.commit_preedit();
    assert_eq!(commit(&mut p), "đ");
    let mut p = test_engine(&[], false);
    p.options_mut().output_charset = Charset::Tcvn3Abc;
    type_keys(&mut p, "dd");
    assert_eq!(preedit(&p), "®");
    p.commit_preedit();
    assert_eq!(commit(&mut p), "®");
}

#[test]
fn telex_preedit_and_commit() {
    for (keys, want) in [("aw", "ă"), ("dd", "đ"), ("tooi", "tôi"), ("chaof", "chào")] {
        let mut p = test_engine(&[], false);
        type_keys(&mut p, keys);
        assert_eq!(preedit(&p), want);
    }
    let mut p = test_engine(&[], false);
    type_keys(&mut p, "chao");
    assert!(p.process_key(KEY_SPACE, 0));
    assert_eq!(commit(&mut p), "chao ");
    assert!(p.preedit().is_empty());
}

#[test]
fn auto_restore() {
    let mut p = test_engine(&["chào"], true);
    type_keys(&mut p, "chaof");
    assert_eq!(preedit(&p), "chào");
    p.process_key(KEY_SPACE, 0);
    assert_eq!(commit(&mut p), "chào ");
    let mut p = test_engine(&["chào"], true);
    type_keys(&mut p, "khoawjm");
    assert_eq!(preedit(&p), "khoặm");
    p.process_key(KEY_SPACE, 0);
    assert_eq!(commit(&mut p), "khoawjm ");

    let mut p = test_engine(&[], false);
    type_keys(&mut p, "tak");
    assert!(p.must_fallback_to_english());
    let mut p = test_engine(&[], false);
    type_keys(&mut p, "tooi");
    assert!(!p.must_fallback_to_english());
    let mut p = test_engine(&[], false);
    type_keys(&mut p, "boawjm");
    assert_eq!(preedit(&p), "boặm");
    p.process_key(KEY_SPACE, 0);
    assert_eq!(commit(&mut p), "boặm ");

    let mut p = test_engine(&[], true);
    type_keys(&mut p, "dd");
    assert!(!p.must_fallback_to_english());
    p.options_mut().dd_free_style = false;
    assert!(p.must_fallback_to_english());

    let mut p = test_engine(&[], true);
    p.options_mut().auto_non_vn_restore = false;
    type_keys(&mut p, "boawjm");
    assert!(!p.must_fallback_to_english());
    p.process_key(KEY_SPACE, 0);
    assert_eq!(commit(&mut p), "boặm ");
    let mut p = test_engine(&[], true);
    type_keys(&mut p, "boawjm");
    assert!(p.must_fallback_to_english());
}

#[test]
fn to_upper() {
    let mut p = test_engine(&[], false);
    assert_eq!(p.to_upper('b'), 'b');
    assert_eq!(p.to_upper('['), '[');
    p.engine().set_bracket_transform_mode(Some(BracketTransformMode::Everywhere));
    for (input, want) in [('[', '{'), (']', '}'), ('{', '['), ('}', ']')] {
        assert_eq!(p.to_upper(input), want);
    }
}

#[test]
fn shift_space_restores_key_strokes() {
    let mut p = test_engine(&[], false);
    type_keys(&mut p, "aw");
    assert_eq!(preedit(&p), "ă");
    p.set_should_restore_key_strokes(true);
    p.process_key(KEY_SPACE, 0);
    assert!(!p.should_restore_key_strokes());
    assert_eq!(preedit(&p), "aw");
}

#[test]
fn backspace() {
    let mut p = test_engine(&[], false);
    type_keys(&mut p, "aw");
    assert!(p.process_key(KEY_BACKSPACE, 0));
    assert!(p.take_commit().is_empty() && p.preedit().is_empty());
    let mut p = test_engine(&[], false);
    type_keys(&mut p, "chao");
    p.process_key(KEY_BACKSPACE, 0);
    assert_eq!(preedit(&p), "cha");
    let mut p = test_engine(&[], false);
    assert!(!p.process_key(KEY_BACKSPACE, 0));
}

#[test]
fn tab_macros() {
    let mut p = test_engine(&[], false);
    p.set_macro_enabled(true);
    p.set_macro_table(Some(macros(&[("vn", "Việt Nam")])));
    type_keys(&mut p, "vn");
    assert!(p.process_key(KEY_TAB, 0));
    assert_eq!(commit(&mut p), "Việt Nam");
    let mut p = test_engine(&[], false);
    p.set_macro_enabled(true);
    p.set_macro_table(Some(macros(&[("vn", "Việt Nam")])));
    type_keys(&mut p, "xx");
    assert!(!p.process_key(KEY_TAB, 0));
    assert_eq!(commit(&mut p), "xx");

    let mut p = test_engine(&[], false);
    p.set_macro_enabled(true);
    p.set_macro_table(Some(macros(&[("nơ", "$TIME giờ")])));
    p.options_mut().time_format = b"%H:%M".to_vec();
    type_keys(&mut p, "now");
    p.process_key(KEY_TAB, 0);
    let text = commit(&mut p);
    assert!(text.ends_with(" giờ") && time_shape(&text[..5]), "{text}");
}

#[test]
fn can_process_key_and_states() {
    let p = test_engine(&[], false);
    for key in [KEY_SPACE, KEY_BACKSPACE, ',' as u32, 'a' as u32, 'w' as u32] {
        assert!(p.can_process_key(key));
    }
    assert!(!p.can_process_key('€' as u32));
    assert!(!p.can_process_key(0x1008FF11)); // XF86AudioLowerVolume, beyond Unicode
    assert!(KeyProcessor::is_valid_state(0));
    assert!(KeyProcessor::is_valid_state(key_state::SHIFT));
    for mask in [key_state::CTRL, key_state::MOD1, key_state::IGNORED, key_state::SUPER, key_state::HYPER, key_state::META] {
        assert!(!KeyProcessor::is_valid_state(mask));
    }
    let mut p = test_engine(&[], false);
    assert!(!p.process_key('a' as u32, key_state::META));
    assert!(p.preedit().is_empty());
}

#[test]
fn english_number_w2u_in_macro_mode() {
    let mut p = test_engine(&[], false);
    p.set_macro_enabled(true);
    type_keys(&mut p, "qwen2");
    assert_eq!(preedit(&p), "qwen2");
    p.commit_preedit_and_reset(Vec::new());
    type_keys(&mut p, "new1");
    assert_eq!(preedit(&p), "new1");
}

#[test]
fn dictionary_prefix() {
    for dd_free_style in [true, false] {
        let mut p = test_engine(&["đcu"], true);
        p.options_mut().dd_free_style = dd_free_style;
        type_keys(&mut p, "ddc");
        assert_eq!(preedit(&p), "đc");
        type_keys(&mut p, "u");
        assert_eq!(preedit(&p), "đcu");
        p.process_key(KEY_SPACE, 0);
        assert_eq!(commit(&mut p), "đcu ");
    }
    let mut p = test_engine(&["đcu"], true);
    p.options_mut().dd_free_style = false;
    type_keys(&mut p, "ddc");
    p.process_key(KEY_SPACE, 0);
    assert_eq!(commit(&mut p), "ddc ");

    let mut p = test_engine(&["đcu"], true);
    p.options_mut().dd_free_style = false;
    type_keys(&mut p, "ddc");
    assert!(!p.should_fallback_to_english(true));
    type_keys(&mut p, "u");
    assert!(!p.should_fallback_to_english(true));
    assert!(!p.must_fallback_to_english());
}

#[test]
fn w2u_modes() {
    for (flags, mode, keys, want) in [
        (EngineFlags::STD, None, "w", "ư"),
        (EngineFlags::STD, None, "bw", "bư"),
        (without(EngineFlags::W2U), None, "w", "w"),
        (without(EngineFlags::W2U), None, "bw", "bw"),
        (EngineFlags::STD, Some(W2uMode::Disabled), "w", "w"),
        (EngineFlags::STD, Some(W2uMode::NonStart), "w", "w"),
        (EngineFlags::STD, Some(W2uMode::NonStart), "bw", "bư"),
        (EngineFlags::STD, Some(W2uMode::Everywhere), "w", "ư"),
    ] {
        let mut p = opt_engine(flags, &[], false);
        p.engine().set_w2u_mode(mode);
        type_keys(&mut p, keys);
        assert_eq!(preedit(&p), want, "{keys} {mode:?}");
    }
    let mut p = opt_engine(EngineFlags::STD, &[], false);
    type_keys(&mut p, "aw");
    assert_eq!(preedit(&p), "ă");
}

#[test]
fn option_matrix() {
    for (auto_restore, with_dicts, want_boawjm) in
        [(true, true, "boawjm "), (true, false, "boặm "), (false, true, "boặm "), (false, false, "boặm ")]
    {
        let mut p = test_engine(&["chào"], with_dicts);
        p.options_mut().auto_non_vn_restore = auto_restore;
        type_keys(&mut p, "chaof");
        p.process_key(KEY_SPACE, 0);
        assert_eq!(commit(&mut p), "chào ");
        let mut p = test_engine(&["chào"], with_dicts);
        p.options_mut().auto_non_vn_restore = auto_restore;
        type_keys(&mut p, "boawjm");
        p.process_key(KEY_SPACE, 0);
        assert_eq!(commit(&mut p), want_boawjm);
    }
    let mut p = test_engine(&["chào"], true);
    type_keys(&mut p, "chaof");
    assert!(!p.must_fallback_to_english());
    let mut p = test_engine(&[], true);
    type_keys(&mut p, "chaof");
    assert!(p.must_fallback_to_english());
}

#[test]
fn styles_and_brackets() {
    let mut p = opt_engine(EngineFlags::STD, &[], false);
    type_keys(&mut p, "hoaf");
    assert_eq!(preedit(&p), "hòa");
    let mut p = opt_engine(without(EngineFlags::STD_TONE_STYLE), &[], false);
    type_keys(&mut p, "hoaf");
    assert_eq!(preedit(&p), "hoà");
    for (keys, want) in [("[", "ơ"), ("t[", "tơ"), ("]", "ư"), ("t]", "tư")] {
        let mut p = test_engine(&[], false);
        p.engine().set_bracket_transform_mode(Some(BracketTransformMode::Everywhere));
        type_keys(&mut p, keys);
        assert_eq!(preedit(&p), want);
    }
    let mut p = test_engine(&[], false);
    assert!(p.engine().flag().has(EngineFlags::FREE_TONE_MARKING));
    let mut p = opt_engine(without(EngineFlags::FREE_TONE_MARKING), &[], false);
    assert!(!p.engine().flag().has(EngineFlags::FREE_TONE_MARKING));
    p.engine().set_flag(EngineFlags::STD | EngineFlags::FREE_TONE_MARKING);
    assert!(p.engine().flag().has(EngineFlags::FREE_TONE_MARKING));
}

#[test]
fn numeric_macro_case_preservation() {
    // test/numeric-macro-case-preservation.cpp: a numeric trigger keeps the expansion's case.
    let mut p = KeyProcessor::new(cached_input_method(InputMethodId::Telex).clone(), true);
    let options = crate::key_processor::Options {
        macro_enabled: true,
        auto_capitalize_macro: true,
        modern_style: false,
        free_marking: false,
        w2u: Some(W2uMode::Disabled),
        bracket_transform: Some(BracketTransformMode::Disabled),
        ..Default::default()
    };
    p.set_options(options);
    p.set_macro_table(Some(macros(&[("123", "Mixed Case")])));
    let mut accepted = true;
    for key in ['1' as u32, '2' as u32, '3' as u32, KEY_TAB] {
        accepted = p.process_key(key, 0) && accepted;
    }
    assert!(accepted);
    assert_eq!(commit(&mut p), "Mixed Case");
}

const OPT_AUTO_NON_VN_RESTORE: u32 = 1 << 0;
const OPT_DD_FREE_STYLE: u32 = 1 << 1;
const OPT_SPELL_CHECK_WITH_DICTS: u32 = 1 << 2;
const OPT_MACRO_ENABLED: u32 = 1 << 3;

fn mode_of(field: &str) -> i32 {
    field.parse().unwrap()
}

/// Replays test/golden/key_processor.tsv (written by the Go wrapper): one processor per case id.
#[test]
fn golden_key_processor() {
    let rows = read_golden("key_processor.tsv");
    let mut processors: std::collections::HashMap<String, KeyProcessor> = Default::default();
    let mut mismatches = Vec::new();
    for row in &rows {
        let p = processors.entry(row[0].clone()).or_insert_with(|| {
            let opts: u32 = row[4].parse().unwrap();
            let mut p = KeyProcessor::new(cached_input_method(InputMethodId::Telex).clone(), opts & OPT_SPELL_CHECK_WITH_DICTS != 0);
            p.set_dictionary(Some(dictionary(&["vietnam", "nguoi", "duong"])));
            let table = if opts & OPT_MACRO_ENABLED != 0 { macros(&[("vn", "Việt Nam"), ("ko", "không")]) } else { macros(&[]) };
            p.set_macro_table(Some(table));
            p.options_mut().auto_non_vn_restore = opts & OPT_AUTO_NON_VN_RESTORE != 0;
            p.options_mut().dd_free_style = opts & OPT_DD_FREE_STYLE != 0;
            p.set_macro_enabled(opts & OPT_MACRO_ENABLED != 0);
            p.engine().set_flag(EngineFlags(row[5].parse().unwrap()));
            p.engine().set_w2u_mode(match mode_of(&row[6]) {
                0 => Some(W2uMode::Disabled),
                1 => Some(W2uMode::NonStart),
                2 => Some(W2uMode::Everywhere),
                _ => None,
            });
            p.engine().set_bracket_transform_mode(match mode_of(&row[7]) {
                0 => Some(BracketTransformMode::Disabled),
                1 => Some(BracketTransformMode::NonStart),
                2 => Some(BracketTransformMode::Everywhere),
                _ => None,
            });
            p
        });
        let key = u32::from_str_radix(&row[2], 16).unwrap();
        let accepted = p.process_key(key, row[3].parse().unwrap());
        let actual_preedit = preedit(p);
        let actual_commit = commit(p);
        let want_preedit = String::from_utf8(from_hex(&row[9])).unwrap();
        let want_commit = String::from_utf8(from_hex(row.get(10).map(String::as_str).unwrap_or(""))).unwrap();
        if accepted != (row[8] == "1") || actual_preedit != want_preedit || actual_commit != want_commit {
            mismatches.push(format!("{} step {}: [{actual_preedit}|{actual_commit}] vs [{want_preedit}|{want_commit}]", row[0], row[1]));
        }
    }
    assert!(rows.len() > 10000);
    assert!(mismatches.is_empty(), "{} mismatches, first: {:?}", mismatches.len(), &mismatches[..mismatches.len().min(10)]);
}
