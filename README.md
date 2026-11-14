[English](README.en.md) | [Tiếng Việt](README.md)

# Vanh

**Vanh** (Việt + Anh) là bộ gõ tiếng Việt cho fcitx5 trên Linux, gõ được tiếng Việt và tiếng Anh xen kẽ mà không cần chuyển chế độ, gõ không gạch chân và không mất hay lặp chữ khi gõ nhanh. Vanh là bản fork của [fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus).

## Tính năng

- **Tự chuyển Anh/Việt.** Đến cuối từ, nếu phím vừa gõ là một từ tiếng Anh thì trả lại đúng phím đã gõ: `test` không thành "tét", `error` không thành "eror". Từ tiếng Việt vẫn giữ nguyên: `cos` → "có".
- **Nhìn ngữ cảnh.** Một từ đọc được cả hai cách thì dựa vào từ đứng cạnh để quyết định (`cho con bus` → "cho con bú", `the bus` → giữ nguyên). Từ trước cũng được sửa lại khi cần: `thí makes` → "this makes".
- **Gõ không gạch chân.** Chữ hiện thẳng vào ô nhập như gõ trên Windows, không có đoạn gạch chân chờ xác nhận.
- **Không mất hay lặp chữ khi gõ nhanh** trên Chrome/Brave (cả thanh địa chỉ), Facebook và các app GNOME: Vanh tự dùng surrounding text khi app hỗ trợ, chờ app cập nhật xong mới sửa chữ, và chuyển sang uinput khi app bỏ lệnh xoá (Messenger, Apple Notes trên web). Terminal vẫn dùng uinput.
- Mọi tính năng còn lại của lotus: Telex/VNI và các kiểu gõ khác, macro, từ điển, đổi bảng mã, emoji.

Gói, addon, bộ gõ, server và file cấu hình đều tên `vanh` (`fcitx5-vanh`, `fcitx5-vanh-server@<user>.service`, `~/.config/fcitx5/conf/vanh.conf`). Gói `fcitx5-vanh` thay thế `fcitx5-lotus`; lần đầu chạy, Vanh tự chép cấu hình lotus cũ (cấu hình, macro, keymap, luật theo app, lịch sử emoji, từ điển người dùng) sang tên mới và đặt Vanh vào chỗ của lotus trong danh sách bộ gõ. File cũ được giữ nguyên.

## Cài từ bản phát hành (Debian/Ubuntu)

Mỗi bản phát hành có sẵn `.deb` (amd64) cho Ubuntu 22.04, 24.04, 25.10, 26.04, 26.10 và Debian 12, 13, testing, unstable:

```bash
ver=6.0.5-1; deb=fcitx5-vanh_${ver}_amd64_$(lsb_release -cs).deb
wget https://github.com/congmnguyen/fcitx5-vanh/releases/download/v${ver%-*}/$deb
sudo apt install ./$deb
sudo systemctl enable --now fcitx5-vanh-server@$USER
im-config -n fcitx5
```

Đăng xuất rồi đăng nhập lại, mở Fcitx5 Configuration (`fcitx5-configtool`) và thêm **Vanh** vào danh sách bộ gõ.

## Build từ mã nguồn (Debian/Ubuntu)

```bash
sudo apt install --no-install-recommends debhelper dh-python cmake g++ extra-cmake-modules fcitx5-modules-dev \
  libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev libinput-dev libnotify-dev \
  libdbus-1-dev libsystemd-dev libudev-dev pkg-config python3 gettext cargo rustc
git clone https://github.com/congmnguyen/fcitx5-vanh && cd fcitx5-vanh
cp -r packaging/debian debian && dpkg-buildpackage -b -us -uc -j$(nproc) && rm -rf debian obj-*
sudo apt install ../fcitx5-vanh_*_amd64.deb python3-pyqt5
sudo systemctl enable --now fcitx5-vanh-server@$USER
fcitx5 -r -d
```

Distro khác (cần thêm `cargo` và `rustc`): `cmake -DCMAKE_INSTALL_PREFIX=/usr . && make && sudo make install` với các gói dev tương ứng.

## Sử dụng

