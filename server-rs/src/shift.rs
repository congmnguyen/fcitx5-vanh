// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Tracks the Shift keys of the physical keyboards (port of server/lotus-physical-shift.{h,cpp}).
//!
//! A Select request is only honoured when every keyboard reports both Shift keys up: a Shift+Left
//! selection would fight a modifier the user holds, so a held or unknown Shift turns it into the
//! Backspace fallback.

use crate::log;
use crate::protocol::KbOp;
use crate::sys;
use crate::uinput::keys;
use std::fs::File;
use std::os::fd::AsFd;
use std::os::unix::fs::OpenOptionsExt;

/// Physical Shift state, distinguishing a known release from an unknown reading.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShiftState {
    Released,
    Held,
    /// `EVIOCGKEY` failed, no keyboard is tracked, or a keyboard cannot be read.
    Unknown,
}

/// Tests one key code inside an `EVIOCGKEY`/`EVIOCGBIT` bitmap.
pub fn key_bit_set(bits: &[u8], code: u16) -> bool {
    let code = usize::from(code);
    bits.get(code / 8).is_some_and(|byte| byte & (1 << (code % 8)) != 0)
}

/// Merges per-keyboard states: Held when any is Held; Unknown when none is given, an untracked
/// keyboard exists or any is Unknown; Released otherwise.
pub fn merge_shift_states(states: &[ShiftState], has_untracked: bool) -> ShiftState {
    if states.contains(&ShiftState::Held) {
        ShiftState::Held
    } else if states.is_empty() || has_untracked || states.contains(&ShiftState::Unknown) {
        ShiftState::Unknown
    } else {
        ShiftState::Released
    }
}

/// Select becomes Backspace unless Shift is confirmed released; other operations are unchanged.
pub fn resolve_keyboard_op(requested: KbOp, state: ShiftState) -> KbOp {
    if requested == KbOp::Select && state != ShiftState::Released {
        KbOp::Backspace
    } else {
        requested
    }
}

/// A keyboard whose live Shift state can be queried.
pub trait ShiftSource {
    /// `Some(true)` when a Shift key is down, `None` when the state cannot be read.
    fn shift_down(&mut self) -> Option<bool>;
}

/// Outcome of opening one device for Shift queries.
pub enum Probe<K> {
    /// The device has neither KEY_LEFTSHIFT nor KEY_RIGHTSHIFT: not a keyboard to track.
    NoShiftKeys,
    /// The device cannot be opened or queried: its Shift state is unknown.
    Unreadable,
    Keyboard(K),
}

/// The physical keyboards the server tracks.
pub struct ShiftTracker<K> {
    keyboards: Vec<(String, K)>,
    /// Keyboards whose Shift state cannot be tracked.
    unreadable: Vec<String>,
}

impl<K> Default for ShiftTracker<K> {
    fn default() -> Self {
        ShiftTracker { keyboards: Vec::new(), unreadable: Vec::new() }
    }
}

impl<K: ShiftSource> ShiftTracker<K> {
    /// Starts tracking `devnode` (a keyboard reported by libinput) unless it is tracked already.
    pub fn track(&mut self, devnode: &str, probe: impl FnOnce(&str) -> Probe<K>) {
        if devnode.is_empty() || self.keyboards.iter().any(|(node, _)| node == devnode) {
            return;
        }
        match probe(devnode) {
            Probe::NoShiftKeys => {}
            Probe::Unreadable => {
                if !self.unreadable.iter().any(|node| node == devnode) {
                    self.unreadable.push(devnode.to_owned());
                }
                log::warn(&format!("Not tracking Shift keys of {devnode}"));
            }
            Probe::Keyboard(keyboard) => {
                self.unreadable.retain(|node| node != devnode);
                self.keyboards.push((devnode.to_owned(), keyboard));
                log::info(&format!("Tracking Shift keys of {devnode}"));
            }
        }
    }

