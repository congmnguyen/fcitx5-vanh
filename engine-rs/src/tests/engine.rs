// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Ported from test/lotus-engine-test.cpp and test/lotus-engine-smoke.cpp (bamboo's bamboo_test.go).

use crate::engine::Engine;
use crate::input_method::cached_input_method;
use crate::test_support::{chars, from_hex, read_golden, string};
use crate::types::{BracketTransformMode, EngineFlags, InputMethodId, Mode, W2uMode};

const VN: Mode = Mode::VIETNAMESE;
const EN: Mode = Mode::ENGLISH;
const VN_FULL: Mode = Mode(Mode::VIETNAMESE.0 | Mode::FULL_TEXT.0);

fn engine_with(id: InputMethodId, flags: EngineFlags) -> Engine {
    Engine::new(cached_input_method(id).clone(), flags)
}

fn std_engine() -> Engine {
    let mut engine = engine_with(InputMethodId::Telex, EngineFlags::STD);
    engine.set_bracket_transform_mode(Some(BracketTransformMode::Everywhere));
    engine
}

fn without(flag: EngineFlags) -> EngineFlags {
    let mut flags = EngineFlags::STD;
    flags.remove(flag);
    flags
}

fn ty(engine: &mut Engine, keys: &str, mode: Mode) {
    engine.process_string(&chars(keys), mode);
}

#[track_caller]
fn check(engine: &Engine, mode: Mode, expected: &str) {
    assert_eq!(string(&engine.processed_string(mode)), expected);
}

/// Resets, types, and checks the Vietnamese rendering.
#[track_caller]
fn case(engine: &mut Engine, keys: &str, expected: &str) {
    engine.reset();
    ty(engine, keys, VN);
    check(engine, VN, expected);
}

#[test]
fn process_strings() {
    let mut e = std_engine();
    case(&mut e, "aw", "ă");
    e.reset();
    ty(&mut e, "uw", VN);
    ty(&mut e, "o", VN);
    ty(&mut e, "w", VN);
    check(&e, VN, "ươ");
    case(&mut e, "chuaarn", "chuẩn");
    case(&mut e, "giamaf", "giầm");
    e.reset();
    ty(&mut e, "dd", VN);
    assert!(e.is_valid(false));
    case(&mut e, "ddafi", "đài");
    e.reset();
    ty(&mut e, "Muoiwq", VN);
    check(&e, EN, "Muoiwq");
    case(&mut e, "mootj", "một");
    case(&mut e, "Thuow", "Thuơ");
    e.remove_last_char(true);
    check(&e, VN, "Thu");
}

#[test]
fn remove_last_char() {
    let mut e = std_engine();
    e.remove_last_char(true);
    ty(&mut e, " ", EN);
    e.remove_last_char(true);
    ty(&mut e, "loanj", VN);
    check(&e, VN, "loạn");
    e.remove_last_char(true);
    check(&e, VN, "lọa");
    ty(&mut e, ":", EN);
    e.remove_last_char(true);
    check(&e, VN, "lọa");

    case(&mut e, "VIEETJ", "VIỆT");
    e.remove_last_char(false);
    check(&e, VN, "VIỆ");
    e.process_key('Q', VN);
    check(&e, EN, "VIEEJQ");
    e.reset();
    ty(&mut e, "IB", EN);
    check(&e, EN, "IB");

    case(&mut e, "hanhj", "hạnh");
    e.remove_last_char(true);
    check(&e, VN, "hạn");
}

#[test]
fn spelling() {
    let mut e = std_engine();
    case(&mut e, "noww", "now");
    check(&e, EN, "noww");
    e.reset();
    ty(&mut e, "sawss", VN);
    check(&e, EN, "sawss");
    case(&mut e, "sawss", "săs");
    e.reset();
    ty(&mut e, "dd", VN);
    assert!(e.is_valid(false));
    check(&e, VN, "đ");
    e.reset();
    ty(&mut e, "SD", VN);
    ty(&mut e, "D", VN);
    check(&e, VN, "SĐ");
    e.reset();
    ty(&mut e, "gi]", VN);
    assert!(e.is_valid(false));
    e.reset();
    ty(&mut e, "to", VN);
    assert!(e.is_valid(true));
}

#[test]
fn telex23_and_brackets() {
    let mut e = std_engine();
    ty(&mut e, "t ]", EN);
    ty(&mut e, "a", VN);
    check(&e, VN, "]a");
    case(&mut e, "]]a", "]a");
    case(&mut e, "[", "ơ");
    case(&mut e, "{", "Ơ");
    e.reset();
    ty(&mut e, "[[", VN);
    check(&e, EN, "[");
    case(&mut e, "{s", "Ớ");
}

