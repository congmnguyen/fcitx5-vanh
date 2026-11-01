#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end typing tests in real apps (Ghostty, Brave) on a private, headless GNOME Shell.

Run:  dbus-run-session -- python3 test/e2e/gnome-e2e.py [--apps ghostty,brave] [--modes smooth,surrounding]

Everything runs inside its own D-Bus session and its own headless gnome-shell (Wayland, virtual
monitor), so nothing reaches the desktop you are using:
  * keys are typed through Mutter's RemoteDesktop API of the private compositor;
  * the addon is the one in build/src, loaded by a private fcitx5 with its own config dir;
  * the uinput server is replaced by a stand-in that receives the addon's requests on the real
    socket protocol (KbMsg) and types the BackSpace / Shift+Left / Delete / Pause keys through the
    same compositor (a real uinput device would reach the session you are using);
  * Brave runs with a throwaway profile and no network (proxy to a closed port, every host
    unresolvable), so typed text cannot leave the machine.

The text typed is a fixed list of synthetic sentences below, not the private corpus. For each
case the expected screen text is what the engine itself produces for the same keys (engine-rs
`replay type --raw`): a difference is a delivery bug (lost, doubled or misplaced characters).
"""
import argparse
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RUN = Path("/tmp/claude-1000/ve2e")  # short: a Wayland socket path must fit in 108 bytes
# Another build to compare against: VANH_E2E_BUILD (a CMake build dir) and VANH_E2E_REPLAY.
BUILD = Path(os.environ.get("VANH_E2E_BUILD", ROOT / "build"))
REPLAY = Path(os.environ.get("VANH_E2E_REPLAY", ROOT / "engine-rs/target/release/examples/replay"))

SENTENCES = [
    "tôi đang sửa lỗi build cho Ghostty",
    "fix typo trong README rồi push lên main",
    "chạy git status rồi commit -m",
    "the poller is stuck khi mất mạng",
    "kiểm tra file ~/.config/fcitx5/profile",
    "Brave không gõ được tiếng Việt trong address bar",
    "max_tokens = 4096 là quá nhiều",
    "được rồi, merge PR này đi",
]

# US layout: char -> (evdev code, shift)
_ROWS = {
    "1234567890-=": [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13],
    "!@#$%^&*()_+": [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13],
    "qwertyuiop[]": [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27],
    "QWERTYUIOP{}": [16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27],
    "asdfghjkl;'`": [30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41],
    'ASDFGHJKL:"~': [30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41],
    "\\zxcvbnm,./": [43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53],
    "|ZXCVBNM<>?": [43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53],
}
KEYMAP = {" ": (57, False), "\n": (28, False), "\t": (15, False)}
for chars, codes in _ROWS.items():
    shifted = chars[0] in "!QA|"
    for ch, code in zip(chars, codes):
        KEYMAP[ch] = (code, shifted)
SHIFT, CTRL, BACKSPACE, LEFT, DELETE, PAUSE, ENTER = 42, 29, 14, 105, 111, 119, 28


class Keyboard:
    """Mutter RemoteDesktop session of the private compositor (evdev key codes)."""

    def __init__(self):
        bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        self.bus = bus
        rd = bus.call_sync("org.gnome.Mutter.RemoteDesktop", "/org/gnome/Mutter/RemoteDesktop", "org.gnome.Mutter.RemoteDesktop", "CreateSession", None, GLib.VariantType("(o)"), 0, -1, None)
        self.path = rd.unpack()[0]
        self._call("Start", None)
        self.lock = threading.Lock()

    def _call(self, method, args):
        self.bus.call_sync("org.gnome.Mutter.RemoteDesktop", self.path, "org.gnome.Mutter.RemoteDesktop.Session", method, args, None, 0, -1, None)

    def key(self, code, down):
        self._call("NotifyKeyboardKeycode", GLib.Variant("(ub)", (code, down)))

    def tap(self, code, mods=()):
        with self.lock:
            for m in mods:
                self.key(m, True)
            self.key(code, True)
            self.key(code, False)
            for m in reversed(mods):
                self.key(m, False)

    def type(self, text, delay):
        for ch in text:
            code, shift = KEYMAP[ch]
            self.tap(code, (SHIFT,) if shift else ())
            if delay:
                time.sleep(delay)


class FakeUinputServer:
    """Stands in for vanh-server: the addon's KbMsg requests become keys of the compositor."""

    MSG = struct.Struct("=b7xQIII")  # KbOp op, size_t count, preDelay, interval, postDelay

    def __init__(self, sock_dir, kb):
        self.kb = kb
        self.requests = []
        ns = sock_dir / os.environ.get("USER", "cong")
        ns.mkdir(parents=True, exist_ok=True)
        os.chmod(ns, 0o700)
        self.socks = []
        for name, handler in (("kb.sock", self._serve_kb), ("mouse.sock", None)):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            path = ns / name
            if path.exists():
                path.unlink()
            s.bind(str(path))
            s.listen(4)
            self.socks.append(s)
            threading.Thread(target=self._accept, args=(s, handler), daemon=True).start()

    def _accept(self, s, handler):
        while True:
            conn, _ = s.accept()
            if handler:
                threading.Thread(target=handler, args=(conn,), daemon=True).start()
            else:
                self.socks.append(conn)  # mouse: kept open, never written to

    def _serve_kb(self, conn):
        while True:
            data = conn.recv(64)
            if not data:
                return
            if len(data) < self.MSG.size:
                data = data + bytes(self.MSG.size - len(data))
            op, count, pre, interval, post = self.MSG.unpack(data[: self.MSG.size])
            self.requests.append((op, count))
            if op == 2:  # Cancel
                continue
            time.sleep(pre / 1000)
            if op == 0:
                for _ in range(count):
                    self.kb.tap(BACKSPACE)
                    time.sleep(interval / 1000)
            elif op == 1:
                for _ in range(count):
                    self.kb.tap(LEFT, (SHIFT,))
                    time.sleep(interval / 1000)
                self.kb.tap(DELETE)
            elif op == 3:  # Return
                for _ in range(min(count, 4)):
                    self.kb.tap(ENTER)
            time.sleep(post / 1000)
            self.kb.tap(PAUSE)


