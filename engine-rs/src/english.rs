// SPDX-License-Identifier: GPL-3.0-or-later

//! English restore at a word break, after Vikey's auto switch (Engine/Engine.cpp, decisions D13,
//! D22, D30, D34, D35 in its DECISIONS.md): when the keys typed are an English word and the
//! Vietnamese reading is missing or much rarer, the word goes back to the keys ("more", not
//! "mỏe"). The word before decides ties ("video call this", "cho con bú") and, once a word went
//! back to English, the following words stay English until one reads clearly Vietnamese.

use crate::data::tech_english::TECH_WORDS;
use crate::data::{vikey_autoswitch as words, vikey_bigram as bigram};

pub const MISSING: i32 = -1000;

/// One word as the auto switch sees it. Scores are 10*log10(occurrences per million).
#[derive(Clone, Debug, PartialEq)]
pub struct Word {
    /// Keys typed, lowercase ASCII letters ("more").
    pub raw: String,
    /// Vietnamese reading, lowercase ("mỏe").
    pub reading: Vec<char>,
    /// English score of the keys.
    pub en: i32,
    /// Vietnamese score of the reading.
    pub vn: i32,
    /// English score from the Vikey list alone (no technical text, no inflection), the scale the
    /// word pair votes were tuned with.
    pub en_general: i32,
    /// Looks the same in both modes ("cho", "makes").
    pub same: bool,
    /// Fewer than two capitals (not an acronym or identifier).
    pub few_caps: bool,
}

impl Default for Word {
    fn default() -> Self {
        Word { raw: String::new(), reading: Vec::new(), en: MISSING, vn: MISSING, en_general: MISSING, same: false, few_caps: true }
    }
}

/// A word just before the current one, separated by a single space.
#[derive(Clone, Debug, Default)]
pub struct Prev {
    pub word: Word,
    /// Keys with their case ("This").
    pub keys: Vec<char>,
    /// Vietnamese reading with its case ("Thí").
    pub vietnamese: Vec<char>,
    /// Text left on screen.
    pub shown: Vec<char>,
    /// Shown as the keys typed.
    pub english: bool,
    /// Put back to English by the auto switch itself.
    pub restored: bool,
}

// Vikey's AS_* constants.
const PREFER: i32 = 10; // the English reading must be 10x more common...
const VN_FLOOR: i32 = 10; // ...and the Vietnamese one rarer than ~10 per million
const EXTREME: i32 = 99; // an English word this much more common always wins
const EN_FLOOR: i32 = 99; // English mode: no floor for turning a tie Vietnamese
const TIE_PREV: i32 = 5; // a tie leaning English (3x) after a clearly English word (D22)
const PLAIN_MARGIN: i32 = 15; // "thí makes": the earlier word must read English ~30x more (D13)
const LOOKBACK_MARGIN: i32 = 10;
// Vanh: an earlier word follows a switch to English only when its English reading is at least as
// common as the Vietnamese one. Vikey allows a Vietnamese reading 10x more common (LOOKBACK_MARGIN),
// which turned "cả release" into "car release" (cả 34 vs car 27).
const LOOKBACK_EN_LEAD: i32 = 0;
const PAIR_OVERRIDES: i32 = 10; // word pairs this sure of the Vietnamese reading beat D22 (D34)
const NEXT_PAIR: i32 = 20; // the next word must be this sure to flip a word back (D35)
const TIE_SPAN: i32 = 15;
const BIGRAM_MIN: i32 = 3;
// Vanh: inside a code token the English reading wins unless it is ~4x rarer ("max_" 18 vs mã 22).
const GLUE_MARGIN: i32 = 6;
// Vanh: after two English words in a row the English reading may be ~3x rarer ("tune sets").
const RUN_MARGIN: i32 = 5;
const BIGRAM_WEIGHT: f64 = 2.0;

/// A NUL terminated entry of a word buffer.
fn entry(buffer: &[u8], offset: u32) -> &[u8] {
    let rest = &buffer[offset as usize..];
    &rest[..rest.iter().position(|&b| b == 0).unwrap_or(rest.len())]
}

/// strcmp semantics: the key stops at its first NUL.
fn c_key(key: &[u8]) -> &[u8] {
    &key[..key.iter().position(|&b| b == 0).unwrap_or(key.len())]
}

/// Binary search like the C++ loop (the lists are sorted and hold no duplicates).
fn search(buffer: &[u8], offsets: &[u32], key: &[u8]) -> Option<usize> {
    let key = c_key(key);
    let (mut lo, mut hi) = (0i64, offsets.len() as i64 - 1);
    while lo <= hi {
        let mid = (lo + hi) / 2;
        match entry(buffer, offsets[mid as usize]).cmp(key) {
            std::cmp::Ordering::Equal => return Some(mid as usize),
            std::cmp::Ordering::Less => lo = mid + 1,
            std::cmp::Ordering::Greater => hi = mid - 1,
        }
    }
    None
}

