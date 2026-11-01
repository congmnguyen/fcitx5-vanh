/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file utf8.cpp
 * @brief Implementation of the UTF-8 <-> UTF-32 helpers.
 */
#include "utf8.h"

#include <algorithm>

namespace lotus::utf8 {

    namespace {
        /// Replacement character returned for every invalid sequence. Go: utf8.RuneError.
        constexpr char32_t kReplacement = 0xFFFD;
        constexpr char32_t kTrailMask   = 0x3FU;

        char32_t           toCodePoint(char c) {
            return static_cast<char32_t>(static_cast<unsigned char>(c));
        }

        bool isContinuation(char c) {
            return (toCodePoint(c) & 0xC0U) == 0x80U;
        }

        bool isSurrogateOrBeyond(char32_t codePoint) {
            return codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU);
        }

        /// Decodes the sequence at in[index] and advances index; an invalid one consumes a
        /// single byte, like Go's range over a string.
        char32_t decodeOne(std::string_view in, std::size_t& index) {
            const char32_t    b0        = toCodePoint(in[index]);
            const std::size_t available = in.size() - index;
            const auto        trail     = [&](std::size_t offset) {
                return toCodePoint(in[index + offset]) & kTrailMask;
            };
            // Whether `count` continuation bytes follow.
            const auto continued = [&](std::size_t count) {
                return available > count &&
                    std::ranges::all_of(in.substr(index + 1, count), isContinuation);
            };

            char32_t    codePoint = kReplacement;
            std::size_t size      = 1;
            if (b0 < 0x80U) {
                codePoint = b0;
            } else if (b0 >= 0xC2U && b0 <= 0xDFU && continued(1)) {
                codePoint = ((b0 & 0x1FU) << 6U) | trail(1U);
                size      = 2;
            } else if (b0 >= 0xE0U && b0 <= 0xEFU && continued(2)) {
                const char32_t value = ((b0 & 0x0FU) << 12U) | (trail(1U) << 6U) | trail(2U);
                if (value >= 0x800U && !isSurrogateOrBeyond(value)) {
                    codePoint = value;
                    size      = 3;
                }
            } else if (b0 >= 0xF0U && b0 <= 0xF4U && continued(3)) {
                const char32_t value =
                    ((b0 & 0x07U) << 18U) | (trail(1U) << 12U) | (trail(2U) << 6U) | trail(3U);
                if (value >= 0x10000U && value <= 0x10FFFFU) {
                    codePoint = value;
                    size      = 4;
                }
            }
            index += size;
            return codePoint;
        }
    } // namespace

    std::size_t length(std::string_view in) {
        return static_cast<std::size_t>(std::ranges::count_if(in, [](char c) {
            return !isContinuation(c);
        }));
    }

    char32_t last(std::string_view in) {
        if (in.empty()) {
            return 0;
        }
        std::size_t start  = in.size();
        std::size_t walked = 0;
        while (start > 0 && walked < 3U && isContinuation(in[start - 1])) {
            --start;
            ++walked;
        }
        if (start == 0) {
            return kReplacement;
        }
        std::size_t    index     = start - 1;
        const char32_t codePoint = decodeOne(in, index);
        return index == in.size() ? codePoint : kReplacement;
    }

    std::u32string decode(std::string_view in) {
        std::u32string out;
        out.reserve(in.size());
        std::size_t index = 0;
        while (index < in.size()) {
            out.push_back(decodeOne(in, index));
        }
        return out;
    }

    std::string encode(std::u32string_view in) {
        std::string out;
        out.reserve(in.size());
        for (char32_t codePoint : in) {
            if (isSurrogateOrBeyond(codePoint)) {
                // Go: string(rune) replaces out of range and surrogate halves with U+FFFD.
                codePoint = kReplacement;
            }
            if (codePoint < 0x80U) {
                out.push_back(static_cast<char>(codePoint));
                continue;
            }
            if (codePoint < 0x800U) {
                out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            } else if (codePoint < 0x10000U) {
                out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
                out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & kTrailMask)));
            } else {
                out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
                out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & kTrailMask)));
                out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & kTrailMask)));
            }
            out.push_back(static_cast<char>(0x80U | (codePoint & kTrailMask)));
        }
        return out;
    }

} // namespace lotus::utf8
