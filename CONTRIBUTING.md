# Contributing to Vanh

Bug reports, fixes and new test cases are welcome. Development happens on Ubuntu; the CI builds
the package for every supported Ubuntu and Debian release.

## Build and test

Install the build dependencies listed in the [README](README.en.md#build-from-source-debianubuntu), then:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
```

`ctest` runs the headless addon tests (`test/*.cpp`), the Rust tests of the engine and the uinput
server (`cargo test`), the settings GUI tests and the public accuracy corpus. None of them touches
the fcitx5 or the uinput server of your session.

A fix for a typing bug comes with a test that fails without it: an engine case in
`engine-rs/src/tests/`, or a headless test in `test/` when the bug is in how text reaches the app.
A word that switches language wrongly also goes into `engine-rs/eval/public.tsv`.

## Before a pull request

The CI job "Format, lint, sanitizers" runs these; the versions are the ones of Ubuntu 26.04
(clang-format 21, Rust 1.93) plus the two pinned below.

```bash
git ls-files '*.cpp' '*.h' | xargs clang-format -i
(cd engine-rs && cargo fmt && cargo clippy --all-targets -- -D warnings)
(cd server-rs && cargo fmt && cargo clippy --all-targets -- -D warnings)
pipx run ruff==0.17.5 check . && pipx run ruff==0.17.5 format .
ls src/*.cpp engine-rs/cxx/*.cpp | xargs -n 1 pipx run --spec clang-tidy==23.1.4 clang-tidy -p build --quiet
```

The tests also have to pass under the sanitizers:

```bash
cmake -S . -B build-asan -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cmake --build build-asan -j$(nproc) && ctest --test-dir build-asan -j$(nproc)
```

The code rules are in [`CODE_STYLE.en.md`](CODE_STYLE.en.md) ([tiếng Việt](CODE_STYLE.md)).

## End-to-end tests

`test/e2e/gnome-e2e.py` types fixed sentences into Ghostty and Brave on a private headless GNOME
Shell and compares the screen with what the engine produces. It needs `gnome-shell`, the apps and
a built `build/`; nothing reaches your own session:

```bash
(cd engine-rs && cargo build --release --example replay)
dbus-run-session -- python3 test/e2e/gnome-e2e.py --config default --apps ghostty
```

## Translations

After changing a user-visible string, refresh the template and the Vietnamese catalogue, then
translate the new entries in `po/vi.po`:

```bash
bash Messages.sh && sh update_translate.sh
msgfmt --check --statistics -o /dev/null po/vi.po
```

## Releases

Bump the version in `CMakeLists.txt` and both `Cargo.toml` files, add the entry to
`packaging/debian/changelog`, `CHANGELOG.md` and the `<releases>` of the metainfo file, then push
a `v*` tag: the CI attaches every `.deb` to a draft release.
