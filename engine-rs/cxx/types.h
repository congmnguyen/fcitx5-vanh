/*
 * SPDX-FileCopyrightText: 2018 Luong Thanh Lam <ltlam93@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file types.h
 * @brief Enumerations shared by the addon and the Rust engine (engine-rs/src/types.rs).
 *
 * The numeric values cross the C ABI, so they must stay in sync with the Rust enumerations.
 */

#ifndef _LOTUS_TYPES_H_
#define _LOTUS_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace lotus {

    /**
     * @brief A set of bit flags of one enumeration.
     * @tparam Flag Flag enumeration.
     */
    template <typename Flag>
    struct FlagSet {
        uint32_t bits = 0U;

        constexpr FlagSet() = default;

        // NOLINTNEXTLINE(google-explicit-constructor, cppcoreguidelines-explicit-constructor)
        constexpr FlagSet(Flag flag) : bits(static_cast<uint32_t>(flag)) {}

        explicit constexpr FlagSet(uint32_t mask) : bits(mask) {}

        [[nodiscard]] constexpr bool intersects(FlagSet other) const {
            return (bits & other.bits) != 0U;
        }
    };

    template <typename Flag>
    constexpr FlagSet<Flag> operator|(Flag first, Flag second) {
        return FlagSet<Flag>{static_cast<uint32_t>(first) | static_cast<uint32_t>(second)};
    }

    template <typename Flag>
    constexpr FlagSet<Flag> operator|(FlagSet<Flag> set, Flag flag) {
        return FlagSet<Flag>{set.bits | static_cast<uint32_t>(flag)};
    }

    /** @brief Modifier bits of a key event, the eight bits the processor reads. */
    enum class KeyState : uint32_t {
        Shift   = 1U << 0U,  ///< Shift (fcitx5 KeyState::Shift).
        Lock    = 1U << 1U,  ///< Caps Lock (fcitx5 KeyState::CapsLock).
        Ctrl    = 1U << 2U,  ///< Control (fcitx5 KeyState::Ctrl).
        Mod1    = 1U << 3U,  ///< Alt (fcitx5 KeyState::Alt / Mod1).
        Ignored = 1U << 25U, ///< fcitx5 KeyState::IgnoredMask, the Go forward mask.
        Super   = 1U << 26U, ///< Gtk virtual Super (fcitx5 KeyState::Super2).
        Hyper   = 1U << 27U, ///< Gtk virtual Hyper (fcitx5 KeyState::Hyper2).
        Meta    = 1U << 28U, ///< Meta (fcitx5 KeyState::Meta).
    };

    /** @brief A set of key state flags. */
    using KeyStates = FlagSet<KeyState>;

    /** @brief Modifier bits that make the processor reject a key event, like Go's isValidState. */
    inline constexpr KeyStates kInvalidStates = KeyState::Ctrl | KeyState::Mod1 |
        KeyState::Ignored | KeyState::Super | KeyState::Hyper | KeyState::Meta;

    /**
     * @brief Key states of a raw modifier mask.
     * @param bits Raw mask; unknown bits are kept.
     * @return Key state set holding those bits.
     */
    constexpr KeyStates keyStatesFromBits(uint32_t bits) {
        return KeyStates{bits};
    }

    /** @brief "w" to "ư" mode, shared with the addon configuration. */
    enum class W2uMode : uint8_t {
        Disabled   = 0,
        NonStart   = 1,
        Everywhere = 2,
    };

    /** @brief Bracket transform mode ("[" -> "ơ", "]" -> "ư"), shared with the addon config. */
    enum class BracketTransformMode : uint8_t {
        Disabled   = 0,
        NonStart   = 1,
        Everywhere = 2,
    };

    /** @brief Output charsets, in the order of the configuration (and of the Rust tables). */
    enum class Charset : std::uint8_t {
        Unicode,               ///< UTF-8, the identity charset
        Bhkcm1,                ///< "BKHCM 1"
        Bhkcm2,                ///< "BKHCM 2"
        NcrDecimal,            ///< "NCR Decimal"
        NcrHex,                ///< "NCR Hex"
        Tcvn3Abc,              ///< "TCVN3 (ABC)"
        Utf8,                  ///< "UTF-8"
        UnicodeCStringDecimal, ///< "Unicode C string Decimal"
        UnicodeCStringHex,     ///< "Unicode C string Hex"
        UnicodeCombining,      ///< "Unicode tổ hợp"
        Viqr,                  ///< "VIQR"
        Viscii,                ///< "VISCII"
        VniWindows,            ///< "VNI Windows"
        Vps,                   ///< "VPS"
        VietwareFull,          ///< "Vietware Full"
        VietwareX,             ///< "Vietware X"
        Windows1258,           ///< "Windows 1258 codepage"
    };

    inline constexpr std::size_t kCharsetCount = 17U;

    /** @brief Built-in input methods, in the order of the configuration (and the Rust tables). */
    enum class InputMethodId : std::uint8_t {
        Telex,           ///< "Telex"
        Vni,             ///< "VNI"
        TelexVni,        ///< "Telex + VNI"
        TelexVniViqr,    ///< "Telex + VNI + VIQR"
        Viqr,            ///< "VIQR"
        MicrosoftLayout, ///< "Microsoft layout"
        VniFrench,       ///< "VNI Bàn phím tiếng Pháp"
        Custom,          ///< "Custom"
    };

    inline constexpr std::size_t kInputMethodCount = 8U;

} // namespace lotus

#endif // _LOTUS_TYPES_H_
