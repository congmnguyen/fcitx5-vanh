/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dictionary.h"

#include "ffi_util.h"

namespace lotus {

    Dictionary::Dictionary() : handle_(lotus_dictionary_new(), lotus_dictionary_free) {}

    void Dictionary::loadFromWords(const std::vector<std::string>& words) {
        std::vector<LotusStr> slices;
        slices.reserve(words.size());
        for (const auto& word : words) {
            slices.push_back(ffi::str(word));
        }
        lotus_dictionary_load_from_words(handle_.get(), slices.data(), slices.size());
    }

    void Dictionary::loadFromText(std::string_view wholeFile) {
        lotus_dictionary_load_from_text(handle_.get(), ffi::str(wholeFile));
    }

    bool Dictionary::hasWord(std::u32string_view word) const {
        return lotus_dictionary_has_word(handle_.get(), ffi::u32(word), word.size());
    }

    bool Dictionary::hasWordOrPrefix(std::u32string_view word) const {
        return lotus_dictionary_has_word_or_prefix(handle_.get(), ffi::u32(word), word.size());
    }

    std::size_t Dictionary::size() const {
        return lotus_dictionary_size(handle_.get());
    }

} // namespace lotus
