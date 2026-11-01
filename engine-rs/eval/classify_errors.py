#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sorts the errors of `replay run --errors FILE` into failure types (first match wins)."""
import collections
import re
import sys

EDGE = "\"'()[]{}.,;:!?*`<>"

def ftype(kind, label, exp, pos):
    core = exp.strip(EDGE)
    letters = re.sub(r"[^A-Za-z]", "", core)
    if pos == "eol" and core == exp and kind != "vn->en":
        return "word before Enter (no restore at line end)"
    if label == "c":
        if core.startswith("-"):
            return "code: flag (--host, -af)"
        if "/" in core or re.search(r"\w\.\w", core):
            return "code: path / file / URL"
        if re.search(r"[0-9a-f]{12,}", core):
            return "code: hash / hex"
        if "_" in core or re.search(r"[a-z][A-Z]", core):
            return "code: identifier (snake/camel)"
        return "code: shell word / other"
    if kind == "vn->en":
        return "Vietnamese restored to English"
    if len(letters) >= 2 and letters.isupper():
        return "acronym / ALL CAPS (ASR, CORS)"
    if kind == "en-mangled" and re.search(r"(ss|ff|rr|xx|jj|ww|aa|ee|oo|dd)", core.lower()):
        return "double letter eaten by Telex undo (diff, session)"
    if len(letters) <= 3:
        return "short ambiguous word (is, as, of)"
    if core[:1].isupper():
        return "capitalised word / proper noun (Rust, These)"
    return "English word missing or outvoted (docs, host, theme)"

def main():
    rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if l.strip()]
    c = collections.Counter()
    ex = collections.defaultdict(collections.Counter)
    for kind, label, exp, got, _id, pos, *_ in rows:
        t = ftype(kind, label, exp, pos)
        c[t] += 1
        ex[t][f"{exp}->{got}"] += 1
    total = sum(c.values())
    for t, n in c.most_common():
        examples = ", ".join(k for k, _ in ex[t].most_common(5))
        print(f"{n:5} {100 * n / total:5.1f}%  {t:52} e.g. {examples}")

if __name__ == "__main__":
    main()
