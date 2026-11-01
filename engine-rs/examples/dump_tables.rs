// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Prints the case and charset tables in a plain text form, to compare a regenerated table set
//! with a dump of the former C++ tables (see tools/gen_data.py).

use lotus_engine::data::{case, charset, vikey_autoswitch as words, vikey_bigram as bigram};

fn write<T: Copy>(name: &str, values: &[T], to_bytes: impl Fn(T) -> Vec<u8>) {
    let bytes: Vec<u8> = values.iter().flat_map(|&v| to_bytes(v)).collect();
    std::fs::write(name, bytes).unwrap();
}

fn main() {
    // With an argument, also writes the Vikey arrays as raw native-endian files into that
    // directory.
    if let Some(dir) = std::env::args().nth(1) {
        let path = |name: &str| format!("{dir}/rs_{name}");
        write(&path("vn_words"), words::VN_WORDS, |b| vec![b]);
        write(&path("vn_offsets"), &words::VN_OFFSETS, |v| v.to_ne_bytes().to_vec());
        write(&path("vn_scores"), &words::VN_SCORES, |v| v.to_ne_bytes().to_vec());
        write(&path("en_words"), words::EN_WORDS, |b| vec![b]);
        write(&path("en_offsets"), &words::EN_OFFSETS, |v| v.to_ne_bytes().to_vec());
        write(&path("en_scores"), &words::EN_SCORES, |v| v.to_ne_bytes().to_vec());
        write(&path("bi_keys"), bigram::KEYS, |b| vec![b]);
        write(&path("bi_offsets"), &bigram::OFFSETS, |v| v.to_ne_bytes().to_vec());
        write(&path("bi_counts"), &bigram::LOG_COUNTS, |v| vec![v]);
    }
    for (a, b) in case::LOWER_UPPER.iter() {
        println!("LU {:X} {:X}", *a as u32, *b as u32);
    }
    for (a, b) in case::UPPER_LOWER.iter() {
        println!("UL {:X} {:X}", *a as u32, *b as u32);
    }
    for c in case::LETTERS.iter() {
        println!("L {:X}", *c as u32);
    }
    for (i, table) in charset::TABLES.iter().enumerate() {
        for (c, value) in table.iter() {
            let hex: String = value.bytes().map(|b| format!("{b:02x}")).collect();
            println!("C {i} {:X} {hex}", *c as u32);
        }
    }
}
