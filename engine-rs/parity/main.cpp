/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file main.cpp
 * @brief Differential test: the former C++ engine and the Rust engine get identical key sequences
 *        and option sets; preedit, commit, rewritten-before text and every return value are
 *        compared after each step.
 *
 * Usage: lotus_parity <golden dir> <dictionary file> <corpus file> [random scenarios] [seed]
 */

#include "driver.h"

// Vikey word lists of the former engine (Telex keys of Vietnamese words, English words).
#include "data/vikey_autoswitch_data.h"

#include <array>
#include <cctype>
#include <string_view>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace parity;

namespace {

    // Small helpers

    std::string utf8(const std::u32string& text) {
        std::string out;
        for (char32_t c : text) {
            const auto cp = static_cast<uint32_t>(c);
            if (cp < 0x80U) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800U) {
                out += static_cast<char>(0xC0U | (cp >> 6U));
                out += static_cast<char>(0x80U | (cp & 0x3FU));
            } else if (cp < 0x10000U) {
                out += static_cast<char>(0xE0U | (cp >> 12U));
                out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
                out += static_cast<char>(0x80U | (cp & 0x3FU));
            } else {
                out += static_cast<char>(0xF0U | ((cp >> 18U) & 0x07U));
                out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
                out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
                out += static_cast<char>(0x80U | (cp & 0x3FU));
            }
        }
        return out;
    }

    std::u32string decode(const std::string& in) {
        std::u32string out;
        for (std::size_t i = 0; i < in.size();) {
            const auto b = static_cast<unsigned char>(in[i]);
            int        n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
            uint32_t   cp = n == 1 ? b : n == 2 ? (b & 0x1FU) : n == 3 ? (b & 0x0FU) : (b & 0x07U);
            for (int k = 1; k < n && i + k < in.size(); ++k) {
                cp = (cp << 6U) | (static_cast<unsigned char>(in[i + k]) & 0x3FU);
            }
            out += static_cast<char32_t>(cp);
            i += n;
        }
        return out;
    }

    std::string hex(const std::u32string& text) {
        std::ostringstream out;
        for (char32_t c : text) {
            out << std::hex << static_cast<uint32_t>(c) << ' ';
        }
        return out.str();
    }

    std::vector<std::string> readLines(const std::string& path) {
        std::ifstream            in(path);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) {
            lines.push_back(line);
        }
        if (lines.empty()) {
            std::cerr << "cannot read " << path << "\n";
            std::exit(2);
        }
        return lines;
    }

    std::vector<std::string> splitTab(const std::string& line) {
        std::vector<std::string> out;
        std::string              field;
        std::istringstream       in(line);
        while (std::getline(in, field, '\t')) {
            out.push_back(field);
        }
        if (!line.empty() && line.back() == '\t') {
            out.emplace_back();
        }
        return out;
    }

    std::vector<std::string> blobWords(const char* blob, const unsigned int* offsets, int count) {
        std::vector<std::string> out;
        out.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            out.emplace_back(blob + offsets[i]);
        }
        return out;
    }

    using Rng = std::mt19937_64;

    std::size_t pick(Rng& rng, std::size_t n) {
        return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    }
    bool chance(Rng& rng, double p) {
        return std::bernoulli_distribution(p)(rng);
    }

    // Key sequence model

    constexpr uint32_t kShift = 1U, kLock = 2U, kCtrl = 4U, kAlt = 8U;
    constexpr uint32_t kBackSpace = 0xFF08U, kTab = 0xFF09U, kReturn = 0xFF0DU, kEscape = 0xFF1BU;

    enum class Op : uint8_t { Key, Rebuild, Forget, Rewrite, Reset, CommitPreedit, MacroEnabled, Restore, SetOptions, Probe, Reshare };

    struct Action {
        Op             op   = Op::Key;
        uint32_t       key  = 0;
        uint32_t       st   = 0;
        bool           flag = false;
        bool           flag2 = false;
        bool           take = true; ///< Take the commit and the rewritten text after the step
        std::u32string text;
        Opts           opts;
    };

    std::string describe(const Action& a) {
        std::ostringstream out;
        switch (a.op) {
            case Op::Key: out << "key " << std::hex << a.key << " st " << a.st << std::dec << (a.take ? "" : " (no take)"); break;
            case Op::Rebuild: out << "rebuild \"" << utf8(a.text) << "\""; break;
            case Op::Forget: out << "forgetContext"; break;
            case Op::Rewrite: out << "setRewriteAllowed " << a.flag; break;
            case Op::Reset: out << "reset"; break;
            case Op::CommitPreedit: out << "commitPreedit"; break;
            case Op::MacroEnabled: out << "setMacroEnabled " << a.flag; break;
            case Op::Restore: out << "setShouldRestoreKeyStrokes " << a.flag; break;
            case Op::SetOptions: out << "setOptions"; break;
            case Op::Probe: out << "canProcessKey " << std::hex << a.key; break;
            case Op::Reshare: out << "reshare macros " << a.flag << " dict " << a.flag2; break;
        }
        return out.str();
    }

    Action key(uint32_t k, uint32_t st = 0) {
        Action a;
        a.key = k;
        a.st  = st;
        return a;
    }

    /** Vietnamese letters with their tones: row = base letter, column = tone (none, huyền, sắc, hỏi, ngã, nặng). */
    struct Letter {
        const char32_t* tones;
        const char*     telex;
        const char*     vni;
        const char*     viqr;
    };
    const std::array<Letter, 24> kLetters = {{
        {U"aàáảãạ", "a", "a", "a"},     {U"ăằắẳẵặ", "aw", "a8", "a("}, {U"âầấẩẫậ", "aa", "a6", "a^"}, {U"eèéẻẽẹ", "e", "e", "e"},
        {U"êềếểễệ", "ee", "e6", "e^"},  {U"iìíỉĩị", "i", "i", "i"},    {U"oòóỏõọ", "o", "o", "o"},    {U"ôồốổỗộ", "oo", "o6", "o^"},
        {U"ơờớởỡợ", "ow", "o7", "o+"},  {U"uùúủũụ", "u", "u", "u"},    {U"ưừứửữự", "uw", "u7", "u+"}, {U"yỳýỷỹỵ", "y", "y", "y"},
        {U"AÀÁẢÃẠ", "A", "A", "A"},     {U"ĂẰẮẲẴẶ", "AW", "A8", "A("}, {U"ÂẦẤẨẪẬ", "AA", "A6", "A^"}, {U"EÈÉẺẼẸ", "E", "E", "E"},
        {U"ÊỀẾỂỄỆ", "EE", "E6", "E^"},  {U"IÌÍỈĨỊ", "I", "I", "I"},    {U"OÒÓỎÕỌ", "O", "O", "O"},    {U"ÔỒỐỔỖỘ", "OO", "O6", "O^"},
        {U"ƠỜỚỞỠỢ", "OW", "O7", "O+"},  {U"UÙÚỦŨỤ", "U", "U", "U"},    {U"ƯỪỨỬỮỰ", "UW", "U7", "U+"}, {U"YỲÝỶỸỴ", "Y", "Y", "Y"},
    }};
    const std::array<const char*, 3> kToneKeys = {"zfsrxj", "021345", " `'?~."};

    /** Style of the generated keys: 0 Telex, 1 VNI, 2 VIQR. */
    std::string typeWord(const std::u32string& word, int style, Rng& rng) {
        std::string keys;
        char        toneKey   = 0;
        bool        toneAtEnd = chance(rng, 0.6);
        bool        lowerTail = chance(rng, 0.3); // "Aa" instead of "AA" for an uppercase modified letter
        for (char32_t c : word) {
            if (c == U'đ' || c == U'Đ') {
                const bool upper = c == U'Đ';
                std::string k    = style == 1 ? "d9" : "dd";
                if (upper) {
                    k[0] = 'D';
                    if (!lowerTail && style != 1) {
                        k[1] = 'D';
                    }
                }
                keys += k;
                continue;
            }
            bool found = false;
            for (const auto& letter : kLetters) {
                for (int tone = 0; tone < 6; ++tone) {
                    if (letter.tones[tone] != c) {
                        continue;
                    }
                    std::string k = style == 0 ? letter.telex : style == 1 ? letter.vni : letter.viqr;
                    if (lowerTail && k.size() > 1 && k[1] >= 'A' && k[1] <= 'Z') {
                        k[1] = static_cast<char>(k[1] - 'A' + 'a');
                    }
                    if (style == 0 && (k == "uw" || k == "UW") && chance(rng, 0.2)) {
                        k = k[0] == 'u' ? "w" : "W";
                    }
                    keys += k;
                    if (tone != 0) {
                        toneKey = kToneKeys[style][tone];
                        if (!toneAtEnd) {
                            keys += toneKey;
                            toneKey = 0;
                        }
                    }
                    found = true;
                    break;
                }
                if (found) {
                    break;
                }
            }
            if (!found) {
                keys += static_cast<char>(c < 0x80 ? c : '?');
            }
        }
        if (toneKey != 0) {
            keys += toneKey;
        }
        return keys;
    }

    void pushKeys(std::vector<Action>& out, const std::string& keys, bool capsLock) {
        for (unsigned char c : keys) {
            uint32_t st = 0;
            if (c >= 'A' && c <= 'Z') {
                st = capsLock ? kLock : kShift;
            } else if (std::string_view("!@#$%^&*()_+{}|:\"<>?~").find(static_cast<char>(c)) != std::string_view::npos) {
                st = kShift;
            }
            out.push_back(key(c == '\n' ? kReturn : c, st));
        }
    }

    // Shared random data

    struct Data {
        std::vector<std::string>                         vikeyVn;     // Telex keys
        std::vector<std::string>                         vikeyEn;     // English words
        std::vector<std::u32string>                      dictWords;   // Unicode Vietnamese words
        std::vector<std::string>                         dictLines;   // UTF-8 lines of the dictionary
        std::string                                      dictText;
        std::vector<std::vector<std::u32string>>         sentences;   // corpus, split on spaces
        std::vector<std::map<std::string, std::string>>  definitions; // built-in input methods
    };

    const std::array<uint32_t, 30> kOddKeys = {
        kBackSpace, kTab, kReturn, kEscape, 0xFF51U /* Left */, 0xFFFFU /* Delete */, 0xFFE1U /* Shift_L */, 0xFF50U /* Home */,
        0x00E0U, 0x00E7U, 0x00E8U, 0x00E9U, 0x00A0U, 0x01B0U /* ư */, 0x0111U /* đ */, 0x1EA1U /* ạ */, 0x01001EA1U /* keysym ạ */,
        0x0110U, 0x00C0U, 0x00F4U, 0xD800U, 0x110000U, 0xFFFFFFFFU, 0x1000000U, 0x0000U, 0x007FU, 0x00DFU, 0x0300U, 0xFE03U, 0x20ACU,
    };

    uint32_t randomKey(Rng& rng) {
        static const std::string kLettersPool = "abcdeghiklmnopqrstuvxyfwjzaeiouaeiounhcgt";
        static const std::string kPunct       = " ,.;:!?'\"()[]{}-_/\\@#$%^&*+=<>~`|0123456789";
        const std::size_t        roll         = pick(rng, 100);
        if (roll < 55) {
            char c = kLettersPool[pick(rng, kLettersPool.size())];
            return chance(rng, 0.12) ? static_cast<uint32_t>(c - 'a' + 'A') : static_cast<uint32_t>(c);
        }
        if (roll < 75) {
            return static_cast<unsigned char>(kPunct[pick(rng, kPunct.size())]);
        }
        if (roll < 88) {
            return chance(rng, 0.6) ? kBackSpace : ' ';
        }
        return kOddKeys[pick(rng, kOddKeys.size())];
    }

    uint32_t randomState(Rng& rng, uint32_t keyVal) {
        const std::size_t roll = pick(rng, 100);
        if (keyVal >= 'A' && keyVal <= 'Z') {
            return roll < 80 ? kShift : roll < 95 ? kLock : kShift | kLock;
        }
        if (roll < 82) {
            return 0;
        }
        static const std::array<uint32_t, 10> kStates = {kShift, kLock, kCtrl, kAlt, 1U << 25U, 1U << 26U, 1U << 27U, 1U << 28U, kShift | kCtrl, 0x10U /* NumLock */};
        if (roll < 98) {
            return kStates[pick(rng, kStates.size())];
        }
        return static_cast<uint32_t>(rng());
    }

    std::u32string randomText(Rng& rng, const Data& data, const std::u32string& recent) {
        switch (pick(rng, 7)) {
            case 0: return recent;
            case 1: return data.dictWords[pick(rng, data.dictWords.size())];
            case 2: {
                const std::string& w = data.vikeyEn[pick(rng, data.vikeyEn.size())];
                return std::u32string(w.begin(), w.end());
            }
            case 3: {
                const auto&    sentence = data.sentences[pick(rng, data.sentences.size())];
                std::u32string out;
                std::size_t    from = pick(rng, sentence.size());
                for (std::size_t i = from; i < sentence.size() && i < from + 3; ++i) {
                    out += sentence[i];
                    if (i + 1 < sentence.size() && i + 1 < from + 3) {
                        out += U' ';
                    }
                }
                return out;
            }
            case 4: {
                std::u32string word = data.dictWords[pick(rng, data.dictWords.size())];
                if (!word.empty() && word[0] < 0x80) {
                    word[0] = static_cast<char32_t>(std::toupper(static_cast<int>(word[0])));
                }
                return word + (chance(rng, 0.5) ? U"," : U" ");
            }
            case 5: return U"";
            default: {
                std::u32string out;
                const std::size_t n = pick(rng, 6);
                for (std::size_t i = 0; i < n; ++i) {
                    // Valid scalars only: the addon rebuilds from decoded UTF-8.
                    static const std::u32string kPool = U"aăâeêioôơuưyđAĂÂĐàáảãạằắẳẵặầấẩẫậèéẻẽẹềếểễệìíỉĩịòóỏõọồốổỗộờớởỡợùúủũụừứửữựỳýỷỹỵbcdghklmnpqrstvx0123 .,!?-' ̀\U0001F600";
                    out += kPool[pick(rng, kPool.size())];
                }
                return out;
            }
        }
    }

    Opts randomOpts(Rng& rng) {
        static const std::array<const char*, 9> kFormats = {"%H:%M", "", "%d/%m/%Y", "%%", "%Y-%m-%d %H:%M", "15:04", "%q%", "%A %B %p", "%y%m%d %I %M %b %a"};
        Opts                                     o;
        o.autoNonVnRestore    = chance(rng, 0.6);
        o.ddFreeStyle         = chance(rng, 0.6);
        o.englishRestore      = chance(rng, 0.5);
        o.macroEnabled        = chance(rng, 0.4);
        o.autoCapitalizeMacro = chance(rng, 0.5);
        o.spellCheckWithDicts = chance(rng, 0.5);
        o.modernStyle         = chance(rng, 0.5);
        o.freeMarking         = chance(rng, 0.6);
        o.charset             = chance(rng, 0.6) ? 0 : static_cast<int>(pick(rng, 17));
        o.w2u                 = static_cast<int>(pick(rng, 4)) - 1;
        o.bracket             = static_cast<int>(pick(rng, 4)) - 1;
        o.timeFormat          = kFormats[pick(rng, kFormats.size())];
        o.dateFormat          = kFormats[pick(rng, kFormats.size())];
        return o;
    }

    Setup randomSetup(Rng& rng, const Data& data) {
        Setup s;
        s.inputMethod = static_cast<int>(pick(rng, 8));
        if (s.inputMethod <= 1 && chance(rng, 0.5)) {
            s.inputMethod = 0; // Telex dominates real use
        }
        s.spellDefault = chance(rng, 0.7);
        if (s.inputMethod == 7 && chance(rng, 0.85)) {
            s.parseCustom = true;
            s.spellDefault = false;
            // A random mix of the built-in lines, plus the odd broken line.
            for (const auto& definition : data.definitions) {
                for (const auto& [k, v] : definition) {
                    if (chance(rng, 0.35)) {
                        s.customDefinition[k] = v;
                    }
                }
            }
            static const std::array<std::pair<const char*, const char*>, 8> kOdd = {{
                {"q", "Foo"}, {"]", "__ư"}, {"[", "__ơ"}, {"Q", "DauSac"}, {"", "DauHuyen"}, {"v", "A_"}, {"k", "UOA_ƯƠĂ"}, {"=", "_Ơ"},
            }};
            for (const auto& [k, v] : kOdd) {
                if (chance(rng, 0.2)) {
                    s.customDefinition[k] = v;
                }
            }
        }
        s.setOptions = chance(rng, 0.92);
        s.rewriteAllowed = chance(rng, 0.6);
        s.withMacros = chance(rng, 0.6);
        static const std::array<std::pair<const char*, const char*>, 14> kMacros = {{
            {"vn", "Việt Nam"}, {"123", "Mixed Case"}, {"tg", "$TIME"}, {"ng", "$DATE ngày"}, {"btw", "by the way"}, {"Hn", "Hà Nội"},
            {"đc", "được"}, {"kb", "không biết"}, {"ko", "không"}, {"HCM", "Hồ Chí Minh"}, {"tks", "thanks $TIME at $DATE"}, {"a", "anh"},
            {"vd", "ví dụ"}, {"Ql", "Quản lý"},
        }};
        for (const auto& [k, v] : kMacros) {
            if (chance(rng, 0.7)) {
                s.macros.emplace_back(k, v);
            }
        }
        s.dictionary = static_cast<int>(pick(rng, 3));
        if (s.dictionary == 1) {
            for (std::size_t i = 0; i < 400; ++i) {
                s.dictWords.push_back(data.dictLines[pick(rng, data.dictLines.size())]);
            }
            s.dictWords.emplace_back("");
            s.dictWords.emplace_back("Nguyễn");
            s.dictWords.emplace_back("ĐƯỢC");
        } else if (s.dictionary == 2) {
            s.dictText = data.dictText;
        }
        return s;
    }

    int keyStyle(const Setup& setup, Rng& rng) {
        switch (setup.inputMethod) {
            case 1: return 1;
            case 2: return chance(rng, 0.5) ? 0 : 1;
            case 3: return static_cast<int>(pick(rng, 3));
            case 4: return 2;
            default: return chance(rng, 0.8) ? 0 : static_cast<int>(pick(rng, 3));
        }
    }

    /** Words typed like a person would: Vietnamese and English mixed, separators, typos. */
    std::vector<Action> sentenceActions(Rng& rng, const Data& data, const Setup& setup, std::size_t words) {
        std::vector<Action> out;
        const bool          capsLock = chance(rng, 0.05);
        // A third of the runs are mostly English, with the words Telex would turn Vietnamese.
        const bool english = chance(rng, 0.33);
        static const std::array<const char*, 84> kCommon = {
            "this", "is", "was", "for", "its", "has", "does", "goes", "makes", "works", "fix", "box", "mix", "next", "text", "after", "offer", "off",
            "see", "need", "keep", "look", "good", "food", "too", "add", "odd", "our", "your", "were", "we", "how", "now", "show", "know", "saw",
            "law", "new", "few", "view", "word", "world", "worse", "cross", "press", "less", "miss", "class", "address", "process", "success", "issue",
            "user", "users", "error", "errors", "sorry", "story", "every", "very", "art", "part", "start", "star", "car", "are", "or", "form",
            "from", "first", "just", "must", "test", "tests", "best", "rest", "last", "fast", "past", "cast", "the", "and", "to", "of"};
        // Another third switch language every few words, separated by single spaces (what the
        // English restore looks back over).
        const bool  switching = !english && chance(rng, 0.5);
        std::size_t segment   = 0;
        bool        inEnglish = chance(rng, 0.5);
        for (std::size_t w = 0; w < words; ++w) {
            std::size_t roll = pick(rng, 100);
            std::string keys;
            if (switching) {
                if (segment == 0) {
                    segment   = 1 + pick(rng, 3);
                    inEnglish = !inEnglish;
                }
                --segment;
                // English: the common words; Vietnamese: corpus words or Vikey Telex words.
                roll = inEnglish ? pick(rng, 60) : chance(rng, 0.5) ? pick(rng, 30) : 50 + pick(rng, 15);
            }
            if ((english || switching) && roll < 60 && (english || inEnglish)) {
                keys = kCommon[pick(rng, kCommon.size())];
            } else if (english && roll < 85) {
                keys = roll < 60 ? kCommon[pick(rng, kCommon.size())] : data.vikeyEn[pick(rng, data.vikeyEn.size())];
                if (chance(rng, 0.08)) {
                    keys[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(keys[0])));
                }
            } else if (roll < 30) {
                const auto& sentence = data.sentences[pick(rng, data.sentences.size())];
                keys                 = typeWord(sentence[pick(rng, sentence.size())], keyStyle(setup, rng), rng);
            } else if (roll < 50) {
                keys = typeWord(data.dictWords[pick(rng, data.dictWords.size())], keyStyle(setup, rng), rng);
            } else if (roll < 65) {
                keys = data.vikeyVn[pick(rng, data.vikeyVn.size())];
            } else if (roll < 92) {
                keys = data.vikeyEn[pick(rng, data.vikeyEn.size())];
                if (chance(rng, 0.15) && !keys.empty()) {
                    keys[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(keys[0])));
                }
            } else {
                static const std::array<const char*, 10> kOdd = {"123", "vn", "tg", "đc", "HCM", "e-mail", "C++", "3.14", "ok!", "x2"};
                keys = kOdd[pick(rng, kOdd.size())];
            }
            if (chance(rng, 0.04) && !keys.empty()) {
                for (auto& c : keys) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
            }
            const std::size_t before = out.size();
            pushKeys(out, keys, capsLock);
            // Typos fixed with Backspace in the middle of the word.
            if (!switching && chance(rng, 0.08) && out.size() > before) {
                const std::size_t at = before + pick(rng, out.size() - before);
                std::vector<Action> fix;
                fix.push_back(key(randomKey(rng)));
                const std::size_t back = 1 + pick(rng, 2);
                for (std::size_t i = 0; i < back; ++i) {
                    fix.push_back(key(kBackSpace));
                }
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), fix.begin(), fix.end());
            }
            const std::size_t sep = switching && chance(rng, 0.9) ? 0 : pick(rng, 100);
            if (sep < 70) {
                out.push_back(key(' '));
            } else if (sep < 80) {
                pushKeys(out, chance(rng, 0.5) ? ", " : ". ", false);
            } else if (sep < 84) {
                out.push_back(key(kReturn));
            } else if (sep < 88) {
                out.push_back(key(kTab));
            } else if (sep < 92) {
                out.push_back(key(kBackSpace));
                out.push_back(key(' '));
            } else if (sep < 95) {
                pushKeys(out, std::string(1, "?!:;"[pick(rng, 4)]) + " ", false);
            } else if (sep < 97) {
                out.push_back(key(' ', kShift));
            } else {
                // A burst of Backspace deleting into the earlier word.
                for (std::size_t i = 0, n = 2 + pick(rng, 6); i < n; ++i) {
                    out.push_back(key(kBackSpace));
                }
            }
        }
        return out;
    }

    std::vector<Action> randomActions(Rng& rng, std::size_t n) {
        std::vector<Action> out;
        for (std::size_t i = 0; i < n; ++i) {
            const uint32_t k = randomKey(rng);
            out.push_back(key(k, randomState(rng, k)));
        }
        return out;
    }

    /** Sprinkles the non-key calls the addon makes between keys. */
    void sprinkle(std::vector<Action>& actions, Rng& rng, double density) {
        std::vector<Action> out;
        for (auto& a : actions) {
            if (chance(rng, density)) {
                Action extra;
                switch (pick(rng, 14)) {
                    case 0:
                    case 1:
                    case 2: extra.op = Op::Rebuild; break;
                    case 3:
                    case 4: extra.op = Op::Forget; break;
                    case 5:
                    case 6: extra.op = Op::Rewrite; extra.flag = chance(rng, 0.7); break;
                    case 7: extra.op = Op::Reset; break;
                    case 8: extra.op = Op::CommitPreedit; break;
                    case 9: extra.op = Op::MacroEnabled; extra.flag = chance(rng, 0.5); break;
                    case 10: extra.op = Op::Restore; extra.flag = chance(rng, 0.8); break;
                    case 11: extra.op = Op::SetOptions; extra.opts = randomOpts(rng); break;
                    case 12: extra.op = Op::Reshare; extra.flag = chance(rng, 0.6); extra.flag2 = chance(rng, 0.6); break;
                    default: extra.op = Op::Probe; extra.key = randomKey(rng); break;
                }
                out.push_back(extra);
            }
            a.take = chance(rng, 0.9);
            out.push_back(a);
        }
        actions = std::move(out);
    }

    // Runner

    struct Stats {
        std::size_t scenarios   = 0;
        std::size_t actions     = 0;
        std::size_t keys        = 0;
        std::size_t comparisons = 0;
        std::size_t mismatches  = 0;
        std::size_t commits     = 0; ///< Steps where a non-empty commit was compared
        std::size_t rewrites    = 0; ///< Steps where a non-empty rewritten-before text was compared
    };

    bool report(Stats& stats, const std::string& where, const std::string& what, const std::string& oldValue, const std::string& newValue,
                const std::vector<Action>& actions, std::size_t step) {
        ++stats.mismatches;
        if (stats.mismatches <= 10) {
            std::cerr << "MISMATCH " << where << " step " << step << " (" << describe(actions[step]) << "): " << what << "\n  old: " << oldValue << "\n  new: " << newValue
                      << "\n  history:";
            for (std::size_t i = step >= 12 ? step - 12 : 0; i <= step; ++i) {
                std::cerr << "\n    " << i << ": " << describe(actions[i]);
            }
            std::cerr << "\n";
        }
        return false;
    }

    bool same(Stats& stats, const std::string& where, const std::string& what, const std::u32string& a, const std::u32string& b, const std::vector<Action>& actions,
              std::size_t step) {
        ++stats.comparisons;
        if (a != b) {
            return report(stats, where, what, utf8(a) + " [" + hex(a) + "]", utf8(b) + " [" + hex(b) + "]", actions, step);
        }
        return true;
    }

    bool sameBool(Stats& stats, const std::string& where, const std::string& what, bool a, bool b, const std::vector<Action>& actions, std::size_t step) {
        ++stats.comparisons;
        if (a != b) {
            return report(stats, where, what, std::to_string(a), std::to_string(b), actions, step);
        }
        return true;
    }

    /** Runs one scenario; false at the first mismatch. */
    bool run(Stats& stats, const std::string& where, const Setup& setup, const Opts& opts, const std::vector<Action>& actions, const Data& data, Rng& rng) {
        ++stats.scenarios;
        auto           oldProc = oldSide().make(setup, opts);
        auto           newProc = newSide().make(setup, opts);
        std::u32string recent;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            const Action& a = actions[i];
            ++stats.actions;
            switch (a.op) {
                case Op::Key: {
                    ++stats.keys;
                    const bool ra = oldProc->processKey(a.key, a.st);
                    const bool rb = newProc->processKey(a.key, a.st);
                    if (!sameBool(stats, where, "processKey", ra, rb, actions, i)) {
                        return false;
                    }
                    break;
                }
                case Op::Rebuild: {
                    const std::u32string text = randomText(rng, data, recent);
                    // Record the text in the action log for the report.
                    const_cast<Action&>(a).text = text;
                    oldProc->rebuildFromText(text);
                    newProc->rebuildFromText(text);
                    break;
                }
                case Op::Forget: oldProc->forgetContext(); newProc->forgetContext(); break;
                case Op::Rewrite: oldProc->setRewriteAllowed(a.flag); newProc->setRewriteAllowed(a.flag); break;
                case Op::Reset: oldProc->reset(); newProc->reset(); break;
                case Op::CommitPreedit: oldProc->commitPreedit(); newProc->commitPreedit(); break;
                case Op::MacroEnabled: oldProc->setMacroEnabled(a.flag); newProc->setMacroEnabled(a.flag); break;
                case Op::Restore: oldProc->setShouldRestoreKeyStrokes(a.flag); newProc->setShouldRestoreKeyStrokes(a.flag); break;
                case Op::SetOptions: oldProc->setOptions(a.opts); newProc->setOptions(a.opts); break;
                case Op::Reshare: oldProc->reshare(a.flag, a.flag2); newProc->reshare(a.flag, a.flag2); break;
                case Op::Probe:
                    if (!sameBool(stats, where, "canProcessKey", oldProc->canProcessKey(a.key), newProc->canProcessKey(a.key), actions, i)) {
                        return false;
                    }
                    break;
            }
            if (!same(stats, where, "preedit", oldProc->preedit(), newProc->preedit(), actions, i)) {
                return false;
            }
            if (!sameBool(stats, where, "shouldRestoreKeyStrokes", oldProc->shouldRestoreKeyStrokes(), newProc->shouldRestoreKeyStrokes(), actions, i)) {
                return false;
            }
            if (a.take || i + 1 == actions.size()) {
                const std::u32string rewrittenA = oldProc->takeRewrittenBefore();
                const std::u32string rewrittenB = newProc->takeRewrittenBefore();
                if (!same(stats, where, "takeRewrittenBefore", rewrittenA, rewrittenB, actions, i)) {
                    return false;
                }
                const std::u32string commitA = oldProc->takeCommit();
                const std::u32string commitB = newProc->takeCommit();
                if (!same(stats, where, "takeCommit", commitA, commitB, actions, i)) {
                    return false;
                }
                stats.commits += commitA.empty() ? 0 : 1;
                stats.rewrites += rewrittenA.empty() ? 0 : 1;
                if (!commitA.empty()) {
                    recent = commitA;
                }
            }
        }
        return true;
    }

    // Non-processor checks

    std::size_t checkUtilities(Stats& stats, const Data& data, Rng& rng) {
        std::size_t failures = 0;
        // encode: every code point of the Latin / Vietnamese ranges, then random texts.
        std::vector<std::u32string> texts;
        for (char32_t c = 0; c < 0x2100; ++c) {
            texts.emplace_back(1, c);
        }
        for (char32_t c = 0x1E00; c < 0x1F00; ++c) {
            texts.emplace_back(1, c);
        }
        for (std::size_t i = 0; i < 3000; ++i) {
            texts.push_back(randomText(rng, data, U"Xin chào thế giới"));
        }
        for (int charset = 0; charset < 17; ++charset) {
            for (const auto& text : texts) {
                ++stats.comparisons;
                if (oldSide().encode(charset, text) != newSide().encode(charset, text)) {
                    if (++failures <= 5) {
                        std::cerr << "MISMATCH encode charset " << charset << " text [" << hex(text) << "]\n";
                    }
                }
            }
        }
        // formatTime: the two calls may straddle a minute; retry once.
        static const std::array<const char*, 22> kFormats = {"%H:%M", "", "%d/%m/%Y", "%%", "%Y-%m-%d %H:%M", "15:04", "%q%", "%A %B %p", "%y%m%d %I %M %b %a",
                                                             "02/01/2006", "Jan Monday", "%", "%%%", "%H%H", "%k:%l", "%e %j %U", "MST", "PM pm",
                                                             "15:04:05", "%Z", "text only", "%D %T %R %F"};
        for (const char* format : kFormats) {
            ++stats.comparisons;
            bool ok = false;
            for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
                ok = oldSide().formatTime(format) == newSide().formatTime(format);
                if (!ok) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
            }
            if (!ok) {
                ++failures;
                std::cerr << "MISMATCH formatTime \"" << format << "\": " << oldSide().formatTime(format) << " vs " << newSide().formatTime(format) << "\n";
            }
        }
        // isValidState over single bits and random masks.
        for (std::size_t i = 0; i < 20000; ++i) {
            const uint32_t st = i < 32 ? (1U << i) : static_cast<uint32_t>(rng());
            ++stats.comparisons;
            if (oldSide().isValidState(st) != newSide().isValidState(st)) {
                ++failures;
            }
        }
        // Dictionary and macro table lookups.
        for (int round = 0; round < 6; ++round) {
            Setup setup = randomSetup(rng, data);
            setup.dictionary = 1 + round % 2;
            std::vector<std::u32string> words;
            for (std::size_t i = 0; i < 4000; ++i) {
                std::u32string w = data.dictWords[pick(rng, data.dictWords.size())];
                const std::size_t cut = pick(rng, 4);
                if (cut == 1 && w.size() > 1) {
                    w.resize(w.size() / 2);
                } else if (cut == 2) {
                    w = randomText(rng, data, w);
                } else if (cut == 3 && !w.empty()) {
                    w[0] = static_cast<char32_t>(std::toupper(static_cast<int>(w[0] & 0x7F)));
                }
                words.push_back(w);
            }
            words.emplace_back();
            stats.comparisons += words.size();
            if (oldSide().dictionaryProbe(setup, words) != newSide().dictionaryProbe(setup, words)) {
                ++failures;
                std::cerr << "MISMATCH dictionary probe round " << round << "\n";
            }
            std::vector<std::u32string> keys = {U"vn", U"VN", U"Vn", U"123", U"tg", U"ng", U"btw", U"hn", U"HN", U"Hn", U"đc", U"ĐC", U"Đc", U"kb", U"hcm", U"HCM", U"x", U"", U"a", U"A", U"ql"};
            stats.comparisons += keys.size();
            if (oldSide().macroProbe(setup, keys) != newSide().macroProbe(setup, keys)) {
                ++failures;
                std::cerr << "MISMATCH macro probe round " << round << "\n";
            }
        }
        stats.mismatches += failures;
        return failures;
    }

    std::vector<std::vector<Action>> goldenCases(const std::string& path, bool withStates) {
        std::vector<std::vector<Action>> cases;
        std::string                      current;
        for (const auto& line : readLines(path)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const auto fields = splitTab(line);
            if (fields[0] != current) {
                current = fields[0];
                cases.emplace_back();
            }
            Action a = key(static_cast<uint32_t>(std::stoul(fields[2], nullptr, 16)));
            if (withStates) {
                a.st = static_cast<uint32_t>(std::stoul(fields[3]));
            }
            cases.back().push_back(a);
        }
        return cases;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: lotus_parity <golden dir> <dictionary file> <corpus file> [random scenarios] [seed]\n";
        return 2;
    }
    const std::string goldenDir = argv[1];
    const std::size_t randomCount = argc > 4 ? std::stoul(argv[4]) : 4000;
    const uint64_t    seed        = argc > 5 ? std::stoull(argv[5]) : 20261031;
    Rng               rng(seed);

    Data data;
    data.vikeyVn     = blobWords(kAutoSwitchVnWords, kAutoSwitchVnOffsets, kAutoSwitchVnCount);
    data.vikeyEn     = blobWords(kAutoSwitchEnWords, kAutoSwitchEnOffsets, kAutoSwitchEnCount);
    data.definitions = builtinDefinitions();
    data.dictLines   = readLines(argv[2]);
    for (const auto& line : data.dictLines) {
        data.dictText += line + (chance(rng, 0.02) ? "\r\n" : "\n");
        data.dictWords.push_back(decode(line));
    }
    for (const auto& line : readLines(argv[3])) {
        std::vector<std::u32string> words;
        std::istringstream          in(line);
        for (std::string w; in >> w;) {
            words.push_back(decode(w));
        }
        if (!words.empty()) {
            data.sentences.push_back(words);
        }
    }

    Stats stats;
    std::size_t failedScenarios = 0;

    // 1. Golden key sequences (written by the Go engine), with defaults and random option sets.
    const auto kpCases     = goldenCases(goldenDir + "/key_processor.tsv", true);
    const auto engineCases = goldenCases(goldenDir + "/engine_core.tsv", false);
    std::size_t goldenScenarios = 0;
    for (const auto* cases : {&kpCases, &engineCases}) {
        for (std::size_t c = 0; c < cases->size(); ++c) {
            for (int variant = 0; variant < 4; ++variant) {
                Setup setup;
                Opts  opts;
                if (variant > 0) {
                    setup = randomSetup(rng, data);
                    opts  = randomOpts(rng);
                }
                auto actions = (*cases)[c];
                if (variant >= 2) {
                    sprinkle(actions, rng, 0.08);
                }
                ++goldenScenarios;
                failedScenarios += run(stats, "golden " + std::to_string(c) + "/" + std::to_string(variant), setup, opts, actions, data, rng) ? 0 : 1;
            }
        }
    }
    const Stats afterGolden = stats;

    // 2. Mixed Vietnamese / English sentences over every option set.
    for (std::size_t s = 0; s < randomCount; ++s) {
        Setup setup   = randomSetup(rng, data);
        Opts  opts    = randomOpts(rng);
        auto  actions = sentenceActions(rng, data, setup, 10 + pick(rng, 40));
        sprinkle(actions, rng, s % 3 == 0 ? 0.0 : 0.04);
        failedScenarios += run(stats, "sentence " + std::to_string(s), setup, opts, actions, data, rng) ? 0 : 1;
    }
    const Stats afterSentences = stats;

    // 3. Random key sequences.
    for (std::size_t s = 0; s < randomCount; ++s) {
        Setup setup   = randomSetup(rng, data);
        Opts  opts    = randomOpts(rng);
        auto  actions = randomActions(rng, 20 + pick(rng, 200));
        sprinkle(actions, rng, 0.06);
        failedScenarios += run(stats, "random " + std::to_string(s), setup, opts, actions, data, rng) ? 0 : 1;
    }
    const Stats afterRandom = stats;

    // 4. Charset encoding, time formats, key states, dictionary and macro lookups.
    const std::size_t utilityFailures = checkUtilities(stats, data, rng);

    const auto line = [](const char* name, const Stats& to, const Stats& from) {
        std::printf("%-10s scenarios %7zu  keys %9zu  actions %9zu  comparisons %10zu  non-empty commits %8zu  rewrites %6zu\n", name, to.scenarios - from.scenarios,
                    to.keys - from.keys, to.actions - from.actions, to.comparisons - from.comparisons, to.commits - from.commits, to.rewrites - from.rewrites);
    };
    std::printf("seed %llu, golden cases %zu + %zu (x4 variants = %zu scenarios)\n", static_cast<unsigned long long>(seed), kpCases.size(), engineCases.size(), goldenScenarios);
    line("golden", afterGolden, Stats{});
    line("sentences", afterSentences, afterGolden);
    line("random", afterRandom, afterSentences);
    std::printf("utilities  comparisons %zu, failures %zu\n", stats.comparisons - afterRandom.comparisons, utilityFailures);
    std::printf("TOTAL scenarios %zu, keys %zu, comparisons %zu, mismatches %zu (failed scenarios %zu)\n", stats.scenarios, stats.keys, stats.comparisons, stats.mismatches,
                failedScenarios);
    return stats.mismatches == 0 ? 0 : 1;
}