fn lookup(buffer: &[u8], offsets: &[u32], scores: &[i8], word: &[u8]) -> i32 {
    search(buffer, offsets, word).map_or(MISSING, |i| scores[i] as i32)
}

fn pair_count(key: &[u8]) -> i32 {
    search(bigram::KEYS, &bigram::OFFSETS, key).map_or(0, |i| (bigram::LOG_COUNTS[i] as f64 / 8.0).exp2().round() as i32)
}

fn vote(cv: i32, ce: i32) -> i32 {
    if cv + ce < BIGRAM_MIN {
        return 0;
    }
    let v = (10.0 * ((cv as f64 + 0.5) / (ce as f64 + 0.5)).log10()).clamp(-30.0, 30.0);
    (v * BIGRAM_WEIGHT).round() as i32
}

/// The UTF-8 bytes of the C++ helper, which only writes up to three byte sequences (the readings
/// are Vietnamese letters, so it never sees anything wider).
fn utf8(s: &[char]) -> Vec<u8> {
    let mut out = Vec::with_capacity(s.len() * 2);
    for &ch in s {
        let c = ch as u32;
        if c < 0x80 {
            out.push(c as u8);
        } else if c < 0x800 {
            out.push((0xC0 | (c >> 6)) as u8);
            out.push((0x80 | (c & 0x3F)) as u8);
        } else {
            out.push((0xE0 | (c >> 12)) as u8);
            out.push((0x80 | ((c >> 6) & 0x3F)) as u8);
            out.push((0x80 | (c & 0x3F)) as u8);
        }
    }
    out
}

/// The word before as it shows: the keys, or the Vietnamese reading.
fn surface(p: &Prev) -> Vec<u8> {
    if p.english {
        p.word.raw.as_bytes().to_vec()
    } else {
        utf8(&p.word.reading)
    }
}

fn joined(a: &[u8], b: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(a.len() + b.len() + 1);
    out.extend_from_slice(a);
    out.push(b'\t');
    out.extend_from_slice(b);
    out
}

/// > 0: the Vietnamese reading follows the word before more often, < 0: the English one (D30).
fn bigram_vote(p: &Prev, w: &Word) -> i32 {
    let before = surface(p);
    vote(pair_count(&joined(&before, &utf8(&w.reading))), pair_count(&joined(&before, w.raw.as_bytes())))
}

/// > 0: the word after follows the Vietnamese reading of the earlier word more often (D35).
fn next_pair_vote(earlier: &Word, next: &Word) -> i32 {
    let after = if next.same { next.raw.as_bytes().to_vec() } else { utf8(&next.reading) };
    vote(pair_count(&joined(&utf8(&earlier.reading), &after)), pair_count(&joined(earlier.raw.as_bytes(), &after)))
}

/// Vowels with their tones (no tone, s, f, r, x, j) and their Telex keys.
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
const TONE_KEYS: [u8; 6] = [0, b's', b'f', b'r', b'x', b'j'];

/// Vikey's word key: Telex letters with ^/đ doubled, horn/breve as w, the tone key last.
fn vietnamese_key(word: &[char]) -> Option<String> {
    let mut out = String::new();
    let mut tone = 0u8;
    for &ch in word {
        if ch == 'đ' {
            out.push_str("dd");
            continue;
        }
        if ch.is_ascii_lowercase() {
            out.push(ch);
            continue;
        }
        let (forms, keys) = VOWELS.iter().find(|(forms, _)| forms.contains(&ch))?;
        out.push_str(keys);
        let t = forms.iter().position(|&f| f == ch).unwrap_or(0);
        if t != 0 {
            tone = TONE_KEYS[t];
        }
    }
    if tone != 0 {
        out.push(tone as char);
    }
    (!out.is_empty() && out.len() < 40).then_some(out)
}

/// The reading is spelled like a Vietnamese syllable: no f/j/w/z left over from the keys, and
/// nothing after the vowels but a final consonant. "dif" (diff after the Telex undo) and "lits"
/// (lists) are not, so they cannot borrow the score of dì and lít, whose keys they look like.
fn syllable_shaped(reading: &[char]) -> bool {
    if reading.iter().any(|c| matches!(c, 'f' | 'j' | 'w' | 'z')) {
        return false;
    }
    let is_vowel = |c: &char| !c.is_ascii() || matches!(c, 'a' | 'e' | 'i' | 'o' | 'u' | 'y');
    let Some(last_vowel) = reading.iter().rposition(is_vowel) else {
        return false;
    };
    let tail: String = reading[last_vowel + 1..].iter().collect();
    matches!(tail.as_str(), "" | "c" | "ch" | "m" | "n" | "ng" | "nh" | "p" | "t")
}

