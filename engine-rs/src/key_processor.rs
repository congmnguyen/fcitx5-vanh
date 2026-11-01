// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
// SPDX-FileCopyrightText: 2026-2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Key event wrapper around the engine, ported 1:1 from bamboo/fcitxbambooengine.go, plus the
//! English restore with its context (the words before the current one).
//!
//! The processor decides which keys reach the engine, when the composition is committed, and
//! how macros, the output charset and the English restore are applied. Key values stay in the
//! fcitx keysym domain because Go compares the raw key value with BackSpace/Space/Tab.

use crate::case_map;
use crate::dictionary::Dictionary;
use crate::encoder::encode;
use crate::engine::Engine;
use crate::english::{self, Prev, Word};
use crate::learning::{self, Learned};
use crate::macro_table::MacroTable;
use crate::rule::InputMethod;
use crate::time_format::format_time;
use crate::types::{key_state, BracketTransformMode, Charset, EngineFlags, Mode, W2uMode};
use crate::utf8;
use crate::utils;
use std::sync::{Arc, RwLock};

pub const KEY_BACKSPACE: u32 = 0xFF08;
pub const KEY_SPACE: u32 = 0x020;
pub const KEY_TAB: u32 = 0xFF09;

/// Case of the word that triggered a macro, 1:1 with Go's VnCase* constants.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MacroCase {
    AllSmall,
    AllCapital,
    NoChange,
}

pub fn determine_macro_case(text: &[char]) -> MacroCase {
    let mut has_lower = false;
    let mut has_upper = false;
    for &c in text {
        if case_map::is_lower(c) {
            has_lower = true;
        } else if case_map::is_upper(c) {
            has_upper = true;
        }
        if has_lower && has_upper {
            return MacroCase::NoChange;
        }
    }
    if has_lower {
        MacroCase::AllSmall
    } else if has_upper {
        MacroCase::AllCapital
    } else {
        MacroCase::NoChange
    }
}

/// Options of the processor, 1:1 with Go's FcitxBambooEngineOption.
#[derive(Clone, Debug, PartialEq)]
pub struct Options {
    /// Restore the raw key strokes of a non-Vietnamese word.
    pub auto_non_vn_restore: bool,
    /// Allow "dd" inside a non-Vietnamese sequence.
    pub dd_free_style: bool,
    /// Restore an English word to its keys at a word break (Vikey lists).
    pub english_restore: bool,
    /// Expand macros at a word break and on Tab.
    pub macro_enabled: bool,
    /// Follow the case of the trigger word.
    pub auto_capitalize_macro: bool,
    /// Spell check against the custom dictionary.
    pub spell_check_with_dicts: bool,
    /// "hòa" instead of "hoà".
    pub modern_style: bool,
    /// Free tone marking style.
    pub free_marking: bool,
    /// Charset of the committed text.
    pub output_charset: Charset,
    /// None follows the engine flags, the mode a fresh engine starts in.
    pub w2u: Option<W2uMode>,
    pub bracket_transform: Option<BracketTransformMode>,
    /// Formats of the $TIME and $DATE macros (UTF-8 bytes).
    pub time_format: Vec<u8>,
    pub date_format: Vec<u8>,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            auto_non_vn_restore: true,
            dd_free_style: true,
            english_restore: false,
            macro_enabled: false,
            auto_capitalize_macro: false,
            spell_check_with_dicts: true,
            modern_style: true,
            free_marking: true,
            output_charset: Charset::Unicode,
            w2u: None,
            bracket_transform: None,
            time_format: b"%H:%M".to_vec(),
            date_format: b"%d/%m/%Y".to_vec(),
        }
    }
}

/// A table shared by every input context.
pub type Shared<T> = Arc<RwLock<T>>;

pub struct KeyProcessor {
    engine: Engine,
    options: Options,
    macro_table: Option<Shared<MacroTable>>,
    dictionary: Option<Shared<Dictionary>>,
    preedit: Vec<char>,
    commit: Vec<char>,
    last_key_with_shift: bool,
    should_restore_key_strokes: bool,
    /// Keys typed for the current word (English restore).
    typed_keys: Vec<char>,
    /// False after a rebuild or Backspace.
    typed_keys_known: bool,
    /// Words before the current one, newest first.
    history: Vec<Prev>,
    /// The word before went back to English.
    english_mode: bool,
    rewrite_allowed: bool,
    rewritten_before: Vec<char>,
    /// Text committed since the last space, while it is known (the token the word is part of).
    token: Vec<char>,
    token_known: bool,
    /// The key ending the word being decided (None: Enter, or no word break).
    break_key: Option<char>,
    /// English words in a row on this line, punctuation between them allowed.
    en_run: u32,
    /// The word just finished, while it can still be reverted to its other reading.
    last_switch: Option<LastSwitch>,
    /// Preferences learned from reverts (opt-in).
    learned: Option<Shared<Learned>>,
}

