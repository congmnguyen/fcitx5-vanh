# SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Differential test of the Rust engine against the former C++ engine (lotus/ up to commit 09516fd).
# Standalone project, not part of the normal build:
#
#   git archive 09516fd lotus | tar -x -C /tmp/old           # the former engine
#   cmake -S engine-rs/parity -B /tmp/parity -DCMAKE_BUILD_TYPE=Release -DOLD_LOTUS_DIR=/tmp/old/lotus
#   cmake --build /tmp/parity -j
#   /tmp/parity/lotus_parity test/golden data/dictionaries/vietnamese.cm.dict engine-rs/parity/corpus.txt [scenarios] [seed]
#
# The former engine is compiled with -Dlotus=lotus_old so both engines link into one program.
cmake_minimum_required(VERSION 3.16)
project(lotus_parity CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT OLD_LOTUS_DIR)
    message(FATAL_ERROR "Set OLD_LOTUS_DIR to the former C++ engine (the lotus/ directory of commit 09516fd)")
endif()

set(BUILD_TESTING OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. engine-rs)

file(GLOB LOTUS_OLD_SOURCES ${OLD_LOTUS_DIR}/*.cpp ${OLD_LOTUS_DIR}/data/*.cpp)
add_library(lotus_old_engine STATIC ${LOTUS_OLD_SOURCES} old_side.cpp)
target_include_directories(lotus_old_engine PRIVATE ${OLD_LOTUS_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lotus_old_engine PRIVATE lotus=lotus_old)

add_library(lotus_new_side STATIC new_side.cpp)
target_link_libraries(lotus_new_side PRIVATE Lotus::Engine)

add_executable(lotus_parity main.cpp)
target_include_directories(lotus_parity PRIVATE ${OLD_LOTUS_DIR})
target_link_libraries(lotus_parity PRIVATE lotus_old_engine lotus_new_side Lotus::Engine)
