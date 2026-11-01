// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Quang Minh <minhnbnt@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Key injection plan: the request state machine of the C++ server's main loop, separated from
//! the I/O so it can be unit tested.
//!
//! * The first key of a request is sent right away, after `preDelay` ms.
//! * The remaining keys are paced by the event loop: one per `interval` ms ([`Planner::tick`]).
//! * Backspace: `count` BackSpace taps, then `postDelay` ms, then the anchor key (Pause).
//! * Select: Shift down, `interval` ms, `count` Left taps, 10 ms, Shift up, Delete, `postDelay`
//!   ms, then the anchor key.
//! * Cancel: drop what is pending. A selection already started is completed and deleted (a
//!   half-selected word must not stay behind), and Shift is never left down.
//! * `interval` is clamped to 1..=1000 ms (0 means the default 5 ms); `preDelay` and
//!   `postDelay` to 0..=1000 ms, the range the addon sends (`clampDelay()` of
//!   src/lotus-state.cpp). The waits block the event loop, so an unclamped value from a confused
//!   or hostile client could freeze the server for up to 49 days.

use crate::log::Level;
use crate::protocol::{KbMsg, KbOp};
use crate::shift::{resolve_keyboard_op, ShiftState};
use crate::uinput::keys;

/// Default ms between injected keys when the request says 0.
pub const DEFAULT_INTERVAL_MS: u32 = 5;
/// Longest accepted interval.
pub const MAX_INTERVAL_MS: u32 = 1000;
/// Longest accepted pre/post delay (`kMaxDelayMs` of src/lotus-state.cpp).
pub const MAX_DELAY_MS: u32 = 1000;
/// Most Return taps one request may ask for.
const MAX_RETURNS: usize = 4;

/// Pause between the last Shift+Left and the Shift release.
pub const SHIFT_RELEASE_DELAY_MS: u32 = 10;

/// Where the plan's effects go: the uinput device and the clock in the server, a recorder in
/// tests.
pub trait Output {
    /// Press + release of `code`.
    fn tap(&mut self, code: u16);
    /// Press (`true`) or release of a held key.
    fn key(&mut self, code: u16, pressed: bool);
    /// Blocking wait.
    fn sleep_ms(&mut self, ms: u32);
    fn log(&mut self, level: Level, message: &str);
}

/// State of the in-flight request.
#[derive(Debug)]
pub struct Planner {
    pending_backspaces: usize,
    /// Remaining Shift+Left taps to pace out.
    pending_selects: usize,
    /// Anchor key still to be injected after the backspace burst.
    pending_anchor: bool,
    /// Shift release + Delete + anchor still to run after the Left burst.
    pending_select_anchor: bool,
    /// The server pressed KEY_LEFTSHIFT for an in-flight selection.
    shift_held: bool,
    post_delay: u32,
    interval: u32,
}

impl Default for Planner {
    fn default() -> Self {
        Planner {
            pending_backspaces: 0,
            pending_selects: 0,
            pending_anchor: false,
            pending_select_anchor: false,
            shift_held: false,
            post_delay: 0,
            interval: DEFAULT_INTERVAL_MS,
        }
    }
}

impl Planner {
    /// Whether keys are still to be paced out.
    pub fn has_pending(&self) -> bool {
        self.pending_backspaces > 0
            || self.pending_selects > 0
            || self.pending_anchor
            || self.pending_select_anchor
    }

    /// ms between two paced keys, `None` when nothing is pending (wait forever).
    pub fn tick_interval(&self) -> Option<u32> {
        self.has_pending().then_some(self.interval)
    }

    /// Whether the server holds Shift down.
    pub fn shift_held(&self) -> bool {
        self.shift_held
    }

    /// Sends the next paced key (the C++ `poll() == 0` branch).
    pub fn tick(&mut self, out: &mut impl Output) {
        if self.pending_backspaces > 0 {
            out.tap(keys::BACKSPACE);
            self.pending_backspaces -= 1;
        } else if self.pending_anchor {
            self.tap_anchor(out);
            self.pending_anchor = false;
        } else if self.pending_selects > 0 {
            out.tap(keys::LEFT);
            self.pending_selects -= 1;
        } else if self.pending_select_anchor && self.shift_held {
            out.sleep_ms(SHIFT_RELEASE_DELAY_MS);
            out.key(keys::LEFTSHIFT, false);
            self.shift_held = false;
        } else if self.pending_select_anchor {
            out.tap(keys::DELETE);
            self.tap_anchor(out);
            self.pending_select_anchor = false;
        }
    }

