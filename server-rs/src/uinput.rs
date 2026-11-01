// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The virtual keyboard the server injects keys through (port of server/lotus-uinput-device.* and
//! the `UinputDevice` class of server/lotus-server.cpp).
//!
//! The C++ server went through libevdev-uinput; this port issues the same ioctls itself
//! (`UI_SET_EVBIT`, `UI_SET_KEYBIT`, `UI_DEV_SETUP` with the `uinput_user_dev` fallback,
//! `UI_DEV_CREATE`), so the created device is identical: same name, ids, event types and keys.

use crate::log;
use crate::names;
use crate::sys;
use std::fs::File;
use std::io;
use std::os::fd::AsFd;
use std::path::PathBuf;

/// Linux event codes (`<linux/input-event-codes.h>`).
pub mod keys {
    pub const EV_SYN: u16 = 0x00;
    pub const EV_KEY: u16 = 0x01;
    pub const SYN_REPORT: u16 = 0;
    pub const BUS_USB: u16 = 0x03;

    pub const BACKSPACE: u16 = 14;
    pub const ENTER: u16 = 28;
    pub const LEFTSHIFT: u16 = 42;
    pub const RIGHTSHIFT: u16 = 54;
    pub const LEFT: u16 = 105;
    pub const DELETE: u16 = 111;
    /// The anchor key: the addon swallows it to learn that a burst is complete.
    pub const PAUSE: u16 = 119;
    pub const KEY_MAX: u16 = 0x2ff;
}

/// Description of a virtual keyboard.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeviceSpec {
    pub name: &'static str,
    pub id: sys::InputId,
    /// Event types enabled, in order (EV_SYN is implicit in libevdev and set explicitly here).
    pub event_types: &'static [u16],
    /// Keys the device can emit, in the order they are enabled.
    pub keys: &'static [u16],
}

/// The Vanh virtual keyboard, identical to the one the C++ server created.
pub fn lotus_keyboard() -> DeviceSpec {
    DeviceSpec {
        name: names::UINPUT_NAME,
        id: sys::InputId { bustype: keys::BUS_USB, vendor: names::UINPUT_VENDOR, product: names::UINPUT_PRODUCT, version: 0 },
        event_types: &[keys::EV_SYN, keys::EV_KEY],
        keys: &[keys::BACKSPACE, keys::LEFT, keys::LEFTSHIFT, keys::DELETE, keys::PAUSE, keys::ENTER],
    }
}

impl DeviceSpec {
    /// The `struct uinput_setup` passed to `UI_DEV_SETUP` (name truncated to 79 bytes + NUL).
    pub fn setup(&self) -> sys::UinputSetup {
        let mut name = [0u8; sys::UINPUT_MAX_NAME_SIZE];
        let bytes = self.name.as_bytes();
        let length = bytes.len().min(name.len() - 1);
        name[..length].copy_from_slice(&bytes[..length]);
        sys::UinputSetup { id: self.id, name, ff_effects_max: 0 }
    }
}

/// A created uinput keyboard; destroyed (`UI_DEV_DESTROY`) when dropped.
pub struct UinputDevice {
    file: File,
    devnode: Option<PathBuf>,
    write_failure_reported: bool,
}

impl UinputDevice {
    /// Opens `/dev/uinput` and creates the device.
    pub fn create(spec: &DeviceSpec) -> io::Result<UinputDevice> {
        let file = File::options().read(true).write(true).open(names::UINPUT_PATH)?;
        let fd = file.as_fd();
        for &kind in spec.event_types {
            sys::ui_set_evbit(fd, kind)?;
        }
        for &code in spec.keys {
            sys::ui_set_keybit(fd, code)?;
        }
        sys::ui_dev_setup(fd, &spec.setup())?;
        sys::ui_dev_create(fd)?;
        let devnode = sys::ui_get_sysname(fd).ok().and_then(|sysname| find_event_node(&sysname));
        Ok(UinputDevice { file, devnode, write_failure_reported: false })
    }

    /// `/dev/input/eventN` of the device, when sysfs names it.
    pub fn devnode(&self) -> Option<&PathBuf> {
        self.devnode.as_ref()
    }

