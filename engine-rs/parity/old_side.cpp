// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The former C++ engine (compiled with -Dlotus=lotus_old, include root = its lotus/ directory).

#define PARITY_OLD 1
#define PARITY_SIDE_FN oldSide
#include "side_impl.inc"

#include "data/input_method_data.h"

std::vector<std::map<std::string, std::string>> parity::builtinDefinitions() {
    std::vector<std::map<std::string, std::string>> out;
    for (const auto& definition : lotus::data::kInputMethodDefinitions) {
        out.emplace_back(definition.begin(), definition.end());
    }
    return out;
}
