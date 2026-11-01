// SPDX-License-Identifier: GPL-3.0-or-later

//! Replay harness: types a labelled corpus (engine-rs/eval/label_corpus.py) as Telex keystrokes
//! through the key processor, the way the uinput modes of the addon apply its output (commits
//! replace the shown word, a rewrite deletes the earlier words first, Enter keeps what is shown),
//! and counts the words that come out wrong.
//!
//!   replay run   CORPUS [--config default|user|bamboo|off] [--split test|dev|all] [--tier typed|doc|all]
//!                       [--worst N] [--errors FILE] [--learn FILE]
//!   replay keys  CORPUS OUT        writes the keystrokes of every segment (one line each, for other engines)
//!   replay score CORPUS OUTPUTS [--split ..] [--tier ..] [--worst N] [--name NAME]
//!                                  scores the text another engine produced for those keystrokes
//!   replay bench CORPUS            latency of process_key over the corpus keystrokes
//!
//! Every Vietnamese word is typed with the first Telex spelling (tone key last, then other orders)
//! that the plain engine renders back to the word; words with no such spelling are left out and
//! counted as "untypeable".

use lotus_engine::dictionary::Dictionary;
use lotus_engine::input_method::cached_input_method;
use lotus_engine::key_processor::{KeyProcessor, Options, Shared};
use lotus_engine::types::{key_state, BracketTransformMode, InputMethodId, W2uMode};
use std::collections::HashMap;
use std::sync::{Arc, RwLock};
use std::time::Instant;

const VOWELS: [([char; 6], &str); 12] = [
    (['a', 'á', 'à', 'ả', 'ã', 'ạ'], "a"),
    (['ă', 'ắ', 'ằ', 'ẳ', 'ẵ', 'ặ'], "aw"),
    (['â', 'ấ', 'ầ', 'ẩ', 'ẫ', 'ậ'], "aa"),
    (['e', 'é', 'è', 'ẻ', 'ẽ', 'ẹ'], "e"),
    (['ê', 'ế', 'ề', 'ể', 'ễ', 'ệ'], "ee"),
    (['i', 'í', 'ì', 'ỉ', 'ĩ', 'ị'], "i"),
    (['o', 'ó', 'ò', 'ỏ', 'õ', 'ọ'], "o"),
    (['ô', 'ố', 'ồ', 'ổ', 'ỗ', 'ộ'], "oo"),
    (['ơ', 'ớ', 'ờ', 'ở', 'ỡ', 'ợ'], "ow"),
    (['u', 'ú', 'ù', 'ủ', 'ũ', 'ụ'], "u"),
    (['ư', 'ứ', 'ừ', 'ử', 'ữ', 'ự'], "uw"),
    (['y', 'ý', 'ỳ', 'ỷ', 'ỹ', 'ỵ'], "y"),
];
const TONE_KEYS: [char; 6] = ['\0', 's', 'f', 'r', 'x', 'j'];

fn lower(c: char) -> char {
    c.to_lowercase().next().unwrap_or(c)
}

/// (Telex keys of the letter without its tone, tone index) for a lowercase letter.
fn letter_keys(c: char) -> Option<(&'static str, usize)> {
    if c == 'đ' {
        return Some(("dd", 0));
    }
    for (forms, keys) in VOWELS.iter() {
        if let Some(t) = forms.iter().position(|&f| f == c) {
            return Some((keys, t));
        }
    }
    None
}

fn is_vn_letter(c: char) -> bool {
    let l = lower(c);
    !l.is_ascii() && letter_keys(l).is_some()
}

fn is_letter(c: char) -> bool {
    c.is_ascii_alphabetic() || is_vn_letter(c)
}

/// Tone-placement-free form of a word ("hòa" == "hoà"), lowercase: letters without tone + tone.
fn canon(word: &[char]) -> String {
    let mut base = String::new();
    let mut tone = 0;
    for &c in word {
        let l = lower(c);
        match letter_keys(l) {
            Some((_, t)) if l != 'đ' => {
                let forms = VOWELS.iter().find(|(f, _)| f.contains(&l)).unwrap().0;
                base.push(forms[0]);
                if t != 0 {
                    tone = t;
                }
            }
            _ => base.push(c),
        }
    }
    base.push(TONE_KEYS[tone]);
    base
}