CONFIG = "user"
MODES = {"smooth": "Uinput (Smooth)", "surrounding": "Surrounding Text", "preedit": "Preedit", "select": "Uinput (Select)"}


def write_fcitx_config(cfg, mode, user_conf):
    fc = cfg / "fcitx5"
    (fc / "conf").mkdir(parents=True, exist_ok=True)
    (fc / "profile").write_text(
        "[Groups/0]\nName=Default\nDefault Layout=us\nDefaultIM=vanh\n\n[Groups/0/Items/0]\nName=keyboard-us\nLayout=\n\n"
        "[Groups/0/Items/1]\nName=vanh\nLayout=\n\n[GroupOrder]\n0=Default\n"
    )
    (fc / "config").write_text("[Hotkey/TriggerKeys]\n0=Control+space\n[Behavior]\nActiveByDefault=True\nShareInputState=No\n")
    text = user_conf
    lines = [l for l in text.splitlines() if not l.startswith("Mode=")]
    lines.insert(0, f'Mode="{MODES[mode]}"')
    (fc / "conf/vanh.conf").write_text("\n".join(lines) + "\n")


def expected(keys, config):
    out = subprocess.run([str(REPLAY), "type", "--config", config, "--raw", keys], capture_output=True, text=True, check=True).stdout
    return out.split(" -> ", 1)[1].rstrip("\n")


def telex_keys(sentences):
    corpus = RUN / "e2e-corpus.tsv"
    corpus.write_text("# split\tsource\ttier\tid\tlabels\ttext\n" + "".join(f"test\te2e\ttyped\t{i}\t{'e' * len(s.split(' '))}\t{s}\n" for i, s in enumerate(sentences)))
    subprocess.run([str(REPLAY), "keys", str(corpus), str(RUN / "e2e-keys.txt")], check=True, capture_output=True)
    return (RUN / "e2e-keys.txt").read_text().split("\n")[: len(sentences)]


