// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! C ABI of the engine, declared for C++ in include/lotus_engine.h.
//!
//! Strings go in as (pointer, length) pairs: UTF-8 bytes for configuration text, UTF-32 code
//! units for engine text (a value that is no Unicode scalar becomes U+FFFD, like a UTF-8 round
//! trip would make it). Strings come out through a sink callback that receives a pointer valid
//! only during the call, so no memory crosses the boundary. Handles are opaque; the caller frees
//! what it created. The functions never unwind (the crate aborts on panic).

use crate::dictionary::Dictionary;
use crate::encoder::encode;
use crate::input_method::cached_input_method;
use crate::key_processor::{KeyProcessor, Options, Shared};
use crate::macro_table::MacroTable;
use crate::rule::InputMethod;
use crate::rules_parser::parse_input_method;
use crate::time_format::format_time;
use crate::types::{BracketTransformMode, Charset, InputMethodId, W2uMode};
use crate::utf8;
use std::ffi::{c_char, c_void};
use std::sync::{Arc, RwLock};

/// A UTF-8 string slice.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LotusStr {
    pub data: *const c_char,
    pub len: usize,
}

pub type LotusU32Sink = extern "C" fn(ctx: *mut c_void, data: *const u32, len: usize);
pub type LotusBytesSink = extern "C" fn(ctx: *mut c_void, data: *const c_char, len: usize);

/// Options of a key processor; `w2u` and `bracket_transform` are -1 to follow the engine flags.
#[repr(C)]
pub struct LotusOptions {
    pub auto_non_vn_restore: bool,
    pub dd_free_style: bool,
    pub english_restore: bool,
    pub macro_enabled: bool,
    pub auto_capitalize_macro: bool,
    pub spell_check_with_dicts: bool,
    pub modern_style: bool,
    pub free_marking: bool,
    pub output_charset: u8,
    pub w2u: i8,
    pub bracket_transform: i8,
    pub time_format: LotusStr,
    pub date_format: LotusStr,
}

pub struct LotusInputMethod(InputMethod);
pub struct LotusMacroTable(Shared<MacroTable>);
pub struct LotusDictionary(Shared<Dictionary>);
pub struct LotusKeyProcessor(KeyProcessor);

unsafe fn bytes<'a>(s: LotusStr) -> &'a [u8] {
    if s.data.is_null() || s.len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(s.data.cast(), s.len)
    }
}

unsafe fn chars(data: *const u32, len: usize) -> Vec<char> {
    if data.is_null() || len == 0 {
        return Vec::new();
    }
    std::slice::from_raw_parts(data, len).iter().map(|&c| utf8::from_u32(c)).collect()
}

fn send_chars(sink: LotusU32Sink, ctx: *mut c_void, text: &[char]) {
    // char has the size and alignment of u32.
    sink(ctx, text.as_ptr().cast(), text.len());
}

fn send_bytes(sink: LotusBytesSink, ctx: *mut c_void, text: &[u8]) {
    sink(ctx, text.as_ptr().cast(), text.len());
}

fn w2u_mode(value: i8) -> Option<W2uMode> {
    match value {
        0 => Some(W2uMode::Disabled),
        1 => Some(W2uMode::NonStart),
        2 => Some(W2uMode::Everywhere),
        _ => None,
    }
}

fn bracket_mode(value: i8) -> Option<BracketTransformMode> {
    match value {
        0 => Some(BracketTransformMode::Disabled),
        1 => Some(BracketTransformMode::NonStart),
        2 => Some(BracketTransformMode::Everywhere),
        _ => None,
    }
}

// ---- Input methods ------------------------------------------------------------------------

/// A built-in input method, owned by the library (never freed). Null for an unknown id.
#[no_mangle]
pub extern "C" fn lotus_input_method_builtin(id: u8) -> *const LotusInputMethod {
    match InputMethodId::from_index(id as usize) {
        // LotusInputMethod is a transparent-in-practice newtype; keep a static copy of each.
        Some(id) => {
            use std::sync::OnceLock;
            static BUILTIN: OnceLock<Vec<LotusInputMethod>> = OnceLock::new();
            let all = BUILTIN.get_or_init(|| InputMethodId::ALL.iter().map(|&i| LotusInputMethod(cached_input_method(i).clone())).collect());
            &all[id as usize]
        }
        None => std::ptr::null(),
    }
}

/// Parses a definition (key -> line pairs, UTF-8). Free with lotus_input_method_free.
///
/// # Safety
/// `keys` and `values` point to `count` valid strings.
#[no_mangle]
pub unsafe extern "C" fn lotus_input_method_parse(keys: *const LotusStr, values: *const LotusStr, count: usize) -> *mut LotusInputMethod {
    let mut definition = Vec::with_capacity(count);
    for i in 0..count {
        definition.push((bytes(*keys.add(i)), bytes(*values.add(i))));
    }
    Box::into_raw(Box::new(LotusInputMethod(parse_input_method(&definition))))
}

