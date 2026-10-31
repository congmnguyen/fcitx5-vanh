// SPDX-License-Identifier: GPL-3.0-or-later
#include "english_restore.h"

#include "data/vikey_autoswitch_data.h"

#include <cstring>
#include <string>

namespace lotus {
    namespace {
        constexpr int kMissing = -1000;
        // Scores are 10*log10(occurrences per million) (Vikey's AS_* constants).
        constexpr int kPrefer  = 10; // the English reading must be 10x more common...
        constexpr int kVnFloor = 10; // ...and the Vietnamese one rarer than ~10 per million
        constexpr int kExtreme = 99; // (Vikey: an English word this much more common always wins)

        int lookup(const char* words, const unsigned int* offsets, const signed char* scores, int count, const char* word) {
            int lo = 0;
            int hi = count - 1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                const int c   = std::strcmp(words + offsets[mid], word);
                if (c == 0)
                    return scores[mid];
                if (c < 0)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
            return kMissing;
        }

        struct Vowel {
            char32_t    forms[6]; // no tone, s, f, r, x, j
            const char* keys;
        };
        constexpr Vowel kVowels[] = {
            {{U'a', U'á', U'à', U'ả', U'ã', U'ạ'}, "a"},  {{U'ă', U'ắ', U'ằ', U'ẳ', U'ẵ', U'ặ'}, "aw"},
            {{U'â', U'ấ', U'ầ', U'ẩ', U'ẫ', U'ậ'}, "aa"}, {{U'e', U'é', U'è', U'ẻ', U'ẽ', U'ẹ'}, "e"},
            {{U'ê', U'ế', U'ề', U'ể', U'ễ', U'ệ'}, "ee"}, {{U'i', U'í', U'ì', U'ỉ', U'ĩ', U'ị'}, "i"},
            {{U'o', U'ó', U'ò', U'ỏ', U'õ', U'ọ'}, "o"},  {{U'ô', U'ố', U'ồ', U'ổ', U'ỗ', U'ộ'}, "oo"},
            {{U'ơ', U'ớ', U'ờ', U'ở', U'ỡ', U'ợ'}, "ow"}, {{U'u', U'ú', U'ù', U'ủ', U'ũ', U'ụ'}, "u"},
            {{U'ư', U'ứ', U'ừ', U'ử', U'ữ', U'ự'}, "uw"}, {{U'y', U'ý', U'ỳ', U'ỷ', U'ỹ', U'ỵ'}, "y"},
        };
        constexpr char kToneKeys[] = {0, 's', 'f', 'r', 'x', 'j'};

        /** Vikey's word key: Telex letters with ^/đ doubled, horn/breve as w, the tone key last. */
        bool vietnameseKey(std::u32string_view word, std::string& out) {
            char tone = 0;
            for (const char32_t ch : word) {
                if (ch == U'đ') {
                    out += "dd";
                    continue;
                }
                if (ch >= U'a' && ch <= U'z') {
                    out += static_cast<char>(ch);
                    continue;
                }
                bool found = false;
                for (const Vowel& v : kVowels) {
                    for (int t = 0; t < 6; ++t) {
                        if (v.forms[t] == ch) {
                            out += v.keys;
                            if (t != 0)
                                tone = kToneKeys[t];
                            found = true;
                            break;
                        }
                    }
                    if (found)
                        break;
                }
                if (!found)
                    return false;
            }
            if (tone != 0)
                out += tone;
            return !out.empty() && out.size() < 40;
        }
    } // namespace

    bool preferEnglishWord(std::u32string_view vietnamese, std::string_view raw) {
        if (raw.size() < 2 || raw.size() >= 40)
            return false;
        for (const char c : raw) {
            if (c < 'a' || c > 'z')
                return false;
        }
        const std::string rawStr(raw);
        const int         en = lookup(kAutoSwitchEnWords, kAutoSwitchEnOffsets, kAutoSwitchEnScores, kAutoSwitchEnCount, rawStr.c_str());
        if (en == kMissing)
            return false;
        std::string key;
        if (!vietnameseKey(vietnamese, key))
            return false;
        const int vn = lookup(kAutoSwitchVnWords, kAutoSwitchVnOffsets, kAutoSwitchVnScores, kAutoSwitchVnCount, key.c_str());
        return vn == kMissing || (vn < kVnFloor && en - vn >= kPrefer) || en - vn >= kExtreme;
    }
} // namespace lotus
