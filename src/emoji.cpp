/*
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "emoji.h"
#include "lotus-utils.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <emoji_public.h>
#include <fcitx-utils/standardpath.h>
#include <fstream>
#include <string_view>
#include <unistd.h>
#include <utility>

EmojiLoader::EmojiLoader(fcitx::AddonManager* addonManager) {
    if (addonManager != nullptr) {
        emojiAddon_ = addonManager->addon("emoji", true);
    }
    historyPath_ = lotusConfigDir() + "/vanh-emoji-history.conf";
    loadFromFcitx5("en");
    loadHistory();
}

void EmojiLoader::loadFromFcitx5(const std::string& language) {
    emojiList.clear();
    if (emojiAddon_ == nullptr) {
        return;
    }

    emojiAddon_->call<fcitx::IEmoji::prefix>(language, "", true, [this](const std::string& key, const std::vector<std::string>& values) {
        for (const auto& emoji : values) {
            EmojiEntry entry;
            entry.output  = emoji;
            entry.trigger = key;
            emojiList.push_back(entry);
        }
        return true;
    });
}

void EmojiLoader::recordHistory(const EmojiEntry& entry) {
    // Remove if already exists to move to top
    historyList.erase(std::ranges::remove_if(historyList, [&](const auto& e) { return e.output == entry.output; }).begin(), historyList.end());

    historyList.insert(historyList.begin(), entry);

    // Keep only the newest entries.
    if (historyList.size() > kMaxEmojiHistory) {
        historyList.resize(kMaxEmojiHistory);
    }

    saveHistory();
}

void EmojiLoader::loadHistory() {
    historyList.clear();
    std::ifstream file(historyPath_);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        // Lines are "trigger=output"; anything else is malformed. Bytes
        // (including UTF-8) are preserved verbatim.
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        std::string output = line.substr(pos + 1);
        if (output.empty()) {
            continue;
        }
        // File order is newest-first: keep the first entry per emoji.
        const bool seen = std::ranges::any_of(historyList, [&](const auto& e) { return e.output == output; });
        if (seen) {
            continue;
        }
        EmojiEntry entry;
        entry.trigger = line.substr(0, pos);
        entry.output  = std::move(output);
        historyList.push_back(std::move(entry));
        if (historyList.size() >= kMaxEmojiHistory) {
            break;
        }
    }
}

void EmojiLoader::saveHistory() {
    // Replace the history file only after the temporary file is fully written.
    // mkstemp gives each call a unique name, so concurrent writers never share
    // a temporary file; rename() then swaps it into place atomically.
    std::string templatePath = historyPath_ + ".tmp.XXXXXX";
    const int   fd           = ::mkstemp(templatePath.data());
    if (fd < 0) {
        return;
    }
    ::close(fd);
    const std::string tmpPath = templatePath;
    {
        std::ofstream file(tmpPath, std::ios::trunc);
        if (!file.is_open()) {
            std::remove(tmpPath.c_str());
            return;
        }
        size_t written = 0;
        for (const auto& entry : historyList) {
            if (written >= kMaxEmojiHistory) {
                break;
            }
            if (entry.output.empty()) {
                continue;
            }
            file << entry.trigger << "=" << entry.output << "\n";
            if (!file) {
                file.close();
                std::remove(tmpPath.c_str());
                return;
            }
            ++written;
        }
        file.flush();
        file.close();
        if (!file) {
            std::remove(tmpPath.c_str());
            return;
        }
    }
    if (std::rename(tmpPath.c_str(), historyPath_.c_str()) != 0) {
        std::remove(tmpPath.c_str());
    }
}

namespace {

    std::string asciiLower(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool isWordSeparator(char c) {
        return c == '_' || c == '-' || c == ' ' || c == '/';
    }

} // namespace

std::vector<EmojiEntry> EmojiLoader::search(const std::string& prefix) {
    if (prefix.empty()) {
        return {};
    }
    // Case-insensitive matching: fold the query and each trigger to lowercase
    // (ASCII minimum); the stored trigger/output bytes are never modified.
    const std::string query = asciiLower(prefix);

    // Tiers beat raw bonus points; the score only breaks ties within a tier.
    enum class Rank : std::uint8_t {
        Exact        = 0,
        Prefix       = 1,
        WordBoundary = 2,
        Fuzzy        = 3,
    };
    struct Scored {
        EmojiEntry entry;
        Rank       rank{};
        int        score = 0;
        size_t     index = 0;
    };
    std::vector<Scored> scored;
    // Single linear scan over the DB is retained (M is small); the bound is on
    // output (at most K results) and on sorting (partial_sort of the first K).
    scored.reserve(std::min(emojiList.size(), kMaxEmojiSearchResults));

    for (size_t entryIndex = 0; entryIndex < emojiList.size(); ++entryIndex) {
        const auto& entry = emojiList[entryIndex];
        // Fast path: triggers are almost always already lowercase, so only
        // fold (allocating) when the trigger contains uppercase ASCII.
        std::string      lowered;
        std::string_view trigger = entry.trigger;
        for (char c : entry.trigger) {
            if (c >= 'A' && c <= 'Z') {
                lowered = asciiLower(entry.trigger);
                trigger = lowered;
                break;
            }
        }
        if (query.size() > trigger.size()) {
            continue;
        }

        Rank   rank       = Rank::Fuzzy;
        size_t spanStart  = 0;
        size_t spanLength = query.size();
        if (trigger == query) {
            rank      = Rank::Exact;
            spanStart = 0;
        } else if (trigger.starts_with(query)) {
            rank      = Rank::Prefix;
            spanStart = 0;
        } else {
            size_t boundaryPos = trigger.size();
            for (size_t i = 1; i + query.size() <= trigger.size(); ++i) {
                if (!isWordSeparator(trigger[i - 1])) {
                    continue;
                }
                if (trigger.compare(i, query.size(), query) == 0) {
                    boundaryPos = i;
                    break;
                }
            }
            if (boundaryPos != trigger.size()) {
                rank      = Rank::WordBoundary;
                spanStart = boundaryPos;
            } else {
                // Fuzzy: the query must be a subsequence of the trigger; the
                // greedy scan doubles as the span measurement.
                size_t queryIndex = 0;
                size_t firstMatch = trigger.size();
                size_t lastMatch  = 0;
                for (size_t i = 0; i < trigger.size(); ++i) {
                    if (queryIndex < query.size() && trigger[i] == query[queryIndex]) {
                        if (queryIndex == 0) {
                            firstMatch = i;
                        }
                        lastMatch = i;
                        ++queryIndex;
                        if (queryIndex == query.size()) {
                            break;
                        }
                    }
                }
                if (queryIndex != query.size()) {
                    continue;
                }
                rank       = Rank::Fuzzy;
                spanStart  = firstMatch;
                spanLength = lastMatch - firstMatch + 1;
            }
        }

        // Tier-anchored within-tier score: start bonus only when the
        // tier-determining span starts at 0 or at a word boundary, penalized by
        // trigger length and span length (shorter, tighter matches first).
        int score = 0;
        if (spanStart == 0) {
            score += 100;
        } else if (isWordSeparator(trigger[spanStart - 1])) {
            score += 50;
        }
        score -= static_cast<int>(trigger.size());
        score -= static_cast<int>(spanLength);

        scored.push_back({.entry = entry, .rank = rank, .score = score, .index = entryIndex});
    }

    const auto topK = [](const auto& a, const auto& b) {
        if (a.rank != b.rank) {
            return a.rank < b.rank;
        }
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.index < b.index;
    };
    const size_t keep = std::min(scored.size(), kMaxEmojiSearchResults);
    std::ranges::partial_sort(scored, scored.begin() + static_cast<std::vector<Scored>::difference_type>(keep), topK);
    scored.resize(keep);

    std::vector<EmojiEntry> finalResults;
    finalResults.reserve(scored.size());
    for (const auto& result : scored) {
        finalResults.push_back(result.entry);
    }
    return finalResults;
}

size_t EmojiLoader::size() const {
    return emojiList.size();
}

void EmojiLoader::setEmojiListForTest(std::vector<EmojiEntry> entries) {
    emojiList = std::move(entries);
}

void EmojiLoader::setHistoryPathForTest(const std::string& path) {
    historyPath_ = path;
    loadHistory();
}
