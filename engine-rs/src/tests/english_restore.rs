// SPDX-License-Identifier: GPL-3.0-or-later

//! Regression tests of the English restore found by the replay harness (engine-rs/eval): each
//! case is a failure type of a real mixed Vietnamese/English/code corpus.

use crate::dictionary::Dictionary;
use crate::input_method::cached_input_method;
use crate::key_processor::{KeyProcessor, Options};
use crate::types::{key_state, BracketTransformMode, InputMethodId, W2uMode};
use std::sync::{Arc, RwLock};

/// The addon defaults, with the shipped dictionary.
fn processor() -> KeyProcessor {
    let mut p = KeyProcessor::new(cached_input_method(InputMethodId::Telex).clone(), true);
    let mut o = Options::default();
    o.english_restore = true;
    o.macro_enabled = true;
    o.w2u = Some(W2uMode::NonStart);
    o.bracket_transform = Some(BracketTransformMode::Disabled);
    p.set_options(o);
    let mut d = Dictionary::default();
    d.load_from_text(&std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../data/dictionaries/vietnamese.cm.dict")).unwrap());
    p.set_dictionary(Some(Arc::new(RwLock::new(d))));
    p.set_rewrite_allowed(true);
    p
}

/// Types the keys like the uinput modes show them (rewrites included), then Enter.
fn typed(keys: &str) -> String {
    let mut p = processor();
    let mut screen: Vec<char> = Vec::new();
    let mut shown = 0; // preedit chars at the end of the screen
    let mut apply = |p: &mut KeyProcessor, screen: &mut Vec<char>, shown: &mut usize, processed: bool, key: Option<char>| {
        let commit = p.take_commit();
        let before = p.take_rewritten_before();
        if !commit.is_empty() || !before.is_empty() {
            screen.truncate(screen.len() - *shown);
            assert!(screen.ends_with(&before), "rewrite of {before:?} not at the end of {screen:?}");
            screen.truncate(screen.len() - before.len());
            screen.extend(commit);
            *shown = 0;
        } else if processed {
            screen.truncate(screen.len() - *shown);
            screen.extend_from_slice(p.preedit());
            *shown = p.preedit().len();
        }
        if !processed {
            *shown = 0;
            screen.extend(key);
        }
    };
    for c in keys.chars() {
        let state = if c.is_ascii_uppercase() { key_state::SHIFT } else { 0 };
        let processed = p.process_key(c as u32, state);
        apply(&mut p, &mut screen, &mut shown, processed, Some(c));
    }
    p.end_word();
    apply(&mut p, &mut screen, &mut shown, true, None);
    screen.into_iter().collect()
}

#[track_caller]
fn check(cases: &[(&str, &str)]) {
    for (keys, want) in cases {
        assert_eq!(typed(keys), *want, "keys {keys:?}");
    }
}

#[test]
fn word_before_enter() {
    // An English word last on a line went out as Vietnamese: Enter skipped the restore.
    check(&[("the poller is", "the poller is"), ("status line as", "status line as"), ("Tooi ddi hocj", "Tôi đi học"), ("cos", "có")]);
}

#[test]
fn words_glued_into_code_tokens() {
    check(&[
        ("max_tokens", "max_tokens"),
        ("--host 127", "--host 127"),
        ("docs/btc", "docs/btc"),
        ("SW_SHOW", "SW_SHOW"),
        ("teen/DOB", "tên/DOB"),
        ("vaf/hoawcj", "và/hoặc"),
        ("Baf Rija-Vuxng Tauf", "Bà Rịa-Vũng Tàu"),
        ("gioowf/max", "giờ/mã"),
    ]);
}

#[test]
fn ambiguous_words_inside_english_runs() {
    check(&[
        ("git has no", "git has no"),
        ("train and test sets", "train and test sets"),
        ("decode, json) as thin", "decode, json) as thin"),
        ("mock been trong", "mock bên trong"),
        ("fix bug car release", "fix bug cả release"),
        ("commit theo max ticket", "commit theo mã ticket"),
    ]);
}

#[test]
fn double_letters_and_identifiers() {
    // The Telex undo ate a letter and the leftover looked like a Vietnamese key string (dì, lít).
    check(&[("the diff were", "the diff were"), ("feature lists", "feature lists"), ("Remove SessionEnd hook", "Remove SessionEnd hook")]);
    // Inflections missing from the list.
    check(&[("This reverts commit", "This reverts commit")]);
}

#[test]
fn technical_vocabulary() {
    check(&[("docs: fix", "docs: fix"), ("the host is", "the host is"), ("version conf trong", "version còn trong"), ("conf nuwax", "còn nữa")]);
}

/// Types `keys`, presses the revert key, then types `after`; returns the screen.
fn typed_with_revert(p: &mut KeyProcessor, keys: &str, after: &str) -> String {
    p.reset();
    p.forget_context();
    let mut screen: Vec<char> = Vec::new();
    let mut shown = 0;
    let apply = |p: &mut KeyProcessor, screen: &mut Vec<char>, shown: &mut usize, processed: bool, key: Option<char>| {
        let commit = p.take_commit();
        let before = p.take_rewritten_before();
        if !commit.is_empty() || !before.is_empty() {
            screen.truncate(screen.len() - *shown);
            assert!(screen.ends_with(&before), "rewrite of {before:?} not at the end of {screen:?}");
            screen.truncate(screen.len() - before.len());
            screen.extend(commit);
            *shown = 0;
        } else if processed {
            screen.truncate(screen.len() - *shown);
            screen.extend_from_slice(p.preedit());
            *shown = p.preedit().len();
        }
        if !processed {
            *shown = 0;
            screen.extend(key);
        }
    };
    for c in keys.chars() {
        let processed = p.process_key(c as u32, 0);
        apply(p, &mut screen, &mut shown, processed, Some(c));
    }
    if p.revert_last_switch() {
        apply(p, &mut screen, &mut shown, true, None);
    }
    for c in after.chars() {
        let processed = p.process_key(c as u32, 0);
        apply(p, &mut screen, &mut shown, processed, Some(c));
    }
    p.end_word();
    apply(p, &mut screen, &mut shown, true, None);
    screen.into_iter().collect()
}

#[test]
fn revert_last_switch() {
    let mut p = processor();
    // An automatic restore goes back to Vietnamese, and a kept Vietnamese word to its keys.
    assert_eq!(typed_with_revert(&mut p, "the host ", "is"), "the hót is");
    assert_eq!(typed_with_revert(&mut p, "commit theo max ", "ticket"), "commit theo max ticket");
    // Nothing to revert once the next word started.
    p.process_key('a' as u32, 0);
    assert!(!p.revert_last_switch());
    p.reset();
    // Pressing it twice switches back.
    for c in "docs ".chars() {
        p.process_key(c as u32, 0);
    }
    let _ = p.take_commit();
    assert!(p.revert_last_switch());
    assert_eq!(super::super::test_support::string(&p.take_commit()), "dóc ");
    assert_eq!(super::super::test_support::string(&p.take_rewritten_before()), "docs ");
    assert!(p.revert_last_switch());
    assert_eq!(super::super::test_support::string(&p.take_commit()), "docs ");
}

#[test]
fn learning_from_reverts() {
    use crate::learning::Learned;
    let learned = Arc::new(RwLock::new(Learned::default()));
    let mut p = processor();
    p.set_learned(Some(learned.clone()));
    assert_eq!(typed_with_revert(&mut p, "dduwowcj host ", ""), "được hót ");
    assert_eq!(learned.read().unwrap().preference("host"), -1);
    // Next time the keys read Vietnamese without a revert...
    assert_eq!(typed_with_revert(&mut p, "", "dduwowcj host rieeng"), "được hót riêng");
    // ...and a kept Vietnamese word reverted to its keys stays English.
    assert_eq!(typed_with_revert(&mut p, "mas ", ""), "mas ");
    assert_eq!(learned.read().unwrap().preference("mas"), 1);
    assert_eq!(typed_with_revert(&mut p, "", "mas"), "mas");
    // Learning off: no memory.
    let mut q = processor();
    assert_eq!(typed_with_revert(&mut q, "", "dduwowcj host rieeng"), "được host riêng");
}

/// Fuzz rounds: `base`, times VANH_FUZZ_SCALE for long local runs.
fn rounds(base: u64) -> u64 {
    std::env::var("VANH_FUZZ_SCALE").ok().and_then(|v| v.parse::<u64>().ok()).map_or(base, |k| base * k)
}

/// xorshift64*: deterministic, no crates.
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
    fn pick<T: Copy>(&mut self, items: &[T]) -> T {
        items[(self.next() % items.len() as u64) as usize]
    }
}