fn vietnamese_score(reading: &[char]) -> i32 {
    if !syllable_shaped(reading) {
        return MISSING;
    }
    match vietnamese_key(reading) {
        Some(key) => lookup(words::VN_WORDS, &words::VN_OFFSETS, &words::VN_SCORES, key.as_bytes()),
        None => MISSING,
    }
}

/// Both ways a word, and the English reading at least 10x the Vietnamese one.
fn clearly_english(w: &Word) -> bool {
    w.en != MISSING && (w.vn == MISSING || w.en - w.vn >= PREFER)
}

fn general_lookup(word: &str) -> i32 {
    lookup(words::EN_WORDS, &words::EN_OFFSETS, &words::EN_SCORES, word.as_bytes())
}

fn tech_lookup(word: &str) -> i32 {
    TECH_WORDS.binary_search_by(|(w, _)| w.as_bytes().cmp(word.as_bytes())).map_or(MISSING, |i| TECH_WORDS[i].1 as i32 - TECH_PENALTY)
}

/// The English score: the Vikey (subtitles) score, or the technical text score (manual pages)
/// when that is higher, so "host", "docs" and "config" count as the common words they are in a
/// developer's text. TECH_PENALTY keeps a word that is only common in manuals a little behind.
fn en_lookup(word: &str) -> i32 {
    general_lookup(word).max(tech_lookup(word))
}

// Vanh: a word missing from the list may be an inflection of one in it ("reverts", "docs").
const INFLECTION_PENALTY: i32 = 5;
const TECH_PENALTY: i32 = 3;
const TECH_VOTE_CAP: i32 = 5;

/// (English score, Vikey score) of the keys. `may_derive`: a word in neither list may still
/// score as camelCase parts or an inflection; only asked when the Vietnamese reading is no word,
/// which keeps the extra lookups off the common path (Vietnamese words typed in Telex).
fn english_scores(raw: &str, keys: &[char], may_derive: bool) -> (i32, i32) {
    let general = general_lookup(raw);
    let direct = general.max(tech_lookup(raw));
    if direct != MISSING || !may_derive {
        return (direct, general);
    }
    // camelCase / PascalCase: every part an English word ("SessionEnd", "SurroundingText").
    if (1..keys.len()).any(|i| keys[i].is_ascii_uppercase() && keys[i - 1].is_ascii_lowercase()) {
        let mut score = i32::MAX;
        let mut start = 0;
        for end in (1..=keys.len()).filter(|&i| i == keys.len() || (keys[i].is_ascii_uppercase() && keys[i - 1].is_ascii_lowercase())) {
            let part = &raw[start..end];
            let s = if part.len() < 2 { MISSING } else { stem_score(part, en_lookup(part)) };
            if s == MISSING {
                return (MISSING, general);
            }
            score = score.min(s);
            start = end;
        }
        return (score, general);
    }
    (stem_score(raw, direct), general)
}

/// The score of the word (`direct`, already looked up), or of its stem minus a penalty.
fn stem_score(word: &str, direct: i32) -> i32 {
    if direct != MISSING {
        return direct;
    }
    for suffix in ["s", "es", "ed", "d", "ing", "ly"] {
        if let Some(stem) = word.strip_suffix(suffix) {
            if stem.len() >= 3 {
                let s = en_lookup(stem);
                if s != MISSING {
                    return s - INFLECTION_PENALTY;
                }
            }
        }
    }
    MISSING
}

/// None when the keys are not letters only, or too short/long.
pub fn analyze(keys: &[char], reading_lower: &[char]) -> Option<Word> {
    if keys.len() < 2 || keys.len() >= 40 {
        return None;
    }
    let mut out = Word::default();
    let mut caps = 0;
    for (i, &key) in keys.iter().enumerate() {
        let mut c = key;
        if c.is_ascii_uppercase() {
            // DD is one capital (Đ): "DDaf Nawngx" is Đà Nẵng (D39).
            if !(i > 0 && c == 'D' && keys[i - 1] == 'D') {
                caps += 1;
            }
            c = c.to_ascii_lowercase();
        }
        if !c.is_ascii_lowercase() {
            return None;
        }
        out.raw.push(c);
    }
    out.few_caps = caps < 2;
    out.reading = reading_lower.to_vec();
    out.same = out.reading.iter().copied().eq(out.raw.chars());
    out.vn = vietnamese_score(&out.reading);
    (out.en, out.en_general) = english_scores(&out.raw, keys, out.vn == MISSING);
    Some(out)
}

