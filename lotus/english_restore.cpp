// SPDX-License-Identifier: GPL-3.0-or-later
#include "english_restore.h"

#include "data/vikey_autoswitch_data.h"
#include "data/vikey_bigram_data.h"

#include <cmath>
#include <cstring>
#include <string>

namespace lotus::english {
    namespace {
        // Vikey's AS_* constants.
        constexpr int kPrefer         = 10; // the English reading must be 10x more common...
        constexpr int kVnFloor        = 10; // ...and the Vietnamese one rarer than ~10 per million
        constexpr int kExtreme        = 99; // an English word this much more common always wins
        constexpr int kEnFloor        = 99; // English mode: no floor for turning a tie Vietnamese
        constexpr int kTiePrev        = 5;  // a tie leaning English (3x) after a clearly English word (D22)
        constexpr int kPlainMargin    = 15; // "thí makes": the earlier word must read English ~30x more (D13)
        constexpr int kLookbackMargin = 10;
        constexpr int kPairOverrides  = 10; // word pairs this sure of the Vietnamese reading beat D22 (D34)
        constexpr int kNextPair       = 20; // the next word must be this sure to flip a word back (D35)
        constexpr int kBigramMin      = 3;
        constexpr double kBigramWeight = 2.0;

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

        int pairCount(const std::string& key) {
            int lo = 0;
            int hi = kBigramCount - 1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                const int c   = std::strcmp(kBigramKeys + kBigramOffsets[mid], key.c_str());
                if (c == 0)
                    return static_cast<int>(std::lround(std::exp2(kBigramLogCounts[mid] / 8.0)));
                if (c < 0)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
            return 0;
        }

        int vote(int cv, int ce) {
            if (cv + ce < kBigramMin)
                return 0;
            double v = 10 * std::log10((cv + 0.5) / (ce + 0.5));
            if (v > 30)
                v = 30;
            if (v < -30)
                v = -30;
            return static_cast<int>(std::lround(v * kBigramWeight));
        }

        std::string utf8(std::u32string_view s) {
            std::string out;
            for (const char32_t c : s) {
                if (c < 0x80) {
                    out += static_cast<char>(c);
                } else if (c < 0x800) {
                    out += static_cast<char>(0xC0 | (c >> 6));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (c >> 12));
                    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            return out;
        }

        /** The word before as it shows: the keys, or the Vietnamese reading. */
        std::string surface(const Prev& p) {
            return p.english ? p.word.raw : utf8(p.word.reading);
        }

        /** > 0: the Vietnamese reading follows the word before more often, < 0: the English one (D30). */
        int bigramVote(const Prev& p, const Word& w) {
            const std::string before = surface(p) + "\t";
            return vote(pairCount(before + utf8(w.reading)), pairCount(before + w.raw));
        }

        /** > 0: the word after follows the Vietnamese reading of the earlier word more often (D35). */
        int nextPairVote(const Word& earlier, const Word& next) {
            const std::string after = "\t" + (next.same ? next.raw : utf8(next.reading));
            return vote(pairCount(utf8(earlier.reading) + after), pairCount(earlier.raw + after));
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

        int vietnameseScore(std::u32string_view reading) {
            std::string key;
            if (!vietnameseKey(reading, key))
                return kMissing;
            return lookup(kAutoSwitchVnWords, kAutoSwitchVnOffsets, kAutoSwitchVnScores, kAutoSwitchVnCount, key.c_str());
        }

        /** Both ways a word, and the English reading at least 10x the Vietnamese one. */
        bool clearlyEnglish(const Word& w) {
            return w.en != kMissing && (w.vn == kMissing || w.en - w.vn >= kPrefer);
        }
    } // namespace

    bool analyze(std::u32string_view keys, std::u32string_view readingLower, Word& out) {
        if (keys.size() < 2 || keys.size() >= 40)
            return false;
        out       = Word{};
        int caps  = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            char32_t c = keys[i];
            if (c >= U'A' && c <= U'Z') {
                // DD is one capital (Đ): "DDaf Nawngx" is Đà Nẵng (D39)
                if (!(i > 0 && (c == U'D') && keys[i - 1] == U'D'))
                    ++caps;
                c = c - U'A' + U'a';
            }
            if (c < U'a' || c > U'z')
                return false;
            out.raw += static_cast<char>(c);
        }
        out.fewCaps = caps < 2;
        out.reading = std::u32string(readingLower);
        out.same    = out.reading == std::u32string(out.raw.begin(), out.raw.end());
        out.en      = lookup(kAutoSwitchEnWords, kAutoSwitchEnOffsets, kAutoSwitchEnScores, kAutoSwitchEnCount, out.raw.c_str());
        out.vn      = vietnameseScore(out.reading);
        return true;
    }

    bool lookbackOk(const Word& w) {
        if (w.same)
            return w.en != kMissing && w.vn == kMissing; // English-only, looks the same ("makes")
        return w.en != kMissing && w.vn != kMissing && w.en >= w.vn - kLookbackMargin;
    }

    bool restoreInVietnamese(const Word& w, const Prev* prev) {
        if (prev != nullptr && prev->english)
            prev = nullptr;
        if (w.same) {
            // "thí makes": an English-only word after an ambiguous word that leans English (D13).
            if (prev == nullptr)
                return false;
            const Word& p = prev->word;
            return lookbackOk(p) && !p.same && p.en >= p.vn + kPlainMargin && w.en != kMissing && w.vn == kMissing;
        }
        if (w.en == kMissing)
            return false;
        if (prev != nullptr && w.vn != kMissing) {
            const int v = bigramVote(*prev, w);
            if (v != 0)
                return w.en - w.vn - v >= kPrefer && (w.vn < kVnFloor || v <= -kPrefer);
            // "video call thí" -> "this" (D22)
            const Word& p = prev->word;
            if (w.en - w.vn >= kTiePrev && p.same && p.en != kMissing && p.vn == kMissing)
                return true;
        }
        return w.vn == kMissing || (w.vn < kVnFloor && w.en - w.vn >= kPrefer) || w.en - w.vn >= kExtreme;
    }

    bool stayEnglish(const Word& w, const Prev* prev) {
        if (w.same || !w.fewCaps || w.vn == kMissing)
            return true;
        int v = 0;
        if (prev != nullptr && w.en != kMissing)
            v = bigramVote(*prev, w);
        const bool toVietnamese = w.en == kMissing || (w.en < kEnFloor && w.vn - w.en + v >= kPrefer);
        if (!toVietnamese)
            return true;
        // A tie right after a clearly English word typed as English stays English: "the theme" (D22),
        // unless word pairs are sure of the Vietnamese reading (D34).
        if (prev != nullptr && prev->english && !prev->restored && w.en != kMissing && v < kPairOverrides && clearlyEnglish(prev->word))
            return true;
        return false;
    }

    bool flipBackBefore(const Prev& prev, const Word& next, bool first) {
        const Word& p = prev.word;
        if (!prev.english || p.same || p.en == kMissing || p.vn == kMissing)
            return false;
        const bool strongNext = first && nextPairVote(p, next) >= kNextPair;
        if (first && prev.restored)
            return strongNext; // switched on purpose: only strong evidence ("its nhất" -> "ít nhất")
        return p.vn >= p.en - kLookbackMargin || strongNext;
    }
} // namespace lotus::english
