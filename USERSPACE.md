# SM8150 & Xiaomi Pad 5 (nabu) Userspace Porting Guide

This guide documents how to port userspace hardware configurations, firmware packages, audio UCM profiles, and sensor daemons from the tested [nabu-pkgs (Arch Linux ARM)](https://github.com/Kumar-Jy/nabu-pkgs) repository to **Fedora Atomic / PocketBlue** (RPM-based distributions).

---

## 📌 Architecture & Mapping

Because Arch Linux and Fedora adhere to standard Linux filesystem hierarchies, packages in `nabu-pkgs` can be converted into Fedora RPM `.spec` files with minimal changes.

| Component | Source Package (`nabu-pkgs`) | Target RPM Package | Target Filesystem Destination |
| :--- | :--- | :--- | :--- |
| **Audio UCM Profiles** | `packages/alsa-ucm-conf-xiaomi-nabu` | `alsa-ucm-conf-sm8150` | `/usr/share/alsa/ucm2/Qualcomm/sm8150/` |
| **Device Firmware** | `packages/linux-firmware-xiaomi-nabu` | `xiaomi-nabu-firmware` | `/lib/firmware/` (`qcom/`, `novatek/`) |
| **Sensors Reverse-Listener** | `packages/hexagonrpcd` & `libssc` | `hexagonrpcd` | `/usr/bin/hexagonrpcd`, `/usr/lib/systemd/system/` |
| **Hardware Video Decode** | `packages/ffmpeg-iris` & `mpv-iris` | `ffmpeg-iris` / `mpv-iris` | `/usr/lib/iris-ffmpeg/`, `/usr/bin/` |
| **Battery / Fuel Gauge** | Standard `upower` (upstream) | Standard `upower` | Fully supported out-of-the-box by upstream UPower with the modern `qcom_bms` driver. |
| **Tablet Mode / Keyboard** | `packages/nabu-tablet-mode` | `nabu-tablet-mode` | `/usr/lib/udev/rules.d/`, `/usr/bin/` |
| **Stylus Notification** | `packages/stylus-popup` | `stylus-popup` | `/etc/xdg/autostart/`, `/usr/bin/` |
| **Qualcomm Storage IPC** | `packages/rmtfs` & `tqftpserv` | `rmtfs` | `/usr/bin/rmtfs`, `/usr/lib/systemd/system/` |

---

## 1. Audio: ALSA UCM2 Profiles (`alsa-ucm-conf-sm8150`)

The Xiaomi Pad 5 uses 4 Cirrus Logic CS35L41 smart amplifiers driven by Qualcomm Hexagon QDSP6. Userspace requires UCM2 profiles to initialize digital routing, headphone detection, and amplifier volume levels.

- **Source in `nabu-pkgs`**: [`packages/alsa-ucm-conf-xiaomi-nabu`](https://github.com/Kumar-Jy/nabu-pkgs/tree/main/packages/alsa-ucm-conf-xiaomi-nabu)
- **Key Files**:
  - `HiFi.conf` &mdash; Defines mixer controls for quad-speaker stereo, 3.5mm Type-C audio, and microphone capture.
  - `sm8150.conf` &mdash; Sound card match and include definitions.

### RPM `.spec` Recipe Example:
```spec
Name:           alsa-ucm-conf-sm8150
Version:        1.0
Release:        1%{?dist}
Summary:        ALSA UCM profiles for SM8150 platforms (Xiaomi Pad 5)
License:        GPL-2.0-or-later
BuildArch:      noarch
Requires:       alsa-ucm-conf

Source0:        HiFi.conf
Source1:        sm8150.conf

%description
ALSA Use Case Manager configuration profiles enabling Quad CS35L41
speaker playback, Type-C audio dongles, and internal microphones.

%install
mkdir -p %{buildroot}%{_datadir}/alsa/ucm2/Qualcomm/sm8150
cp %{SOURCE0} %{buildroot}%{_datadir}/alsa/ucm2/Qualcomm/sm8150/HiFi.conf
cp %{SOURCE1} %{buildroot}%{_datadir}/alsa/ucm2/Qualcomm/sm8150/sm8150.conf

%files
%{_datadir}/alsa/ucm2/Qualcomm/sm8150/*
```

---

## 2. Firmware Package (`xiaomi-nabu-firmware`)

The kernel drivers rely on firmware files located in `/lib/firmware/`:
- **Source in `nabu-pkgs`**: [`packages/linux-firmware-xiaomi-nabu`](https://github.com/Kumar-Jy/nabu-pkgs/tree/main/packages/linux-firmware-xiaomi-nabu)

### Essential Firmware Files:
1. **GPU Zap Shader**:
   `/lib/firmware/qcom/a640_zap.mbn` (or `/lib/firmware/qcom/sm8150/xiaomi/nabu/a640_zap.mbn`)
   *Required by Adreno 640 to initialize 3D graphics (Turnip / Freedreno).*
2. **Touchscreen**:
   `/lib/firmware/novatek/novatek_nt36523_fw.bin`
   *Required by `nt36523` touch controller on probe/resume.*
3. **Hexagon Co-Processors**:
   - `/lib/firmware/qcom/sm8150/xiaomi/nabu/adsp.mbn` (Audio DSP)
   - `/lib/firmware/qcom/sm8150/xiaomi/nabu/cdsp.mbn` (Compute DSP)
   - `/lib/firmware/qcom/sm8150/xiaomi/nabu/slpi.mbn` (Sensor Low Power Island)
4. **Wireless (WCN3990)**:
   - `/lib/firmware/wlan/prima/WCNSS_qcom_wlan_nv.bin`
   - `/lib/firmware/qcom/sm8150/xiaomi/nabu/wlanmdsp.mbn`

---

## 3. Sensors & Auto-Rotation (`hexagonrpcd` & `iio-sensor-proxy`)

The tablet's accelerometer, gyroscope, and ambient light sensors live on the SLPI coprocessor. They communicate with the kernel through FastRPC.

- **Source in `nabu-pkgs`**: [`packages/hexagonrpcd`](https://github.com/Kumar-Jy/nabu-pkgs/tree/main/packages/hexagonrpcd)
- **Role**: `hexagonrpcd` is a daemon that responds to remote FastRPC filesystem and sensor calls from the SLPI firmware.
- **Systemd Service (`hexagonrpcd.service`)**:
  ```ini
  [Unit]
  Description=Hexagon FastRPC Reverse RPC Daemon for SLPI Sensors
  After=qrtr-ns.service pd-mapper.service
  Requires=qrtr-ns.service pd-mapper.service

  [Service]
  Type=simple
  ExecStart=/usr/bin/hexagonrpcd -f /lib/firmware/qcom/sm8150/xiaomi/nabu/sensors
  Restart=always
  RestartSec=2

  [Install]
  WantedBy=multi-user.target
  ```
- **Userspace Interaction**:
  Once `hexagonrpcd` runs, the kernel registers standard Linux IIO devices (`/sys/bus/iio/devices/iio:device*`).
  `iio-sensor-proxy` reads orientation changes and signals GNOME / KDE / Phosh to rotate the display.

---

## 4. Hardware Video Acceleration (`ffmpeg-iris` & `mpv-iris`)

The Qualcomm SM8150 Iris VPU provides hardware-accelerated decoding for H.264, HEVC (including 10-bit), and VP9 via the Linux V4L2 mem2mem interface (`/dev/video-dec0`).

- **Source in `nabu-pkgs`**:
  - [`packages/ffmpeg-iris`](https://github.com/Kumar-Jy/nabu-pkgs/tree/main/packages/ffmpeg-iris)
  - [`packages/mpv-iris`](https://github.com/Kumar-Jy/nabu-pkgs/tree/main/packages/mpv-iris)
  - [`packages/iris-vaapi`](https://github.com/Kumar-Jy/nabu-pkgs/tree/main/packages/iris-vaapi)
- **Key Configuration Flags**:
  - Builds FFmpeg with `--enable-v4l2-m2m`, `--enable-libdrm`, and `--enable-vaapi`.
  - Maps DRM-Prime P010 buffers directly into Wayland surfaces using DMA-BUF for zero-copy 10-bit playback.
- **MPV Execution**:
  ```bash
  mpv --hwdec=v4l2m2m-copy --vo=dmabuf-wayland video.webm
  ```

---

## 5. Power & Battery Reporting (Standard Upstream `upower`)

With modern Linux 7.3 and the updated `qcom_bms` / `ln8000` drivers, the kernel properly exposes standard Linux power supply sysfs nodes (`/sys/class/power_supply/qcom-battery` and `qcom-usb`).

- **Current Status**: Standard upstream `upower` works **completely out of the box** without any downstream patches.
- **Legacy Workaround (`upower-nncc`)**: In older kernel versions (6.x), UPower had an aggressive heuristic that incorrectly marked the battery as "discharging" if the PMIC reported zero current before charging negotiation completed. The downstream `upower-nncc` package was used to bypass this check. With the 7.3 kernel driver fixes (accurate disconnect ordering and AICL lockout prevention), `upower-nncc` is no longer needed. Standard distro `upower` (Fedora / Arch) works cleanly.

---

## 6. Tablet Mode & Accessories (`nabu-tablet-mode`)

Detects attachment of the Xiaomi magnetic keyboard cover and toggles touch rejection and on-screen keyboard states.

- **Source in `nabu-pkgs`**: [`packages/nabu-tablet-mode`](https://github.com/Kumar-Jy/nabu-pkgs/tree/main/packages/nabu-tablet-mode)
- **Udev Rule**:
  ```udev
  ACTION=="add|remove", SUBSYSTEM=="input", ATTRS{name}=="xiaomi-keyboard", TAG+="systemd", ENV{SYSTEMD_WANTS}="nabu-tablet-mode@%E{ACTION}.service"
  ```
