// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Vietnamese typing engine of fcitx5-vanh (Vanh).
//!
//! A port of the Go bamboo-core engine (MIT, (c) 2018 Luong Thanh Lam, kept in CREDITS) by way of
//! the former C++ port, plus the fcitx glue logic of fcitx5-bamboo (`key_processor`) and the
//! automatic English restore after Vikey (`english`). The fcitx5 addon talks to it through the C
//! ABI in `ffi` (see include/lotus_engine.h).

pub mod case_map;
pub mod composition;
pub mod data;
pub mod dictionary;
pub mod encoder;
pub mod engine;
pub mod engine_utils;
pub mod english;
pub mod ffi;
pub mod input_method;
pub mod key_processor;
pub mod macro_table;
pub mod rebuild;
pub mod rule;
pub mod rules_parser;
pub mod spelling;
pub mod time_format;
pub mod types;
pub mod utf8;
pub mod utils;

#[cfg(test)]
mod test_support;
#[cfg(test)]
mod tests;
