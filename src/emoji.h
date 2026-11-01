/*
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file emoji.h
 * @brief Emoji search and loading utilities.
 */

#ifndef EMOJI_H
#define EMOJI_H

#include <cstddef>
#include <string>
#include <vector>

#include <fcitx/addonmanager.h>

#include "common-export.h"
#include "emoji-entry.h"

/**
 * @brief Emoji loader with fuzzy search capability.
 *
 * Loads emoji list from Fcitx5 emoji addon and provides fuzzy matching search.
 */
class LOTUS_EXPORT EmojiLoader {
  public:
    /// Maximum entries kept in (and written back to) the emoji history file.
    static constexpr std::size_t kMaxEmojiHistory = 9;
    /// Upper bound on results returned by search().
    /// Keeps per-keystroke allocation/sort cheap; the UI still paginates 9/page.
    static constexpr std::size_t kMaxEmojiSearchResults = 100;

    /**
     * @brief Constructs loader and initializes emoji list from Fcitx5.
     * @param addonManager Fcitx5 addon manager instance; may be null in
     *        tests (skips the Fcitx5 load, inject data via
     *        setEmojiListForTest).
     */
    explicit EmojiLoader(fcitx::AddonManager* addonManager);
    /**
     * @brief Searches emoji by prefix with fuzzy matching.
     * @param prefix Search query.
     * @return Sorted list of matching emojis.
     */
    std::vector<EmojiEntry> search(const std::string& prefix);

    /**
     * @brief Records an emoji into history.
     * @param entry The emoji entry to record.
     */
    void recordHistory(const EmojiEntry& entry);

    /**
     * @brief Gets the emoji history.
     * @return List of recently used emojis.
     */
    [[nodiscard]] const std::vector<EmojiEntry>& history() const {
        return historyList;
    }

    /**
     * @brief Gets total emoji count.
     * @return Number of emojis loaded.
     */
    [[nodiscard]] size_t size() const;

    /**
     * @brief Replaces the emoji list (test seam).
     * @param entries Entries the next search() calls observe.
     */
    void setEmojiListForTest(std::vector<EmojiEntry> entries);

    /**
     * @brief Redirects the history file and reloads (test seam).
     * @param path History file path the loader reads from.
     */
    void setHistoryPathForTest(const std::string& path);

  private:
    std::vector<EmojiEntry> historyList;   ///< Recently used emojis
    std::vector<EmojiEntry> emojiList;     ///< Internal emoji storage
    fcitx::AddonInstance*   emojiAddon_{}; ///< Fcitx5 emoji addon
    std::string             historyPath_;

    /** @brief Loads emoji history from disk. */
    void loadHistory();

    /** @brief Saves emoji history to disk. */
    void saveHistory();
};

#endif // EMOJI_H
