[English](README.en.md) | [Tiếng Việt](README.md)

# Vanh

**Vanh** (Việt + Anh) là bộ gõ tiếng Việt cho fcitx5 trên Linux, gõ được tiếng Việt và tiếng Anh xen kẽ mà không cần chuyển chế độ. Vanh là bản fork của [fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus).

## Tính năng

- **Tự chuyển Anh/Việt.** Đến cuối từ, nếu phím vừa gõ là một từ tiếng Anh thì trả lại đúng phím đã gõ: `test` không thành "tét", `error` không thành "eror". Từ tiếng Việt vẫn giữ nguyên: `cos` → "có".
- **Nhìn ngữ cảnh.** Một từ đọc được cả hai cách thì dựa vào từ đứng cạnh để quyết định (`cho con bus` → "cho con bú", `the bus` → giữ nguyên). Từ trước cũng được sửa lại khi cần: `thí makes` → "this makes".
- **Surrounding text tự động** trong Uinput mode khi app hỗ trợ (Brave, Facebook, thanh địa chỉ): không còn mất chữ hay lặp chữ khi gõ nhanh. Terminal vẫn dùng uinput.
- Mọi tính năng còn lại của lotus: Telex/VNI và các kiểu gõ khác, gõ không gạch chân, macro, từ điển, đổi bảng mã, emoji.

Tên addon, tên gói và file cấu hình vẫn là `lotus`, nên cấu hình lotus cũ dùng được luôn.

## Build và cài (Debian/Ubuntu)

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

Distro khác (cần thêm `cargo` và `rustc`): `cmake -DCMAKE_INSTALL_PREFIX=/usr . && make && sudo make install` với các gói dev tương ứng.

## Giấy phép và ghi công

GPL-3.0-or-later, xem [`LICENSE`](LICENSE). Vanh dựa trên:

- **[fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus)** (GPL-3.0-or-later), fork từ **[VMK](https://github.com/thanhpy2009/VMK)**.
- **[bamboo-core](https://github.com/BambooEngine/bamboo-core)**: engine gõ gốc, MIT, © 2018 Luong Thanh Lam ([`engine-rs/LICENSE.bamboo-core`](engine-rs/LICENSE.bamboo-core)).
- **[fcitx5-bamboo](https://github.com/fcitx/fcitx5-bamboo)**: phần glue fcitx5 dựa theo, LGPL-2.1.
- **[Vikey](https://github.com/tonyminibot/Vikey)** (GPL-3.0): danh sách từ, tần suất và cặp từ cho tự chuyển Anh/Việt. Nguồn dữ liệu gốc: FrequencyWords/OpenSubtitles 2018 (CC BY-SA 4.0), FineWeb-2/FineWeb và MADLAD-400 (ODC-By), HPLT 2 (CC0).

Ghi công đầy đủ trong [`CREDITS`](CREDITS).
