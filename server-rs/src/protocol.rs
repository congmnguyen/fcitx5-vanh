// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Keyboard socket wire format (port of common/common-protocol.h).
//!
//! The addon sends one `KbMsg` per `SOCK_SEQPACKET` packet, in native byte order and native C
//! layout:
//!
//! ```c
//! struct KbMsg { int8_t op; size_t count; uint32_t preDelay; uint32_t interval; uint32_t postDelay; };
//! ```
//!
//! 32 bytes on LP64 (op at 0, count at 8, the delays at 16/20/24, 4 bytes tail padding), 20 bytes
//! on ILP32. The server also accepts a 4-byte packet (`sizeof(int32_t)`): its first byte is the
//! op and every other field reads as zero, as in the C++ server.

use std::mem::{offset_of, size_of};

/// Mirror of the C++ `KbMsg`, used for its size and field offsets only.
#[repr(C)]
#[allow(dead_code)]
struct WireKbMsg {
    op: i8,
    count: usize,
    pre_delay: u32,
    interval: u32,
    post_delay: u32,
}

/// `sizeof(KbMsg)`.
pub const KB_MSG_SIZE: usize = size_of::<WireKbMsg>();
/// `sizeof(int32_t)`, the short packet the server also accepts.
pub const SHORT_MSG_SIZE: usize = size_of::<i32>();

#[cfg(target_pointer_width = "64")]
const _: () = {
    assert!(KB_MSG_SIZE == 32);
    assert!(offset_of!(WireKbMsg, count) == 8);
    assert!(offset_of!(WireKbMsg, pre_delay) == 16);
    assert!(offset_of!(WireKbMsg, interval) == 20);
    assert!(offset_of!(WireKbMsg, post_delay) == 24);
};

/// Operation requested by the addon (C++ `KbOp`, an `int8_t`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KbOp {
    /// Emit `count` BackSpace taps, then the anchor key.
    Backspace,
    /// Select `count` characters with Shift+Left, then Delete, then the anchor key.
    Select,
    /// Drop the keys still pending from the in-flight request and release Shift.
    Cancel,
    /// Emit `count` Return taps (at most 4), then the anchor key: Enter typed during a
    /// replacement, sent after its commit as a real key (GTK4 apps drop keys the addon forwards).
    Return,
    /// Any other byte; ignored (logged when `count > 0`).
    Unknown(i8),
}

impl KbOp {
    pub fn from_raw(raw: i8) -> Self {
        match raw {
            0 => KbOp::Backspace,
            1 => KbOp::Select,
            2 => KbOp::Cancel,
            3 => KbOp::Return,
            other => KbOp::Unknown(other),
        }
    }

    pub fn raw(self) -> i8 {
        match self {
            KbOp::Backspace => 0,
            KbOp::Select => 1,
            KbOp::Cancel => 2,
            KbOp::Return => 3,
            KbOp::Unknown(raw) => raw,
        }
    }
}

/// One decoded keyboard request.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KbMsg {
    pub op: KbOp,
    /// Characters to delete/select (autofill included).
    pub count: usize,
    /// ms to wait before sending any key.
    pub pre_delay: u32,
    /// ms between consecutive injected keys.
    pub interval: u32,
    /// ms to wait before the final anchor key.
    pub post_delay: u32,
}

fn read_u32(buf: &[u8; KB_MSG_SIZE], offset: usize) -> u32 {
    u32::from_ne_bytes(buf[offset..offset + 4].try_into().expect("4 bytes"))
}

/// Decodes a packet of `len` bytes received into `buf` (zeroed before the `recv()`).
///
/// Returns `None` for a malformed length (anything but [`KB_MSG_SIZE`] or [`SHORT_MSG_SIZE`]).
pub fn decode(buf: &[u8; KB_MSG_SIZE], len: usize) -> Option<KbMsg> {
    if len != KB_MSG_SIZE && len != SHORT_MSG_SIZE {
        return None;
    }
    let count_at = offset_of!(WireKbMsg, count);
    let count = usize::from_ne_bytes(buf[count_at..count_at + size_of::<usize>()].try_into().expect("usize bytes"));
    Some(KbMsg {
        op: KbOp::from_raw(buf[offset_of!(WireKbMsg, op)] as i8),
        count,
        pre_delay: read_u32(buf, offset_of!(WireKbMsg, pre_delay)),
        interval: read_u32(buf, offset_of!(WireKbMsg, interval)),
        post_delay: read_u32(buf, offset_of!(WireKbMsg, post_delay)),
    })
}

/// Encodes a request exactly as the C++ `KbSender` sends it (padding zeroed).
pub fn encode(msg: &KbMsg) -> [u8; KB_MSG_SIZE] {
    let mut buf = [0u8; KB_MSG_SIZE];
    buf[offset_of!(WireKbMsg, op)] = msg.op.raw() as u8;
    let count_at = offset_of!(WireKbMsg, count);
    buf[count_at..count_at + size_of::<usize>()].copy_from_slice(&msg.count.to_ne_bytes());
    for (offset, value) in [
        (offset_of!(WireKbMsg, pre_delay), msg.pre_delay),
        (offset_of!(WireKbMsg, interval), msg.interval),
        (offset_of!(WireKbMsg, post_delay), msg.post_delay),
    ] {
        buf[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
    }
    buf
}

#[cfg(test)]
mod tests {
    use super::*;

    fn msg(op: KbOp, count: usize, pre_delay: u32, interval: u32, post_delay: u32) -> KbMsg {
        KbMsg { op, count, pre_delay, interval, post_delay }
    }

    #[test]
    fn round_trip() {
        for original in [msg(KbOp::Backspace, 3, 0, 7, 0), msg(KbOp::Select, 5, 12, 9, 40), msg(KbOp::Cancel, 0, 0, 0, 0)] {
            assert_eq!(decode(&encode(&original), KB_MSG_SIZE), Some(original));
        }
    }

    #[cfg(all(target_pointer_width = "64", target_endian = "little"))]
    #[test]
    fn byte_layout_matches_the_cxx_struct() {
        // KbMsg{.op = Select, .count = 0x0102, .preDelay = 3, .interval = 4, .postDelay = 5}
        let mut expected = [0u8; 32];
        expected[0] = 1;
        expected[8] = 0x02;
        expected[9] = 0x01;
        expected[16] = 3;
        expected[20] = 4;
        expected[24] = 5;
        assert_eq!(encode(&msg(KbOp::Select, 0x0102, 3, 4, 5)), expected);
    }

    #[test]
    fn short_packet_reads_the_op_and_zeroes() {
        let mut buf = [0u8; KB_MSG_SIZE];
        buf[0] = 2;
        buf[1] = 0x7f; // part of the int32, but padding in KbMsg
        assert_eq!(decode(&buf, SHORT_MSG_SIZE), Some(msg(KbOp::Cancel, 0, 0, 0, 0)));
    }

    #[test]
    fn malformed_lengths() {
        let buf = [0u8; KB_MSG_SIZE];
        for len in [0, 1, 3, 5, 8, KB_MSG_SIZE - 1] {
            assert_eq!(decode(&buf, len), None, "len {len}");
        }
    }

    #[test]
    fn unknown_ops_keep_their_value() {
        assert_eq!(KbOp::from_raw(7), KbOp::Unknown(7));
        assert_eq!(KbOp::from_raw(-1), KbOp::Unknown(-1));
        let mut buf = [0u8; KB_MSG_SIZE];
        buf[0] = 0xff;
        assert_eq!(decode(&buf, KB_MSG_SIZE).unwrap().op, KbOp::Unknown(-1));
    }
}
