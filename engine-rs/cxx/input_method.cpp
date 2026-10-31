/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "input_method.h"

#include "ffi_util.h"

#include <array>
#include <cstddef>
#include <vector>

namespace lotus {

    InputMethod parseInputMethod(const std::unordered_map<std::string, std::string>& definition) {
        std::vector<LotusStr> keys;
        std::vector<LotusStr> values;
        keys.reserve(definition.size());
        values.reserve(definition.size());
        for (const auto& [key, value] : definition) {
            keys.push_back(ffi::str(key));
            values.push_back(ffi::str(value));
        }
        InputMethod method;
        method.handle_ = std::shared_ptr<const LotusInputMethod>(lotus_input_method_parse(keys.data(), values.data(), keys.size()),
                                                                 [](const LotusInputMethod* handle) { lotus_input_method_free(const_cast<LotusInputMethod*>(handle)); });
        return method;
    }

    const InputMethod& cachedInputMethod(InputMethodId id) {
        static const std::array<InputMethod, kInputMethodCount> kCache = [] {
            std::array<InputMethod, kInputMethodCount> cache;
            for (std::size_t i = 0; i < kInputMethodCount; ++i) {
                // Built-in methods belong to the library: no deleter.
                cache.at(i).handle_ = std::shared_ptr<const LotusInputMethod>(lotus_input_method_builtin(static_cast<uint8_t>(i)), [](const LotusInputMethod*) {});
            }
            return cache;
        }();
        return kCache.at(static_cast<std::size_t>(id));
    }

} // namespace lotus
