/*
 * SPDX-FileCopyrightText: 2026 congmnguyen <congminhnguyen@proton.me>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
#include "vanh-migration.h"

#if LOTUS_USE_MODERN_FCITX_API
#include <fcitx-utils/standardpaths.h>
#else
#include <fcitx-utils/standardpath.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <unistd.h>

namespace vanh::migration {

    namespace {

        /** @brief Strips spaces and tabs at both ends. */
        std::string_view trim(std::string_view text) {
            const auto begin = text.find_first_not_of(" \t\r");
            if (begin == std::string_view::npos) {
                return {};
            }
            const auto end = text.find_last_not_of(" \t\r");
            return text.substr(begin, end - begin + 1);
        }

        /** @brief Splits text into lines; each line keeps its terminating '\n' (if any). */
        std::vector<std::string_view> splitLines(std::string_view text) {
            std::vector<std::string_view> lines;
            std::size_t                   start = 0;
            while (start < text.size()) {
                const auto newline = text.find('\n', start);
                const auto end     = newline == std::string_view::npos ? text.size() : newline + 1;
                lines.push_back(text.substr(start, end - start));
                start = end;
            }
            return lines;
        }

        /** @brief Line content without its line terminator. */
        std::string_view body(std::string_view line) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.remove_suffix(1);
            }
            return line;
        }

        /** @brief Section name of a "[...]" line, std::nullopt for any other line. */
        std::optional<std::string_view> sectionOf(std::string_view line) {
            const auto content = trim(body(line));
            if (content.size() >= 2 && content.front() == '[' && content.back() == ']') {
                return content.substr(1, content.size() - 2);
            }
            return std::nullopt;
        }

        /** @brief Key and value of a "key=value" line (both trimmed), std::nullopt otherwise. */
        std::optional<std::pair<std::string_view, std::string_view>> keyValueOf(std::string_view line) {
            const auto content = body(line);
            const auto trimmed = trim(content);
            if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
                return std::nullopt;
            }
            const auto equal = content.find('=');
            if (equal == std::string_view::npos) {
                return std::nullopt;
            }
            return std::make_pair(trim(content.substr(0, equal)), trim(content.substr(equal + 1)));
        }

        /** @brief The line with its value replaced, keeping the key text and the terminator. */
        std::string replaceValue(std::string_view line, std::string_view value) {
            const auto  content = body(line);
            std::string result(content.substr(0, content.find('=') + 1));
            result += value;
            result += line.substr(content.size());
            return result;
        }

        /**
         * @brief Classifies a profile section.
         * @param section Section name, e.g. "Groups/0" or "Groups/0/Items/2".
         * @param group Output: the group part ("Groups/0").
         * @return 1 for a group section, 2 for a group item section, 0 otherwise.
         */
        int classifySection(std::string_view section, std::string_view& group) {
            constexpr std::string_view groupsPrefix = "Groups/";
            if (!section.starts_with(groupsPrefix)) {
                return 0;
            }
            const auto slash = section.find('/', groupsPrefix.size());
            if (slash == std::string_view::npos) {
                group = section;
                return 1;
            }
            group = section.substr(0, slash);
            return section.substr(slash).starts_with("/Items/") ? 2 : 0;
        }

        std::string readFile(const std::filesystem::path& path, bool& ok) {
            std::ifstream file(path, std::ios::binary);
            ok = static_cast<bool>(file);
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }

        /** @brief Writes a file through a temporary sibling and a rename, keeping the old mode. */
        bool writeFileAtomically(const std::filesystem::path& path, std::string_view content, std::error_code& error) {
            const auto temporary = path.parent_path() / (path.filename().string() + ".vanh-migration.tmp");
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                if (!file) {
                    error = std::error_code(errno != 0 ? errno : EIO, std::generic_category());
                    return false;
                }
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
                file.flush();
                if (!file) {
                    error = std::error_code(EIO, std::generic_category());
                    std::filesystem::remove(temporary, error);
                    return false;
                }
            }
            std::error_code ignored;
            const auto      status = std::filesystem::status(path, ignored);
            if (!ignored && std::filesystem::exists(status)) {
                std::filesystem::permissions(temporary, status.permissions(), ignored);
            }
            std::filesystem::rename(temporary, path, error);
            if (error) {
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            return true;
        }

        /**
         * @brief Copies one file to a new name unless the new name exists.
         * @param transform Optional rewrite of the content (nullptr copies the bytes).
         */
        void migrateFile(const std::filesystem::path& from, const std::filesystem::path& to, std::string (*transform)(std::string_view), std::vector<std::string>& log) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(from, error) || std::filesystem::exists(to, error)) {
                return;
            }
            std::filesystem::create_directories(to.parent_path(), error);
            if (error) {
                log.push_back("Cannot create " + to.parent_path().string() + ": " + error.message());
                return;
            }
            bool       ok      = false;
            const auto content = readFile(from, ok);
            if (!ok) {
                log.push_back("Cannot read " + from.string());
                return;
            }
            if (!writeFileAtomically(to, transform != nullptr ? transform(content) : content, error)) {
                log.push_back("Cannot write " + to.string() + ": " + error.message());
                return;
            }
            log.push_back("Migrated " + from.string() + " -> " + to.string());
        }

        std::string migrateMainConfigTextFn(std::string_view text) {
            return migrateMainConfigText(text);
        }

    } // namespace

    const std::vector<FileRename>& configFileRenames() {
        static const std::vector<FileRename> renames{
            {.from = "conf/lotus.conf", .to = "conf/vanh.conf"},
            {.from = "conf/lotus-macro-table.conf", .to = "conf/vanh-macro-table.conf"},
            {.from = "conf/lotus-custom-keymap.conf", .to = "conf/vanh-custom-keymap.conf"},
            {.from = "conf/lotus-app-rules.conf", .to = "conf/vanh-app-rules.conf"},
            {.from = "conf/lotus-emoji-history.conf", .to = "conf/vanh-emoji-history.conf"},
        };
        return renames;
    }

    std::string migrateMainConfigText(std::string_view text) {
        std::string result;
        result.reserve(text.size());
        for (const auto line : splitLines(text)) {
            const auto keyValue = keyValueOf(line);
            if (keyValue && keyValue->first == "UseLotusIcons") {
                const auto content = body(line);
                const auto keyPos  = content.find("UseLotusIcons");
                std::string rewritten(line);
                rewritten.replace(keyPos, std::string_view("UseLotusIcons").size(), "UseVanhIcons");
                result += rewritten;
            } else {
                result += line;
            }
        }
        return result;
    }

    std::optional<std::string> migrateProfileText(std::string_view text) {
        const auto lines = splitLines(text);

        // Pass 1: which groups already list the new input method.
        std::map<std::string_view, bool> groupHasNew;
        std::string_view                 group;
        int                              kind = 0;
        for (const auto line : lines) {
            if (const auto section = sectionOf(line)) {
                kind = classifySection(*section, group);
                continue;
            }
            if (kind == 2) {
                if (const auto keyValue = keyValueOf(line); keyValue && keyValue->first == "Name" && keyValue->second == kNewId) {
                    groupHasNew[group] = true;
                }
            }
        }

        // Pass 2: rename.
        std::string result;
        result.reserve(text.size());
        bool changed = false;
        kind         = 0;
        for (const auto line : lines) {
            if (const auto section = sectionOf(line)) {
                kind = classifySection(*section, group);
                result += line;
                continue;
            }
            const auto keyValue = keyValueOf(line);
            if (keyValue && keyValue->second == kOldId) {
                const bool renameItem    = kind == 2 && keyValue->first == "Name" && !groupHasNew[group];
                const bool renameDefault = kind == 1 && keyValue->first == "DefaultIM";
                if (renameItem || renameDefault) {
                    result += replaceValue(line, kNewId);
                    changed = true;
                    continue;
                }
            }
            result += line;
        }
        if (!changed) {
            return std::nullopt;
        }
        return result;
    }

    std::vector<ProfileGroup> parseProfileGroups(std::string_view text) {
        std::vector<ProfileGroup>                 groups;
        std::map<std::string_view, std::size_t>   groupIndex;
        std::string_view                          group;
        int                                       kind = 0;
        ProfileGroup::Item*                       item = nullptr;
        auto                                      groupFor = [&groups, &groupIndex](std::string_view section) -> ProfileGroup& {
            const auto [it, inserted] = groupIndex.try_emplace(section, groups.size());
            if (inserted) {
                groups.emplace_back();
            }
            return groups[it->second];
        };
        for (const auto line : splitLines(text)) {
            if (const auto section = sectionOf(line)) {
                kind = classifySection(*section, group);
                item = nullptr;
                if (kind == 1) {
                    groupFor(group);
                } else if (kind == 2) {
                    item = &groupFor(group).items.emplace_back();
                }
                continue;
            }
            const auto keyValue = keyValueOf(line);
            if (!keyValue) {
                continue;
            }
            if (kind == 1) {
                if (keyValue->first == "Name") {
                    groupFor(group).name = keyValue->second;
                } else if (keyValue->first == "DefaultIM") {
                    groupFor(group).defaultIM = keyValue->second;
                }
            } else if (kind == 2 && item != nullptr) {
                if (keyValue->first == "Name") {
                    item->name = keyValue->second;
                } else if (keyValue->first == "Layout") {
                    item->layout = keyValue->second;
                }
            }
        }
        return groups;
    }

    std::vector<std::string> migrateUserFiles(const std::filesystem::path& fcitxConfigDir, const std::filesystem::path& fcitxDataDir) {
        std::vector<std::string> log;
        for (const auto& rename : configFileRenames()) {
            migrateFile(fcitxConfigDir / rename.from, fcitxConfigDir / rename.to, rename.from == "conf/lotus.conf" ? &migrateMainConfigTextFn : nullptr, log);
        }

        const auto      oldData = fcitxDataDir / std::string(kOldId);
        const auto      newData = fcitxDataDir / std::string(kNewId);
        std::error_code error;
        if (std::filesystem::is_directory(oldData, error)) {
            for (auto it = std::filesystem::recursive_directory_iterator(oldData, error); !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
                if (it->is_regular_file(error)) {
                    migrateFile(it->path(), newData / std::filesystem::relative(it->path(), oldData), nullptr, log);
                }
            }
            if (error) {
                log.push_back("Cannot list " + oldData.string() + ": " + error.message());
            }
        }
        return log;
    }

    std::vector<std::string> migrateProfileFile(const std::filesystem::path& profilePath) {
        std::vector<std::string> log;
        std::error_code          error;
        if (!std::filesystem::is_regular_file(profilePath, error)) {
            return log;
        }
        bool       ok   = false;
        const auto text = readFile(profilePath, ok);
        if (!ok) {
            log.push_back("Cannot read " + profilePath.string());
            return log;
        }
        const auto migrated = migrateProfileText(text);
        if (!migrated) {
            return log;
        }
        if (!writeFileAtomically(profilePath, *migrated, error)) {
            log.push_back("Cannot write " + profilePath.string() + ": " + error.message());
            return log;
        }
        log.push_back("Migrated input method \"lotus\" to \"vanh\" in " + profilePath.string());
        return log;
    }

    bool profileNeedsMigration(const std::filesystem::path& profilePath) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(profilePath, error)) {
            return false;
        }
        bool       ok   = false;
        const auto text = readFile(profilePath, ok);
        return ok && migrateProfileText(text).has_value();
    }

    std::filesystem::path userConfigDir() {
#if LOTUS_USE_MODERN_FCITX_API
        return fcitx::StandardPaths::global().userDirectory(fcitx::StandardPathsType::PkgConfig);
#else
        return fcitx::StandardPath::global().userDirectory(fcitx::StandardPath::Type::PkgConfig);
#endif
    }

    std::filesystem::path userDataDir() {
#if LOTUS_USE_MODERN_FCITX_API
        return fcitx::StandardPaths::global().userDirectory(fcitx::StandardPathsType::PkgData);
#else
        return fcitx::StandardPath::global().userDirectory(fcitx::StandardPath::Type::PkgData);
#endif
    }

} // namespace vanh::migration