/// Vietnamese mode: may an earlier word follow a switch to English (it reads English too)?
pub fn lookback_ok(w: &Word) -> bool {
    if w.same {
        return w.en != MISSING && w.vn == MISSING; // English-only, looks the same ("makes")
    }
    w.en != MISSING && w.vn != MISSING && w.en >= w.vn + LOOKBACK_EN_LEAD
}

/// The word before that may lean the current one English or Vietnamese. In Vietnamese mode a
/// word that looks the same both ways ("it") right after a clearly Vietnamese word is read as
/// Vietnamese text, so English word pairs with it do not count ("học it bên", not "it been").
pub fn context_prev(history: &[Prev], english_mode: bool) -> Option<&Prev> {
    let prev = history.first()?;
    if !english_mode && !prev.english && prev.word.same {
        if let Some(before) = history.get(1) {
            let b = &before.word;
            let vietnamese = !before.english && !b.same && b.vn != MISSING && (b.en == MISSING || b.vn - b.en >= PREFER);
            if vietnamese {
                return None;
            }
        }
    }
    Some(prev)
}

/// A word glued into a code token ("max_tokens", "docs/", "--host", "SW_SHOW"): back to the keys
/// unless the Vietnamese reading is a common word, clearly more common than the English one
/// ("tên/DOB", "và/hoặc").
/// `strong`: glued by a character prose does not use between words ('_', '-', '.', '='...); a '/'
/// alone ("docs/", "mùng/mồng") also needs the keys to be an English word.
pub fn restore_glued(w: &Word, strong: bool) -> bool {
    if w.same {
        return false;
    }
    if w.en != MISSING && w.en + GLUE_MARGIN >= w.vn {
        return true;
    }
    if !w.few_caps && (strong || w.vn == MISSING) {
        return true; // SW_SHOW, but not `TIÊU ĐỀ`
    }
    strong && (w.vn == MISSING || w.vn < VN_FLOOR)
}

/// Inside a run of English words (`run` of them before this one on the line, punctuation between
/// allowed), a word whose English reading is about as common as the Vietnamese one goes back to
/// its keys: "git has no", "tune sets" after two English words. A clearly Vietnamese word stays
/// ("fix bug cả": cả 34 vs car 27).
pub fn restore_in_run(w: &Word, run: u32) -> bool {
    if w.same || w.en == MISSING || !w.few_caps || run == 0 {
        return false;
    }
    // One English word before: the English reading must lean English ("git has"); two: it may be a
    // little rarer ("tune and holdout sets").
    let margin = if run >= 2 { RUN_MARGIN } else { -RUN_MARGIN };
    w.vn == MISSING || w.en + margin >= w.vn
}

/// The run of English words after this one.
pub fn next_run(w: &Word, shown_english: bool, run: u32) -> u32 {
    // English only, or far more common in English ("git": the Vietnamese list has it at -2).
    let english_only = w.en != MISSING && (w.vn == MISSING || w.en - w.vn >= 2 * PREFER);
    if shown_english && !w.same || (w.same && english_only) {
        run + 1
    } else if w.vn != MISSING && (!w.same || w.en <= w.vn) {
        0 // Vietnamese, or the same both ways but more common in Vietnamese ("theo")
    } else {
        run
    }
}

/// The English score where it is weighed against the Vietnamese one by Vikey's own rules: the
/// technical text may lift the subtitle score by TECH_VOTE_CAP at most ("version còn": conf is
/// common in manuals only, còn stays).
fn en_vote(w: &Word) -> i32 {
    if w.en == MISSING {
        return MISSING;
    }
    w.en_general.max(w.en - TECH_VOTE_CAP).min(w.en)
}

