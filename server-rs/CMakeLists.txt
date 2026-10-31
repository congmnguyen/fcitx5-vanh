# SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
# SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ <nhktmdzhg@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The uinput helper daemon, written in Rust (formerly server/, C++). Like engine-rs/, the crate
# has no dependency and cargo runs offline with its home and target directories in the build tree.
find_program(CARGO_EXECUTABLE cargo REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)

set(LOTUS_SERVER_BINARY_NAME "fcitx5-lotus-server")
set(LOTUS_SERVER_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(LOTUS_SERVER_CARGO_HOME "${CMAKE_CURRENT_BINARY_DIR}/cargo-home")
set(LOTUS_SERVER_BINARY "${LOTUS_SERVER_CARGO_TARGET_DIR}/release/${LOTUS_SERVER_BINARY_NAME}")
set(LOTUS_SERVER_KB_REPLAY "${LOTUS_SERVER_CARGO_TARGET_DIR}/release/examples/kb_replay")

# ':'-separated: a ';' would split the argument of `cmake -E env`.
set(LOTUS_SERVER_LINK_DIRS ${LIBINPUT_LIBRARY_DIRS} ${LIBUDEV_LIBRARY_DIRS})
list(REMOVE_DUPLICATES LOTUS_SERVER_LINK_DIRS)
string(REPLACE ";" ":" LOTUS_SERVER_LINK_DIRS "${LOTUS_SERVER_LINK_DIRS}")

set(LOTUS_SERVER_CARGO_ENV
    "CARGO_HOME=${LOTUS_SERVER_CARGO_HOME}"
    "CARGO_TARGET_DIR=${LOTUS_SERVER_CARGO_TARGET_DIR}"
    "LOTUS_FCITX5_EXECUTABLE=${LOTUS_FCITX5_EXECUTABLE}"
    "LOTUS_ALT_EXECUTABLE_PREFIX=${LOTUS_ALT_EXECUTABLE_PREFIX}"
    "LOTUS_SERVER_LINK_DIRS=${LOTUS_SERVER_LINK_DIRS}"
)

# Rebuild when the paths compiled into the binary change.
configure_file(build-config.txt.in "${CMAKE_CURRENT_BINARY_DIR}/build-config.txt" @ONLY)

set(LOTUS_SERVER_OUTPUTS "${LOTUS_SERVER_BINARY}")
set(LOTUS_SERVER_CARGO_TARGETS --bin ${LOTUS_SERVER_BINARY_NAME})
if (BUILD_TESTING)
    # Helper of the C++/Rust wire test (test/server-wire-compat.cpp); never installed.
    list(APPEND LOTUS_SERVER_OUTPUTS "${LOTUS_SERVER_KB_REPLAY}")
    list(APPEND LOTUS_SERVER_CARGO_TARGETS --example kb_replay)
endif()

file(GLOB_RECURSE LOTUS_SERVER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.rs" "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.rs")
add_custom_command(
    OUTPUT ${LOTUS_SERVER_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E env ${LOTUS_SERVER_CARGO_ENV}
            "${CARGO_EXECUTABLE}" build --offline --locked --release ${LOTUS_SERVER_CARGO_TARGETS}
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    DEPENDS ${LOTUS_SERVER_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/build.rs"
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
            "${CMAKE_CURRENT_BINARY_DIR}/build-config.txt"
    COMMENT "Building the Rust uinput server with cargo"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(lotus_server_rust ALL DEPENDS ${LOTUS_SERVER_OUTPUTS})

install(PROGRAMS "${LOTUS_SERVER_BINARY}" DESTINATION ${CMAKE_INSTALL_BINDIR})

# Command of the server unit tests; test/CMakeLists.txt registers it, after include(CTest).
set(LOTUS_SERVER_RUST_TEST_COMMAND
    ${CMAKE_COMMAND} -E env ${LOTUS_SERVER_CARGO_ENV}
    "${CARGO_EXECUTABLE}" test --offline --locked --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    PARENT_SCOPE
)
set(LOTUS_SERVER_KB_REPLAY "${LOTUS_SERVER_KB_REPLAY}" PARENT_SCOPE)
