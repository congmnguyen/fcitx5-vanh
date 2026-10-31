/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file ffi_util.h
 * @brief Helpers of the C++ wrappers around the C ABI (not part of the public interface).
 */

#ifndef _LOTUS_FFI_UTIL_H_
#define _LOTUS_FFI_UTIL_H_

#include "lotus_engine.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lotus::ffi {

    static_assert(sizeof(char32_t) == sizeof(uint32_t));

    inline LotusStr str(std::string_view text) {
        return LotusStr{.data = text.data(), .len = text.size()};
    }

    inline const uint32_t* u32(std::u32string_view text) {
        return reinterpret_cast<const uint32_t*>(text.data()); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

    /** Sink that assigns the received UTF-32 text to the std::u32string passed as context. */
    inline void assignU32(void* ctx, const uint32_t* data, std::size_t len) {
        auto* out = static_cast<std::u32string*>(ctx);
        out->assign(reinterpret_cast<const char32_t*>(data), len); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    }

    /** Sink that assigns the received bytes to the std::string passed as context. */
    inline void assignBytes(void* ctx, const char* data, std::size_t len) {
        static_cast<std::string*>(ctx)->assign(data, len);
    }

} // namespace lotus::ffi

#endif // _LOTUS_FFI_UTIL_H_