    /// Drops `devnode` (LIBINPUT_EVENT_DEVICE_REMOVED).
    pub fn forget(&mut self, devnode: &str) {
        if devnode.is_empty() {
            return;
        }
        self.keyboards.retain(|(node, _)| node != devnode);
        self.unreadable.retain(|node| node != devnode);
    }

    /// Aggregated live Shift state across the tracked keyboards.
    pub fn shift_state(&mut self) -> ShiftState {
        let states: Vec<ShiftState> = self
            .keyboards
            .iter_mut()
            .map(|(devnode, keyboard)| match keyboard.shift_down() {
                Some(true) => ShiftState::Held,
                Some(false) => ShiftState::Released,
                None => {
                    log::warn(&format!("Failed to read Shift key state of {devnode}"));
                    ShiftState::Unknown
                }
            })
            .collect();
        merge_shift_states(&states, !self.unreadable.is_empty())
    }
}

/// Bytes of a bitmap covering every key code (`(KEY_MAX + 7) / 8`).
const KEY_BITS_LEN: usize = (keys::KEY_MAX as usize).div_ceil(8);

/// A physical evdev keyboard, opened read-only (the server runs in group `input`).
pub struct EvdevKeyboard {
    file: File,
}

impl EvdevKeyboard {
    /// Opens `devnode` and checks that it can report a Shift key, like the C++ libevdev check.
    pub fn probe(devnode: &str) -> Probe<EvdevKeyboard> {
        let Ok(file) = File::options().read(true).custom_flags(sys::O_NONBLOCK).open(devnode)
        else {
            return Probe::Unreadable;
        };
        let mut types = [0u8; 4];
        let mut codes = [0u8; KEY_BITS_LEN];
        if sys::evdev_get_bits(file.as_fd(), 0, &mut types).is_err()
            || sys::evdev_get_bits(file.as_fd(), keys::EV_KEY, &mut codes).is_err()
        {
            return Probe::Unreadable;
        }
        let has_shift = key_bit_set(&types, keys::EV_KEY)
            && (key_bit_set(&codes, keys::LEFTSHIFT) || key_bit_set(&codes, keys::RIGHTSHIFT));
        if has_shift {
            Probe::Keyboard(EvdevKeyboard { file })
        } else {
            Probe::NoShiftKeys
        }
    }
}

