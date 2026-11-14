# Changelog

## Unreleased

- D-Bus frontends: a character typed while the previous commit was still on its way no longer
  lands before it ("ái" became "iá").
- A backspace that a GTK app forwards twice no longer ends a replacement early.
- The accuracy of the English/Vietnamese switch is measured on a public corpus
  (`engine-rs/eval/public.tsv`) on every build.

## 6.0.5 (2026-11-04)

- Surrounding text through GNOME Shell: edits are paced and wait for the app to show the text an
  edit deletes, so fast typing no longer garbles words ("được" became "đươcợc").
- A field switches to the uinput server when the app drops deletions (Apple Notes on the web,
  Messenger on facebook.com).
- Revert key: turns the last word to its other reading (English ↔ Vietnamese).
- Enter restores the word like a word break, sent through the uinput server.
- English restore: better context for developer text and for same-looking words after a clearly
  Vietnamese word; a word the Telex undo made is kept ("inss" stays "ins").
- Hardened IPC between the addon and the uinput server.
- New tray icons: a V badge, a grey E badge and a smiley.

## 6.0.4 (2026-10-31)

- Renamed the package, addon, input method, server and data paths from lotus to vanh; the old
  user configuration and service enablement are migrated.
- Detection of the address bar in Chromium-based browsers on X11.
- Fixed w2u in surrounding text mode and the restore of invalid words in uinput mode.

Earlier history is that of [fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus).