/// A finished word and the reading it did not get.
#[derive(Clone, Debug)]
struct LastSwitch {
    shown: Vec<char>,
    other: Vec<char>,
    /// The word break typed after it (empty at Enter).
    tail: Vec<char>,
    /// Lowercase keys, the learning key.
    keys: String,
    /// The shown text is the keys.
    english: bool,
}

/// How a word sits in its token.
enum Glue {
    None,
    /// Glued by a character that also joins prose words ('/', ':', '`') when `strong` is false.
    Code { strong: bool },
    Hex,
    /// Glued to Vietnamese text ("tên/ngày"): an ordinary word.
    Vietnamese,
}

/// Characters that glue a word into a code token (identifier, path, flag) right before it.
const GLUE_BEFORE: &[char] = &['_', '/', '\\', '.', '=', '@', '#', '$', '%', '&', '+', '~', '|', '`', ':'];
/// Word break keys that glue the word to what follows ("max_", "docs/").
const GLUE_AFTER: &[char] = &['_', '/', '\\', '=', '@', '#', '$', '%', '&', '+', '~', '|'];

fn replace_all(text: &[char], needle: &[char], replacement: &[char]) -> Vec<char> {
    if needle.is_empty() {
        return text.to_vec();
    }
    let mut out = Vec::with_capacity(text.len());
    let mut index = 0;
    while index < text.len() {
        if text[index..].starts_with(needle) {
            out.extend_from_slice(replacement);
            index += needle.len();
        } else {
            out.push(text[index]);
            index += 1;
        }
    }
    out
}

fn contains(text: &[char], needle: &[char]) -> bool {
    text.windows(needle.len()).any(|w| w == needle)
}

impl KeyProcessor {
    /// Like Go's NewEngine (spell check on) / NewCustomEngine (spell check off).
    pub fn new(input_method: InputMethod, spell_check_with_dicts_default: bool) -> KeyProcessor {
        KeyProcessor {
            engine: Engine::new(input_method, EngineFlags::STD),
            options: Options { spell_check_with_dicts: spell_check_with_dicts_default, ..Options::default() },
            macro_table: None,
            dictionary: None,
            preedit: Vec::new(),
            commit: Vec::new(),
            last_key_with_shift: false,
            should_restore_key_strokes: false,
            typed_keys: Vec::new(),
            typed_keys_known: true,
            history: Vec::new(),
            english_mode: false,
            rewrite_allowed: false,
            rewritten_before: Vec::new(),
            token: Vec::new(),
            token_known: true,
            break_key: None,
            en_run: 0,
            last_switch: None,
            learned: None,
        }
    }

    /// Applies a whole option set, like Go's EngineSetOption.
    pub fn set_options(&mut self, options: Options) {
        let mut flags = EngineFlags::STD;
        if options.modern_style {
            flags.remove(EngineFlags::STD_TONE_STYLE);
        } else {
            flags.add(EngineFlags::STD_TONE_STYLE);
        }
        if options.free_marking {
            flags.add(EngineFlags::FREE_TONE_MARKING);
        } else {
            flags.remove(EngineFlags::FREE_TONE_MARKING);
        }
        // Go compared against 0: an unset (follow flags) mode enabled the flag, Disabled cleared it.
        if options.w2u != Some(W2uMode::Disabled) {
            flags.add(EngineFlags::W2U);
        } else {
            flags.remove(EngineFlags::W2U);
        }
        if options.bracket_transform != Some(BracketTransformMode::Disabled) {
            flags.add(EngineFlags::BRACKET_TRANSFORM);
        } else {
            flags.remove(EngineFlags::BRACKET_TRANSFORM);
        }
        self.engine.set_flag(flags);
        self.engine.set_w2u_mode(options.w2u);
        self.engine.set_bracket_transform_mode(options.bracket_transform);
        self.options = options;
    }

    /// The options in use. Writing them does not recompute the engine flags (only set_options
    /// does), like setting the Go fields directly.
    pub fn options_mut(&mut self) -> &mut Options {
        &mut self.options
    }

    pub fn options(&self) -> &Options {
        &self.options
    }

