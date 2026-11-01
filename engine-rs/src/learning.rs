// SPDX-License-Identifier: GPL-3.0-or-later

//! Opt-in local learning from reverts: each time the user reverts an automatic switch, the keys
//! of that word lean one step toward the reading the user picked. The table lives in memory and
//! in a small text file the addon keeps in the user's data directory; nothing leaves the machine.
//!
//! File format, one word per line: `keys<TAB>score`, score > 0 = the keys (English), < 0 = the
//! Vietnamese reading. Lines that do not parse are skipped.

use std::collections::HashMap;

/// A score this far from 0 decides the word on its own.
pub const DECIDES: i32 = 1;
const LIMIT: i32 = 8;

#[derive(Clone, Debug, Default)]
pub struct Learned {
    prefs: HashMap<String, i32>,
}

impl Learned {
    /// > 0: keep the keys, < 0: the Vietnamese reading, 0: no preference.
    pub fn preference(&self, keys_lower: &str) -> i32 {
        self.prefs.get(keys_lower).copied().unwrap_or(0)
    }

    /// The user picked the keys (`english`) or the Vietnamese reading for these keys.
    pub fn record(&mut self, keys_lower: &str, english: bool) {
        let score = self.prefs.entry(keys_lower.to_owned()).or_insert(0);
        *score = (*score + if english { 1 } else { -1 }).clamp(-LIMIT, LIMIT);
        if *score == 0 {
            self.prefs.remove(keys_lower);
        }
    }

    pub fn load_from_text(&mut self, text: &str) {
        self.prefs.clear();
        for line in text.lines() {
            let Some((keys, score)) = line.split_once('\t') else {
                continue;
            };
            let valid =
                !keys.is_empty() && keys.len() < 40 && keys.bytes().all(|b| b.is_ascii_lowercase());
            if let (true, Ok(score)) = (valid, score.trim().parse::<i32>()) {
                if score != 0 {
                    self.prefs.insert(keys.to_owned(), score.clamp(-LIMIT, LIMIT));
                }
            }
        }
    }

    pub fn to_text(&self) -> String {
        let mut rows: Vec<_> = self.prefs.iter().collect();
        rows.sort();
        rows.iter().map(|(k, v)| format!("{k}\t{v}\n")).collect()
    }

    pub fn len(&self) -> usize {
        self.prefs.len()
    }

    pub fn is_empty(&self) -> bool {
        self.prefs.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn records_and_round_trips() {
        let mut l = Learned::default();
        l.record("host", true);
        l.record("host", true);
        l.record("mas", false);
        assert_eq!(l.preference("host"), 2);
        assert_eq!(l.preference("mas"), -1);
        l.record("mas", true);
        assert_eq!(l.preference("mas"), 0);
        let text = l.to_text();
        assert_eq!(text, "host\t2\n");
        let mut m = Learned::default();
        m.load_from_text("host\t2\nBAD\t3\nx y\t1\nfoo\tnope\nbar\t-99\n");
        assert_eq!(m.preference("host"), 2);
        assert_eq!(m.preference("bar"), -LIMIT);
        assert_eq!(m.len(), 2);
    }
}