#[test]
fn named_words() {
    let mut e = std_engine();
    for (keys, expected) in [
        ("wowfi", "ười"),
        ("catr", "catr"),
        ("toowi", "tơi"),
        ("aloo", "alô"),
        ("tooss", "tôs"),
        ("tosos", "tôs"),
        ("duwoi", "dươi"),
        ("oddp", "ođp"),
        ("kimso", "kímo"),
        ("toorr", "tôr"),
        ("tnoss", "tnos"),
        ("ddawks", "đắk"),
        ("tooi oo HIEEUR", "HIỂU"),
        ("NGUOIW", "NGƯƠI"),
        ("duwongwj", "duongwj"),
        ("loz", "loz"),
        ("losz", "lo"),
        ("tôifs", "tối"),
        ("tốif", "tồi"),
        ("tốiz", "tôi"),
        ("linuxx", "linux"),
        ("buwoo", "buô"),
        ("buowco", "buôc"),
        ("cuoiwo", "cuôi"),
        ("acha", "acha"),
        ("nhuw", "như"),
        ("tooi", "tôi"),
        ("[[oo", "[ô"),
        ("oo]", "ôư"),
        ("chuyểnz", "chuyên"),
        ("nhueej", "nhuệ"),
        ("cuongw", "cương"),
        ("quawcj", "quặc"),
        ("m[f", "mờ"),
        ("d]s", "dứ"),
    ] {
        case(&mut e, keys, expected);
    }
    e.reset();
    ty(&mut e, "ww", VN);
    check(&e, EN, "w");
    check(&e, VN, "w");
    e.reset();
    ty(&mut e, "wiw", VN);
    check(&e, VN, "uiw");
    check(&e, EN, "wiw");
    e.reset();
    ty(&mut e, "reff", VN);
    ty(&mut e, "resh", EN);
    check(&e, EN, "reffresh");
    check(&e, VN, "refresh");
    e.reset();
    ty(&mut e, "reff", VN);
    e.remove_last_char(true);
    e.process_key('f', VN);
    check(&e, VN, "rè");
    e.reset();
    ty(&mut e, "gis", VN);
    ty(&mut e, "a", VN);
    check(&e, VN, "giá");
    e.reset();
    ty(&mut e, "losz", VN);
    check(&e, EN, "losz");
    e.reset();
    ty(&mut e, "tôifs", VN);
    check(&e, EN, "tôifs");
    e.reset();
    ty(&mut e, "tốif", VN);
    check(&e, EN, "tốif");
    for (keys, full, valid) in [("nhuw", true, true), ("thuw", true, true), ("thow", true, true), ("tooi", true, true), ("arch", false, false), ("chury", true, true)] {
        e.reset();
        ty(&mut e, keys, VN);
        assert_eq!(e.is_valid(full), valid, "{keys}");
    }
    e.reset();
    ty(&mut e, "turyn", VN);
    e.remove_last_char(true);
    e.remove_last_char(true);
    check(&e, VN, "tủ");
    e.reset();
    ty(&mut e, "tôi）t", EN);
    check(&e, VN, "t");
}

#[test]
fn other_input_methods() {
    let mut e = engine_with(InputMethodId::Vni, EngineFlags::STD);
    ty(&mut e, "o55", VN);
    check(&e, VN, "o5");

    let mut e = engine_with(InputMethodId::Telex, without(EngineFlags::STD_TONE_STYLE));
    e.set_bracket_transform_mode(Some(BracketTransformMode::Everywhere));
    case(&mut e, "choas", "choá");
    case(&mut e, "bieecs", "biếc");
    case(&mut e, "uese", "uế");

    let mut e = engine_with(InputMethodId::MicrosoftLayout, EngineFlags::STD);
    ty(&mut e, "112", VN);
    check(&e, VN, "1â");
    e.restore_last_word(false);
    check(&e, EN, "12");
    e.reset();
    ty(&mut e, "d[]ng9 t4i", VN);
    e.restore_last_word(false);
    check(&e, VN, "t4i");
}

#[test]
fn restore_last_word() {
    let mut e = std_engine();
    ty(&mut e, "duwongj tooi", VN);
    e.restore_last_word(false);
    check(&e, VN, "tooi");
    e.reset();
    ty(&mut e, "afq", VN);
    e.restore_last_word(false);
    e.remove_last_char(true);
    e.process_key('f', VN);
}

#[test]
fn w2u_flag() {
    let mut e = engine_with(InputMethodId::Telex, without(EngineFlags::W2U));
    e.process_key('w', VN);
    check(&e, VN, "w");
    let mut e = engine_with(InputMethodId::Telex, EngineFlags::STD | EngineFlags::W2U);
    e.process_key('w', VN);
    check(&e, VN, "ư");
    case(&mut e, "bw", "bư");
    case(&mut e, "W", "Ư");
}