    pub fn set_macro_enabled(&mut self, enabled: bool) {
        self.options.macro_enabled = enabled;
    }

    pub fn set_should_restore_key_strokes(&mut self, restore: bool) {
        self.should_restore_key_strokes = restore;
    }

    pub fn should_restore_key_strokes(&self) -> bool {
        self.should_restore_key_strokes
    }

    pub fn set_macro_table(&mut self, table: Option<Shared<MacroTable>>) {
        self.macro_table = table;
    }

    pub fn set_dictionary(&mut self, dictionary: Option<Shared<Dictionary>>) {
        self.dictionary = dictionary;
    }

    pub fn engine(&mut self) -> &mut Engine {
        &mut self.engine
    }

    pub fn engine_ref(&self) -> &Engine {
        &self.engine
    }

    /// Processes one key event, like Go's preeditProcessKeyEvent; true when the key was handled.
    pub fn process_key(&mut self, key_val: u32, states: u32) -> bool {
        // Go defers updateLastKeyWithShift, so the bookkeeping runs on every return path.
        let accepted = self.preedit_process_key_event(key_val, states);
        self.update_last_key_with_shift(key_val, states);
        accepted
    }

    /// The key as a character; keysyms beyond Unicode (and surrogates) are no engine key.
    fn key_char(key_val: u32) -> Option<char> {
        char::from_u32(key_val)
    }

    fn engine_can_process(&self, key_val: u32) -> bool {
        Self::key_char(key_val).is_some_and(|c| self.engine.can_process_key(c))
    }

    fn preedit_process_key_event(&mut self, key_val: u32, states: u32) -> bool {
        self.last_switch = None; // a revert only follows the word break right away
        let has_preedit = !self.preedit.is_empty();

        // Workaround for chrome's address bar and Google SpreadSheets.
        if !self.should_restore_key_strokes
            && (!Self::is_valid_state(states)
                || !self.can_process_key(key_val)
                || (!self.options.macro_enabled && !has_preedit && !self.engine_can_process(key_val)))
        {
            if has_preedit {
                let text = self.get_preedit_string();
                self.commit_preedit_and_reset(text);
            }
            self.forget_context();
            return false;
        }

        if key_val == KEY_BACKSPACE {
            if !has_preedit {
                self.forget_context(); // into the text before: the earlier words may change
                return false;
            }
            if self.rune_count() == 1 {
                self.commit_preedit_and_reset(Vec::new());
                return true;
            }
            self.typed_keys_known = false;
            self.engine.remove_last_char(true);
            let text = self.get_preedit_string();
            self.update_preedit(&text);
            return true;
        }

        let old_text = self.get_preedit_string();
        if key_val == KEY_TAB {
            match self.get_macro_text() {
                Some(macro_text) => self.commit_preedit_and_reset(macro_text),
                None => {
                    let text = self.get_composed_string(&old_text);
                    self.commit_preedit_and_reset(text);
                    return false;
                }
            }
            return true;
        }

        let (new_text, is_word_break) = self.get_commit_text(key_val, states, &old_text);
        if is_word_break {
            self.commit_preedit_and_reset(new_text);
            return true;
        }
        self.update_preedit(&new_text);
        true
    }

