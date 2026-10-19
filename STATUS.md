# Xiaomi Pad 5 (nabu) Hardware Support Status

This document tracks the hardware support status and driver implementation details for the Xiaomi Pad 5 (**nabu**, Qualcomm Snapdragon 860 / SM8150-AC) across kernel branches (`6.14`, `6.16`, `6.18`, `7.2`, and the current `7.3` branch).

---

## Hardware Support Matrix

| Category | Component / Hardware | Linux 6.14 | Linux 6.16 | Linux 6.18 | Linux 7.2 | Linux 7.3 | Driver & Technical Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| **Display** | 2.5K WQHD+ LCD (2560x1600 @ 120Hz) | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Novatek NT36523 DSI panel, 120Hz refresh rate, DPU vsync & KMS active |
| **Touch** | Capacitive Multi-touch | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Novatek NT36523 SPI touchscreen (`novatek/novatek_nt36523_fw.bin`), 10-point touch |
| **Graphics** | Adreno 640 GPU 3D Acceleration | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Mesa Turnip Vulkan 1.3 (`0x6040001`), Freedreno OpenGL 4.6, GMU firmware v2.0.261 |
| **Video Decode** | Iris/Venus VPU Hardware Decoding | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | `iris_driver` on `aa00000.video-codec` (`/dev/video0`, `/dev/video1`), native 7.3 VPU2 architecture |
| **Audio (Speakers)**| Quad CS35L41 Stereo Amplifiers | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | 4x Cirrus Logic CS35L41 over I2S/TDM (`QUAT_TDM_RX_0`), ALSA UCM + PipeWire — known high-volume speaker cracking under investigation |
| **Audio (Mic)** | Built-in Multi-Microphone Array | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Qualcomm WCD9341 on `hw:0,1` (MultiMedia2), clean 16-bit 48kHz capture |
| **Camera (Rear)**| 13MP OmniVision OV13B10 | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | I2C `4-0010` via CCI, `msm_csiphy0` on CAMSS, CN3927 VCM autofocus, 4208x3120 capture |
| **Camera (Front)**| 8MP OmniVision OV8856 | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | I2C `5-0010` via CCI, `msm_csiphy1`, native `SBGGR10_1X10` format alignment |
| **Flash / Torch** | Dual Rear LED Flash | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | PM8150L flash controller (`/sys/class/leds/white:flash`), controllable via `nabu-torch` |
| **Wireless** | Wi-Fi 5 (802.11ac 2.4/5GHz) | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Qualcomm WCN3990 via `ath10k_snoc`, persistent MAC via `nabu-pmac` |
| **Bluetooth** | Bluetooth 5.0 | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Qualcomm WCN3990 via BlueZ (`hci0`), UART interface, power-off idle, geni sleep IRQ mask |
| **Sensors** | Accelerometer & Gyroscope | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | SLPI/SSC over FastRPC (`/dev/fastrpc-sdsp`), SDSP high-IOVA alias mapping & PDR gating |
| **Screen Rotation**| Automatic Screen Rotation | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Driven by `iio-sensor-proxy` + `nabu-tablet-mode.service` (`/dev/input/event6`) |
| **Auto-Brightness**| Ambient Light Sensor (ALS) | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | SLPI light sensor over QRTR/IIO, handled by `nabu-autobrightness` / `gsd-power` |
| **Stylus (Input)** | Xiaomi Smart Pen (Drawing & Input) | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | `NVTCapacitivePen` (`/dev/input/event4`), 4096 pressure levels, tilt, barrel buttons |
| **Stylus (Charging)**| Wireless Magnetic Pen Charging | ⚠️ WIP | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Renesas IDTP9418 wireless charger (`/sys/class/power_supply/idtp9418`), I2C `3-003b` |
| **Accessories** | Magnetic Pogo Keyboard Cover | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Dedicated DWC3 host controller (`&usb_2` / `a800000.usb`), 4 USB HID interfaces (`3206:3ffc`) |
| **Battery & Power**| PM8150B Battery Telemetry & QC/PD | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | `pm8150b-charger` + `qcom-battery` fuel gauge, 15W USB-PD / DCP detection, correct sign logic |
| **Sleep** | S2idle Suspend & Resume | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | S2idle deep sleep, Novatek panel/touchscreen wake-up, BT RTS pull-up prevention |
| **USB** | USB Type-C 2.0 & OTG | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | DWC3 OTG controller (`&usb_1` / `a600000.usb`), high-speed D+/D- role switching |
| **External Display**| USB DisplayLink Output | ✅ Working | ✅ Working | ✅ Working | ✅ Working | ✅ Working | Supported via DisplayLink USB docks (`evdi` module + DisplayLink daemon) |
| **External Display**| USB-C DisplayPort Alt-Mode | ❌ Hardware N/A | ❌ Hardware N/A | ❌ Hardware N/A | ❌ Hardware N/A | ❌ Hardware N/A | SoC USB 3.0 lines lack physical board routing to the Type-C port |

