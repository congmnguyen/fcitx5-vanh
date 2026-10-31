// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Passes the packaging-time paths to the crate (CMake sets them, see CMakeLists.txt) and the
// library directories pkg-config found for libinput/libudev.
use std::env;

fn main() {
    for (name, default) in [("LOTUS_FCITX5_EXECUTABLE", "/usr/bin/fcitx5"), ("LOTUS_ALT_EXECUTABLE_PREFIX", "")] {
        println!("cargo:rerun-if-env-changed={name}");
        let value = env::var(name).unwrap_or_else(|_| default.to_owned());
        println!("cargo:rustc-env={name}={value}");
    }
    println!("cargo:rerun-if-env-changed=LOTUS_SERVER_LINK_DIRS");
    if let Ok(dirs) = env::var("LOTUS_SERVER_LINK_DIRS") {
        for dir in dirs.split(':').filter(|dir| !dir.is_empty()) {
            println!("cargo:rustc-link-search=native={dir}");
        }
    }
}
