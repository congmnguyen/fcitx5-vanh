// SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
// SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Every externally visible name of the server, in one place.
//!
//! The C++ addon (src/lotus-utils.cpp, common/common-socket-name.h, src/lotus-monitor.cpp) must
//! agree with these values. The executable name itself is set in Cargo.toml (`[[bin]] name`) and
//! in CMakeLists.txt (`LOTUS_SERVER_BINARY_NAME`).

/// syslog identity (`openlog` ident), also the program name of the daemon.
pub const SYSLOG_IDENT: &std::ffi::CStr = c"fcitx5-vanh-server";

/// Prefix of both abstract socket names: `<prefix><namespace>-<suffix>`.
pub const SOCKET_PREFIX: &str = "vanhsocket-";
/// Suffix of the keyboard request socket.
pub const KB_SOCKET_SUFFIX: &str = "kbSocket";
/// Suffix of the mouse click notification socket.
pub const MOUSE_SOCKET_SUFFIX: &str = "mouseSocket";
/// Environment variable that replaces the user segment of the socket names.
pub const SOCKET_NAMESPACE_ENV: &str = "VANH_SOCKET_NAMESPACE";

/// Command line flag naming the user the server works for.
pub const USER_FLAG: &str = "-u";

/// Name of the virtual keyboard; the physical Shift tracker skips the device with this name.
pub const UINPUT_NAME: &str = "Vanh-Uinput-Server";
/// Vendor id of the virtual keyboard.
pub const UINPUT_VENDOR: u16 = 0x1234;
/// Product id of the virtual keyboard.
pub const UINPUT_PRODUCT: u16 = 0x5678;

/// uinput control node.
pub const UINPUT_PATH: &str = "/dev/uinput";
/// Directory of the evdev nodes (`/dev/input/<sysname>`).
pub const INPUT_DEV_DIR: &str = "/dev/input/";
/// sysfs directory of the virtual input devices (uinput devnode lookup).
pub const SYS_VIRTUAL_INPUT_DIR: &str = "/sys/devices/virtual/input/";
/// libinput seat the server listens on.
pub const SEAT: &std::ffi::CStr = c"seat0";

/// fcitx5 executable allowed to connect (CMake `LOTUS_FCITX5_EXECUTABLE`).
pub const FCITX5_EXECUTABLE: &str = env!("LOTUS_FCITX5_EXECUTABLE");
/// Extra accepted prefix, e.g. `/nix/store/` (CMake `LOTUS_ALT_EXECUTABLE_PREFIX`).
pub const ALT_EXECUTABLE_PREFIX: &str = env!("LOTUS_ALT_EXECUTABLE_PREFIX");