    /// The keyboard client went away: never leave Shift down, drop the selection.
    pub fn disconnected(&mut self, out: &mut impl Output) {
        self.drop_selection(out);
    }

    /// The server is exiting.
    pub fn shutdown(&mut self, out: &mut impl Output) {
        self.release_shift(out);
    }

    fn tap_anchor(&self, out: &mut impl Output) {
        if self.post_delay > 0 {
            out.sleep_ms(self.post_delay);
        }
        out.tap(keys::PAUSE);
    }

    fn release_shift(&mut self, out: &mut impl Output) {
        if self.shift_held {
            out.key(keys::LEFTSHIFT, false);
            self.shift_held = false;
        }
    }

    fn press_shift(&mut self, out: &mut impl Output) {
        if !self.shift_held {
            out.key(keys::LEFTSHIFT, true);
            self.shift_held = true;
        }
    }

    fn drop_selection(&mut self, out: &mut impl Output) {
        self.release_shift(out);
        self.pending_selects = 0;
        self.pending_select_anchor = false;
    }

    /// Handles one well-formed request. `shift` is the live physical Shift state.
    pub fn handle(&mut self, msg: &KbMsg, shift: ShiftState, out: &mut impl Output) {
        self.interval = match msg.interval {
            0 => DEFAULT_INTERVAL_MS,
            interval => interval.min(MAX_INTERVAL_MS),
        };
        let op = resolve_keyboard_op(msg.op, shift);
        if op != msg.op {
            out.log(
                Level::Info,
                if shift == ShiftState::Held {
                    "Shift is held: replacing with Backspace instead of Shift+Left"
                } else {
                    "Shift state unknown, using safe Backspace fallback instead of Shift+Left"
                },
            );
        }
        if op == KbOp::Cancel {
            self.cancel(out);
            return;
        }
        if msg.count == 0 {
            return;
        }
        if let KbOp::Unknown(raw) = op {
            out.log(Level::Warn, &format!("Unknown keyboard message op: {raw}"));
            return;
        }
        let pre_delay = msg.pre_delay.min(MAX_DELAY_MS);
        if pre_delay > 0 {
            out.sleep_ms(pre_delay);
        }
        self.post_delay = msg.post_delay.min(MAX_DELAY_MS);
        match op {
            KbOp::Select => {
                self.press_shift(out);
                out.sleep_ms(self.interval);
                out.tap(keys::LEFT);
                // A previous burst may still be draining.
                self.pending_selects = self.pending_selects.saturating_add(msg.count - 1);
                self.pending_select_anchor = true;
                self.pending_anchor = false;
            }
            KbOp::Backspace => {
                if self.shift_held {
                    self.drop_selection(out);
                }
                self.pending_anchor = true;
                self.pending_backspaces = self.pending_backspaces.saturating_add(msg.count - 1);
                out.tap(keys::BACKSPACE);
            }
            KbOp::Return => {
                // Shift+Return is another key in many apps.
                self.drop_selection(out);
                for _ in 0..msg.count.min(MAX_RETURNS) {
                    out.tap(keys::ENTER);
                }
                self.pending_anchor = true;
            }
            KbOp::Cancel | KbOp::Unknown(_) => unreachable!(),
        }
    }

    fn cancel(&mut self, out: &mut impl Output) {
        // Finish the burst instead of leaving a partial selection behind: the Select handler taps
        // its first Shift+Left before any later message (same socket, FIFO order), so a selection
        // already exists here.
        if self.pending_select_anchor {
            if self.pending_selects > 0 {
                self.press_shift(out);
                while self.pending_selects > 0 {
                    out.sleep_ms(self.interval);
                    out.tap(keys::LEFT);
                    self.pending_selects -= 1;
                }
            }
            self.release_shift(out);
            out.tap(keys::DELETE);
        } else {
            self.release_shift(out);
        }
        self.pending_backspaces = 0;
        self.pending_selects = 0;
        self.pending_anchor = false;
        self.pending_select_anchor = false;
    }
}

