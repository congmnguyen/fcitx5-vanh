/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file dictionary.h
 * @brief Custom word list of the spell check (engine-rs/src/dictionary.rs), from
 *        bamboo/dictionary.go.
 *
 * Like the macro table, a key processor keeps its own reference to the dictionary data, so a
 * dictionary shared through a std::shared_ptr is seen by every processor that uses it.
 */

#ifndef _LOTUS_DICTIONARY_H_
#define _LOTUS_DICTIONARY_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct LotusDictionary;

namespace lotus {

    /** @brief Sorted, duplicate-free list of lowercase words, like Go's dictionary. */
    class Dictionary {
      public:
        Dictionary();

        /**
         * @brief Replaces the words, like Go's NewDictionaryFromWords: lowercased, empty ones
         *        dropped.
         * @param words Words, UTF-8.
         */
        void loadFromWords(const std::vector<std::string>& words);

        /**
         * @brief Replaces the words with those of a dictionary file, like Go's NewDictionary.
         *
         * The text is split on '\n' with the trailing '\r' of a CRLF line removed, every line is
         * lowercased, and empty lines are dropped.
         * @param wholeFile Whole content of the dictionary file.
         */
        void loadFromText(std::string_view wholeFile);

        /** @brief True when the word is stored; an empty word never matches. */
        [[nodiscard]] bool hasWord(std::u32string_view word) const;

        /** @brief True when a stored word equals or begins with the word; empty never matches. */
        [[nodiscard]] bool hasWordOrPrefix(std::u32string_view word) const;

        /** @brief Number of stored words. */
        [[nodiscard]] std::size_t size() const;

        /** @brief Handle for the C ABI. */
        [[nodiscard]] const LotusDictionary* handle() const {
            return handle_.get();
        }

      private:
        std::unique_ptr<LotusDictionary, void (*)(LotusDictionary*)> handle_;
    };

} // namespace lotus

#endif // _LOTUS_DICTIONARY_H_