---

## Key Subsystem Notes

### 1. Storage & Boot Stability
- **UFS ICE Bypass**: The inline crypto engine phandle stall on the UFS controller is removed (`/delete-property/ qcom,ice;`) to prevent boot hangs and emergency shell timeouts.

---

## Linux 6.18 Port Fixes

### 1. UFS — boot hung during link training
**Problem**: The controller was negotiating gear/timing combinations the Samsung UFS part on this board can't lock, and it was also poking a reset line the board doesn't wire up, plus an inline-crypto phandle that stalled outright. Result: hang during mount, sometimes down to an emergency shell.

**Fix** (device tree only, no driver code):
- `freq-table-hz` — 8-row table, live gears `<37500000 300000000>`, dead gears `<0 0>` so the driver can't select them
- Deleted inherited `resets` / `reset-names`
- Deleted `qcom,ice`, disabled `&ice`

### 2. Memory map — auto-rotation and light sensor crashed the DSP
**Problem**: FastRPC buffers were mapped at an address the SDSP can't reach, so the SMMU raised Translation Fault `0x402` and `sensor_process` died.

**Fix** (`drivers/misc/fastrpc.c`):
- Map an alias address (`dma_addr + (sid << 32)`, SID `0x5a1`) into the same SMMU domain, point `buf->phys` at it
- Wait for `msm/slpi/root_pd` before mapping, so the protection domain exists first

### 3. Display — grey half and vertical seam
**Problem**: The DSI lane routing register was being skipped whenever the PLL enable refcount said it was already on. Upstream 6.18 also added a display-intf reset loop that blanked interfaces the bootloader had already handed over.

**Fix**:
- `dsi_phy_7nm.c` — always program `PLL_SYSTEM_MUXES` — confirmed fixed on hardware
- Dropped the intf reset loop; removed the panel's 90° rotation; dropped `CLK_OPS_PARENT_ENABLE` on the pixel clocks; restored `pm_runtime` around PLL init

---

## Linux 7.2 Port

The full nabu/SM8150 stack was ported forward from the 6.18 baseline onto a pristine upstream **Linux 7.2.9** tree (branch `7.2`, commit `3318bca60`). All of the 6.18 fixes above carry over and every subsystem in the matrix above is working on 7.2.9.

- **Toolchain**: cross-built with `aarch64-linux-gnu-gcc` 15.2.0 / binutils 2.46; 40 merge conflicts resolved
- **Portability & Cleanup fixes**: Migrated out-of-tree drivers to standard `gpiod_*` APIs and removed obsolete `include/linux/of_gpio.h`; corrected DAPM audio speaker routing directly in `sm8150-xiaomi-nabu.dts` without driver workarounds; optimized IDTP9418 pen wireless charging I2C transfers with `regmap_bulk_read/write` and restricted device node permissions (0660); added safe timeout to SLIMbus QMI wait; `sm8150.dtsi` dedup + `uart13`; `q6asm.c` APR const fix; `ov8856.c` label fix; `ath10k/core.c` `#include <linux/hex.h>`; `idtp9418.c` `alarm_start_timer`; `q6asm-dai.c` `.pcm_new`
- **Packaging**: `linux-nabu` 7.2.9-0 (`arch=('aarch64')`, kernel release `7.2.9-0-krjy`) with 774 modules and a matching `linux-nabu-headers` package
- **Verification**: installed kernel image hashes match the local build (`Image` `8592c5d6…` / `Image.gz` `0baed5f9…`); `build/Makefile` reports `VERSION=7 PATCHLEVEL=2 SUBLEVEL=9`
