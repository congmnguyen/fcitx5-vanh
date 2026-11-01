/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file utf8.h
 * @brief UTF-8 <-> UTF-32 helpers for the Lotus engine.
 *
 * The engine keeps its state in UTF-32 (`char32_t`), so every conversion happens at the
 * boundary: keys arrive as UCS-4 and commit/preedit leave as UTF-8. The decoding rules match
 * Go's `for range string` / `utf8.DecodeRuneInString`: an invalid sequence yields U+FFFD and
 * consumes a single byte.
 */

#ifndef _LOTUS_UTF8_H_
#define _LOTUS_UTF8_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace lotus::utf8 {

    /**
     * @brief Counts the code points of a UTF-8 string without validating it.
     * @param in UTF-8 string.
     * @return Number of code points (invalid bytes count as one code point each).
     */
    std::size_t length(std::string_view in);

    /**
     * @brief Returns the last code point of a UTF-8 string.
     * @param in UTF-8 string.
     * @return Last code point, 0 for an empty string, U+FFFD for an invalid tail.
     */
    char32_t last(std::string_view in);

    /**
     * @brief Decodes UTF-8 into UTF-32.
     * @param in UTF-8 string.
     * @return Code points; every invalid sequence becomes U+FFFD and consumes one byte.
     */
    std::u32string decode(std::string_view in);

    /**
     * @brief Encodes UTF-32 into UTF-8.
     * @param in Code points.
     * @return UTF-8 string.
     */
    std::string encode(std::u32string_view in);

} // namespace lotus::utf8

#endif // _LOTUS_UTF8_H_
