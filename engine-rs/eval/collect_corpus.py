#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Collects a private typing corpus from the user's own text (never leaves the machine).

Sources: commit messages the user authored, PR/issue titles and bodies the user wrote (through
the local `gh`), ~/.bash_history and the user's markdown notes. Output: one JSON object per text
block in OUT/raw.jsonl with its source and whether an AI assistant co-wrote it.

Usage: collect_corpus.py OUT_DIR [--emails a@b,c@d] [--gh-user NAME]
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HOME = Path.home()
REPO_ROOTS = [HOME / "code", HOME]
SKIP_DIRS = {
    "node_modules", ".git", ".venv", "venv", "site-packages", "target", "build", "dist", "cache",
    "results", "artifacts", ".cache", ".cargo", ".rustup", ".local", ".config", "vendor", "snap",
}
AI_MARKERS = re.compile(r"co-authored-by:\s*claude|generated with \[?claude|🤖 generated|noreply@anthropic\.com", re.I)
TRAILER = re.compile(r"^(co-authored-by|signed-off-by|change-id|reviewed-by):", re.I)


def run(cmd, cwd=None):
    try:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def git_repos():
    seen = set()
    for root in REPO_ROOTS:
        if not root.is_dir():
            continue
        for d in sorted(root.iterdir()):
            if (d / ".git").exists() and d.resolve() not in seen:
                seen.add(d.resolve())
                yield d


def commits(repo, emails):
    out = run(["git", "log", "--all", "--no-merges", "--format=%H%x00%ae%x00%B%x1e"], cwd=repo)
    for rec in out.split("\x1e"):
        rec = rec.strip("\n")
        if not rec:
            continue
        sha, email, body = (rec.split("\x00") + ["", ""])[:3]
        if email.lower() not in emails:
            continue
        ai = bool(AI_MARKERS.search(body))
        lines = [l for l in body.splitlines() if not TRAILER.match(l.strip()) and not AI_MARKERS.search(l)]
        text = "\n".join(lines).strip()
        if text:
            yield {"source": "commit", "id": f"{repo.name}@{sha[:10]}", "ai": ai, "text": text}


def gh_items(user):
    for kind in ("prs", "issues"):
        out = run(["gh", "search", kind, "--author", user, "--limit", "1000", "--json", "repository,number,title,body"])
        if not out:
            continue
        for it in json.loads(out):
            body = it.get("body") or ""
            ai = bool(AI_MARKERS.search(body))
            body = "\n".join(l for l in body.splitlines() if not AI_MARKERS.search(l))
            text = (it["title"] + "\n" + body).strip()
            yield {"source": "pr" if kind == "prs" else "issue", "id": f"{it['repository']['nameWithOwner']}#{it['number']}", "ai": ai, "text": text}


def bash_history():
    path = HOME / ".bash_history"
    if not path.exists():
        return
    for i, line in enumerate(path.read_text(errors="replace").splitlines()):
        line = line.rstrip()
        if line and not line.startswith("#"):
            yield {"source": "bash", "id": f"bash:{i}", "ai": False, "text": line}


def authored_by_user(path, emails):
    """In a git repo: the file's history has a commit by the user; outside one: always."""
    top = run(["git", "rev-parse", "--show-toplevel"], cwd=path.parent).strip()
    if not top:
        return True, False
    log = run(["git", "log", "--format=%ae%x00%B%x1e", "--", str(path)], cwd=top)
    recs = [r for r in log.split("\x1e") if r.strip()]
    if not recs:
        return False, False  # untracked in a repo: generated or not the user's
    mine = [r for r in recs if r.strip().split("\x00")[0].lower() in emails]
    ai = any(AI_MARKERS.search(r) for r in mine)
    return bool(mine), ai


def markdown(emails):
    roots = [HOME / "code", HOME / "Documents", HOME / "Desktop", HOME / "aitc-prep", HOME / "ubuntu-setup"]
    for root in roots:
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith(".")]
            for name in filenames:
                if not name.endswith(".md"):
                    continue
                p = Path(dirpath) / name
                if p.stat().st_size > 2_000_000:
                    continue
                mine, ai = authored_by_user(p, emails)
                if not mine:
                    continue
                yield {"source": "markdown", "id": str(p.relative_to(HOME)), "ai": ai, "text": p.read_text(errors="replace")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--emails", default="congminhnguyen@proton.me,facker.ncf@gmail.com,kv8haiquan@gmail.com")
    ap.add_argument("--gh-user", default="congmnguyen")
    a = ap.parse_args()
    emails = {e.strip().lower() for e in a.emails.split(",")}
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    os.chmod(out, 0o700)
    n = 0
    with open(out / "raw.jsonl", "w") as f:
        for gen in ([c for r in git_repos() for c in commits(r, emails)], gh_items(a.gh_user), bash_history(), markdown(emails)):
            for rec in gen:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")
                n += 1
    os.chmod(out / "raw.jsonl", 0o600)
    print(f"{n} text blocks -> {out / 'raw.jsonl'}", file=sys.stderr)


if __name__ == "__main__":
    main()
