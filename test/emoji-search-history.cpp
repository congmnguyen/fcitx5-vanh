// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file emoji-search-history.cpp
 * @brief Production-backed ranking tests for EmojiLoader::search.
 *
 * Calls the real EmojiLoader::search (no algorithm copies) and checks the
 * tier order exact > prefix > word-boundary > fuzzy, case-insensitivity,
 * the Top-K output cap and deterministic tie-breaking.
 */

#include "emoji.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

    int  failures = 0;

    void check(bool ok, const std::string& step, const std::string& expected, const std::string& actual) {
        if (ok) {
            return;
        }
        ++failures;
        std::cerr << "Step: " << step << "\nExpected: " << expected << "\nActual: " << actual << "\n";
    }

    std::string triggers(const std::vector<EmojiEntry>& entries) {
        std::string out;
        out.reserve(entries.size() * 12);
        for (const auto& entry : entries) {
            out += '[';
            out += entry.trigger;
            out += ']';
        }
        return {out};
    }

    std::vector<EmojiEntry> tierEntries() {
        return {
            {.trigger = "cat", .output = "C"},         {.trigger = "cat_face", .output = "F"}, {.trigger = "black_cat", .output = "B"},
            {.trigger = "educational", .output = "E"}, {.trigger = "dog", .output = "D"},
        };
    }

    std::size_t indexOf(const std::vector<EmojiEntry>& results, const std::string& trigger) {
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (results[i].trigger == trigger) {
                return i;
            }
        }
        return results.size();
    }

    bool contains(const std::vector<EmojiEntry>& results, const std::string& trigger) {
        return std::ranges::any_of(results, [&](const EmojiEntry& entry) { return entry.trigger == trigger; });
    }

} // namespace

int main() {
    // Seams link: point history at a missing file; history reloads empty.
    {
        EmojiLoader     loader(nullptr);
        const auto      missing = std::filesystem::temp_directory_path() / "vanh-emoji-search-history-missing.conf";
        std::error_code ec;
        std::filesystem::remove(missing, ec);
        loader.setHistoryPathForTest(missing.string());
        check(loader.history().empty(), "missing history", "empty", std::to_string(loader.history().size()));
    }

    // Tier order exact > prefix > boundary > fuzzy, plus case-insensitive exact.
    {
        EmojiLoader loader(nullptr);
        loader.setEmojiListForTest(tierEntries());

        const auto results = loader.search("cat");
        check(results.size() == 4, "tier size", "4", triggers(results));
        check(indexOf(results, "cat") < indexOf(results, "cat_face"), "exact>prefix", "cat first", triggers(results));
        check(indexOf(results, "cat_face") < indexOf(results, "black_cat"), "prefix>boundary", "cat_face before black_cat", triggers(results));
        check(indexOf(results, "black_cat") < indexOf(results, "educational"), "boundary>fuzzy", "black_cat before educational", triggers(results));
        check(!contains(results, "dog"), "non-match excluded", "no dog", triggers(results));

        const auto upper = loader.search("CAT");
        check(!upper.empty() && upper[0].trigger == "cat", "CAT exact ci", "cat", triggers(upper));

        check(loader.search("xyz").empty(), "non-subsequence", "0", triggers(loader.search("xyz")));
        check(loader.search("").empty(), "empty", "0", "non-empty");
    }

    // Top-K: the best match is inserted last, so naive truncation (first 100)
    // would drop it; selection must keep the best 100 instead.
    {
        EmojiLoader             loader(nullptr);
        std::vector<EmojiEntry> entries;
        entries.reserve(101);
        for (int i = 0; i < 100; ++i) {
            entries.push_back({.trigger = "rank_" + std::to_string(i), .output = "E"});
        }
        entries.push_back({.trigger = "rank", .output = "BEST"});
        loader.setEmojiListForTest(std::move(entries));
        const auto results = loader.search("rank");
        check(results.size() == EmojiLoader::kMaxEmojiSearchResults, "top-k size", "100", std::to_string(results.size()));
        check(!results.empty() && results[0].trigger == "rank", "best match first", "rank", triggers(results));
        check(!contains(results, "rank_99"), "worst dropped", "rank_99 absent", triggers(results));
    }

    // Duplicate triggers keep insertion order (deterministic tie-break).
    {
        EmojiLoader loader(nullptr);
        loader.setEmojiListForTest({
            {.trigger = "dup", .output = "A"},
            {.trigger = "dup", .output = "B"},
        });
        const auto results = loader.search("dup");
        check(results.size() == 2, "dup size", "2", triggers(results));
        check(results.size() == 2 && results[0].output == "A" && results[1].output == "B", "dup order", "[A][B]",
              results.size() == 2 ? "[" + results[0].output + "][" + results[1].output + "]" : triggers(results));
    }

    if (failures > 0) {
        std::cerr << "FAILURES: " << failures << "\n";
        return 1;
    }
    std::cout << "emoji-search-history: all checks passed\n";
    return 0;
}
