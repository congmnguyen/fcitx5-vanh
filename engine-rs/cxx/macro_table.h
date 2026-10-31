/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file macro_table.h
 * @brief Text macro expansion table (engine-rs/src/macro_table.rs), ported from bamboo/macrotable.go.
 *
 * A macro maps the text of the word being typed to its replacement ("vn" -> "Việt Nam"). The table
 * stores the keys verbatim and lowercases the query before the lookup. The addon shares one table
 * between input contexts through a std::shared_ptr; a key processor keeps its own reference to the
 * table data, so later changes through this object are seen by the processors that use it.
 */

#ifndef _LOTUS_MACRO_TABLE_H_
#define _LOTUS_MACRO_TABLE_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct LotusMacroTable;

namespace lotus {

    /** @brief Key to replacement text map driving the macro expansion, like Go's MacroTable. */
    class MacroTable {
      public:
        MacroTable();

        /** @brief True when no macro is defined. */
        [[nodiscard]] bool empty() const;

        /**
         * @brief Replacement of a trigger word.
         * @param key Trigger text; it is lowercased before the lookup.
         * @return Replacement text, or std::nullopt when no macro matches.
         */
        [[nodiscard]] std::optional<std::u32string> get(std::u32string_view key) const;

        /**
         * @brief Adds or replaces a macro.
         * @param keyUtf8 Trigger text, UTF-8, stored verbatim.
         * @param valueUtf8 Replacement text, UTF-8.
         */
        void set(std::string_view keyUtf8, std::string_view valueUtf8);

        /** @brief Handle for the C ABI. */
        [[nodiscard]] const LotusMacroTable* handle() const {
            return handle_.get();
        }

      private:
        std::unique_ptr<LotusMacroTable, void (*)(LotusMacroTable*)> handle_;
    };

} // namespace lotus

#endif // _LOTUS_MACRO_TABLE_H_