/// The word alone through a processor with the given restores (no context).
fn alone(keys: &str, auto: bool) -> String {
    let mut p = KeyProcessor::new(cached_input_method(InputMethodId::Telex).clone(), false);
    let mut o = Options::default();
    o.auto_non_vn_restore = auto;
    o.w2u = Some(W2uMode::NonStart);
    o.bracket_transform = Some(BracketTransformMode::Disabled);
    p.set_options(o);
    for c in keys.chars() {
        p.process_key(c as u32, if c.is_ascii_uppercase() { key_state::SHIFT } else { 0 });
    }
    p.process_key(' ' as u32, 0);
    let mut out = super::super::test_support::string(&p.take_commit());
    out.pop();
    out
}

/// Random typing: no key is ever lost or doubled. Every separator typed shows up exactly once,
/// every word comes out as its Vietnamese rendering or as the keys typed (never with a letter
/// dropped or added), and a rewrite only ever deletes text that is on screen.
#[test]
fn fuzz_no_key_lost() {
    const LETTERS: &[char] = &['a', 'e', 'o', 'u', 'i', 'y', 'd', 'w', 's', 'f', 'r', 'x', 'j', 'z', 't', 'n', 'g', 'h', 'c', 'm', 'p', 'l', 'k', 'b', 'v', 'q', 'A', 'D', 'E', 'O', 'S'];
    const SEPARATORS: &[char] = &[' ', ' ', ' ', ' ', '.', ',', '-', '_', '/', ':', '(', ')', '"', '\'', '=', '1', '2', '0'];
    let words: &[&str] = &["the", "is", "as", "docs", "host", "max", "tooi", "dduwowcj", "vieejt", "nam", "cos", "theer", "diff", "lists", "git", "has", "car", "mono", "sets", "conf"];
    let mut rng = Rng(0x5eed_2026_1101);
    for round in 0..rounds(500) {
        let mut p = processor();
        let mut keys = String::new();
        let mut typed_words: Vec<String> = Vec::new();
        let n = 1 + rng.next() % 8;
        for i in 0..n {
            let w: String = if rng.next() % 2 == 0 {
                rng.pick(words).to_string()
            } else {
                (0..1 + rng.next() % 7).map(|_| rng.pick(LETTERS)).collect()
            };
            keys.push_str(&w);
            typed_words.push(w);
            if i + 1 < n || rng.next() % 2 == 0 {
                let mut sep = String::new();
                sep.push(rng.pick(SEPARATORS));
                if rng.next() % 4 == 0 {
                    sep.push(' ');
                }
                keys.push_str(&sep);
            }
        }
        let out = typed_with_revert(&mut p, "", &keys);
        let is_sep = |c: char| !c.is_alphabetic();
        let seps_in = keys.chars().filter(|&c| is_sep(c)).collect::<String>();
        let seps_out = out.chars().filter(|&c| is_sep(c)).collect::<String>();
        assert_eq!(seps_in, seps_out, "round {round}: separators of {keys:?} -> {out:?}");
        let out_words: Vec<&str> = out.split(is_sep).filter(|w| !w.is_empty()).collect();
        let in_words: Vec<&str> = keys.split(is_sep).filter(|w| !w.is_empty()).collect();
        assert_eq!(in_words.len(), out_words.len(), "round {round}: words of {keys:?} -> {out:?}");
        for (k, o) in in_words.iter().zip(&out_words) {
            let ok = *o == *k || *o == alone(k, false) || *o == alone(k, true);
            assert!(ok, "round {round}: word {k:?} came out {o:?} in {keys:?} -> {out:?}");
        }
    }
}

