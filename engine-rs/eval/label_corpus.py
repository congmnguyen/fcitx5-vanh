#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turns raw.jsonl (collect_corpus.py) into the labelled replay set OUT/corpus.tsv.

Each row is one typed segment: what the user would type in one go before Enter (a line, cut at
characters a US keyboard + Telex cannot type, such as emoji or curly quotes). Every token
(text between single spaces) gets a label:
  v  Vietnamese        e  English        c  code (identifiers, flags, paths, shell)
  o  no letters (numbers, punctuation)
The split is by text block (a commit, an issue, a markdown paragraph, a shell line), hashed, so
the same block always lands in the same split: 20% test, 80% dev.

Columns: split  source  tier  id  labels  text
tier: "typed" (the user's own writing: commits/issues/PRs without an AI trailer, shell history,
notes outside repos) or "doc" (markdown in repos and AI co-written text, which the user may not
have typed key by key).
"""
import hashlib
import json
import re
import sys
import unicodedata
from pathlib import Path

VN_LETTERS = "àáảãạăằắẳẵặâầấẩẫậèéẻẽẹêềếểễệìíỉĩịòóỏõọôồốổỗộơờớởỡợùúủũụưừứửữựỳýỷỹỵđ"
VN_SET = set(VN_LETTERS + VN_LETTERS.upper())
VN_RE = re.compile(f"[{VN_LETTERS}]", re.I)
TYPEABLE = set(chr(c) for c in range(0x20, 0x7F)) | VN_SET
CODE_CHARS = set("_/\\=<>{}|@#$%^&*~`+")
EDGE = "\"'()[]{}.,;:!?*`<>"
URL = re.compile(r"[a-z]+://|www\.", re.I)

ROOT = Path(__file__).resolve().parents[2]
DICT = {w.strip() for w in (ROOT / "data/dictionaries/vietnamese.cm.dict").read_text().splitlines() if w.strip()}
# Plain-ASCII words that are also Vietnamese syllables but, in this text, almost always English.
EN_STOP = set(
    "the to in on of and for is it at by an as be or no so do go me we he up us if all can not but you "
    "this that then than test code data file user model mode log run set get push pull merge main fix "
    "bug app web page time line case call text team plan task done use new top tool tools token box".split()
)


def split_of(key):
    return "test" if int(hashlib.sha1(key.encode()).hexdigest(), 16) % 5 == 0 else "dev"


def label(token, vn_line, code_ctx):
    core = token.strip(EDGE)
    if not any(ch.isalpha() for ch in core):
        return "o"
    if code_ctx:
        return "c"
    if VN_RE.search(core):
        return "c" if any(ch in CODE_CHARS for ch in core) else "v"
    if URL.search(core) or any(ch in CODE_CHARS for ch in core) or core.startswith("-"):
        return "c"
    if re.search(r"[A-Za-z]\.[A-Za-z]", core) or re.search(r"[0-9]", core):
        return "c"
    if re.search(r"[a-z][A-Z]", core):
        return "c"  # camelCase / PascalCase identifiers
    if vn_line and core.lower() in DICT and core.lower() not in EN_STOP:
        return "v"
    return "e"


def segments(line):
    """Pieces of the line a keyboard can type, cut at anything else (and at tabs)."""
    out, cur = [], []
    for ch in line:
        if ch in TYPEABLE:
            cur.append(ch)
        else:
            if cur:
                out.append("".join(cur))
            cur = []
    if cur:
        out.append("".join(cur))
    return [s.strip() for s in out if s.strip()]


def main():
    out_dir = Path(sys.argv[1])
    rows = []
    seen = {}
    for raw in (out_dir / "raw.jsonl").read_text().splitlines():
        r = json.loads(raw)
        text = unicodedata.normalize("NFC", r["text"])
        src = r["source"]
        tier = "typed" if (not r["ai"] and src in ("commit", "pr", "issue", "bash")) or (src == "markdown" and not r["id"].startswith("code/")) else "doc"
        if src == "markdown" and r["id"].startswith(("Documents/", "Desktop/")):
            tier = "typed"
        blocks = re.split(r"\n\s*\n", text) if src == "markdown" else [text]
        in_fence = False
        for bi, block in enumerate(blocks):
            key = f"{r['id']}#{bi}"
            sp = split_of(key)
            for line in block.splitlines():
                if re.match(r"\s*(co-authored-by|signed-off-by):", line, re.I):
                    continue  # trailers are pasted, not typed
                if line.strip().startswith("```"):
                    in_fence = not in_fence
                    continue
                for seg in segments(line):
                    # Boilerplate repeated across many blocks (PR templates) would weigh many
                    # times: keep at most two copies of a segment.
                    seen[seg] = seen.get(seg, 0) + 1
                    if seen[seg] > 2:
                        continue
                    toks = seg.split(" ")
                    letters = [t for t in toks if any(c.isalpha() for c in t)]
                    vn_line = bool(letters) and sum(1 for t in letters if VN_RE.search(t)) / len(letters) >= 0.3
                    labels, in_tick = [], False
                    for t in toks:
                        starts = t.count("`")
                        code_ctx = in_fence or src == "bash" or in_tick or starts > 0
                        labels.append(label(t, vn_line, code_ctx))
                        if starts % 2 == 1:
                            in_tick = not in_tick
                    if not any(l != "o" for l in labels):
                        continue
                    rows.append((sp, src, tier, key, "".join(labels), seg))
    with open(out_dir / "corpus.tsv", "w") as f:
        f.write("# split\tsource\ttier\tid\tlabels\ttext\n")
        for row in rows:
            f.write("\t".join(x.replace("\t", " ") for x in row) + "\n")
    stats = {}
    for sp, src, tier, _, labels, _ in rows:
        for l in labels:
            k = (sp, tier, l)
            stats[k] = stats.get(k, 0) + 1
    for k in sorted(stats):
        print(*k, stats[k], sep="\t", file=sys.stderr)


if __name__ == "__main__":
    main()