    fn get_commit_text(&mut self, key_val: u32, states: u32, old_text: &[char]) -> (Vec<char>, bool) {
        // Restore key strokes by pressing Shift + Space.
        if self.should_restore_key_strokes {
            self.should_restore_key_strokes = false;
            self.engine.restore_last_word(!utils::has_any_vietnamese_rune(old_text));
            return (self.get_preedit_string(), false);
        }
        // A keysym beyond Unicode is no engine key, no digit and no word break: it ends up in the
        // last branch below, which does not look at the key.
        let Some(mut key) = Self::key_char(key_val) else {
            return (Vec::new(), true);
        };
        let can_process = self.engine.can_process_key(key);
        if can_process || (self.options.macro_enabled && key.is_ascii_digit()) {
            if !can_process && self.should_fallback_to_english(true) {
                self.engine.restore_last_word(false);
            }
            if states & key_state::LOCK != 0 {
                key = self.to_upper(key);
            }
            self.typed_keys.push(key);
            let mode = self.get_bamboo_input_mode();
            self.engine.process_key(key, mode);
            if self.engine.input_method().appending_keys.contains(&key) {
                let full_seq = self.get_processed_string(Mode::VIETNAMESE);
                let new_text = if self.should_fallback_to_english(true) {
                    self.get_processed_string(Mode::ENGLISH | Mode::FULL_TEXT)
                } else {
                    full_seq.clone()
                };
                if full_seq.last() == Some(&key) {
                    // [[ => [
                    let ret = self.get_preedit_string();
                    let is_word_break = ret.last().is_some_and(|&c| utils::is_word_break_symbol(c));
                    // Go calls this hacking: the word break rune left behind by the doubled key
                    // is re-typed as a space in English mode.
                    if is_word_break {
                        self.engine.remove_last_char(false);
                        self.engine.process_key(' ', Mode::ENGLISH);
                    }
                    return (ret, is_word_break);
                }
                if new_text.last() == Some(&key) {
                    // f] => f]
                    let is_word_break = utils::is_word_break_symbol(key);
                    if is_word_break {
                        self.engine.remove_last_char(false);
                        self.engine.process_key(' ', Mode::ENGLISH);
                    }
                    let mut text = old_text.to_vec();
                    text.push(key);
                    return (text, is_word_break);
                }
                // ] => o?
                return (self.get_preedit_string(), false);
            }
            return (self.get_preedit_string(), false);
        }
        if utils::is_word_break_symbol(key) {
            if self.options.macro_enabled {
                let expansion = self.macro_table.as_ref().and_then(|t| t.read().unwrap().get(old_text).map(<[char]>::to_vec));
                if let Some(macro_val) = expansion {
                    self.history.clear();
                    self.engine.reset();
                    let mut text = self.expand_macro(old_text, &macro_val);
                    text.push(key);
                    return (text, true);
                }
            }
            return (self.break_word(Some(key), old_text), true);
        }
        (Vec::new(), true)
    }

    /// The word ends (a word break key, or Enter when `key` is None): decides between the
    /// Vietnamese text and the keys and returns the text to commit (the break key included).
    fn break_word(&mut self, key: Option<char>, old_text: &[char]) -> Vec<char> {
        self.break_key = key;
        // The word as typed, before a restore changes the engine.
        let keys = self.current_keys();
        let vietnamese = self.get_processed_string(Mode::VIETNAMESE | Mode::FULL_TEXT);
        let info = if self.options.english_restore {
            english::analyze(&keys, &self.get_processed_string(Mode::VIETNAMESE | Mode::LOWER_CASE))
        } else {
            None
        };
        let word;
        let is_english;
        // Also for words that show no Vietnamese letter but lost a Telex undo key ("eror").
        if info.as_ref().is_some_and(|w| self.restore_to_keys(w, &keys)) {
            self.engine.restore_last_word(false);
            word = keys.clone();
            is_english = true;
        } else if utils::has_any_vietnamese_rune(old_text) && self.invalid_vietnamese() {
            // (the English restore was just asked: only the invalid word check is left)
            self.engine.restore_last_word(false);
            word = self.get_processed_string(Mode::ENGLISH | Mode::FULL_TEXT);
            is_english = true;
        } else {
            word = old_text.to_vec();
            is_english = false;
        }
        self.break_key = None;
        if let Some(w) = info.as_ref() {
            self.en_run = english::next_run(w, is_english, self.en_run);
        }
        let mut text = self.finish_word(info.as_ref(), &keys, &vietnamese, &word, is_english, key.unwrap_or('\n'));
        // What the revert key brings back: the other reading (moved, not copied: the key path).
        if let Some(w) = info {
            let other = if is_english { vietnamese } else { keys };
            if other != word && !other.is_empty() {
                self.last_switch = Some(LastSwitch {
                    shown: word.clone(),
                    other,
                    tail: key.into_iter().collect(),
                    keys: w.raw,
                    english: is_english,
                });
            }
        }
        match key {
            Some(' ') | None => {
                self.token.clear();
                self.token_known = true;
            }
            Some(k) => {
                self.token.extend_from_slice(&word);
                self.token.push(k);
            }
        }
        text.extend_from_slice(&word);
        if let Some(key) = key {
            text.push(key);
            self.engine.process_key(key, Mode::ENGLISH);
        }
        text
    }

    /// Enter (or another line end) after a word: the word is decided as at a word break, so an
    /// English word typed last on a line goes back to its keys too ("... is" + Enter, not "í").
    /// The text goes to the commit (with take_rewritten_before), the engine is reset and the
    /// context forgotten. True when the commit differs from the preedit shown, i.e. the caller
    /// must replace the shown word before passing Enter on.
    pub fn end_word(&mut self) -> bool {
        let shown = self.get_preedit_string();
        if shown.is_empty() || self.should_restore_key_strokes {
            self.forget_context();
            self.token_known = true;
            return false;
        }
        let mut text = self.break_word(None, &shown);
        if text.is_empty() {
            text = shown.clone();
        }
        let before = std::mem::take(&mut self.rewritten_before);
        let changed = text != shown || !before.is_empty();
        self.commit_preedit_and_reset(text);
        self.forget_context();
        self.last_switch = None; // the cursor is on the next line now
        self.token_known = true; // a new line starts
        self.rewritten_before = before;
        changed
    }