/// Random keys with Backspace, Enter, Tab and the revert key mixed in: never a panic, and a
/// rewrite or revert never deletes text that is not on screen.
#[test]
fn fuzz_backspace_revert_enter() {
    use crate::key_processor::{KEY_BACKSPACE, KEY_TAB};
    const KEYS: &[char] = &['a', 'e', 'o', 'd', 'w', 's', 'f', 'r', 'x', 'j', 't', 'n', 'h', ' ', ' ', '.', '_', '/', '1', 'S', 'D'];
    let mut rng = Rng(0xfeed_beef_0042);
    for round in 0..rounds(2000) {
        let mut p = processor();
        let mut screen: Vec<char> = Vec::new();
        let mut shown = 0usize;
        for _ in 0..(rng.next() % 40) {
            let r = rng.next() % 20;
            let (processed, typed) = match r {
                0 => {
                    let before_len = screen.len();
                    let processed = p.process_key(KEY_BACKSPACE, 0);
                    if !processed && before_len > 0 && shown == 0 {
                        screen.pop(); // the app deletes a committed char
                    }
                    (processed, None)
                }
                1 => {
                    if p.revert_last_switch() {
                        (true, None)
                    } else {
                        continue;
                    }
                }
                2 => {
                    p.end_word();
                    (true, Some('\n'))
                }
                3 => (p.process_key(KEY_TAB, 0), Some('\t')),
                _ => {
                    let c = rng.pick(KEYS);
                    (p.process_key(c as u32, if c.is_ascii_uppercase() { key_state::SHIFT } else { 0 }), Some(c))
                }
            };
            let commit = p.take_commit();
            let before = p.take_rewritten_before();
            if !commit.is_empty() || !before.is_empty() || (processed && p.preedit().is_empty() && shown > 0) {
                screen.truncate(screen.len() - shown);
                assert!(screen.ends_with(&before), "round {round}: rewrite {before:?} not at the end of {screen:?}");
                screen.truncate(screen.len() - before.len());
                screen.extend(commit);
                shown = 0;
            } else if processed {
                screen.truncate(screen.len() - shown);
                screen.extend_from_slice(p.preedit());
                shown = p.preedit().len();
            }
            if typed == Some('\n') {
                p.reset();
                p.forget_context();
                screen.push('\n');
                shown = 0;
            } else if !processed {
                shown = 0;
                if let Some(c) = typed {
                    screen.push(c);
                }
            }
        }
    }
}
