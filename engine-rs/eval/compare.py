#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs the replay harness for every engine on one split and prints a markdown table.

compare.py CORPUS --new NEW_REPLAY --base BASE_REPLAY [--unikey UNIKEY_REPLAY] [--split test] [--tier all]
"""
import argparse, re, subprocess, sys, tempfile, os

ROWS = [("Vietnamese restored to English", "VN→EN"), ("Vietnamese otherwise mangled", "VN mangled"),
        ("English turned Vietnamese", "EN→VN"), ("English otherwise mangled", "EN mangled"),
        ("code token mangled", "code mangled"), ("other (spacing/punctuation)", "other"), ("TOTAL", "TOTAL")]

def parse(out):
    res = {}
    for line in out.splitlines():
        for full, short in ROWS:
            if line.startswith(full):
                nums = line[len(full):].split()
                res[short] = (int(nums[0]), float(nums[1]))
        m = re.match(r"segments (\d+).*words (\d+) \(vn (\d+), en (\d+), code (\d+)\)", line)
        if m:
            res["words"] = tuple(map(int, m.groups()))
    return res

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus"); ap.add_argument("--new", required=True); ap.add_argument("--base", required=True)
    ap.add_argument("--unikey"); ap.add_argument("--split", default="test"); ap.add_argument("--tier", default="all")
    ap.add_argument("--extra", default="")
    a = ap.parse_args()
    sel = ["--split", a.split, "--tier", a.tier, "--worst", "0"] + a.extra.split()
    runs = [
        ("Vanh before (defaults)", [a.base, "run", a.corpus, "--config", "default", "--no-enter-fix"] + sel),
        ("Vanh after (defaults)", [a.new, "run", a.corpus, "--config", "default"] + sel),
        ("Vanh before (your config)", [a.base, "run", a.corpus, "--config", "user", "--no-enter-fix"] + sel),
        ("Vanh after (your config)", [a.new, "run", a.corpus, "--config", "user"] + sel),
        ("bamboo-core rules (ibus-bamboo proxy)", [a.base, "run", a.corpus, "--config", "bamboo", "--no-enter-fix"] + sel),
    ]
    results = [(name, parse(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)) for name, cmd in runs]
    if a.unikey:
        with tempfile.TemporaryDirectory() as d:
            keys, outs = os.path.join(d, "keys"), os.path.join(d, "out")
            subprocess.run([a.new, "keys", a.corpus, keys], check=True)
            with open(keys) as fi, open(outs, "w") as fo:
                subprocess.run([a.unikey, "1"], stdin=fi, stdout=fo, check=True)
            out = subprocess.run([a.new, "score", a.corpus, outs] + sel, capture_output=True, text=True, check=True).stdout
            results.append(("fcitx5-unikey 5.1.10 engine", parse(out)))
    w = results[0][1]["words"]
    print(f"split={a.split} tier={a.tier}: {w[1]} words (vn {w[2]}, en {w[3]}, code {w[4]}), {w[0]} segments; errors per 1,000 words (count)\n")
    print("| engine | " + " | ".join(s for _, s in ROWS) + " |")
    print("|---|" + "---:|" * len(ROWS))
    for name, r in results:
        print(f"| {name} | " + " | ".join(f"{r[s][1]:.2f} ({r[s][0]})" for _, s in ROWS) + " |")

if __name__ == "__main__":
    main()
