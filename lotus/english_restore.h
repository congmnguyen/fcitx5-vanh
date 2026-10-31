// SPDX-License-Identifier: GPL-3.0-or-later
//
// English restore at a word break, after Vikey's auto switch (Engine/Engine.cpp, decisions D13, D22,
// D30, D34, D35 in its DECISIONS.md): when the keys typed are an English word and the Vietnamese
// reading is missing or much rarer, the word goes back to the keys ("more", not "mỏe"). The word
// before decides ties ("video call this", "cho con bú") and, once a word went back to English, the
// following words stay English until one reads clearly Vietnamese.
#pragma once

#include <string>
#include <string_view>

namespace lotus::english {
    inline constexpr int kMissing = -1000;

    /** One word as the auto switch sees it. Scores are 10*log10(occurrences per million). */
    struct Word {
        std::string    raw;          ///< Keys typed, lowercase ASCII letters ("more")
        std::u32string reading;      ///< Vietnamese reading, lowercase ("mỏe")
        int            en = kMissing; ///< English score of the keys
        int            vn = kMissing; ///< Vietnamese score of the reading
        bool           same = false;  ///< Looks the same in both modes ("cho", "makes")
        bool           fewCaps = true; ///< Fewer than two capitals (not an acronym or identifier)
    };

    /** A word just before the current one, separated by a single space. */
    struct Prev {
        Word           word;
        std::u32string keys;          ///< Keys with their case ("This")
        std::u32string vietnamese;    ///< Vietnamese reading with its case ("Thí")
        std::u32string shown;         ///< Text left on screen
        bool           english  = false; ///< Shown as the keys typed
        bool           restored = false; ///< Put back to English by the auto switch itself
    };

    /** False when the keys are not letters only, or too short/long. */
    bool analyze(std::u32string_view keys, std::u32string_view readingLower, Word& out);

    /** Vietnamese mode: should the word go back to its keys? */
    bool restoreInVietnamese(const Word& w, const Prev* prev);

    /** English mode (the word before went back to English): should the word stay as the keys? */
    bool stayEnglish(const Word& w, const Prev* prev);

    /** Vietnamese mode: may an earlier word follow a switch to English (it reads English too)? */
    bool lookbackOk(const Word& w);

    /** English mode, a word turned Vietnamese: should the word before it turn Vietnamese too? */
    bool flipBackBefore(const Prev& prev, const Word& next, bool first);
} // namespace lotus::english