/// Vietnamese mode: should the word go back to its keys?
pub fn restore_in_vietnamese(w: &Word, prev: Option<&Prev>) -> bool {
    let prev = prev.filter(|p| !p.english);
    if w.same {
        // "thí makes": an English-only word after an ambiguous word that leans English (D13).
        let Some(prev) = prev else {
            return false;
        };
        let p = &prev.word;
        return lookback_ok(p) && !p.same && p.en >= p.vn + PLAIN_MARGIN && w.en != MISSING && w.vn == MISSING;
    }
    if w.en == MISSING {
        return false;
    }
    if let Some(prev) = prev {
        if w.vn != MISSING {
            let v = if w.en_general == MISSING { 0 } else { bigram_vote(prev, w) };
            if v != 0 {
                // Vanh: the pair vote weighs against the subtitle score it was tuned with; a word
                // missing from the subtitles ("conf") gets no vote ("version còn").
                let en = en_vote(w);
                return en - w.vn - v >= PREFER && (w.vn < VN_FLOOR || v <= -PREFER);
            }
            // "video call thí" -> "this" (D22)
            let p = &prev.word;
            if w.en - w.vn >= TIE_PREV && p.same && p.en != MISSING && p.vn == MISSING {
                return true;
            }
        }
    }
    w.vn == MISSING || (w.vn < VN_FLOOR && w.en - w.vn >= PREFER) || w.en - w.vn >= EXTREME
}

/// English mode (the word before went back to English): should the word stay as the keys?
pub fn stay_english(w: &Word, prev: Option<&Prev>) -> bool {
    if w.same || !w.few_caps || w.vn == MISSING {
        return true;
    }
    let v = match prev {
        Some(prev) if w.en != MISSING => bigram_vote(prev, w),
        _ => 0,
    };
    let to_vietnamese = w.en == MISSING || (w.en < EN_FLOOR && w.vn - en_vote(w) + v >= PREFER);
    if !to_vietnamese {
        return true;
    }
    // A tie right after a clearly English word typed as English stays English: "the theme"
    // (D22), unless word pairs are sure of the Vietnamese reading (D34).
    if let Some(prev) = prev {
        // Vanh: only a near tie ("theme" 14 vs thêm 27); "inbound có" (cos 15 vs có 42) is none.
        let tie = w.en + TIE_SPAN >= w.vn;
        if prev.english && !prev.restored && tie && w.en != MISSING && v < PAIR_OVERRIDES && clearly_english(&prev.word) {
            return true;
        }
    }
    false
}

/// English mode, a word turned Vietnamese: should the word before it turn Vietnamese too?
pub fn flip_back_before(prev: &Prev, next: &Word, first: bool) -> bool {
    let p = &prev.word;
    if !prev.english || p.same || p.en == MISSING || p.vn == MISSING {
        return false;
    }
    let strong_next = first && next_pair_vote(p, next) >= NEXT_PAIR;
    if first && prev.restored {
        return strong_next; // switched on purpose: only strong evidence ("its nhất" -> "ít nhất")
    }
    p.vn >= p.en - LOOKBACK_MARGIN || strong_next
}

#[cfg(test)]
mod tests {
    use super::*;

    fn chars(s: &str) -> Vec<char> {
        s.chars().collect()
    }

    #[test]
    fn word_lists_are_sorted() {
        for (buffer, offsets) in [
            (words::VN_WORDS, &words::VN_OFFSETS[..]),
            (words::EN_WORDS, &words::EN_OFFSETS[..]),
            (bigram::KEYS, &bigram::OFFSETS[..]),
        ] {
            assert!(offsets.windows(2).all(|w| entry(buffer, w[0]) < entry(buffer, w[1])));
        }
        assert_eq!(words::VN_OFFSETS.len(), words::VN_SCORES.len());
        assert_eq!(words::EN_OFFSETS.len(), words::EN_SCORES.len());
        assert_eq!(bigram::OFFSETS.len(), bigram::LOG_COUNTS.len());
        assert_eq!(words::VN_OFFSETS.len(), 8143);
        assert_eq!(words::EN_OFFSETS.len(), 41225);
        assert_eq!(bigram::OFFSETS.len(), 160386);
    }

    #[test]
    fn analyzes_words() {
        let w = analyze(&chars("more"), &chars("mỏe")).unwrap();
        assert_eq!(w.raw, "more");
        assert!(w.en != MISSING && !w.same && w.few_caps);
        assert!(restore_in_vietnamese(&w, None));
        let cho = analyze(&chars("cho"), &chars("cho")).unwrap();
        assert!(cho.same && cho.vn != MISSING);
        assert!(!restore_in_vietnamese(&cho, None));
        assert!(analyze(&chars("a"), &chars("a")).is_none());
        assert!(analyze(&chars("a1"), &chars("a1")).is_none());
        assert!(!analyze(&chars("HTML"), &chars("html")).unwrap().few_caps);
        assert!(analyze(&chars("DDaf"), &chars("đà")).unwrap().few_caps);
        assert_eq!(vietnamese_key(&chars("người")).as_deref(), Some("nguwowif"));
        assert_eq!(vietnamese_key(&chars("đặng")).as_deref(), Some("ddawngj"));
        assert_eq!(vietnamese_key(&chars("x1")), None);
    }
}
