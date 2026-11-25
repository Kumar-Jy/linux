# SM8150 Linux Mainline & Fedora ARK Status

> **Branch:** `v7.3.6-sm8150-arkify`  
> **Base Kernel:** Linux 7.3.6 (`v7.3.6`)  
> **Target Devices:** Xiaomi Pad 5 (*nabu*), Surface Duo, and SM8150 platforms

---

## Hardware Working Status

| Hardware Component | Status | Details |
| :--- | :---: | :--- |
| **Display** | ✅ Working | 2.5K WQHD+ (2560x1600), seamless 120Hz / 60Hz runtime switching |
| **Touchscreen & Pen** | ✅ Working | 10-point multi-touch, touch sync preserved across sleep / wake |
| **Backlight** | ✅ Working | Smooth flicker-free 2048-step exponential brightness curve |
| **GPU (3D Graphics)** | ✅ Working | Adreno 640 via Mesa Turnip (Vulkan 1.3) & Freedreno (OpenGL 4.6) |
| **Video Acceleration** | ✅ Working | Hardware H.264, HEVC (10-bit), and VP9 decoding via Iris VPU5 |
| **Audio (Speakers)** | ✅ Working | Quad stereo speakers via Hexagon QDSP6 & CS35L41 amplifiers |
| **Audio (Type-C / BT)** | ✅ Working | Type-C audio dongles with hotplug debounce & Bluetooth A2DP |
| **Camera (Rear)** | ✅ Working | 13MP OmniVision OV13B10 with autofocus (CN3927) via CAMSS (`msm_csiphy0`) |
| **Camera (Front)** | ✅ Working | 8MP OmniVision OV8856 via CAMSS (`msm_csiphy1`) |
| **Flash / Torch** | ✅ Working | Dual rear LED flash via PM8150L (`white:flash`) |
| **Wi-Fi** | ✅ Working | Qualcomm WCN3990 (802.11ac, dual-band 2.4/5 GHz) |
| **Bluetooth** | ✅ Working | Bluetooth 5.0 with low-energy (BLE) support |
| **Battery & Charging** | ✅ Working | Accurate fuel gauge (BMS) and LN8000 fast charging |
| **Sleep & Wakeup** | ✅ Working | Deep `s2idle` sleep with reliable power key wakeup via PDC |
| **Storage** | ✅ Working | High-speed UFS 3.1 internal flash |
| **Keyboard Cover** | ✅ Working | Xiaomi magnetic keyboard cover driver (`CONFIG_XIAOMI_KEYBOARD=m`) |
| **Sensors** | ✅ Working | Auto-rotation & ambient light via FastRPC SLPI sensor island |

---

## Required Userspace Components

To ensure all hardware features operate smoothly in userspace (Fedora, Arch, Debian/Ubuntu):

| Subsystem | Required Userspace / Packages | Notes |
| :--- | :--- | :--- |
| **Firmware** | `linux-firmware` + SM8150 device firmware in `/lib/firmware/` | GPU zap shader (`a640_zap.mbn`), GMU, Hexagon ADSP/CDSP, and WCN3990 |
| **Graphics (3D)** | Mesa Turnip (`vulkan-freedreno`) & Freedreno (`mesa-dri-drivers`) | Provides Vulkan 1.3 and OpenGL 4.6 hardware acceleration |
| **Audio** | `alsa-ucm-conf` + `pipewire` & `wireplumber` | ALSA UCM profiles for SM8150 Quad CS35L41 speaker routing and mic input |
| **Qualcomm Services** | `qrtr` (`qrtr-ns`) & `pd-mapper` | IPC daemon and protection domain mapper for modem, audio DSP, and sensors |
| **Sensors & Rotation** | `iio-sensor-proxy` | Screen auto-rotation and ambient light sensing over FastRPC SLPI IIO |
| **Touchscreen** | `novatek_nt36523_fw.bin` in `/lib/firmware/novatek/` | Firmware binary for Novatek NT36523 touch controller |
| **Bluetooth & Wi-Fi** | `bluez`, `NetworkManager` or `iwd` | Bluetooth 5.0 stack (`bluetooth.service`) and Wi-Fi connection manager |

### Recommended Kernel Cmdline
Add to your bootloader or `/etc/cmdline.d/`:
```text
mem_sleep_default=s2idle fw_devlink=permissive fbcon=rotate:1 acpi=off
```

---

## Changelog: PocketBlue 7.0 &rarr; 7.3

### 🚀 Kernel & Platform Core
- **Rebase to Linux 7.3.6**: Updated upstream stable baseline from 7.0.9 to 7.3.6.
- **Improved Sleep & Wake**: Routed SPMI power key wakeup through PDC pin 1 for reliable deep sleep resume.
- **Clock Stability**: Configured TRION PLL relock parameters for clean resume from suspend.

### 🎬 Video & Graphics
- **Iris VPU5 Video Decode**: Added hardware decoding for VP9, H.264, and 10-bit HEVC with dedicated extradata buffers and proper power domain sequencing.
- **Display Refresh Rate (DFPS)**: Enabled seamless runtime switching between 60Hz and 120Hz.
- **Backlight Curve**: Added fine-grained 2048-step exponential brightness curve for smooth backlight dimming.

### 🔊 Audio & Peripherals
- **Audio DMA Isolation**: Marked Qualcomm BAM DMA engine as `DMA_PRIVATE` to prevent channel stealing and audio stutter on distribution kernels.
- **Audio Teardown Fix**: Fixed WCD934x SLIMbus port teardown sequence and added AFE port auto-recovery.
- **Type-C Audio**: Fixed CC line debounce handling for stable Type-C headphone hotplugging.
- **Xiaomi Keyboard Cover**: Added support for the Xiaomi Pad 5 magnetic keyboard cover with pinctrl preservation across display blanking.

### 📦 Fedora ARK & Packaging
- **Kernel Configs**: Enabled `CONFIG_XIAOMI_KEYBOARD=m` and modular Qualcomm BMS battery/charging drivers for `aarch64`.
- **Packaging Parity**: Streamlined tree structure to match upstream mainline and PocketBlue conventions.
- **RPM & SRPM Generation**: Fully verified `dist-configs` and `dist-srpm` builds for Fedora 41+ (`.sm8150.fc41`).

---

## Building

```bash
# Build Fedora ARK configs
make -C redhat dist-configs

# Build Source RPM (.src.rpm)
make NO_CONFIGCHECKS=1 DIST=.fc41 UPSTREAMBUILD_GIT_ONLY=0 dist-srpm
```
