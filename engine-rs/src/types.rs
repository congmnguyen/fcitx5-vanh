// SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Enumerations and bit flags shared by the engine, 1:1 with the Go package bamboo.

use std::ops::BitOr;

/// A set of typing modes (bit flags), 1:1 with Go's `Mode`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Mode(pub u32);

impl Mode {
    pub const VIETNAMESE: Mode = Mode(1 << 0);
    pub const ENGLISH: Mode = Mode(1 << 1);
    pub const TONE_LESS: Mode = Mode(1 << 2);
    pub const MARK_LESS: Mode = Mode(1 << 3);
    pub const LOWER_CASE: Mode = Mode(1 << 4);
    pub const FULL_TEXT: Mode = Mode(1 << 5);
    pub const PUNCTUATION: Mode = Mode(1 << 6);
    pub const IN_REVERSE_ORDER: Mode = Mode(1 << 7);

    pub const fn has(self, other: Mode) -> bool {
        self.0 & other.0 != 0
    }
}

impl BitOr for Mode {
    type Output = Mode;
    fn bitor(self, rhs: Mode) -> Mode {
        Mode(self.0 | rhs.0)
    }
}

/// Engine flags (bit flags), replacing the Go `E*` constants.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EngineFlags(pub u32);

impl EngineFlags {
    pub const FREE_TONE_MARKING: EngineFlags = EngineFlags(1 << 0);
    pub const STD_TONE_STYLE: EngineFlags = EngineFlags(1 << 1);
    pub const AUTO_CORRECT: EngineFlags = EngineFlags(1 << 2);
    pub const W2U: EngineFlags = EngineFlags(1 << 3);
    pub const BRACKET_TRANSFORM: EngineFlags = EngineFlags(1 << 4);
    /// Default engine flags, the Go `EstdFlags` set.
    pub const STD: EngineFlags = EngineFlags(0b1111);

    pub const fn has(self, flag: EngineFlags) -> bool {
        self.0 & flag.0 != 0
    }

    pub fn add(&mut self, flag: EngineFlags) {
        self.0 |= flag.0;
    }

    pub fn remove(&mut self, flag: EngineFlags) {
        self.0 &= !flag.0;
    }
}

impl BitOr for EngineFlags {
    type Output = EngineFlags;
    fn bitor(self, rhs: EngineFlags) -> EngineFlags {
        EngineFlags(self.0 | rhs.0)
    }
}

/// Modifier bits of a key event (the fcitx5 values).
pub mod key_state {
    pub const SHIFT: u32 = 1 << 0;
    pub const LOCK: u32 = 1 << 1;
    pub const CTRL: u32 = 1 << 2;
    pub const MOD1: u32 = 1 << 3;
    pub const IGNORED: u32 = 1 << 25;
    pub const SUPER: u32 = 1 << 26;
    pub const HYPER: u32 = 1 << 27;
    pub const META: u32 = 1 << 28;
    /// Modifier bits that make the processor reject a key event, like Go's isValidState.
    pub const INVALID: u32 = CTRL | MOD1 | IGNORED | SUPER | HYPER | META;
}

/// "w" to "ư" mode, shared with the addon configuration. `None` (in an `Option`) follows the
/// engine flags.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum W2uMode {
    Disabled = 0,
    NonStart = 1,
    Everywhere = 2,
}

/// Bracket transform mode, shared with the addon configuration. `None` follows the engine flags.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BracketTransformMode {
    Disabled = 0,
    NonStart = 1,
    Everywhere = 2,
}

/// Rule effect types, 1:1 with Go's EffectType.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EffectType {
    Appending,
    MarkTransformation,
    ToneTransformation,
    Replacing,
}

/// Marks, 1:1 with Go's Mark; the value is also the index inside a mark family row.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mark {
    None = 0,
    Hat = 1,
    Breve = 2,
    Horn = 3,
    Dash = 4,
    Raw = 5,
}

impl Mark {
    pub fn from_index(index: usize) -> Mark {
        match index {
            0 => Mark::None,
            1 => Mark::Hat,
            2 => Mark::Breve,
            3 => Mark::Horn,
            4 => Mark::Dash,
            _ => Mark::Raw,
        }
    }
}

/// Tones, 1:1 with Go's Tone; the value is also the index inside a vowel tone group.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tone {
    None = 0,
    Grave = 1,
    Acute = 2,
    Hook = 3,
    Tilde = 4,
    Dot = 5,
}

impl Tone {
    pub fn from_index(index: usize) -> Tone {
        match index {
            0 => Tone::None,
            1 => Tone::Grave,
            2 => Tone::Acute,
            3 => Tone::Hook,
            4 => Tone::Tilde,
            _ => Tone::Dot,
        }
    }
}

/// Output charsets, in the order of the addon configuration.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Charset {
    Unicode,
    Bhkcm1,
    Bhkcm2,
    NcrDecimal,
    NcrHex,
    Tcvn3Abc,
    Utf8,
    UnicodeCStringDecimal,
    UnicodeCStringHex,
    UnicodeCombining,
    Viqr,
    Viscii,
    VniWindows,
    Vps,
    VietwareFull,
    VietwareX,
    Windows1258,
}

pub const CHARSET_COUNT: usize = 17;

impl Charset {
    pub const ALL: [Charset; CHARSET_COUNT] = [
        Charset::Unicode,
        Charset::Bhkcm1,
        Charset::Bhkcm2,
        Charset::NcrDecimal,
        Charset::NcrHex,
        Charset::Tcvn3Abc,
        Charset::Utf8,
        Charset::UnicodeCStringDecimal,
        Charset::UnicodeCStringHex,
        Charset::UnicodeCombining,
        Charset::Viqr,
        Charset::Viscii,
        Charset::VniWindows,
        Charset::Vps,
        Charset::VietwareFull,
        Charset::VietwareX,
        Charset::Windows1258,
    ];

    pub fn from_index(index: usize) -> Option<Charset> {
        Self::ALL.get(index).copied()
    }
}

/// Built-in input methods, in the order of the addon configuration.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InputMethodId {
    Telex,
    Vni,
    TelexVni,
    TelexVniViqr,
    Viqr,
    MicrosoftLayout,
    VniFrench,
    Custom,
}

pub const INPUT_METHOD_COUNT: usize = 8;

impl InputMethodId {
    pub const ALL: [InputMethodId; INPUT_METHOD_COUNT] = [
        InputMethodId::Telex,
        InputMethodId::Vni,
        InputMethodId::TelexVni,
        InputMethodId::TelexVniViqr,
        InputMethodId::Viqr,
        InputMethodId::MicrosoftLayout,
        InputMethodId::VniFrench,
        InputMethodId::Custom,
    ];

    pub fn from_index(index: usize) -> Option<InputMethodId> {
        Self::ALL.get(index).copied()
    }
}