/// Recording [`Output`] for tests and the replay helper.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct Recorder {
    pub events: Vec<Event>,
}

/// One recorded effect.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
    Tap(u16),
    Down(u16),
    Up(u16),
    Sleep(u32),
    Log(Level, String),
}

impl Output for Recorder {
    fn tap(&mut self, code: u16) {
        self.events.push(Event::Tap(code));
    }
    fn key(&mut self, code: u16, pressed: bool) {
        self.events.push(if pressed { Event::Down(code) } else { Event::Up(code) });
    }
    fn sleep_ms(&mut self, ms: u32) {
        self.events.push(Event::Sleep(ms));
    }
    fn log(&mut self, level: Level, message: &str) {
        self.events.push(Event::Log(level, message.to_owned()));
    }
}

impl Recorder {
    /// The key effects only, without sleeps and logs.
    pub fn keys(&self) -> Vec<Event> {
        self.events
            .iter()
            .filter(|event| matches!(event, Event::Tap(_) | Event::Down(_) | Event::Up(_)))
            .cloned()
            .collect()
    }

    /// One line per effect (`tap 14`, `down 42`, `up 42`, `sleep 5`), the replay helper's format.
    pub fn transcript(&self) -> String {
        let mut text = String::new();
        for event in &self.events {
            match event {
                Event::Tap(code) => text.push_str(&format!("tap {code}\n")),
                Event::Down(code) => text.push_str(&format!("down {code}\n")),
                Event::Up(code) => text.push_str(&format!("up {code}\n")),
                Event::Sleep(ms) => text.push_str(&format!("sleep {ms}\n")),
                Event::Log(..) => {}
            }
        }
        text
    }
}

#[cfg(test)]
mod tests {
    use super::Event::{Down, Log, Sleep, Tap, Up};
    use super::*;
    use crate::shift::ShiftState::{Held, Released, Unknown};

    const BS: u16 = keys::BACKSPACE;
    const LEFT: u16 = keys::LEFT;
    const SHIFT: u16 = keys::LEFTSHIFT;
    const DEL: u16 = keys::DELETE;
    const PAUSE: u16 = keys::PAUSE;

    fn msg(op: KbOp, count: usize, pre_delay: u32, interval: u32, post_delay: u32) -> KbMsg {
        KbMsg { op, count, pre_delay, interval, post_delay }
    }

    fn drain(planner: &mut Planner, out: &mut Recorder) -> usize {
        let mut ticks = 0;
        while planner.has_pending() {
            planner.tick(out);
            ticks += 1;
            assert!(ticks < 10_000, "plan never finishes");
        }
        ticks
    }

