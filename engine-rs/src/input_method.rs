// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Built-in input methods, parsed once.

use crate::data::INPUT_METHOD_DEFINITIONS;
use crate::rule::InputMethod;
use crate::rules_parser::parse_input_method;
use crate::types::{InputMethodId, INPUT_METHOD_COUNT};
use std::sync::OnceLock;

/// The parsed built-in input method (Custom is empty).
pub fn cached_input_method(id: InputMethodId) -> &'static InputMethod {
    static CACHE: OnceLock<Vec<InputMethod>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| (0..INPUT_METHOD_COUNT).map(|i| parse_input_method(INPUT_METHOD_DEFINITIONS[i])).collect());
    &cache[id as usize]
}
