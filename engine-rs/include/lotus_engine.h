/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file lotus_engine.h
 * @brief C ABI of the Rust typing engine (engine-rs/src/ffi.rs).
 *
 * Strings go in as (pointer, length): UTF-8 for configuration text (LotusStr), UTF-32 for engine
 * text (a value that is no Unicode scalar becomes U+FFFD). Strings come out through a sink
 * callback whose pointer is only valid during the call. Handles are opaque and freed by whoever
 * created them; a key processor keeps its own reference to a shared macro table or dictionary.
 * No function unwinds. The C++ wrappers in engine-rs/cxx are the intended users.
 */

#ifndef LOTUS_ENGINE_FFI_H
#define LOTUS_ENGINE_FFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LotusInputMethod  LotusInputMethod;
typedef struct LotusMacroTable   LotusMacroTable;
typedef struct LotusDictionary   LotusDictionary;
typedef struct LotusLearned      LotusLearned;
typedef struct LotusKeyProcessor LotusKeyProcessor;

/** A UTF-8 string slice. */
typedef struct LotusStr {
    const char* data;
    size_t      len;
} LotusStr;

typedef void (*LotusU32Sink)(void* ctx, const uint32_t* data, size_t len);
typedef void (*LotusBytesSink)(void* ctx, const char* data, size_t len);

/** Options of a key processor; w2u and bracket_transform are -1 to follow the engine flags. */
typedef struct LotusOptions {
    bool     auto_non_vn_restore;
    bool     dd_free_style;
    bool     english_restore;
    bool     macro_enabled;
    bool     auto_capitalize_macro;
    bool     spell_check_with_dicts;
    bool     modern_style;
    bool     free_marking;
    uint8_t  output_charset;
    int8_t   w2u;
    int8_t   bracket_transform;
    LotusStr time_format;
    LotusStr date_format;
} LotusOptions;

/* Input methods. A built-in one belongs to the library and is never freed. */
const LotusInputMethod* lotus_input_method_builtin(uint8_t id);
LotusInputMethod*       lotus_input_method_parse(
          const LotusStr* keys, const LotusStr* values, size_t count);
void lotus_input_method_free(LotusInputMethod* method);

/* Macro table (trigger -> expansion). */
LotusMacroTable* lotus_macro_table_new(void);
void             lotus_macro_table_free(LotusMacroTable* table);
void             lotus_macro_table_set(LotusMacroTable* table, LotusStr key, LotusStr value);
bool             lotus_macro_table_is_empty(const LotusMacroTable* table);
bool             lotus_macro_table_get(
                const LotusMacroTable* table, const uint32_t* key, size_t len, LotusU32Sink sink, void* ctx);

/* Custom dictionary for the spell check. */
LotusDictionary* lotus_dictionary_new(void);
void             lotus_dictionary_free(LotusDictionary* dictionary);
void             lotus_dictionary_load_from_text(LotusDictionary* dictionary, LotusStr text);
void             lotus_dictionary_load_from_words(
                LotusDictionary* dictionary, const LotusStr* words, size_t count);
bool lotus_dictionary_has_word(const LotusDictionary* dictionary, const uint32_t* word, size_t len);
bool lotus_dictionary_has_word_or_prefix(
    const LotusDictionary* dictionary, const uint32_t* word, size_t len);
size_t lotus_dictionary_size(const LotusDictionary* dictionary);

/* Preferences learned from reverts (opt-in). */
LotusLearned* lotus_learned_new(void);
void          lotus_learned_free(LotusLearned* learned);
void          lotus_learned_load_from_text(LotusLearned* learned, LotusStr text);
void          lotus_learned_to_text(const LotusLearned* learned, LotusBytesSink sink, void* ctx);
size_t        lotus_learned_size(const LotusLearned* learned);

/* Key processor. */
LotusKeyProcessor* lotus_key_processor_new(
    const LotusInputMethod* method, bool spell_check_with_dicts_default);
void lotus_key_processor_free(LotusKeyProcessor* processor);
void lotus_key_processor_set_options(LotusKeyProcessor* processor, const LotusOptions* options);
void lotus_key_processor_set_macro_enabled(LotusKeyProcessor* processor, bool enabled);
void lotus_key_processor_set_should_restore_key_strokes(LotusKeyProcessor* processor, bool restore);
bool lotus_key_processor_should_restore_key_strokes(const LotusKeyProcessor* processor);
void lotus_key_processor_set_macro_table(
    LotusKeyProcessor* processor, const LotusMacroTable* table);
void lotus_key_processor_set_dictionary(
    LotusKeyProcessor* processor, const LotusDictionary* dictionary);
void lotus_key_processor_set_learned(LotusKeyProcessor* processor, const LotusLearned* learned);
bool lotus_key_processor_process_key(
    LotusKeyProcessor* processor, uint32_t key_val, uint32_t states);
bool lotus_key_processor_can_process_key(const LotusKeyProcessor* processor, uint32_t key_val);
void lotus_key_processor_preedit(const LotusKeyProcessor* processor, LotusU32Sink sink, void* ctx);
void lotus_key_processor_take_commit(LotusKeyProcessor* processor, LotusU32Sink sink, void* ctx);
void lotus_key_processor_commit_preedit(LotusKeyProcessor* processor);
void lotus_key_processor_reset(LotusKeyProcessor* processor);
void lotus_key_processor_rebuild_from_text(
    LotusKeyProcessor* processor, const uint32_t* text, size_t len);
void lotus_key_processor_set_rewrite_allowed(LotusKeyProcessor* processor, bool allowed);
void lotus_key_processor_take_rewritten_before(
    LotusKeyProcessor* processor, LotusU32Sink sink, void* ctx);
void lotus_key_processor_forget_context(LotusKeyProcessor* processor);
bool lotus_key_processor_end_word(LotusKeyProcessor* processor);
bool lotus_key_processor_revert_last_switch(LotusKeyProcessor* processor);

/* Utilities. */
void lotus_encode(
    uint8_t charset, const uint32_t* text, size_t len, LotusBytesSink sink, void* ctx);
void lotus_format_time(LotusStr format, LotusBytesSink sink, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* LOTUS_ENGINE_FFI_H */