/// Telex spellings of one Vietnamese word, most common typing style first.
fn telex_candidates(word: &[char]) -> Vec<Vec<char>> {
    let letters: Vec<char> = word.iter().map(|&c| lower(c)).collect();
    let all_caps = word.len() > 1 && word.iter().all(|&c| !c.is_alphabetic() || c.is_uppercase());
    // Keys for each letter and the tone.
    let mut per_letter: Vec<(Vec<char>, bool)> = Vec::new(); // (keys, is vowel)
    let mut tone = 0;
    for &l in &letters {
        match letter_keys(l) {
            Some((keys, t)) => {
                if t != 0 {
                    tone = t;
                }
                per_letter.push((keys.chars().collect(), l != 'đ'));
            }
            None => per_letter.push((vec![l], false)),
        }
    }
    let cased = |keys: Vec<Vec<char>>, tone_key: Option<(usize, char)>| -> Vec<char> {
        // keys[i] are the keys of letter i; the tone key goes after letter tone_key.0.
        let mut out = Vec::new();
        for (i, k) in keys.iter().enumerate() {
            let upper = word[i].is_uppercase();
            for (j, &c) in k.iter().enumerate() {
                out.push(if all_caps || (upper && j == 0) { c.to_ascii_uppercase() } else { c });
            }
            if let Some((at, t)) = tone_key {
                if at == i {
                    out.push(if all_caps { t.to_ascii_uppercase() } else { t });
                }
            }
        }
        out
    };
    let keys: Vec<Vec<char>> = per_letter.iter().map(|(k, _)| k.clone()).collect();
    let last = keys.len() - 1;
    let last_vowel = per_letter.iter().rposition(|(_, v)| *v).unwrap_or(last);
    let tone_char = TONE_KEYS[tone];
    let mut out = Vec::new();
    let with_tone = |at: usize| if tone == 0 { None } else { Some((at, tone_char)) };
    if TONE_AFTER_VOWEL.load(std::sync::atomic::Ordering::Relaxed) {
        out.push(cased(keys.clone(), with_tone(last_vowel)));
    }
    out.push(cased(keys.clone(), with_tone(last)));
    out.push(cased(keys.clone(), with_tone(last_vowel)));
    // "uow" for ươ.
    let mut short = keys.clone();
    for i in 0..short.len().saturating_sub(1) {
        if short[i] == ['u', 'w'] && short[i + 1] == ['o', 'w'] {
            short[i] = vec!['u'];
        }
    }
    out.push(cased(short.clone(), with_tone(last)));
    out.push(cased(short, with_tone(last_vowel)));
    out.dedup();
    out
}

#[derive(Clone, Copy, PartialEq)]
enum Config {
    Default,
    User,
    /// The user's config with AutoNonVnRestore and SpellCheck back on.
    UserRestore,
    Bamboo,
    Off,
}

fn options(config: Config) -> Options {
    let mut o = Options::default();
    match config {
        Config::Default => {
            o.english_restore = true;
            o.auto_non_vn_restore = true;
            o.spell_check_with_dicts = true;
            o.modern_style = true;
            o.macro_enabled = true;
            o.w2u = Some(W2uMode::NonStart);
            o.bracket_transform = Some(BracketTransformMode::Disabled);
        }
        Config::User => {
            o.english_restore = true;
            o.auto_non_vn_restore = false;
            o.spell_check_with_dicts = false;
            o.modern_style = false;
            o.macro_enabled = false;
            o.w2u = Some(W2uMode::Disabled);
            o.bracket_transform = Some(BracketTransformMode::Disabled);
        }
        Config::UserRestore => {
            o = options(Config::User);
            o.auto_non_vn_restore = true;
            o.spell_check_with_dicts = true;
        }
        Config::Bamboo => {
            // bamboo-core with fcitx5-bamboo's wrapper (what ibus-bamboo runs, minus its UI layer):
            // invalid words go back to the keys, no English restore.
            o.english_restore = false;
            o.auto_non_vn_restore = true;
            o.spell_check_with_dicts = true;
            o.modern_style = false;
            o.macro_enabled = false;
            o.w2u = Some(W2uMode::NonStart);
            o.bracket_transform = Some(BracketTransformMode::Disabled);
        }
        Config::Off => {
            o.english_restore = false;
            o.auto_non_vn_restore = false;
            o.spell_check_with_dicts = false;
            o.macro_enabled = false;
            o.w2u = Some(W2uMode::Disabled);
            o.bracket_transform = Some(BracketTransformMode::Disabled);
        }
    }
    o
}