/// # Safety
/// `method` comes from lotus_input_method_parse (or is null).
#[no_mangle]
pub unsafe extern "C" fn lotus_input_method_free(method: *mut LotusInputMethod) {
    if !method.is_null() {
        drop(Box::from_raw(method));
    }
}

// ---- Macro table --------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn lotus_macro_table_new() -> *mut LotusMacroTable {
    Box::into_raw(Box::new(LotusMacroTable(Arc::new(RwLock::new(MacroTable::default())))))
}

/// # Safety
/// `table` comes from lotus_macro_table_new (or is null). Processors keep their own reference.
#[no_mangle]
pub unsafe extern "C" fn lotus_macro_table_free(table: *mut LotusMacroTable) {
    if !table.is_null() {
        drop(Box::from_raw(table));
    }
}

/// # Safety
/// Valid handle and strings.
#[no_mangle]
pub unsafe extern "C" fn lotus_macro_table_set(table: *mut LotusMacroTable, key: LotusStr, value: LotusStr) {
    (*table).0.write().unwrap().set(bytes(key), bytes(value));
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_macro_table_is_empty(table: *const LotusMacroTable) -> bool {
    (*table).0.read().unwrap().is_empty()
}

/// Sends the expansion of a trigger word; false (and no call) when there is none.
///
/// # Safety
/// Valid handle and string.
#[no_mangle]
pub unsafe extern "C" fn lotus_macro_table_get(
    table: *const LotusMacroTable,
    key: *const u32,
    len: usize,
    sink: LotusU32Sink,
    ctx: *mut c_void,
) -> bool {
    match (*table).0.read().unwrap().get(&chars(key, len)) {
        Some(value) => {
            send_chars(sink, ctx, value);
            true
        }
        None => false,
    }
}

// ---- Dictionary ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn lotus_dictionary_new() -> *mut LotusDictionary {
    Box::into_raw(Box::new(LotusDictionary(Arc::new(RwLock::new(Dictionary::default())))))
}

/// # Safety
/// `dictionary` comes from lotus_dictionary_new (or is null).
#[no_mangle]
pub unsafe extern "C" fn lotus_dictionary_free(dictionary: *mut LotusDictionary) {
    if !dictionary.is_null() {
        drop(Box::from_raw(dictionary));
    }
}

/// # Safety
/// Valid handle and string.
#[no_mangle]
pub unsafe extern "C" fn lotus_dictionary_load_from_text(dictionary: *mut LotusDictionary, text: LotusStr) {
    (*dictionary).0.write().unwrap().load_from_text(bytes(text));
}

/// # Safety
/// Valid handle; `words` points to `count` valid strings.
#[no_mangle]
pub unsafe extern "C" fn lotus_dictionary_load_from_words(dictionary: *mut LotusDictionary, words: *const LotusStr, count: usize) {
    let list: Vec<&[u8]> = (0..count).map(|i| bytes(*words.add(i))).collect();
    (*dictionary).0.write().unwrap().load_from_words(&list);
}

/// # Safety
/// Valid handle and string.
#[no_mangle]
pub unsafe extern "C" fn lotus_dictionary_has_word(dictionary: *const LotusDictionary, word: *const u32, len: usize) -> bool {
    (*dictionary).0.read().unwrap().has_word(&chars(word, len))
}

/// # Safety
/// Valid handle and string.
#[no_mangle]
pub unsafe extern "C" fn lotus_dictionary_has_word_or_prefix(dictionary: *const LotusDictionary, word: *const u32, len: usize) -> bool {
    (*dictionary).0.read().unwrap().has_word_or_prefix(&chars(word, len))
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_dictionary_size(dictionary: *const LotusDictionary) -> usize {
    (*dictionary).0.read().unwrap().words().len()
}

// ---- Key processor ------------------------------------------------------------------------

/// A processor driving its own copy of the input method. Free with lotus_key_processor_free.
///
/// # Safety
/// `method` is a valid input method handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_new(method: *const LotusInputMethod, spell_check_with_dicts_default: bool) -> *mut LotusKeyProcessor {
    Box::into_raw(Box::new(LotusKeyProcessor(KeyProcessor::new((*method).0.clone(), spell_check_with_dicts_default))))
}

/// # Safety
/// `processor` comes from lotus_key_processor_new (or is null).
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_free(processor: *mut LotusKeyProcessor) {
    if !processor.is_null() {
        drop(Box::from_raw(processor));
    }
}

