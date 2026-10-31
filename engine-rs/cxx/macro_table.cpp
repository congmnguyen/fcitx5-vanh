/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "macro_table.h"

#include "ffi_util.h"

namespace lotus {

    MacroTable::MacroTable() : handle_(lotus_macro_table_new(), lotus_macro_table_free) {}

    bool MacroTable::empty() const {
        return lotus_macro_table_is_empty(handle_.get());
    }

    std::optional<std::u32string> MacroTable::get(std::u32string_view key) const {
        std::u32string value;
        if (!lotus_macro_table_get(handle_.get(), ffi::u32(key), key.size(), ffi::assignU32, &value)) {
            return std::nullopt;
        }
        return value;
    }

    void MacroTable::set(std::string_view keyUtf8, std::string_view valueUtf8) {
        lotus_macro_table_set(handle_.get(), ffi::str(keyUtf8), ffi::str(valueUtf8));
    }

} // namespace lotus