impl ShiftSource for EvdevKeyboard {
    fn shift_down(&mut self) -> Option<bool> {
        let mut bits = [0u8; KEY_BITS_LEN];
        sys::evdev_get_key_state(self.file.as_fd(), &mut bits).ok()?;
        Some(key_bit_set(&bits, keys::LEFTSHIFT) || key_bit_set(&bits, keys::RIGHTSHIFT))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::log::{capture, Level};
    use std::cell::Cell;
    use std::rc::Rc;
    use ShiftState::{Held, Released, Unknown};

    #[test]
    fn resolve_falls_back_unless_released() {
        assert_eq!(resolve_keyboard_op(KbOp::Select, Held), KbOp::Backspace);
        assert_eq!(resolve_keyboard_op(KbOp::Select, Unknown), KbOp::Backspace);
        assert_eq!(resolve_keyboard_op(KbOp::Select, Released), KbOp::Select);
        for state in [Held, Unknown, Released] {
            assert_eq!(resolve_keyboard_op(KbOp::Backspace, state), KbOp::Backspace);
            assert_eq!(resolve_keyboard_op(KbOp::Cancel, state), KbOp::Cancel);
            assert_eq!(resolve_keyboard_op(KbOp::Unknown(9), state), KbOp::Unknown(9));
        }
    }

    #[test]
    fn key_bitmap_decoding() {
        let mut bits = [0u8; KEY_BITS_LEN];
        assert!(!key_bit_set(&bits, keys::LEFTSHIFT) && !key_bit_set(&bits, keys::RIGHTSHIFT));
        bits[usize::from(keys::LEFTSHIFT) / 8] |= 1 << (keys::LEFTSHIFT % 8);
        assert!(key_bit_set(&bits, keys::LEFTSHIFT) && !key_bit_set(&bits, keys::RIGHTSHIFT));
        bits[usize::from(keys::LEFTSHIFT) / 8] = 0;
        bits[usize::from(keys::RIGHTSHIFT) / 8] |= 1 << (keys::RIGHTSHIFT % 8);
        assert!(key_bit_set(&bits, keys::RIGHTSHIFT) && !key_bit_set(&bits, keys::LEFTSHIFT));
        assert!(!key_bit_set(&bits[..1], keys::KEY_MAX), "out of range reads as up");
    }

    #[test]
    fn merge_rules() {
        assert_eq!(merge_shift_states(&[], false), Unknown);
        assert_eq!(merge_shift_states(&[], true), Unknown);
        assert_eq!(merge_shift_states(&[Released, Released], false), Released);
        assert_eq!(merge_shift_states(&[Released, Unknown, Released], false), Unknown);
        assert_eq!(merge_shift_states(&[Held, Unknown, Released], false), Held);
        assert_eq!(merge_shift_states(&[Released, Released], true), Unknown);
        assert_eq!(merge_shift_states(&[Held], false), Held);
        assert_eq!(merge_shift_states(&[Unknown], false), Unknown);
        assert_eq!(merge_shift_states(&[Held, Released], true), Held);
        assert_eq!(merge_shift_states(&[Held], true), Held);
    }

    #[derive(Clone)]
    struct FakeKeyboard(Rc<Cell<Option<bool>>>);

    impl ShiftSource for FakeKeyboard {
        fn shift_down(&mut self) -> Option<bool> {
            self.0.get()
        }
    }

    #[test]
    fn tracker_bookkeeping() {
        let mut tracker = ShiftTracker::<FakeKeyboard>::default();
        assert_eq!(tracker.shift_state(), Unknown, "no keyboard: unknown");

        let first = Rc::new(Cell::new(Some(false)));
        tracker.track("/dev/input/event3", |_| Probe::Keyboard(FakeKeyboard(first.clone())));
        assert_eq!(tracker.shift_state(), Released);
        first.set(Some(true));
        assert_eq!(tracker.shift_state(), Held);
        first.set(None);
        assert_eq!(tracker.shift_state(), Unknown);
        assert!(capture::take().iter().any(|(level, line)| *level == Level::Warn
            && line == "Failed to read Shift key state of /dev/input/event3"));
        first.set(Some(false));

        // A duplicate add is not probed again.
        tracker.track("/dev/input/event3", |_| panic!("probed twice"));
        // A device without Shift keys is ignored.
        tracker.track("/dev/input/event4", |_| Probe::NoShiftKeys);
        assert_eq!(tracker.shift_state(), Released);

        // An unreadable keyboard poisons the state until it goes away or becomes readable.
        tracker.track("/dev/input/event5", |_| Probe::Unreadable);
        assert_eq!(tracker.shift_state(), Unknown);
        assert!(capture::take()
            .iter()
            .any(|(_, line)| line == "Not tracking Shift keys of /dev/input/event5"));
        let second = Rc::new(Cell::new(Some(false)));
        tracker.track("/dev/input/event5", |_| Probe::Keyboard(FakeKeyboard(second.clone())));
        assert_eq!(tracker.shift_state(), Released);
        tracker.forget("/dev/input/event5");
        tracker.track("/dev/input/event5", |_| Probe::Unreadable);
        assert_eq!(tracker.shift_state(), Unknown);
        tracker.forget("/dev/input/event5");
        assert_eq!(tracker.shift_state(), Released);

        tracker.forget("/dev/input/event3");
        assert_eq!(tracker.shift_state(), Unknown, "last keyboard gone");
        tracker.track("", |_| panic!("empty devnode probed"));
        tracker.forget("");
    }

    #[test]
    fn probing_a_missing_node_is_unreadable() {
        assert!(matches!(EvdevKeyboard::probe("/dev/input/no-such-vanh-node"), Probe::Unreadable));
    }

    #[test]
    fn probing_a_non_evdev_file_is_unreadable() {
        assert!(matches!(EvdevKeyboard::probe("/dev/null"), Probe::Unreadable));
    }
}
