// SPDX-License-Identifier: GPL-3.0-or-later
//! Prints the English-restore scores of words: explain KEYS=READING ...
use lotus_engine::english::{analyze, MISSING};
fn main() {
    for a in std::env::args().skip(1) {
        let (k, r) = a.split_once('=').unwrap();
        let w = analyze(&k.chars().collect::<Vec<_>>(), &r.chars().collect::<Vec<_>>()).unwrap();
        let s = |v: i32| if v == MISSING { "-".to_string() } else { v.to_string() };
        println!("{k:>10} {r:>8} en {:>4} vn {:>4} same {}", s(w.en), s(w.vn), w.same);
    }
}
