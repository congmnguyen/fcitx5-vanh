/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file encoder.h
 * @brief Output charset conversion (engine-rs/src/encoder.rs), ported from bamboo-core's encoder.go.
 */

#ifndef _LOTUS_ENCODER_H_
#define _LOTUS_ENCODER_H_

#include "types.h"

#include <string>
#include <string_view>

namespace lotus {

    /**
     * @brief Converts text to the bytes of an output charset, like Go's bamboo.Encode.
     * @param charset Target charset.
     * @param text Text to convert.
     * @return Encoded bytes (UTF-8 for Unicode; characters a table lacks are copied as UTF-8).
     */
    std::string encode(Charset charset, std::u32string_view text);

} // namespace lotus

#endif // _LOTUS_ENCODER_H_