    fn encode_text(&self, text: &[char]) -> Vec<char> {
        // Go keeps the encoded bytes in a string; at the boundary of the port they are decoded back
        // to code points. Unicode is the identity.
        if self.options.output_charset == Charset::Unicode {
            return text.to_vec();
        }
        encode(self.options.output_charset, text).chars().collect()
    }

    fn get_macro_text(&self) -> Option<Vec<char>> {
        if !self.options.macro_enabled || self.macro_table_empty() {
            return None;
        }
        let text = self.get_processed_string(Mode::PUNCTUATION);
        let macro_val = self.macro_table.as_ref()?.read().unwrap().get(&text)?.to_vec();
        Some(self.expand_macro(&text, &macro_val))
    }

    fn get_processed_string(&self, mode: Mode) -> Vec<char> {
        self.engine.processed_string(mode)
    }

    fn get_preedit_string(&self) -> Vec<char> {
        if self.should_fallback_to_english(true) {
            return self.get_processed_string(Mode::ENGLISH | Mode::FULL_TEXT);
        }
        self.get_processed_string(Mode::PUNCTUATION | Mode::FULL_TEXT)
    }

    fn get_composed_string(&self, old_text: &[char]) -> Vec<char> {
        if utils::has_any_vietnamese_rune(old_text) && self.must_fallback_to_english() {
            return self.get_processed_string(Mode::ENGLISH | Mode::FULL_TEXT);
        }
        old_text.to_vec()
    }

    fn rune_count(&self) -> usize {
        self.get_preedit_string().len()
    }

    fn get_bamboo_input_mode(&self) -> Mode {
        if self.should_fallback_to_english(false) {
            Mode::ENGLISH
        } else {
            Mode::VIETNAMESE
        }
    }

    fn update_last_key_with_shift(&mut self, key_val: u32, states: u32) {
        self.last_key_with_shift = self.engine_can_process(key_val) && states & key_state::SHIFT != 0;
    }

    fn macro_table_empty(&self) -> bool {
        self.macro_table.as_ref().is_none_or(|t| t.read().unwrap().is_empty())
    }

    fn macro_lookup(&self, text: &[char]) -> bool {
        self.macro_table.as_ref().is_some_and(|t| t.read().unwrap().get(text).is_some())
    }

    /// Expands a macro value, like Go's expandMacro.
    pub fn expand_macro(&self, trigger: &[char], macro_text: &[char]) -> Vec<char> {
        const TIME: [char; 5] = ['$', 'T', 'I', 'M', 'E'];
        const DATE: [char; 5] = ['$', 'D', 'A', 'T', 'E'];
        let mut text = macro_text.to_vec();
        if !self.options.time_format.is_empty() && contains(&text, &TIME) {
            text = replace_all(&text, &TIME, &utf8::decode(&format_time(&self.options.time_format)));
        }
        if !self.options.date_format.is_empty() && contains(&text, &DATE) {
            text = replace_all(&text, &DATE, &utf8::decode(&format_time(&self.options.date_format)));
        }
        if self.options.auto_capitalize_macro {
            match determine_macro_case(trigger) {
                MacroCase::AllSmall => return case_map::lower(&text),
                MacroCase::AllCapital => return case_map::upper(&text),
                MacroCase::NoChange => {}
            }
        }
        text
    }

    /// Stores the encoded preedit text and clears the pending commit, like Go's updatePreedit.
    pub fn update_preedit(&mut self, processed: &[char]) {
        let encoded = self.encode_text(processed);
        if encoded.is_empty() {
            self.preedit.clear();
            self.commit.clear();
            return;
        }
        self.preedit = encoded;
    }

    /// Stores a text to commit and resets the engine, like Go's commitPreeditAndReset.
    pub fn commit_preedit_and_reset(&mut self, text: Vec<char>) {
        self.typed_keys.clear();
        self.typed_keys_known = true;
        self.commit = text;
        self.preedit.clear();
        self.engine.reset();
    }

