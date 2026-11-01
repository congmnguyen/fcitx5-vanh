// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Minimal hand-written libinput/libudev binding (the `LibinputContext`, `LibinputEvent` and
//! `open_restricted`/`close_restricted` parts of server/lotus-server.{h,cpp}).
//!
//! Only the calls the server makes are declared. Ownership: [`Libinput`] owns one udev and one
//! libinput reference; [`Event`] owns one event; [`Device`] borrows the device of an event and
//! cannot outlive it. All `unsafe` code of the binding is in this file.

use crate::sys;
use std::ffi::{c_char, c_int, c_void, CStr};
use std::marker::PhantomData;
use std::os::fd::RawFd;

#[repr(C)]
struct udev {
    _opaque: [u8; 0],
}
#[repr(C)]
struct libinput {
    _opaque: [u8; 0],
}
#[repr(C)]
struct libinput_event {
    _opaque: [u8; 0],
}
#[repr(C)]
struct libinput_event_pointer {
    _opaque: [u8; 0],
}
#[repr(C)]
struct libinput_device {
    _opaque: [u8; 0],
}

/// `struct libinput_interface`.
#[repr(C)]
struct LibinputInterface {
    open_restricted:
        extern "C" fn(path: *const c_char, flags: c_int, user_data: *mut c_void) -> c_int,
    close_restricted: extern "C" fn(fd: c_int, user_data: *mut c_void),
}

const LIBINPUT_EVENT_DEVICE_ADDED: c_int = 1;
const LIBINPUT_EVENT_DEVICE_REMOVED: c_int = 2;
const LIBINPUT_EVENT_POINTER_BUTTON: c_int = 402;
const LIBINPUT_BUTTON_STATE_PRESSED: c_int = 1;
const LIBINPUT_DEVICE_CAP_KEYBOARD: c_int = 0;
const LIBINPUT_CONFIG_TAP_ENABLED: c_int = 1;
const LIBINPUT_CONFIG_TAP_MAP_LRM: c_int = 0;

#[link(name = "udev")]
extern "C" {
    fn udev_new() -> *mut udev;
    fn udev_unref(udev: *mut udev) -> *mut udev;
}

#[link(name = "input")]
extern "C" {
    fn libinput_udev_create_context(
        interface: *const LibinputInterface,
        user_data: *mut c_void,
        udev: *mut udev,
    ) -> *mut libinput;
    fn libinput_udev_assign_seat(libinput: *mut libinput, seat_id: *const c_char) -> c_int;
    fn libinput_unref(libinput: *mut libinput) -> *mut libinput;
    fn libinput_get_fd(libinput: *mut libinput) -> c_int;
    fn libinput_dispatch(libinput: *mut libinput) -> c_int;
    fn libinput_get_event(libinput: *mut libinput) -> *mut libinput_event;
    fn libinput_event_get_type(event: *mut libinput_event) -> c_int;
    fn libinput_event_destroy(event: *mut libinput_event);
    fn libinput_event_get_device(event: *mut libinput_event) -> *mut libinput_device;
    fn libinput_event_get_pointer_event(event: *mut libinput_event) -> *mut libinput_event_pointer;
    fn libinput_event_pointer_get_button_state(event: *mut libinput_event_pointer) -> c_int;
    fn libinput_device_get_name(device: *mut libinput_device) -> *const c_char;
    fn libinput_device_get_sysname(device: *mut libinput_device) -> *const c_char;
    fn libinput_device_has_capability(device: *mut libinput_device, capability: c_int) -> c_int;
    fn libinput_device_config_tap_get_finger_count(device: *mut libinput_device) -> c_int;
    fn libinput_device_config_tap_set_enabled(device: *mut libinput_device, enable: c_int)
        -> c_int;
    fn libinput_device_config_tap_set_button_map(device: *mut libinput_device, map: c_int)
        -> c_int;
}

extern "C" fn open_restricted(path: *const c_char, flags: c_int, _user_data: *mut c_void) -> c_int {
    if path.is_null() {
        return -22; // -EINVAL
    }
    // SAFETY: libinput passes a valid NUL-terminated device path.
    sys::open_raw(unsafe { CStr::from_ptr(path) }, flags)
}

extern "C" fn close_restricted(fd: c_int, _user_data: *mut c_void) {
    sys::close_raw(fd as RawFd);
}

static INTERFACE: LibinputInterface = LibinputInterface { open_restricted, close_restricted };

/// A libinput context on a udev seat.
pub struct Libinput {
    udev: *mut udev,
    li: *mut libinput,
}

