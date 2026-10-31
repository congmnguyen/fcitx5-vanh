// SPDX-License-Identifier: GPL-3.0-or-later
//
// English restore at a word break, after Vikey's auto switch (single word, no look-back):
// when the keys typed are an English word and the Vietnamese reading is missing or much rarer,
// the word goes back to the keys ("more", not "mỏe").
#pragma once

#include <string_view>

namespace lotus {
    /**
     * @brief Whether a word should be restored to the keys typed.
     * @param vietnamese The Vietnamese reading, lowercase ("mỏe").
     * @param raw The keys typed, lowercase ASCII letters ("more").
     */
    bool preferEnglishWord(std::u32string_view vietnamese, std::string_view raw);
} // namespace lotus