    fn english_restore_text(&self) -> Option<Vec<char>> {
        if !self.options.english_restore {
            return None;
        }
        let vn = self.get_processed_string(Mode::VIETNAMESE | Mode::LOWER_CASE);
        let keys = self.current_keys();
        let w = english::analyze(&keys, &vn)?;
        self.restore_to_keys(&w, &keys).then_some(keys)
    }

    /// The English restore decision for the word ending now (`w`, analysed from `keys`).
    fn restore_to_keys(&self, w: &english::Word, keys: &[char]) -> bool {
        match self.glued(keys) {
            Glue::Hex => return true, // "…0727aa95": hex digits, never a word
            // Part of an identifier, path or flag ("max_tokens", "docs/", "--host"): the keys,
            // unless they read clearly as a Vietnamese word ("và/hoặc", "tên/DOB").
            Glue::Code { strong } => return english::restore_glued(w, strong),
            Glue::Vietnamese => {}
            Glue::None => {}
        }
        match self.learned_preference(&w.raw) {
            p if p >= learning::DECIDES => return true,
            p if p <= -learning::DECIDES => return false,
            _ => {}
        }
        let prev = english::context_prev(&self.history, self.english_mode);
        if !self.english_mode && english::restore_in_run(w, self.en_run) {
            return true; // "the poller is", "git has no", "utf8) as thin"
        }
        if self.english_mode { english::stay_english(w, prev) } else { english::restore_in_vietnamese(w, prev) }
    }

    /// How the word is glued to code characters around it, inside one token.
    fn glued(&self, keys: &[char]) -> Glue {
        let digit_before = self.token_known && self.token.last().is_some_and(char::is_ascii_digit);
        let digit_after = self.break_key.is_some_and(|k| k.is_ascii_digit());
        if (digit_before || digit_after) && keys.iter().all(char::is_ascii_hexdigit) {
            return Glue::Hex;
        }
        let weak = |c: char| matches!(c, '/' | ':' | '`');
        let after = self.break_key.filter(|k| GLUE_AFTER.contains(k));
        let before = if self.token_known { self.token.last().copied() } else { None };
        if before.is_none() && after.is_none() {
            return Glue::None;
        }
        // The word piece right before the glue character ("giờ" in "giờ/mã").
        let piece = self.token[..self.token.len().saturating_sub(1)].iter().rev().take_while(|&&c| !utils::is_word_break_symbol(c));
        let vietnamese_token = self.token_known && piece.into_iter().any(|&c| !c.is_ascii() && utils::is_vietnamese_rune(case_map::lower(&[c])[0]));
        let before = before.filter(|&c| {
            GLUE_BEFORE.contains(&c) || c.is_ascii_digit() ||
                // "-af", "--host", "MM-DD", but not "Rịa-Vũng".
                (c == '-' && !vietnamese_token)
        });
        if before.is_none() && after.is_none() {
            return Glue::None;
        }
        if vietnamese_token {
            return Glue::Vietnamese;
        }
        let strong = before.is_some_and(|c| !weak(c)) || after.is_some_and(|c| !weak(c));
        Glue::Code { strong }
    }

    fn current_keys(&self) -> Vec<char> {
        // The keys as typed, Telex undo keys included ("error", not "eror"); after a rebuild only
        // the engine's rendering of the keys is known.
        if self.typed_keys_known && !self.typed_keys.is_empty() {
            self.typed_keys.clone()
        } else {
            self.get_processed_string(Mode::ENGLISH | Mode::FULL_TEXT)
        }
    }