fn dictionary() -> Shared<Dictionary> {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../data/dictionaries/vietnamese.cm.dict");
    let mut d = Dictionary::default();
    d.load_from_text(&std::fs::read(path).expect("dictionary"));
    Arc::new(RwLock::new(d))
}

fn processor(config: Config, dict: &Shared<Dictionary>) -> KeyProcessor {
    let mut p = KeyProcessor::new(cached_input_method(InputMethodId::Telex).clone(), true);
    p.set_options(options(config));
    p.set_dictionary(Some(dict.clone()));
    p.set_rewrite_allowed(true);
    p
}

fn key_event(c: char) -> (u32, u32) {
    let shifted = c.is_ascii_uppercase() || "~!@#$%^&*()_+{}|:\"<>?".contains(c);
    (c as u32, if shifted { key_state::SHIFT } else { 0 })
}

/// What the uinput modes leave on screen for a run of keys, ended by Enter.
/// The addon runs the restore at Enter (`--no-enter-fix` models the addon before it did).
/// `--tone-after-vowel`: type the tone key right after the vowels ("Vieejt"), not last ("Vieetj").
static TONE_AFTER_VOWEL: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
static ENTER_FIX: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(true);

struct Screen {
    committed: Vec<char>,
    preedit: Vec<char>,
}

impl Screen {
    fn new() -> Screen {
        Screen { committed: Vec::new(), preedit: Vec::new() }
    }

    fn key(&mut self, p: &mut KeyProcessor, c: char) {
        let (kv, st) = key_event(c);
        let processed = p.process_key(kv, st);
        let commit = p.take_commit();
        if !commit.is_empty() || (processed && p.preedit().is_empty() && !self.preedit.is_empty()) {
            let before = p.take_rewritten_before();
            if !before.is_empty() {
                if self.committed.ends_with(&before) {
                    self.committed.truncate(self.committed.len() - before.len());
                } else {
                    // The engine asked to delete text that is not there: count it as damage.
                    self.committed.push('\u{FFFD}');
                }
            }
            self.preedit.clear();
            self.committed.extend_from_slice(&commit);
            if !processed {
                self.committed.push(c);
            }
            return;
        }
        if processed {
            self.preedit = p.preedit().to_vec();
        } else {
            self.committed.append(&mut self.preedit);
            self.committed.push(c);
        }
    }

    /// Enter: the shown text stays, the engine starts over.
    fn enter(&mut self, p: &mut KeyProcessor) -> Vec<char> {
        if ENTER_FIX.load(std::sync::atomic::Ordering::Relaxed) && p.end_word() {
            let commit = p.take_commit();
            let before = p.take_rewritten_before();
            if self.committed.ends_with(&before) {
                self.committed.truncate(self.committed.len() - before.len());
            } else {
                self.committed.push('\u{FFFD}');
            }
            self.preedit.clear();
            self.committed.extend_from_slice(&commit);
        }
        let _ = p.take_commit();
        p.reset();
        p.forget_context();
        let mut out = std::mem::take(&mut self.committed);
        out.append(&mut self.preedit);
        out
    }
}

struct Row {
    split: String,
    source: String,
    tier: String,
    id: String,
    labels: Vec<char>,
    text: String,
}

fn read_corpus(path: &str) -> Vec<Row> {
    std::fs::read_to_string(path)
        .expect("corpus")
        .lines()
        .filter(|l| !l.starts_with('#') && !l.is_empty())
        .filter_map(|l| {
            let f: Vec<&str> = l.splitn(6, '\t').collect();
            (f.len() == 6).then(|| Row {
                split: f[0].into(),
                source: f[1].into(),
                tier: f[2].into(),
                id: f[3].into(),
                labels: f[4].chars().collect(),
                text: f[5].into(),
            })
        })
        .collect()
}

/// Keystrokes for the corpus, with a cache of verified Telex spellings.
struct Typist {
    dict: Shared<Dictionary>,
    cache: HashMap<Vec<char>, Option<Vec<char>>>,
    pub untypeable: HashMap<String, usize>,
}

impl Typist {
    fn word_keys(&mut self, word: &[char]) -> Option<Vec<char>> {
        if let Some(k) = self.cache.get(word) {
            return k.clone();
        }
        let mut found = None;
        for cand in telex_candidates(word) {
            let mut p = processor(Config::Off, &self.dict);
            let mut s = Screen::new();
            for &c in &cand {
                s.key(&mut p, c);
            }
            s.key(&mut p, ' ');
            let out = s.enter(&mut p);
            if out.len() == word.len() + 1 && (out[..word.len()] == *word || canon(&out[..word.len()]) == canon(word)) {
                found = Some(cand);
                break;
            }
        }
        self.cache.insert(word.to_vec(), found.clone());
        found
    }

