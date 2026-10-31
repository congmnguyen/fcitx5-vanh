// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Output charset encoder, 1:1 with bamboo's encoder.go.

use crate::data::charset::TABLES;
use crate::types::Charset;

/// Encodes text in a charset; the legacy charsets keep the bytes Go kept in its strings, which
/// are themselves UTF-8 text (code points standing for the legacy bytes).
pub fn encode(charset: Charset, text: &[char]) -> String {
    let table = TABLES[charset as usize];
    let mut out = String::with_capacity(text.len());
    for &c in text {
        match table.binary_search_by(|entry| entry.0.cmp(&c)) {
            Ok(index) => out.push_str(table[index].1),
            Err(_) => out.push(c),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tables_are_sorted() {
        for table in TABLES.iter() {
            assert!(table.windows(2).all(|w| w[0].0 < w[1].0));
        }
        assert!(TABLES[Charset::Unicode as usize].is_empty());
    }

    // Ported from test/lotus-encoder-test.cpp: every code point of the golden corpus (written by
    // the Go engine) encodes to the same bytes.
    #[test]
    fn golden_charset_corpus() {
        let names = [
            "Unicode", "BKHCM 1", "BKHCM 2", "NCR Decimal", "NCR Hex", "TCVN3 (ABC)", "UTF-8", "Unicode C string Decimal",
            "Unicode C string Hex", "Unicode tổ hợp", "VIQR", "VISCII", "VNI Windows", "VPS", "Vietware Full", "Vietware X",
            "Windows 1258 codepage",
        ];
        let rows = crate::test_support::read_golden("charset.tsv");
        assert!(rows.len() > 1000);
        let mut seen = std::collections::HashSet::new();
        for row in rows {
            let charset = names.iter().position(|n| *n == row[0]).map(|i| Charset::ALL[i]).expect("known charset");
            seen.insert(charset as usize);
            let code_point = char::from_u32(u32::from_str_radix(&row[1][2..], 16).unwrap()).unwrap();
            let expected = crate::test_support::from_hex(row.get(2).map(String::as_str).unwrap_or(""));
            assert_eq!(encode(charset, &[code_point]).as_bytes(), expected.as_slice(), "{} U+{:04X}", row[0], code_point as u32);
        }
        assert_eq!(seen.len(), 17);
    }

    #[test]
    fn unknown_code_points_pass_through() {
        assert_eq!(encode(Charset::Tcvn3Abc, &['x', '€']), "x€");
        assert_eq!(encode(Charset::Unicode, &['ạ']), "ạ");
    }
}
