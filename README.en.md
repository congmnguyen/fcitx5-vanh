[English](README.en.md) | [Tiếng Việt](README.md)

# Vanh

**Vanh** (Việt + Anh, "Vietnamese + English") is a Vietnamese input method for fcitx5 on Linux. It lets you mix Vietnamese and English in the same text without switching modes. Vanh is a fork of [fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus).

## Features

- **Automatic English/Vietnamese switching.** At the end of a word, if the keys you typed are an English word, Vanh puts them back as typed: `test` stays "test" instead of "tét", and `error` stays "error" instead of "eror". Vietnamese words are kept: `cos` → "có".
- **Context.** For a word that reads both ways, the word next to it decides (`cho con bus` → "cho con bú", while `the bus` stays). An earlier word is fixed when the next one makes it clear: `thí makes` → "this makes".
- **Automatic surrounding text** in Uinput mode when the app supports it (Brave, Facebook, the address bar), so fast typing no longer drops or doubles letters. Terminals keep using uinput.
- Everything else from lotus: Telex/VNI and other input methods, typing without underline, macros, dictionary, charsets, emoji.

The addon, package and config file are still named `lotus`, so an existing lotus configuration works as is.

## Build and install (Debian/Ubuntu)

```bash
sudo apt install --no-install-recommends debhelper dh-python cmake g++ extra-cmake-modules fcitx5-modules-dev \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev libinput-dev libnotify-dev \
  libdbus-1-dev libudev-dev pkg-config python3 gettext cargo rustc
git clone https://github.com/congmnguyen/fcitx5-vanh && cd fcitx5-vanh
cp -r packaging/debian debian && dpkg-buildpackage -b -us -uc -j$(nproc) && rm -rf debian obj-*
sudo apt install ../fcitx5-lotus_*_amd64.deb python3-pyqt5
sudo systemctl enable --now fcitx5-lotus-server@$USER
fcitx5 -r -d
```

Other distros (`cargo` and `rustc` are needed too): `cmake -DCMAKE_INSTALL_PREFIX=/usr . && make && sudo make install` with the matching dev packages.

## License and credits

GPL-3.0-or-later, see [`LICENSE`](LICENSE). Vanh builds on:

- **[fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus)** (GPL-3.0-or-later), itself a fork of **[VMK](https://github.com/thanhpy2009/VMK)**.
- **[bamboo-core](https://github.com/BambooEngine/bamboo-core)**: the original typing engine, MIT, © 2018 Luong Thanh Lam ([`engine-rs/LICENSE.bamboo-core`](engine-rs/LICENSE.bamboo-core)).
- **[fcitx5-bamboo](https://github.com/fcitx/fcitx5-bamboo)**: the fcitx5 glue is based on it, LGPL-2.1.
- **[Vikey](https://github.com/tonyminibot/Vikey)** (GPL-3.0): word lists, frequencies and word pairs for English/Vietnamese switching. Original data sources: FrequencyWords/OpenSubtitles 2018 (CC BY-SA 4.0), FineWeb-2/FineWeb and MADLAD-400 (ODC-By), HPLT 2 (CC0).

Full credits in [`CREDITS`](CREDITS).