    #[test]
    fn backspace_burst_then_anchor() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 3, 20, 7, 30), Released, &mut out);
        assert_eq!(out.events, vec![Sleep(20), Tap(BS)], "pre-delay, then the first key at once");
        assert_eq!(planner.tick_interval(), Some(7));
        out.events.clear();
        assert_eq!(drain(&mut planner, &mut out), 3);
        assert_eq!(out.events, vec![Tap(BS), Tap(BS), Sleep(30), Tap(PAUSE)]);
        assert_eq!(planner.tick_interval(), None);
    }

    #[test]
    fn zero_delays_send_no_sleep() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 1, 0, 0, 0), Released, &mut out);
        drain(&mut planner, &mut out);
        assert_eq!(out.events, vec![Tap(BS), Tap(PAUSE)]);
    }

    #[test]
    fn interval_defaults_and_clamps() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 2, 0, 0, 0), Released, &mut out);
        assert_eq!(planner.tick_interval(), Some(DEFAULT_INTERVAL_MS));
        planner.handle(&msg(KbOp::Backspace, 2, 0, 5000, 0), Released, &mut out);
        assert_eq!(planner.tick_interval(), Some(MAX_INTERVAL_MS));
        // Even a no-op message updates the pacing, as in the C++ server.
        planner.handle(&msg(KbOp::Backspace, 0, 0, 9, 0), Released, &mut out);
        assert_eq!(planner.tick_interval(), Some(9));
    }

    #[test]
    fn pre_and_post_delays_are_clamped() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 1, u32::MAX, 0, u32::MAX), Released, &mut out);
        drain(&mut planner, &mut out);
        assert_eq!(out.events, vec![Sleep(MAX_DELAY_MS), Tap(BS), Sleep(MAX_DELAY_MS), Tap(PAUSE)]);

        out.events.clear();
        planner.handle(&msg(KbOp::Select, 1, 1001, 3, 4_000_000), Released, &mut out);
        drain(&mut planner, &mut out);
        assert_eq!(out.events.first(), Some(&Sleep(MAX_DELAY_MS)));
        assert!(
            out.events.contains(&Sleep(MAX_DELAY_MS))
                && !out.events.iter().any(|event| matches!(event, Sleep(ms) if *ms > MAX_DELAY_MS)),
            "{:?}",
            out.events
        );

        // In-range values pass unchanged.
        out.events.clear();
        planner.handle(&msg(KbOp::Backspace, 1, MAX_DELAY_MS, 0, 1), Released, &mut out);
        drain(&mut planner, &mut out);
        assert_eq!(out.events, vec![Sleep(MAX_DELAY_MS), Tap(BS), Sleep(1), Tap(PAUSE)]);
    }

    #[test]
    fn select_burst() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Select, 3, 4, 6, 25), Released, &mut out);
        assert_eq!(out.events, vec![Sleep(4), Down(SHIFT), Sleep(6), Tap(LEFT)]);
        assert!(planner.shift_held());
        out.events.clear();
        drain(&mut planner, &mut out);
        assert_eq!(
            out.events,
            vec![
                Tap(LEFT),
                Tap(LEFT),
                Sleep(SHIFT_RELEASE_DELAY_MS),
                Up(SHIFT),
                Tap(DEL),
                Sleep(25),
                Tap(PAUSE)
            ]
        );
        assert!(!planner.shift_held());
    }

    #[test]
    fn select_with_held_or_unknown_shift_falls_back_to_backspace() {
        for (state, line) in [
            (Held, "Shift is held: replacing with Backspace instead of Shift+Left"),
            (Unknown, "Shift state unknown, using safe Backspace fallback instead of Shift+Left"),
        ] {
            let mut planner = Planner::default();
            let mut out = Recorder::default();
            planner.handle(&msg(KbOp::Select, 2, 0, 5, 0), state, &mut out);
            assert_eq!(out.events, vec![Log(Level::Info, line.to_owned()), Tap(BS)]);
            drain(&mut planner, &mut out);
            assert_eq!(out.keys(), vec![Tap(BS), Tap(BS), Tap(PAUSE)]);
            assert!(!planner.shift_held(), "never presses Shift against the user's modifier");
        }
    }

    #[test]
    fn cancel_mid_selection_completes_and_deletes() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Select, 4, 0, 3, 50), Released, &mut out);
        planner.tick(&mut out); // second Left
        out.events.clear();
        planner.handle(&msg(KbOp::Cancel, 0, 0, 0, 0), Released, &mut out);
        // Interval of the cancel message is 0, so the remaining Lefts use the default.
        assert_eq!(out.events, vec![Sleep(5), Tap(LEFT), Sleep(5), Tap(LEFT), Up(SHIFT), Tap(DEL)]);
        assert!(!planner.has_pending() && !planner.shift_held());
    }

    #[test]
    fn cancel_after_the_lefts_releases_and_deletes_without_anchor() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Select, 1, 0, 3, 0), Released, &mut out);
        out.events.clear();
        planner.handle(&msg(KbOp::Cancel, 0, 0, 3, 0), Released, &mut out);
        assert_eq!(out.events, vec![Up(SHIFT), Tap(DEL)]);
        // Shift already released by the tick: Delete only.
        let mut planner = Planner::default();
        planner.handle(&msg(KbOp::Select, 1, 0, 3, 0), Released, &mut out);
        planner.tick(&mut out);
        out.events.clear();
        planner.handle(&msg(KbOp::Cancel, 0, 0, 3, 0), Released, &mut out);
        assert_eq!(out.events, vec![Tap(DEL)]);
    }

    #[test]
    fn cancel_drops_a_backspace_burst_and_its_anchor() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 5, 0, 3, 0), Released, &mut out);
        planner.handle(&msg(KbOp::Cancel, 0, 0, 0, 0), Held, &mut out);
        assert_eq!(out.events, vec![Tap(BS)]);
        assert!(!planner.has_pending());
    }

    #[test]
    fn backspace_after_a_selection_releases_shift_first() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Select, 3, 0, 3, 0), Released, &mut out);
        out.events.clear();
        planner.handle(&msg(KbOp::Backspace, 2, 0, 3, 0), Released, &mut out);
        assert_eq!(out.events, vec![Up(SHIFT), Tap(BS)]);
        drain(&mut planner, &mut out);
        assert_eq!(
            out.keys(),
            vec![Up(SHIFT), Tap(BS), Tap(BS), Tap(PAUSE)],
            "the selection is dropped"
        );
    }

    #[test]
    fn select_while_a_backspace_burst_drains() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 2, 0, 3, 0), Released, &mut out);
        planner.handle(&msg(KbOp::Select, 2, 0, 3, 0), Released, &mut out);
        drain(&mut planner, &mut out);
        // The backspace anchor is replaced by the selection anchor; the BackSpace keeps draining first.
        assert_eq!(
            out.keys(),
            vec![
                Tap(BS),
                Down(SHIFT),
                Tap(LEFT),
                Tap(BS),
                Tap(LEFT),
                Up(SHIFT),
                Tap(DEL),
                Tap(PAUSE)
            ]
        );
    }

    #[test]
    fn disconnect_releases_shift_and_drops_the_selection() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Select, 3, 0, 3, 0), Released, &mut out);
        out.events.clear();
        planner.disconnected(&mut out);
        assert_eq!(out.events, vec![Up(SHIFT)]);
        assert!(!planner.has_pending());
    }

    #[test]
    fn disconnect_keeps_a_backspace_burst() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 2, 0, 3, 0), Released, &mut out);
        planner.disconnected(&mut out);
        assert!(
            planner.has_pending(),
            "as in the C++ server, BackSpace already promised still goes out"
        );
    }

    #[test]
    fn shutdown_releases_shift() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.shutdown(&mut out);
        assert!(out.events.is_empty());
        planner.handle(&msg(KbOp::Select, 2, 0, 3, 0), Released, &mut out);
        out.events.clear();
        planner.shutdown(&mut out);
        assert_eq!(out.events, vec![Up(SHIFT)]);
    }

    #[test]
    fn zero_count_and_unknown_ops() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 0, 100, 3, 100), Released, &mut out);
        planner.handle(&msg(KbOp::Select, 0, 100, 3, 100), Released, &mut out);
        planner.handle(&msg(KbOp::Unknown(7), 0, 0, 3, 0), Released, &mut out);
        assert!(out.events.is_empty());
        planner.handle(&msg(KbOp::Unknown(7), 2, 0, 3, 0), Released, &mut out);
        assert_eq!(out.events, vec![Log(Level::Warn, "Unknown keyboard message op: 7".to_owned())]);
        assert!(!planner.has_pending());
    }

    #[test]
    fn huge_counts_saturate() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, usize::MAX, 0, 3, 0), Released, &mut out);
        planner.handle(&msg(KbOp::Backspace, usize::MAX, 0, 3, 0), Released, &mut out);
        assert_eq!(planner.pending_backspaces, usize::MAX);
        planner.handle(&msg(KbOp::Cancel, 0, 0, 3, 0), Released, &mut out);
        assert!(!planner.has_pending());
    }

    #[test]
    fn transcript_format() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Backspace, 1, 0, 3, 2), Released, &mut out);
        drain(&mut planner, &mut out);
        assert_eq!(out.transcript(), "tap 14\nsleep 2\ntap 119\n");
    }

    #[test]
    fn return_taps_enter_then_the_anchor() {
        let mut planner = Planner::default();
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Return, 1, 0, 3, 2), Released, &mut out);
        drain(&mut planner, &mut out);
        assert_eq!(out.transcript(), "tap 28\nsleep 2\ntap 119\n");
        let mut out = Recorder::default();
        planner.handle(&msg(KbOp::Return, 99, 0, 3, 0), Released, &mut out);
        drain(&mut planner, &mut out);
        assert_eq!(out.keys().len(), MAX_RETURNS + 1, "capped, then the anchor");
    }
}
