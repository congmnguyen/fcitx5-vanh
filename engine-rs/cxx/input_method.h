/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file input_method.h
 * @brief Input methods of the Rust engine: the built-in ones and parsed custom definitions.
 */

#ifndef _LOTUS_INPUT_METHOD_H_
#define _LOTUS_INPUT_METHOD_H_

#include "types.h"

#include <memory>
#include <string>
#include <unordered_map>

struct LotusInputMethod;

namespace lotus {

    /** @brief A parsed input method (opaque, cheap to copy). */
    class InputMethod {
      public:
        /** @brief Handle for the C ABI. */
        [[nodiscard]] const LotusInputMethod* handle() const {
            return handle_.get();
        }

      private:
        friend InputMethod parseInputMethod(
            const std::unordered_map<std::string, std::string>& definition);
        friend const InputMethod&               cachedInputMethod(InputMethodId id);
        std::shared_ptr<const LotusInputMethod> handle_;
    };

    /**
     * @brief Parses a definition (key -> definition line), like Go's ParseInputMethod.
     * @param definition Definition lines by key, UTF-8.
     * @return Parsed input method.
     */
    InputMethod parseInputMethod(const std::unordered_map<std::string, std::string>& definition);

    /**
     * @brief A built-in input method, parsed once.
     * @param id Input method; Custom has no built-in definition (an empty method).
     * @return Input method that lives as long as the program.
     */
    [[nodiscard]] const InputMethod& cachedInputMethod(InputMethodId id);

} // namespace lotus

#endif // _LOTUS_INPUT_METHOD_H_