    /// Keys of a segment; None when a Vietnamese word in it cannot be typed.
    fn segment_keys(&mut self, text: &str) -> Option<Vec<char>> {
        let chars: Vec<char> = text.chars().collect();
        let mut keys = Vec::new();
        let mut i = 0;
        while i < chars.len() {
            if is_letter(chars[i]) {
                let start = i;
                while i < chars.len() && is_letter(chars[i]) {
                    i += 1;
                }
                let word = &chars[start..i];
                if word.iter().any(|&c| is_vn_letter(c)) {
                    match self.word_keys(word) {
                        Some(k) => keys.extend(k),
                        None => {
                            *self.untypeable.entry(word.iter().collect()).or_default() += 1;
                            return None;
                        }
                    }
                } else {
                    keys.extend_from_slice(word);
                }
            } else {
                keys.push(chars[i]);
                i += 1;
            }
        }
        Some(keys)
    }
}

#[derive(Clone)]
struct Err {
    label: char,
    kind: &'static str,
    expected: String,
    got: String,
    context: String,
    id: String,
    eol: bool,
}

fn classify(label: char, expected: &str, got: &str) -> &'static str {
    let has_vn = |s: &str| s.chars().any(is_vn_letter);
    match label {
        'v' => {
            if !has_vn(got) && has_vn(expected) {
                "vn->en"
            } else {
                "vn-mangled"
            }
        }
        'e' => {
            if has_vn(got) {
                "en->vn"
            } else {
                "en-mangled"
            }
        }
        'c' => "code-mangled",
        _ => "other-mangled",
    }
}

fn same_word(expected: &str, got: &str) -> bool {
    if expected == got {
        return true;
    }
    let e: Vec<char> = expected.chars().collect();
    let g: Vec<char> = got.chars().collect();
    e.len() == g.len() && e.iter().any(|&c| is_vn_letter(c)) && canon(&e) == canon(&g)
}

#[derive(Default)]
struct Tally {
    words: HashMap<char, usize>,
    errors: Vec<Err>,
    segments: usize,
    skipped_segments: usize,
    skipped_words: usize,
}

fn score_row(t: &mut Tally, row: &Row, out: &str) {
    let exp: Vec<&str> = row.text.split(' ').collect();
    let got: Vec<&str> = out.split(' ').collect();
    for &l in row.labels.iter() {
        if l != 'o' {
            *t.words.entry(l).or_default() += 1;
        }
    }
    if exp.len() != got.len() {
        // Spaces moved: blame every letter token that differs by position up to the first gap.
        let e = Err { label: 'x', kind: "split", expected: row.text.clone(), got: out.into(), context: row.text.clone(), id: row.id.clone(), eol: false };
        t.errors.push(e);
        return;
    }
    for (i, (&e, &g)) in exp.iter().zip(&got).enumerate() {
        let l = row.labels.get(i).copied().unwrap_or('o');
        if !same_word(e, g) {
            let lo = i.saturating_sub(3);
            let hi = (i + 3).min(exp.len());
            t.errors.push(Err {
                label: l,
                kind: classify(l, e, g),
                expected: e.into(),
                got: g.into(),
                context: format!("{}  =>  {}", exp[lo..hi].join(" "), got[lo..hi].join(" ")),
                id: row.id.clone(),
                eol: i + 1 == exp.len(),
            });
        }
    }
}

