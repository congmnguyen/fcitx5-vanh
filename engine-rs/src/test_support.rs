// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Reader of the golden corpus in test/golden (written by the Go engine).

use std::path::PathBuf;

pub fn golden_dir() -> PathBuf {
    match std::env::var_os("LOTUS_GOLDEN_DIR") {
        Some(dir) => PathBuf::from(dir),
        None => PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../test/golden"),
    }
}

/// Rows of a golden file, tab separated; comments and empty lines are skipped.
pub fn read_golden(name: &str) -> Vec<Vec<String>> {
    let path = golden_dir().join(name);
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    text.lines()
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .map(|line| line.split('\t').map(str::to_owned).collect())
        .collect()
}

pub fn from_hex(hex: &str) -> Vec<u8> {
    (0..hex.len() / 2).map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).unwrap()).collect()
}

pub fn chars(s: &str) -> Vec<char> {
    s.chars().collect()
}

pub fn string(c: &[char]) -> String {
    c.iter().collect()
}