    /// Records the finished word for the next one and returns the rewritten earlier words.
    fn finish_word(
        &mut self,
        info: Option<&Word>,
        keys: &[char],
        vietnamese: &[char],
        shown: &[char],
        is_english: bool,
        break_key: char,
    ) -> Vec<char> {
        self.rewritten_before.clear();
        if !self.options.english_restore {
            return Vec::new();
        }
        let Some(w) = info else {
            self.history.clear(); // digits, one letter...: the chain of words breaks here
            return Vec::new();
        };
        let was_english = self.english_mode;
        let mut new_before = Vec::new();
        let mut old_before = Vec::new();
        if self.rewrite_allowed {
            let mut back = 0;
            if is_english && !was_english && w.few_caps {
                // Ambiguous words just before follow the switch to English ("thí error" -> "this error").
                while back < self.history.len() && !self.history[back].english && english::lookback_ok(&self.history[back].word) {
                    back += 1;
                }
                while back > 0 && self.history[back - 1].word.same {
                    back -= 1; // no need to retype unchanged words at the far end
                }
                for b in (0..back).rev() {
                    new_before.extend_from_slice(&self.history[b].keys);
                    new_before.push(' ');
                    old_before.extend_from_slice(&self.history[b].shown);
                    old_before.push(' ');
                }
            } else if !is_english && was_english {
                // Back to Vietnamese: a word kept English may read Vietnamese before this one ("lát nữa").
                while back < self.history.len() && english::flip_back_before(&self.history[back], w, back == 0) {
                    back += 1;
                }
                for b in (0..back).rev() {
                    new_before.extend_from_slice(&self.history[b].vietnamese);
                    new_before.push(' ');
                    old_before.extend_from_slice(&self.history[b].shown);
                    old_before.push(' ');
                }
            }
        }
        if new_before == old_before {
            new_before.clear();
            old_before.clear();
        }
        self.rewritten_before = old_before;

        self.english_mode = is_english;
        if is_english != was_english {
            self.history.clear(); // a new run of words in the other language
        }
        if break_key == ' ' {
            self.history.insert(
                0,
                Prev {
                    word: w.clone(),
                    keys: keys.to_vec(),
                    vietnamese: vietnamese.to_vec(),
                    shown: shown.to_vec(),
                    english: is_english,
                    restored: is_english && !was_english,
                },
            );
            self.history.truncate(2);
        } else {
            self.history.clear(); // only words separated by a single space count
        }
        new_before
    }

    /// Allows the English restore to rewrite the words before the current one ("thí makes" ->
    /// "this makes"); only for modes that can delete committed text (uinput, surrounding).
    pub fn set_rewrite_allowed(&mut self, allowed: bool) {
        self.rewrite_allowed = allowed;
    }

    /// Takes the on-screen text of the earlier words the last commit rewrites (with the spaces
    /// after them), to delete before the commit; empty when nothing is rewritten.
    pub fn take_rewritten_before(&mut self) -> Vec<char> {
        if self.rewritten_before.is_empty() {
            return Vec::new();
        }
        let text = std::mem::take(&mut self.rewritten_before);
        self.encode_text(&text)
    }

    /// Shares the table of preferences learned from reverts; None turns learning off.
    pub fn set_learned(&mut self, learned: Option<Shared<Learned>>) {
        self.learned = learned;
    }

    fn learned_preference(&self, keys_lower: &str) -> i32 {
        self.learned.as_ref().map_or(0, |l| l.read().unwrap().preference(keys_lower))
    }

    /// One-key correction of the word just finished (the cursor still right after its word
    /// break): the word changes to its other reading, the keys typed ("host") or the Vietnamese
    /// one ("hót"). The new text goes to the commit and the text to delete first to
    /// take_rewritten_before, as for a rewrite. With learning on, the choice is remembered for
    /// these keys. Pressing it again switches back. False when there is nothing to revert.
    pub fn revert_last_switch(&mut self) -> bool {
        if !self.preedit.is_empty() {
            return false;
        }
        let Some(last) = self.last_switch.take() else {
            return false;
        };
        if let Some(learned) = &self.learned {
            learned.write().unwrap().record(&last.keys, !last.english);
        }
        let mut before = last.shown.clone();
        before.extend_from_slice(&last.tail);
        let mut text = last.other.clone();
        text.extend_from_slice(&last.tail);
        self.forget_context();
        self.token_known = last.tail.as_slice() == [' '];
        self.commit = text;
        self.rewritten_before = before;
        self.last_switch = Some(LastSwitch { shown: last.other, other: last.shown, english: !last.english, ..last });
        true
    }

    /// Forgets the words before the cursor (click, cursor move, focus change, Backspace into an
    /// earlier word): the English restore then judges the next word on its own.
    pub fn forget_context(&mut self) {
        self.last_switch = None;
        self.en_run = 0;
        self.token.clear();
        self.token_known = false;
        self.history.clear();
        self.english_mode = false;
        self.rewritten_before.clear();
    }

    /// The typed word must go back to its keys at a word break, like Go's mustFallbackToEnglish.
    pub fn must_fallback_to_english(&self) -> bool {
        self.english_restore_text().is_some() || self.invalid_vietnamese()
    }

