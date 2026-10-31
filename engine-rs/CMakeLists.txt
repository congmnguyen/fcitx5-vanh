# SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vietnamese typing engine, written in Rust (a port of bamboo/bamboo-core, MIT, kept in CREDITS, plus
# the English restore). Cargo builds the crate into a static library; the thin C++ wrappers in cxx/
# keep the C++ interface the addon uses. The crate has no dependency, so the build runs offline.
find_program(CARGO_EXECUTABLE cargo REQUIRED)

set(LOTUS_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
# Cargo needs a writable home even with no dependency; keep it inside the build tree (package
# builders often run with an unwritable or missing $HOME).
set(LOTUS_CARGO_HOME "${CMAKE_CURRENT_BINARY_DIR}/cargo-home")
set(LOTUS_RUST_LIBRARY "${LOTUS_CARGO_TARGET_DIR}/release/liblotus_engine.a")
set(LOTUS_CARGO_ENV
    "CARGO_HOME=${LOTUS_CARGO_HOME}"
    "CARGO_TARGET_DIR=${LOTUS_CARGO_TARGET_DIR}"
)

file(GLOB_RECURSE LOTUS_RUST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.rs")
add_custom_command(
    OUTPUT "${LOTUS_RUST_LIBRARY}"
    COMMAND ${CMAKE_COMMAND} -E env ${LOTUS_CARGO_ENV}
            "${CARGO_EXECUTABLE}" build --offline --locked --release --lib
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    DEPENDS ${LOTUS_RUST_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml" "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
    COMMENT "Building the Rust typing engine with cargo"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(lotus_engine_rust DEPENDS "${LOTUS_RUST_LIBRARY}")

add_library(lotus_engine_rust_lib STATIC IMPORTED GLOBAL)
set_target_properties(lotus_engine_rust_lib PROPERTIES IMPORTED_LOCATION "${LOTUS_RUST_LIBRARY}")
add_dependencies(lotus_engine_rust_lib lotus_engine_rust)
find_package(Threads REQUIRED)
# Libraries the Rust standard library needs on Linux (`--print native-static-libs`).
target_link_libraries(lotus_engine_rust_lib INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m)

add_library(lotus_engine_cxx STATIC
    cxx/dictionary.cpp
    cxx/input_method.cpp
    cxx/key_processor.cpp
    cxx/macro_table.cpp
    cxx/utf8.cpp
)
add_dependencies(lotus_engine_cxx lotus_engine_rust)
set_target_properties(lotus_engine_cxx PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(lotus_engine_cxx PRIVATE -Wall -Wextra)
target_compile_features(lotus_engine_cxx PUBLIC cxx_std_20)
# Consumers use bare includes ("key_processor.h", "utf8.h"), like the former C++ engine.
target_include_directories(lotus_engine_cxx
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cxx
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(lotus_engine_cxx PUBLIC lotus_engine_rust_lib)
# Keep the Rust standard library and the C ABI out of the dynamic symbol tables of the shared
# objects the engine ends up in.
target_link_options(lotus_engine_cxx INTERFACE "LINKER:--exclude-libs,liblotus_engine.a")
add_library(Lotus::Engine ALIAS lotus_engine_cxx)

# Command of the engine unit tests (ported from the former C++ tests) and the golden replays;
# test/CMakeLists.txt registers it, after include(CTest).
set(LOTUS_RUST_TEST_COMMAND
    ${CMAKE_COMMAND} -E env ${LOTUS_CARGO_ENV} "LOTUS_GOLDEN_DIR=${PROJECT_SOURCE_DIR}/test/golden"
    "${CARGO_EXECUTABLE}" test --offline --locked --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    PARENT_SCOPE
)
