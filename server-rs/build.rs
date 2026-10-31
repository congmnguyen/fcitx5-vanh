// SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Passes the library directories pkg-config found for libinput/libudev (CMake sets them, see
// CMakeLists.txt).
use std::env;

fn main() {
    println!("cargo:rerun-if-env-changed=LOTUS_SERVER_LINK_DIRS");
    if let Ok(dirs) = env::var("LOTUS_SERVER_LINK_DIRS") {
        for dir in dirs.split(':').filter(|dir| !dir.is_empty()) {
            println!("cargo:rustc-link-search=native={dir}");
        }
    }
}