#[test]
fn free_cv_spelling() {
    let check_case = |e: &mut Engine, typed: &str, want: &str, valid: bool| {
        case(e, typed, want);
        assert_eq!(e.is_valid(true), valid, "{typed}");
    };
    let mut e = std_engine();
    check_case(&mut e, "boawjm", "boặm", true);
    check_case(&mut e, "moawsm", "moắm", true);
    check_case(&mut e, "noawfm", "noằm", true);
    check_case(&mut e, "khoawsm", "khoắm", true);
    check_case(&mut e, "loawjm", "loặm", true);
    check_case(&mut e, "tak", "tak", false);
    check_case(&mut e, "tawsk", "tắk", true);
    let mut e = engine_with(InputMethodId::Telex, without(EngineFlags::W2U));
    check_case(&mut e, "boawjm", "boặm", true);
}

#[test]
fn bracket_modes() {
    let mut e = engine_with(InputMethodId::Telex, EngineFlags::STD);
    e.set_bracket_transform_mode(Some(BracketTransformMode::Everywhere));
    for (keys, want) in [("[", "ơ"), ("]", "ư"), ("[[", "[")] {
        e.reset();
        ty(&mut e, keys, VN);
        check(&e, VN_FULL, want);
    }
    e.set_bracket_transform_mode(Some(BracketTransformMode::NonStart));
    for (keys, want) in [("[", "["), ("a[", "aơ")] {
        e.reset();
        ty(&mut e, keys, VN);
        check(&e, VN_FULL, want);
    }
    e.set_bracket_transform_mode(Some(BracketTransformMode::Disabled));
    for (keys, want) in [("[", "["), ("]", "]"), ("m[f", "m[f")] {
        e.reset();
        ty(&mut e, keys, VN);
        check(&e, VN_FULL, want);
    }
}

#[test]
fn smoke() {
    // test/lotus-engine-smoke.cpp
    let mut e = engine_with(InputMethodId::Telex, EngineFlags::STD);
    assert_eq!(e.input_method().keys.len(), 11);
    assert_eq!(e.flag(), EngineFlags::STD);
    case(&mut e, "dd", "đ");
    assert!(e.is_valid(true));
    // RemoveLastChar drops the last appending node together with every node that targets it.
    e.remove_last_char(true);
    check(&e, VN, "");
    case(&mut e, "duwowngf", "dường");
    assert!(e.is_valid(true));
    case(&mut e, "tieengs", "tiếng");
    e.reset();
    assert!(!e.can_process_key('[') && e.can_process_key('w'));
    e.set_bracket_transform_mode(Some(BracketTransformMode::Everywhere));
    assert!(e.can_process_key('['));
    case(&mut e, "d[", "dơ");
    case(&mut e, "dd", "đ");
    assert_eq!(e.composition().len(), 2);
    e.reset();
    check(&e, VN, "");
    // A composition of at most one node is valid, the empty one included.
    assert!(e.is_valid(true));
}

fn w2u_of(field: &str) -> Option<W2uMode> {
    match field.parse::<i32>().unwrap() {
        0 => Some(W2uMode::Disabled),
        1 => Some(W2uMode::NonStart),
        2 => Some(W2uMode::Everywhere),
        _ => None,
    }
}

fn bracket_of(field: &str) -> Option<BracketTransformMode> {
    match field.parse::<i32>().unwrap() {
        0 => Some(BracketTransformMode::Disabled),
        1 => Some(BracketTransformMode::NonStart),
        2 => Some(BracketTransformMode::Everywhere),
        _ => None,
    }
}

/// Replays test/golden/engine_core.tsv (written by the Go engine): one engine per case id.
#[test]
fn golden_engine_core() {
    let rows = read_golden("engine_core.tsv");
    let mut engines: std::collections::HashMap<String, Engine> = Default::default();
    let mut mismatches = Vec::new();
    for row in &rows {
        let engine = engines.entry(row[0].clone()).or_insert_with(|| {
            let mut e = engine_with(InputMethodId::Telex, EngineFlags(row[4].parse().unwrap()));
            e.set_w2u_mode(w2u_of(&row[5]));
            e.set_bracket_transform_mode(bracket_of(&row[6]));
            e
        });
        let key = char::from_u32(u32::from_str_radix(&row[2], 16).unwrap()).unwrap();
        engine.process_key(key, Mode(row[3].parse().unwrap()));
        let actual = string(&engine.processed_string(VN_FULL));
        let expected = String::from_utf8(from_hex(&row[7])).unwrap();
        let valid = engine.is_valid(true);
        if actual != expected || valid != (row[8] == "1") {
            mismatches.push(format!("{} step {}: [{actual}] vs [{expected}], valid {valid}", row[0], row[1]));
        }
    }
    assert!(rows.len() > 10000);
    assert!(mismatches.is_empty(), "{} mismatches, first: {:?}", mismatches.len(), &mismatches[..mismatches.len().min(10)]);
}