class Session:
    def __init__(self, args):
        self.args = args
        self.procs = []
        if RUN.exists():
            shutil.rmtree(RUN)
        RUN.mkdir(parents=True)
        os.chmod(RUN, 0o700)
        self.env = dict(os.environ)
        for k in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "GNOME_SETUP_DISPLAY"):
            self.env.pop(k, None)
        # Nothing may reach the desktop session: drop every pointer into its runtime dir
        # (keyring, ssh agent, pipewire, ...).
        for k in [k for k, v in self.env.items() if "/run/user/" in v and k != "DBUS_SESSION_BUS_ADDRESS"]:
            self.env.pop(k)
        self.env.update(
            XDG_RUNTIME_DIR=str(RUN / "xdg"),
            XDG_CONFIG_HOME=str(RUN / "config"),
            XDG_DATA_HOME=str(RUN / "data"),
            XDG_CACHE_HOME=str(RUN / "cache"),
            XDG_STATE_HOME=str(RUN / "state"),
            XDG_DATA_DIRS=f"{RUN / 'share'}:/usr/local/share:/usr/share",
            FCITX_ADDON_DIRS=f"{BUILD / 'src'}:/usr/lib/x86_64-linux-gnu/fcitx5",
            VANH_SOCKET_DIR=str(RUN / "s"),
            VANH_SERVER_UID=str(os.getuid()),
            GTK_IM_MODULE="fcitx",
            QT_IM_MODULE="fcitx",
            XMODIFIERS="@im=fcitx",
            NO_AT_BRIDGE="0",
            GTK_A11Y="atspi",
        )
        for d in ("xdg", "config", "data", "cache", "state", "share/fcitx5/addon", "share/fcitx5/inputmethod"):
            (RUN / d).mkdir(parents=True, exist_ok=True)
        os.chmod(RUN / "xdg", 0o700)
        shutil.copy(BUILD / "src/vanh-addon.conf", RUN / "share/fcitx5/addon/vanh.conf")
        shutil.copy(BUILD / "src/vanh.conf", RUN / "share/fcitx5/inputmethod/vanh.conf")

    def spawn(self, cmd, log, **env):
        e = dict(self.env, **env)
        p = subprocess.Popen(cmd, env=e, stdout=open(RUN / log, "w"), stderr=subprocess.STDOUT, start_new_session=True)
        self.procs.append(p)
        return p

    def start_shell(self):
        self.spawn(["gnome-shell", "--headless", "--wayland", "--no-x11", "--wayland-display=wl-e2e", "--virtual-monitor", "1600x1000"], "shell.log")
        sock = RUN / "xdg/wl-e2e"
        for _ in range(100):
            if sock.exists():
                break
            time.sleep(0.1)
        else:
            raise SystemExit("gnome-shell did not start (see shell.log)")
        self.env["WAYLAND_DISPLAY"] = "wl-e2e"
        time.sleep(2)
        self.kb = Keyboard()
        self.server = FakeUinputServer(RUN / "s", self.kb)

    def start_fcitx(self, mode, user_conf):
        write_fcitx_config(RUN / "config", mode, user_conf)
        self.fcitx = self.spawn(["fcitx5", "--verbose=vanh=5"], f"fcitx5-{mode}.log")
        time.sleep(2.5)

    def stop_fcitx(self):
        self.fcitx.terminate()
        self.fcitx.wait(5)

    def stop(self):
        for p in reversed(self.procs):
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        time.sleep(0.5)
        for p in self.procs:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


# ---------------------------------------------------------------- Ghostty