- **Chế độ gõ.** Phím `` ` `` mở menu chế độ: Uinput (Smooth), Uinput (Super Smooth), Uinput (Select), Surrounding Text, Preedit, Emoji Picker, OFF. Mặc định là Preedit (có gạch chân). Để gõ không gạch chân, chọn một chế độ Uinput (cần server `fcitx5-vanh-server@<user>` đang chạy) hoặc Surrounding Text.
- **Đảo từ.** `Shift+BackSpace` đổi từ vừa gõ sang cách đọc còn lại (Anh ↔ Việt) khi Vanh đoán sai.
- **Cài đặt.** `fcitx5-vanh-settings` chỉnh kiểu gõ, bảng mã, phím tắt, macro, từ điển riêng, keymap riêng, và sao lưu/khôi phục cấu hình.
- **Luật theo app.** Trong menu chế độ có thể gán một chế độ riêng cho app đang dùng; luật được lưu trong `~/.config/fcitx5/conf/vanh-app-rules.conf`.

## Độ chính xác

Bộ câu công khai [`engine-rs/eval/public.tsv`](engine-rs/eval/public.tsv) gồm 116 câu Việt/Anh xen kẽ (975 từ) được gõ lại bằng Telex qua engine; mỗi lần build, `ctest` kiểm tra không từ nào ra sai.

| Cấu hình | Từ sai | Trên 1000 từ |
|---|---|---|
| Vanh (mặc định) | 0 | 0 |
| Chỉ khôi phục từ sai chính tả (kiểu bamboo) | 24 | 24,6 |
| Telex thuần | 97 | 99,5 |

Bộ câu này nhỏ và được viết riêng cho việc đo, nên dễ hơn văn bản thật. Chạy lại:

```bash
cd engine-rs && cargo run --release --example replay -- run eval/public.tsv --split all --config default
```

`--config bamboo` và `--config off` cho hai dòng còn lại.

## Gỡ lỗi

- **Thông báo "the uinput server is not running".** Kiểm tra `systemctl status fcitx5-vanh-server@$USER`; nếu chưa bật thì chạy `sudo systemctl enable --now fcitx5-vanh-server@$USER`. Khi server chạy, `/run/vanh/$USER/` có `kb.sock` và `mouse.sock`.
- **Server không mở được `/dev/uinput`.** `getfacl /dev/uinput` phải có dòng `user:uinput_proxy:rw-`. Nếu thiếu: `sudo modprobe uinput && sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput`.
- **Log.** Server: `journalctl -u fcitx5-vanh-server@$USER -e`. Bộ gõ: chạy `fcitx5 -r --verbose=vanh=5` trong terminal.
- **Một app mất hoặc lặp chữ.** Thử gán chế độ khác cho app đó trong menu chế độ, rồi [mở issue](https://github.com/congmnguyen/fcitx5-vanh/issues/new/choose) kèm tên app, phiên bản, desktop và X11 hay Wayland.

## Đóng góp

Cách build, chạy test và lint nằm trong [`CONTRIBUTING.md`](CONTRIBUTING.md); thay đổi theo từng bản trong [`CHANGELOG.md`](CHANGELOG.md).

## Giấy phép và ghi công

GPL-3.0-or-later, xem [`LICENSE`](LICENSE). Vanh dựa trên:

- **[fcitx5-lotus](https://github.com/LotusInputMethod/fcitx5-lotus)** (GPL-3.0-or-later), fork từ **[VMK](https://github.com/thanhpy2009/VMK)**.
- **[bamboo-core](https://github.com/BambooEngine/bamboo-core)**: engine gõ gốc, MIT, © 2018 Luong Thanh Lam ([`engine-rs/LICENSE.bamboo-core`](engine-rs/LICENSE.bamboo-core)).
- **[fcitx5-bamboo](https://github.com/fcitx/fcitx5-bamboo)**: phần glue fcitx5 dựa theo, LGPL-2.1.
- **[Vikey](https://github.com/tonyminibot/Vikey)** (GPL-3.0): danh sách từ, tần suất và cặp từ cho tự chuyển Anh/Việt. Nguồn dữ liệu gốc: FrequencyWords/OpenSubtitles 2018 (CC BY-SA 4.0), FineWeb-2/FineWeb và MADLAD-400 (ODC-By), HPLT 2 (CC0).

Ghi công đầy đủ trong [`CREDITS`](CREDITS).