    /// The auto restore part of must_fallback_to_english: the word is no Vietnamese word.
    fn invalid_vietnamese(&self) -> bool {
        if !self.options.auto_non_vn_restore {
            return false;
        }
        let vn_seq = self.get_processed_string(Mode::VIETNAMESE | Mode::LOWER_CASE);
        if vn_seq.is_empty() {
            return false;
        }
        if self.options.english_restore && self.learned.is_some() {
            let keys: String = self.current_keys().iter().map(|c| c.to_ascii_lowercase()).collect();
            if self.learned_preference(&keys) <= -learning::DECIDES {
                return false; // the user reverted these keys to Vietnamese before
            }
        }
        // dd is allowed even in a non-Vietnamese sequence because it is a common abbreviation.
        if self.options.dd_free_style && vn_seq.contains(&'đ') {
            return false;
        }
        if self.options.spell_check_with_dicts {
            return self.dictionary.as_ref().is_none_or(|d| !d.read().unwrap().has_word(&vn_seq));
        }
        !self.engine.is_valid(true)
    }

    /// The composition must render as English, like Go's shouldFallbackToEnglish.
    pub fn should_fallback_to_english(&self, check_vn_rune: bool) -> bool {
        if !self.options.auto_non_vn_restore {
            return false;
        }
        let vn_seq = self.get_processed_string(Mode::VIETNAMESE | Mode::LOWER_CASE);
        let Some(&last) = vn_seq.last() else {
            return false;
        };
        if self.options.macro_enabled && !self.macro_table_empty() && self.macro_lookup(&self.get_processed_string(Mode::PUNCTUATION)) {
            return false;
        }
        // dd is allowed even in a non-Vietnamese sequence because it is a common abbreviation.
        if self.options.dd_free_style && !utils::has_any_vietnamese_vowel(&vn_seq) && (last == 'd' || vn_seq.contains(&'đ')) {
            return false;
        }
        if check_vn_rune && !utils::has_any_vietnamese_rune(&vn_seq) {
            return false;
        }
        // A dictionary word (or the beginning of one) must not go back to the raw key strokes
        // while it is still being typed; an incomplete word is still rejected at the word break
        // by must_fallback_to_english.
        if self.options.spell_check_with_dicts
            && self.dictionary.as_ref().is_some_and(|d| d.read().unwrap().has_word_or_prefix(&vn_seq))
        {
            return false;
        }
        !self.engine.is_valid(false)
    }

    /// The processor wants a key event, like Go's canProcessKey.
    pub fn can_process_key(&self, key_val: u32) -> bool {
        if key_val == KEY_SPACE || key_val == KEY_BACKSPACE || Self::key_char(key_val).is_some_and(utils::is_word_break_symbol) {
            return true;
        }
        if key_val == KEY_TAB
            && self.options.macro_enabled
            && !self.macro_table_empty()
            && !self.preedit.is_empty()
            && self.macro_lookup(&self.get_processed_string(Mode::PUNCTUATION))
        {
            return true;
        }
        self.engine_can_process(key_val)
    }

    /// False when control, alt, super, hyper, meta or the forward modifier is held.
    pub fn is_valid_state(states: u32) -> bool {
        states & key_state::INVALID == 0
    }

    /// Uppercases a key for the bracket transform, like Go's toUpper.
    pub fn to_upper(&self, key: char) -> char {
        let swapped = match key {
            '[' => '{',
            ']' => '}',
            '{' => '[',
            '}' => ']',
            _ => return key,
        };
        if self.engine.can_process_key(key) {
            swapped
        } else {
            key
        }
    }

    pub fn preedit(&self) -> &[char] {
        &self.preedit
    }

    /// Takes the text to commit and clears it, like Go's EnginePullCommit.
    pub fn take_commit(&mut self) -> Vec<char> {
        let text = std::mem::take(&mut self.commit);
        self.encode_text(&text)
    }

    /// Commits the current preedit text, like Go's EngineCommitPreedit.
    pub fn commit_preedit(&mut self) {
        let text = self.get_preedit_string();
        self.commit_preedit_and_reset(text);
    }

    /// Forgets the composition and the pending commit, like Go's ResetEngine.
    pub fn reset(&mut self) {
        self.commit_preedit_and_reset(Vec::new());
    }

    /// Rebuilds the composition from a text, like Go's EngineRebuildFromText.
    pub fn rebuild_from_text(&mut self, text: &[char]) {
        self.engine.rebuild_from_text(text);
        self.typed_keys.clear();
        self.typed_keys_known = false;
        // Go stores the raw rendering here (EngineRebuildFromText does not encode it), unlike
        // update_preedit which stores the charset encoded text.
        self.preedit = self.get_preedit_string();
        self.commit.clear();
    }

    pub fn last_key_with_shift(&self) -> bool {
        self.last_key_with_shift
    }
}