def ghostty_cases(s, mode, cases, delay, results):
    hist = RUN / f"hist-{mode}"
    rc = RUN / "bashrc"
    rc.write_text(f"PS1='$ '\nHISTFILE={hist}\nHISTCONTROL=\nshopt -s histappend\nPROMPT_COMMAND='history -a'\nunset HISTIGNORE\n")
    pwfile = RUN / f"pw-{mode}-{delay}"
    pwscript = RUN / "pw.sh"
    pwscript.write_text(f"#!/bin/bash\nwhile true; do read -rs -p '[sudo] password for cong: ' pw; printf '%s\\n' \"$pw\" >> {pwfile}; echo; done\n")
    pwscript.chmod(0o755)
    conf = RUN / "ghostty.conf"
    conf.write_text("gtk-single-instance = false\nconfirm-close-surface = false\nwindow-width = 120\nwindow-height = 30\n")
    term = s.spawn(["ghostty", "--config-default-files=false", f"--config-file={conf}", "-e", "bash", "--rcfile", str(rc), "-i"], f"ghostty-{mode}.log")
    time.sleep(3)
    s.kb.type("x", 0.05)  # warm-up: the first key after the window maps can come before its IM
    s.kb.tap(22, (CTRL,))  # Ctrl+U: clears the line
    time.sleep(0.3)
    hist.write_text("")
    for name, keys, want_shell in cases:
        s.kb.type("# ", 0.03)  # a comment line: saved in the history, never run
        s.kb.type(keys, delay)
        s.kb.type("\n", 0)
        time.sleep(0.8)
        got = hist.read_text().splitlines()
        line = got[-1][2:] if got and got[-1].startswith("# ") else (got[-1] if got else "")
        results.append(("Ghostty", mode, "shell prompt", name, delay, want_shell, line))
    # Focus change mid-sentence: open and close a second terminal window while typing.
    name, keys, want = cases[0]
    half = keys.index(" ", len(keys) // 2)
    s.kb.type("# ", 0.03)
    s.kb.type(keys[:half], delay)
    other = s.spawn(["ghostty", "--config-default-files=false", f"--config-file={conf}", "-e", "sleep", "2"], "ghostty-other.log")
    time.sleep(3.5)  # it takes focus, exits, focus comes back
    s.kb.type(keys[half:], delay)
    s.kb.type("\n", 0)
    time.sleep(0.8)
    got = hist.read_text().splitlines()
    line = got[-1][2:] if got and got[-1].startswith("# ") else (got[-1] if got else "")
    results.append(("Ghostty", mode, "shell prompt, focus change", name, delay, want, line))
    other.wait(5)
    # Shortcut: Ctrl+W (bash: delete the word before the cursor) right after a Vietnamese word.
    s.kb.type("# tieengs Vieetj", 0.03)
    s.kb.tap(17, (CTRL,))
    s.kb.type("\n", 0)
    time.sleep(0.8)
    got = hist.read_text().splitlines()
    results.append(("Ghostty", mode, "shortcut Ctrl+W", "tiếng Việt + Ctrl+W", 0.03, "# tiếng ", got[-1] if got else ""))
    term.terminate()
    time.sleep(0.5)
    # Password prompt (read -s, like sudo): the terminal gives no sign that it is a password.
    pw = s.spawn(["ghostty", "--config-default-files=false", f"--config-file={conf}", "-e", str(pwscript)], f"ghostty-pw-{mode}.log")
    time.sleep(3)
    s.kb.type("dduowcj123\n", 0.03)
    s.kb.type("Hunter2pass\n", 0.03)
    time.sleep(0.8)
    got = pwfile.read_text().splitlines() if pwfile.exists() else []
    # Expected: the keys as typed (a password must never change); the engine's own output is
    # noted in the case name.
    for i, keys in enumerate(("dduowcj123", "Hunter2pass")):
        results.append(("Ghostty", mode, "password prompt", f"{keys} (engine: {expected(keys, CONFIG)})", 0.03, keys, got[i] if len(got) > i else ""))
    pw.terminate()
    time.sleep(0.5)


# ---------------------------------------------------------------- Brave

PAGE = """<!doctype html><meta charset=utf-8><title>vanh e2e</title>
<style>body{font:16px sans-serif} input,textarea,div{display:block;width:900px;margin:12px}</style>
<input id=input><input id=search type=search><textarea id=textarea rows=4></textarea>
<div id=editable contenteditable style="border:1px solid #888;min-height:60px"></div>
"""


class Cdp:
    def __init__(self, port, url):
        import websocket

        page = None
        for _ in range(100):
            try:
                tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
                page = next(t for t in tabs if t["type"] == "page")
                break
            except Exception:
                time.sleep(0.2)
        if page is None:
            raise RuntimeError("no Brave tab over CDP")
        self.ws = websocket.create_connection(page["webSocketDebuggerUrl"], suppress_origin=True)
        self.n = 0
        if not page["url"].startswith("file://"):  # Brave Origin opens its startup page first
            self.call("Page.navigate", {"url": url})
            time.sleep(2)

    def call(self, method, params):
        self.n += 1
        self.ws.send(json.dumps({"id": self.n, "method": method, "params": params}))
        while True:
            r = json.loads(self.ws.recv())
            if r.get("id") == self.n:
                return r.get("result", {})

    def eval(self, expr):
        self.n += 1
        self.ws.send(json.dumps({"id": self.n, "method": "Runtime.evaluate", "params": {"expression": expr, "returnByValue": True}}))
        while True:
            r = json.loads(self.ws.recv())
            if r.get("id") == self.n:
                return r["result"]["result"].get("value")


def brave_cases(s, mode, cases, delay, results):
    page = RUN / "page.html"
    page.write_text(PAGE)
    port = 9333
    prof = RUN / f"brave-{mode}"
    brave = s.spawn(
        ["/opt/brave.com/brave-origin/brave", f"--user-data-dir={prof}", "--no-first-run", "--no-default-browser-check", "--password-store=basic", "--force-renderer-accessibility",
         "--proxy-server=127.0.0.1:9", "--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE 127.0.0.1", "--disable-background-networking",
         "--disable-component-update", "--disable-sync", "--disable-features=BraveRewards,BraveWallet", f"--remote-debugging-port={port}",
         "--remote-allow-origins=*", f"file://{page}"],
        f"brave-{mode}.log",
    )
    time.sleep(6)
    cdp = Cdp(port, f"file://{page}")
    cdp.call("Page.bringToFront", {})  # a new window may open without the keyboard focus
    time.sleep(0.5)
    cdp.call("Browser.grantPermissions", {"permissions": ["clipboardReadWrite", "clipboardSanitizedWrite"]})
    cdp.eval("document.getElementById('input').focus()")
    s.kb.type("x", 0.05)  # warm-up, as in the terminal
    time.sleep(0.3)
    def field_text(field):
        return (cdp.eval(f"(e=>e.value!==undefined?e.value:e.innerText)(document.getElementById('{field}'))") or "").rstrip("\n")

    for field in ("input", "search", "textarea", "editable"):
        multiline = field in ("textarea", "editable")
        for name, keys, want in cases:
            cdp.eval(f"(e=>{{e.focus(); if(e.value!==undefined) e.value=''; else e.textContent='';}})(document.getElementById('{field}'))")
            time.sleep(0.3)
            s.kb.type(keys, delay)
            # Enter in a multi-line field; a single-line field ends the word with a space.
            s.kb.type("\n" if multiline else " ", 0)
            time.sleep(0.8)
            w = want if multiline else expected(keys + " ", CONFIG)
            results.append(("Brave", mode, field, name, delay, w, field_text(field)))
    # Focus change mid-sentence: half in one field, Tab away and back, then the rest.
    name, keys, want = cases[0]
    half = keys.index(" ", len(keys) // 2)
    cdp.eval("(e=>{e.focus();e.value='';})(document.getElementById('input'))")
    time.sleep(0.3)
    s.kb.type(keys[:half], delay)
    s.kb.tap(15)  # Tab
    time.sleep(0.3)
    s.kb.tap(15, (SHIFT,))
    time.sleep(0.3)
    s.kb.tap(107)  # End: keyboard focus selects the whole field, the cursor goes back to its end
    time.sleep(0.1)
    s.kb.type(keys[half:] + " ", delay)
    time.sleep(0.8)
    results.append(("Brave", mode, "input, focus change (Tab, Shift+Tab)", name, delay, expected(keys + " ", CONFIG), field_text("input")))
    # Address bar (Ctrl+L), read back through AT-SPI; never submitted (Escape clears it).
    for name, keys, want in cases[:4]:
        s.kb.tap(38, (CTRL,))  # Ctrl+L
        time.sleep(0.5)
        s.kb.type(keys, delay)
        s.kb.type(" ", 0)
        time.sleep(0.8)
        s.kb.tap(30, (CTRL,))  # Ctrl+A
        s.kb.tap(46, (CTRL,))  # Ctrl+C
        time.sleep(0.3)
        for _ in range(3):
            s.kb.tap(1)  # Escape: the omnibox text is dropped, never submitted
        time.sleep(0.3)
        cdp.call("Page.bringToFront", {})
        cdp.eval("document.getElementById('input').focus()")
        time.sleep(0.3)
        got = cdp.call("Runtime.evaluate", {"expression": "navigator.clipboard.readText()", "awaitPromise": True, "returnByValue": True, "userGesture": True})
        got = got.get("result", {}).get("value")
        results.append(("Brave", mode, "address bar", name, delay, expected(keys + " ", CONFIG), got if isinstance(got, str) else "<unreadable>"))
    brave.terminate()
    time.sleep(1)


def omnibox_text():
    """The text of the focused entry of Brave's window (AT-SPI), None when not found."""
    try:
        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi

        desktop = Atspi.get_desktop(0)
        stack = [desktop.get_child_at_index(i) for i in range(desktop.get_child_count())]
        stack = [a for a in stack if a and "brave" in (a.get_name() or "").lower() or (a and "chrom" in (a.get_name() or "").lower())]
        seen = 0
        while stack and seen < 20000:
            node = stack.pop()
            seen += 1
            if node is None:
                continue
            try:
                states = node.get_state_set()
                if states.contains(Atspi.StateType.FOCUSED) and states.contains(Atspi.StateType.EDITABLE):
                    t = node.get_text_iface()
                    if t is not None:
                        return t.get_text(0, t.get_character_count())
                for i in range(min(node.get_child_count(), 200)):
                    stack.append(node.get_child_at_index(i))
            except Exception:
                continue
    except Exception as e:  # no AT-SPI
        print("atspi:", e, file=sys.stderr)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apps", default="ghostty,brave")
    ap.add_argument("--modes", default="smooth,surrounding")
    ap.add_argument("--delays", default="0.03,0.005")
    ap.add_argument("--config", default="user", help="engine options: user (your vanh.conf) or default")
    ap.add_argument("--out", default=str(RUN.parent / "ve2e-results.json"))
    a = ap.parse_args()
    if "DBUS_SESSION_BUS_ADDRESS" not in os.environ or "dbus-run-session" not in open(f"/proc/{os.getppid()}/cmdline").read():
        raise SystemExit("run inside dbus-run-session (a private bus), see the docstring")
    global CONFIG
    CONFIG = a.config
    user_conf = (Path.home() / ".config/fcitx5/conf/vanh.conf").read_text() if a.config == "user" else ""
    keys = telex_keys(SENTENCES)
    s = Session(a)
    results = []
    try:
        s.start_shell()
        for mode in a.modes.split(","):
            s.start_fcitx(mode, user_conf)
            cases = [(t, k, expected(k, a.config)) for t, k in zip(SENTENCES, keys)]
            for t, k, w in cases:
                assert expected("# " + k, a.config) == "# " + w, (t, w)  # the comment prefix changes nothing
            for delay in (float(d) for d in a.delays.split(",")):
                if "ghostty" in a.apps:
                    ghostty_cases(s, mode, cases, delay, results)
                if "brave" in a.apps:
                    brave_cases(s, mode, cases, delay, results)
            s.stop_fcitx()
    except Exception:
        import traceback

        traceback.print_exc()
    finally:
        s.stop()
        Path(a.out).write_text(json.dumps({"results": results, "server_requests": s.server.requests[:50] if hasattr(s, "server") else []}, ensure_ascii=False, indent=1))
    bad = [r for r in results if r[5] != r[6]]
    for r in results:
        print(("ok " if r[5] == r[6] else "BAD"), *r[:5], repr(r[5]), "->", repr(r[6]), sep=" | ")
    print(f"{len(results) - len(bad)}/{len(results)} ok")


if __name__ == "__main__":
    main()