fn report(name: &str, t: &Tally, worst: usize, errors_file: Option<&str>) {
    let n = |l: char| *t.words.get(&l).unwrap_or(&0);
    let total = n('v') + n('e') + n('c');
    let count = |k: &str| t.errors.iter().filter(|e| e.kind == k).count();
    let per_k = |c: usize, d: usize| if d == 0 { 0.0 } else { 1000.0 * c as f64 / d as f64 };
    println!("== {name}");
    println!(
        "segments {} (skipped {} with untypeable words), words {} (vn {}, en {}, code {})",
        t.segments, t.skipped_segments, total, n('v'), n('e'), n('c')
    );
    let vn_en = count("vn->en");
    let vn_m = count("vn-mangled");
    let en_vn = count("en->vn");
    let en_m = count("en-mangled");
    let code = count("code-mangled");
    let split = count("split") + count("other-mangled");
    println!("{:<34}{:>7}{:>14}{:>16}", "error", "count", "per 1k words", "per 1k of class");
    for (label, c, d) in [
        ("Vietnamese restored to English", vn_en, n('v')),
        ("Vietnamese otherwise mangled", vn_m, n('v')),
        ("English turned Vietnamese", en_vn, n('e')),
        ("English otherwise mangled", en_m, n('e')),
        ("code token mangled", code, n('c')),
        ("other (spacing/punctuation)", split, total),
    ] {
        println!("{:<34}{:>7}{:>14.2}{:>16.2}", label, c, per_k(c, total), per_k(c, d));
    }
    let all = t.errors.len();
    println!("{:<34}{:>7}{:>14.2}", "TOTAL", all, per_k(all, total));
    let mut groups: HashMap<(String, String, &str), (usize, String)> = HashMap::new();
    for e in &t.errors {
        let g = groups.entry((e.expected.clone(), e.got.clone(), e.kind)).or_insert((0, e.context.clone()));
        g.0 += 1;
    }
    let mut g: Vec<_> = groups.into_iter().collect();
    g.sort_by(|a, b| b.1 .0.cmp(&a.1 .0).then(a.0.cmp(&b.0)));
    if worst > 0 {
        println!("-- worst {worst} (count, kind, expected -> got, context)");
        for ((e, got, k), (c, ctx)) in g.iter().take(worst) {
            println!("{c:>4} {k:<13} {e:?} -> {got:?}   | {ctx}");
        }
    }
    if let Some(path) = errors_file {
        let mut s = String::new();
        for e in &t.errors {
            s.push_str(&format!("{}\t{}\t{}\t{}\t{}\t{}\t{}\n", e.kind, e.label, e.expected, e.got, e.id, if e.eol { "eol" } else { "mid" }, e.context));
        }
        std::fs::write(path, s).unwrap();
    }
}

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

