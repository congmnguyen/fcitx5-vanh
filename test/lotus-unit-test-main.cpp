/*
 * SPDX-FileCopyrightText: 2026 Nguyen Hoang Ky <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs every C++ unit test group in one process each; ctest still owns one
 * entry per group, so labels, timeouts and crash isolation are unchanged. The
 * engine's own tests are in Rust (engine-rs, ctest entry lotus_engine_rust_tests).
 */
#include <array>
#include <cstdio>
#include <span>
#include <string_view>

int run_utf8();
int run_engine_api();
int run_golden_smoke();
int run_numeric_macro_case();

int main(int argc, char** argv) {
    struct Test {
        const char* name;
        int (*fn)();
    };
    const std::array tests{
        Test{.name = "utf8", .fn = run_utf8},
        Test{.name = "engine_api", .fn = run_engine_api},
        Test{.name = "golden_smoke", .fn = run_golden_smoke},
        Test{.name = "numeric_macro_case", .fn = run_numeric_macro_case},
    };

    const std::span<char* const> args(argv, static_cast<std::size_t>(argc));
    if (args.size() != 3 || std::string_view(args[1]) != "--run") {
        std::fprintf(stderr, "usage: %s --run <test>\navailable:", args[0]);
        for (const auto& t : tests) {
            std::fprintf(stderr, " %s", t.name);
        }
        std::fprintf(stderr, "\n");
        return 2;
    }
    const std::string_view wanted(args[2]);
    for (const auto& t : tests) {
        if (wanted == t.name) {
            return t.fn();
        }
    }
    std::fprintf(stderr, "unknown test: %s\n", args[2]);
    return 2;
}
