# SPDX-FileCopyrightText: 2026 Cong Nguyen <congmnguyen@users.noreply.github.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for ui.pages.learned_editor parsing and formatting."""

import unittest

import _stubs  # noqa: F401
from ui.pages.learned_editor import (
    LEARNED_SCORE_LIMIT,
    format_learned_text,
    is_valid_learned_key,
    parse_learned_text,
)


class LearnedWordsFormatTests(unittest.TestCase):
    def test_key_validation(self):
        self.assertTrue(is_valid_learned_key("host"))
        self.assertTrue(is_valid_learned_key("a"))
        self.assertFalse(is_valid_learned_key(""))
        self.assertFalse(is_valid_learned_key("Host"))
        self.assertFalse(is_valid_learned_key("a b"))
        self.assertFalse(is_valid_learned_key("a1"))
        self.assertFalse(is_valid_learned_key("a" * 40))

    def test_parse_skips_invalid_lines_and_clamps_scores(self):
        raw = "host\t2\nBAD\t3\nx y\t1\nfoo\tnope\nbar\t-99\nzero\t0\nconf\t99\n"
        self.assertEqual(
            parse_learned_text(raw),
            {"host": 2, "bar": -LEARNED_SCORE_LIMIT, "conf": LEARNED_SCORE_LIMIT},
        )

    def test_format_sorts_and_filters_invalid_entries(self):
        text = format_learned_text({"mas": -1, "host": 2, "zero": 0, "BAD": 1, "big": 99})
        self.assertEqual(text, f"big\t{LEARNED_SCORE_LIMIT}\nhost\t2\nmas\t-1\n")
        self.assertEqual(parse_learned_text(text), {"big": 8, "host": 2, "mas": -1})


if __name__ == "__main__":
    unittest.main()