    fn write(&mut self, kind: u16, code: u16, value: i32) -> bool {
        match sys::write_input_event(self.file.as_fd(), kind, code, value) {
            Ok(()) => true,
            Err(error) => {
                // Only the first failure: a broken device must not flood the log.
                if !self.write_failure_reported {
                    self.write_failure_reported = true;
                    log::error(&format!("Failed to write an uinput event: {}", sys::error_text(&error)));
                }
                false
            }
        }
    }

    /// Press, SYN, release, SYN.
    pub fn tap(&mut self, code: u16) {
        let _ = self.write(keys::EV_KEY, code, 1)
            && self.write(keys::EV_SYN, keys::SYN_REPORT, 0)
            && self.write(keys::EV_KEY, code, 0)
            && self.write(keys::EV_SYN, keys::SYN_REPORT, 0);
    }

    /// One key event (1 = press, 0 = release) followed by SYN.
    pub fn key(&mut self, code: u16, pressed: bool) {
        let _ = self.write(keys::EV_KEY, code, i32::from(pressed)) && self.write(keys::EV_SYN, keys::SYN_REPORT, 0);
    }
}

impl Drop for UinputDevice {
    fn drop(&mut self) {
        let _ = sys::ui_dev_destroy(self.file.as_fd());
    }
}

/// Finds `/dev/input/eventN` from `/sys/devices/virtual/input/<sysname>/eventN`, as libevdev does.
fn find_event_node(sysname: &[u8]) -> Option<PathBuf> {
    let sysname = std::str::from_utf8(sysname).ok()?;
    if sysname.is_empty() || sysname.contains('/') {
        return None;
    }
    let dir = std::fs::read_dir(format!("{}{sysname}", names::SYS_VIRTUAL_INPUT_DIR)).ok()?;
    dir.filter_map(Result::ok)
        .filter_map(|entry| entry.file_name().into_string().ok())
        .find(|name| name.starts_with("event"))
        .map(|name| PathBuf::from(format!("{}{name}", names::INPUT_DEV_DIR)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_identity_matches_the_cxx_server() {
        let spec = lotus_keyboard();
        assert_eq!(spec.name, "Vanh-Uinput-Server");
        assert_eq!(spec.id, sys::InputId { bustype: 0x03, vendor: 0x1234, product: 0x5678, version: 0 });
        assert_eq!(spec.event_types, &[keys::EV_SYN, keys::EV_KEY], "EV_KEY only (EV_SYN is implicit), no EV_REL");
        assert_eq!(spec.keys, &[14, 105, 42, 111, 119, 28], "BackSpace, Left, LeftShift, Delete, Pause, Enter");
        assert!(!spec.keys.contains(&30), "no unrelated key (KEY_A)");
        let setup = spec.setup();
        assert_eq!(&setup.name[..18], b"Vanh-Uinput-Server");
        assert!(setup.name[18..].iter().all(|&byte| byte == 0));
        assert_eq!(setup.ff_effects_max, 0);
    }

    #[test]
    fn long_names_are_truncated_with_a_terminator() {
        let spec = DeviceSpec { name: "x".repeat(100).leak(), ..lotus_keyboard() };
        let setup = spec.setup();
        assert_eq!(setup.name[78], b'x');
        assert_eq!(setup.name[79], 0);
    }

    #[test]
    fn sysname_lookup_rejects_odd_names() {
        assert_eq!(find_event_node(b""), None);
        assert_eq!(find_event_node(b"../../etc"), None);
        assert_eq!(find_event_node(b"no-such-vanh-input"), None);
    }

    /// Creates the real device. Opt-in (LOTUS_TEST_UINPUT=1) and skipped without write access to
    /// /dev/uinput, like the C++ device test it extends: it adds a short-lived keyboard to the
    /// running session, but never emits a key.
    #[test]
    fn create_real_device_when_allowed() {
        if std::env::var_os("LOTUS_TEST_UINPUT").is_none_or(|value| value != "1") {
            eprintln!("SKIP: set LOTUS_TEST_UINPUT=1 to create a real uinput device");
            return;
        }
        match UinputDevice::create(&lotus_keyboard()) {
            Ok(device) => {
                let node = device.devnode().expect("sysfs names the event node");
                assert!(node.to_string_lossy().starts_with("/dev/input/event"));
            }
            Err(error) => eprintln!("SKIP: cannot create a uinput device: {error}"),
        }
    }
}