/// # Safety
/// Valid handle and options.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_set_options(processor: *mut LotusKeyProcessor, options: *const LotusOptions) {
    let o = &*options;
    let options = Options {
        auto_non_vn_restore: o.auto_non_vn_restore,
        dd_free_style: o.dd_free_style,
        english_restore: o.english_restore,
        macro_enabled: o.macro_enabled,
        auto_capitalize_macro: o.auto_capitalize_macro,
        spell_check_with_dicts: o.spell_check_with_dicts,
        modern_style: o.modern_style,
        free_marking: o.free_marking,
        output_charset: Charset::from_index(o.output_charset as usize).unwrap_or(Charset::Unicode),
        w2u: w2u_mode(o.w2u),
        bracket_transform: bracket_mode(o.bracket_transform),
        time_format: bytes(o.time_format).to_vec(),
        date_format: bytes(o.date_format).to_vec(),
    };
    (*processor).0.set_options(options);
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_set_macro_enabled(processor: *mut LotusKeyProcessor, enabled: bool) {
    (*processor).0.set_macro_enabled(enabled);
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_set_should_restore_key_strokes(processor: *mut LotusKeyProcessor, restore: bool) {
    (*processor).0.set_should_restore_key_strokes(restore);
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_should_restore_key_strokes(processor: *const LotusKeyProcessor) -> bool {
    (*processor).0.should_restore_key_strokes()
}

/// Shares a macro table (null disables the lookups); the processor keeps its own reference.
///
/// # Safety
/// Valid handles.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_set_macro_table(processor: *mut LotusKeyProcessor, table: *const LotusMacroTable) {
    let table = if table.is_null() { None } else { Some((*table).0.clone()) };
    (*processor).0.set_macro_table(table);
}

/// Shares a dictionary (null disables the lookups); the processor keeps its own reference.
///
/// # Safety
/// Valid handles.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_set_dictionary(processor: *mut LotusKeyProcessor, dictionary: *const LotusDictionary) {
    let dictionary = if dictionary.is_null() { None } else { Some((*dictionary).0.clone()) };
    (*processor).0.set_dictionary(dictionary);
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_process_key(processor: *mut LotusKeyProcessor, key_val: u32, states: u32) -> bool {
    (*processor).0.process_key(key_val, states)
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_can_process_key(processor: *const LotusKeyProcessor, key_val: u32) -> bool {
    (*processor).0.can_process_key(key_val)
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_preedit(processor: *const LotusKeyProcessor, sink: LotusU32Sink, ctx: *mut c_void) {
    send_chars(sink, ctx, (*processor).0.preedit());
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_take_commit(processor: *mut LotusKeyProcessor, sink: LotusU32Sink, ctx: *mut c_void) {
    let text = (*processor).0.take_commit();
    send_chars(sink, ctx, &text);
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_commit_preedit(processor: *mut LotusKeyProcessor) {
    (*processor).0.commit_preedit();
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_reset(processor: *mut LotusKeyProcessor) {
    (*processor).0.reset();
}

/// # Safety
/// Valid handle and string.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_rebuild_from_text(processor: *mut LotusKeyProcessor, text: *const u32, len: usize) {
    (*processor).0.rebuild_from_text(&chars(text, len));
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_set_rewrite_allowed(processor: *mut LotusKeyProcessor, allowed: bool) {
    (*processor).0.set_rewrite_allowed(allowed);
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_take_rewritten_before(processor: *mut LotusKeyProcessor, sink: LotusU32Sink, ctx: *mut c_void) {
    let text = (*processor).0.take_rewritten_before();
    send_chars(sink, ctx, &text);
}

/// # Safety
/// Valid handle.
#[no_mangle]
pub unsafe extern "C" fn lotus_key_processor_forget_context(processor: *mut LotusKeyProcessor) {
    (*processor).0.forget_context();
}

// ---- Utilities ----------------------------------------------------------------------------

/// Encodes text in a charset (UTF-8 bytes of the code points standing for the legacy bytes).
///
/// # Safety
/// Valid string.
#[no_mangle]
pub unsafe extern "C" fn lotus_encode(charset: u8, text: *const u32, len: usize, sink: LotusBytesSink, ctx: *mut c_void) {
    let charset = Charset::from_index(charset as usize).unwrap_or(Charset::Unicode);
    send_bytes(sink, ctx, encode(charset, &chars(text, len)).as_bytes());
}

/// Formats the current local time like the $TIME / $DATE macros.
///
/// # Safety
/// Valid string.
#[no_mangle]
pub unsafe extern "C" fn lotus_format_time(format: LotusStr, sink: LotusBytesSink, ctx: *mut c_void) {
    send_bytes(sink, ctx, &format_time(bytes(format)));
}