fn selected<'a>(rows: &'a [Row], args: &[String]) -> Vec<&'a Row> {
    let split = arg(args, "--split").unwrap_or_else(|| "test".into());
    let tier = arg(args, "--tier").unwrap_or_else(|| "all".into());
    let source = arg(args, "--source");
    rows.iter()
        .filter(|r| split == "all" || r.split == split)
        .filter(|r| tier == "all" || r.tier == tier)
        .filter(|r| source.as_deref().is_none_or(|s| r.source == s))
        .collect()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let cmd = args.get(1).map(String::as_str).unwrap_or("");
    let dict = dictionary();
    if cmd == "type" {
        // replay type [--config X] [--raw] TEXT...: the output for each text (Telex keys unless --raw).
        let config = match arg(&args, "--config").as_deref() {
            Some("user") => Config::User,
            Some("user-restore") => Config::UserRestore,
            Some("bamboo") => Config::Bamboo,
            _ => Config::Default,
        };
        let mut typist = Typist { dict: dict.clone(), cache: HashMap::new(), untypeable: HashMap::new() };
        let raw = args.iter().any(|a| a == "--raw");
        let mut p = processor(config, &dict);
        let mut skip = false;
        for a in &args[2..] {
            if skip || a == "--raw" {
                skip = false;
                continue;
            }
            if a == "--config" {
                skip = true;
                continue;
            }
            let keys: Vec<char> = if raw { a.chars().collect() } else { typist.segment_keys(a).unwrap_or_default() };
            let mut s = Screen::new();
            for &c in &keys {
                s.key(&mut p, c);
            }
            let out: String = s.enter(&mut p).into_iter().collect();
            println!("{} | {} -> {}", if out == *a { "ok " } else { "BAD" }, keys.iter().collect::<String>(), out);
        }
        return;
    }
    let corpus = args.get(2).expect("corpus path");
    let rows = read_corpus(corpus);
    let mut typist = Typist { dict: dict.clone(), cache: HashMap::new(), untypeable: HashMap::new() };
    if args.iter().any(|a| a == "--tone-after-vowel") {
        TONE_AFTER_VOWEL.store(true, std::sync::atomic::Ordering::Relaxed);
    }
    if args.iter().any(|a| a == "--no-enter-fix") {
        ENTER_FIX.store(false, std::sync::atomic::Ordering::Relaxed);
    }
    let worst: usize = arg(&args, "--worst").map_or(50, |w| w.parse().unwrap());
    match cmd {
        "keys" => {
            let mut s = String::new();
            for r in &rows {
                match typist.segment_keys(&r.text) {
                    Some(k) => s.extend(k),
                    None => s.push('\u{1}'),
                }
                s.push('\n');
            }
            std::fs::write(&args[3], s).unwrap();
        }
        "run" => {
            let config = match arg(&args, "--config").as_deref() {
                Some("user") => Config::User,
                Some("user-restore") => Config::UserRestore,
                Some("bamboo") => Config::Bamboo,
                Some("off") => Config::Off,
                _ => Config::Default,
            };
            let mut p = processor(config, &dict);
            let mut t = Tally::default();
            for r in selected(&rows, &args) {
                let Some(keys) = typist.segment_keys(&r.text) else {
                    t.skipped_segments += 1;
                    t.skipped_words += r.labels.iter().filter(|&&l| l != 'o').count();
                    continue;
                };
                t.segments += 1;
                let mut s = Screen::new();
                for &c in &keys {
                    s.key(&mut p, c);
                }
                let out: String = s.enter(&mut p).into_iter().collect();
                score_row(&mut t, r, &out);
            }
            let name = arg(&args, "--name").unwrap_or_else(|| format!("vanh ({})", arg(&args, "--config").unwrap_or("default".into())));
            report(&name, &t, worst, arg(&args, "--errors").as_deref());
            let mut u: Vec<_> = typist.untypeable.iter().collect();
            u.sort_by(|a, b| b.1.cmp(a.1));
            let top: Vec<String> = u.iter().take(10).map(|(w, c)| format!("{w}:{c}")).collect();
            println!("untypeable words: {} ({})", u.len(), top.join(" "));
        }
        "score" => {
            let outputs: Vec<String> = std::fs::read_to_string(&args[3]).unwrap().lines().map(String::from).collect();
            assert_eq!(outputs.len(), rows.len(), "one output line per corpus row");
            let index: HashMap<*const Row, usize> = rows.iter().enumerate().map(|(i, r)| (r as *const Row, i)).collect();
            let mut t = Tally::default();
            for r in selected(&rows, &args) {
                let out = &outputs[index[&(r as *const Row)]];
                if out.starts_with('\u{1}') {
                    t.skipped_segments += 1;
                    continue;
                }
                t.segments += 1;
                score_row(&mut t, r, out);
            }
            report(&arg(&args, "--name").unwrap_or("external".into()), &t, worst, arg(&args, "--errors").as_deref());
        }
        "bench" => {
            let config = match arg(&args, "--config").as_deref() {
                Some("user") => Config::User,
                Some("user-restore") => Config::UserRestore,
                Some("bamboo") => Config::Bamboo,
                _ => Config::Default,
            };
            let all: Vec<Vec<char>> = rows.iter().filter_map(|r| typist.segment_keys(&r.text)).collect();
            let mut p = processor(config, &dict);
            let mut lat: Vec<u64> = Vec::new();
            let (mut letter_ns, mut letter_n, mut other_ns, mut other_n) = (0u64, 0u64, 0u64, 0u64);
            for _ in 0..2 {
                lat.clear();
                (letter_ns, letter_n, other_ns, other_n) = (0, 0, 0, 0);
                for keys in &all {
                    let mut s = Screen::new();
                    for &c in keys {
                        let t0 = Instant::now();
                        let (kv, st) = key_event(c);
                        p.process_key(kv, st);
                        let _ = p.take_commit();
                        let _ = p.take_rewritten_before();
                        let ns = t0.elapsed().as_nanos() as u64;
                        if c.is_ascii_alphabetic() { letter_ns += ns; letter_n += 1 } else { other_ns += ns; other_n += 1 }
                        lat.push(ns);
                    }
                    let _ = s.enter(&mut p);
                }
            }
            lat.sort_unstable();
            let q = |f: f64| lat[((lat.len() as f64 - 1.0) * f) as usize] as f64 / 1000.0;
            let mean = lat.iter().sum::<u64>() as f64 / lat.len() as f64 / 1000.0;
            println!("keys {} mean {:.2}us p50 {:.2}us p99 {:.2}us p99.9 {:.2}us max {:.2}us", lat.len(), mean, q(0.5), q(0.99), q(0.999), q(1.0));
            println!("  letters {} mean {:.2}us, other keys {} mean {:.2}us", letter_n, letter_ns as f64 / letter_n as f64 / 1000.0, other_n, other_ns as f64 / other_n as f64 / 1000.0);
        }
        _ => eprintln!("usage: replay run|keys|score|bench CORPUS ..."),
    }
}