impl Libinput {
    /// Creates the context and assigns `seat`; `None` when any step fails.
    pub fn new(seat: &CStr) -> Option<Libinput> {
        // SAFETY: udev_new() has no precondition; a null result is handled.
        let udev = unsafe { udev_new() };
        if udev.is_null() {
            return None;
        }
        // SAFETY: INTERFACE is 'static and udev a live reference (libinput takes its own).
        let li = unsafe { libinput_udev_create_context(&INTERFACE, std::ptr::null_mut(), udev) };
        let context = Libinput { udev, li };
        if li.is_null() {
            return None;
        }
        // SAFETY: li is live and seat NUL-terminated.
        if unsafe { libinput_udev_assign_seat(li, seat.as_ptr()) } != 0 {
            return None;
        }
        Some(context)
    }

    /// The epoll descriptor to poll for input.
    pub fn fd(&self) -> RawFd {
        // SAFETY: self.li is live.
        unsafe { libinput_get_fd(self.li) }
    }

    /// `libinput_dispatch()`; `Err(-errno)` on failure.
    pub fn dispatch(&mut self) -> Result<(), i32> {
        // SAFETY: self.li is live.
        match unsafe { libinput_dispatch(self.li) } {
            0 => Ok(()),
            rc => Err(rc),
        }
    }

    /// Next queued event, `None` when the queue is empty.
    pub fn next_event(&mut self) -> Option<Event> {
        // SAFETY: self.li is live; the returned event is owned by the caller.
        let event = unsafe { libinput_get_event(self.li) };
        (!event.is_null()).then_some(Event { raw: event })
    }
}

impl Drop for Libinput {
    fn drop(&mut self) {
        // SAFETY: each pointer holds one reference taken in new().
        unsafe {
            if !self.li.is_null() {
                libinput_unref(self.li);
            }
            udev_unref(self.udev);
        }
    }
}

/// What the server cares about in an event.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EventKind {
    DeviceAdded,
    DeviceRemoved,
    PointerButton { pressed: bool },
    Other,
}

/// One owned libinput event.
pub struct Event {
    raw: *mut libinput_event,
}

impl Event {
    pub fn kind(&self) -> EventKind {
        // SAFETY: self.raw is a live event.
        match unsafe { libinput_event_get_type(self.raw) } {
            LIBINPUT_EVENT_DEVICE_ADDED => EventKind::DeviceAdded,
            LIBINPUT_EVENT_DEVICE_REMOVED => EventKind::DeviceRemoved,
            LIBINPUT_EVENT_POINTER_BUTTON => {
                // SAFETY: the event is a pointer event, so the cast is valid (and non-null).
                let pressed = unsafe {
                    let pointer = libinput_event_get_pointer_event(self.raw);
                    !pointer.is_null()
                        && libinput_event_pointer_get_button_state(pointer)
                            == LIBINPUT_BUTTON_STATE_PRESSED
                };
                EventKind::PointerButton { pressed }
            }
            _ => EventKind::Other,
        }
    }

    /// The device the event belongs to.
    pub fn device(&self) -> Device<'_> {
        // SAFETY: every libinput event has a device, valid while the event lives.
        Device { raw: unsafe { libinput_event_get_device(self.raw) }, _event: PhantomData }
    }
}

impl Drop for Event {
    fn drop(&mut self) {
        // SAFETY: the event is owned and destroyed exactly once.
        unsafe { libinput_event_destroy(self.raw) };
    }
}

/// A device borrowed from an event.
pub struct Device<'a> {
    raw: *mut libinput_device,
    _event: PhantomData<&'a Event>,
}

fn owned_string(text: *const c_char) -> Option<String> {
    // SAFETY: libinput returns null or a NUL-terminated string valid while the device lives.
    (!text.is_null()).then(|| unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned())
}

impl Device<'_> {
    pub fn name(&self) -> Option<String> {
        // SAFETY: self.raw is live for 'a.
        owned_string(unsafe { libinput_device_get_name(self.raw) })
    }

    /// Kernel name, e.g. `event4`.
    pub fn sysname(&self) -> Option<String> {
        // SAFETY: self.raw is live for 'a.
        owned_string(unsafe { libinput_device_get_sysname(self.raw) })
    }

    pub fn is_keyboard(&self) -> bool {
        // SAFETY: self.raw is live for 'a.
        unsafe { libinput_device_has_capability(self.raw, LIBINPUT_DEVICE_CAP_KEYBOARD) != 0 }
    }

    /// Touchpads: enable tap-to-click with the left/right/middle map, so taps report as clicks.
    pub fn enable_tapping(&self) {
        // SAFETY: self.raw is live for 'a.
        unsafe {
            if libinput_device_config_tap_get_finger_count(self.raw) > 0 {
                libinput_device_config_tap_set_enabled(self.raw, LIBINPUT_CONFIG_TAP_ENABLED);
                libinput_device_config_tap_set_button_map(self.raw, LIBINPUT_CONFIG_TAP_MAP_LRM);
            }
        }
    }
}
