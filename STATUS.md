# Xiaomi Pad 5 (nabu) Hardware Support Status

This document tracks the hardware support status and driver implementation details for the Xiaomi Pad 5 (**nabu**, Qualcomm Snapdragon 860 / SM8150-AC) on the **Linux 7.3** kernel branch (`nabu/7.3.y`).

---

## Hardware Support Matrix (Linux 7.3)

| Category | Component / Hardware | Status | Driver & Technical Notes |
| :--- | :--- | :---: | :--- |
| **Display** | 2.5K WQHD+ LCD (2560x1600 @ 120Hz / 60Hz) | ✅ Working | Novatek NT36523 DSI panel, 120Hz/60Hz seamless DFPS runtime switching, DPU line-scheduled VFP timing |
| **Touch** | Capacitive Multi-touch & Pen Sync | ✅ Working | Novatek NT36523 SPI touchscreen (`novatek/novatek_nt36523_fw.bin`), 10-point touch, touch sync preserved across 60Hz blank/wake |
| **Backlight** | KTZ8866 Backlight Controller | ✅ Working | 26.4V OVP, 23mA full scale, 2048-entry non-linear exponential brightness curve (flicker-free at 60Hz/120Hz) |
| **Graphics** | Adreno 640 GPU 3D Acceleration | ✅ Working | Mesa Turnip Vulkan 1.3 (`0x6040001`), Freedreno OpenGL 4.6, GMU firmware v2.0.261 |
| **Video Decode** | Iris/Venus VPU Hardware Decoding | ✅ Working | `iris_driver` on `aa00000.video-codec` (`/dev/video0`, `/dev/video1`), native 7.3 VPU5 dual-core architecture, VA-API browser acceleration |
| **Audio (Speakers)**| Quad CS35L41 Stereo Amplifiers | ✅ Working | 4x Cirrus Logic CS35L41 over I2S/TDM (`QUAT_TDM_RX_0`), ALSA UCM + PipeWire |
| **Audio (Headphones)**| Wired Type-C / 3.5mm Headphone Jack | ✅ Working | Dedicated `MultiMedia3` frontend + `SLIMBUS_0_RX <-> AIF1_PB` DAI-link on WCD9340 with ADSP port recovery |
| **Audio (Mic)** | Built-in Multi-Microphone Array | ✅ Working | Qualcomm WCD9341 on `hw:0,1` (MultiMedia2), clean 16-bit 48kHz capture |
| **Camera (Rear)**| 13MP OmniVision OV13B10 | ✅ Working | I2C `4-0010` via CCI, `msm_csiphy0` on CAMSS, CN3927 VCM autofocus, 4208x3120 capture |
| **Camera (Front)**| 8MP OmniVision OV8856 | ✅ Working | I2C `5-0010` via CCI, `msm_csiphy1`, native `SBGGR10_1X10` format alignment |
| **Flash / Torch** | Dual Rear LED Flash | ✅ Working | PM8150L flash controller (`/sys/class/leds/white:flash`), controllable via `nabu-torch` |
| **Wireless** | Wi-Fi 5 (802.11ac 2.4/5GHz) | ✅ Working | Qualcomm WCN3990 via `ath10k_snoc`, persistent MAC via `nabu-pmac`, `wireless-regdb` DFS support |
| **Bluetooth** | Bluetooth 5.0 | ✅ Working | Qualcomm WCN3990 via BlueZ (`hci0`), UART interface, power-off idle, geni sleep IRQ mask |
| **Sensors** | Accelerometer & Gyroscope | ✅ Working | SLPI/SSC over FastRPC (`/dev/fastrpc-sdsp`), SDSP high-IOVA alias mapping & PDR gating |
| **Screen Rotation**| Automatic Screen Rotation | ✅ Working | Driven by `iio-sensor-proxy` + `nabu-tablet-mode.service` (`/dev/input/event6`) |
| **Auto-Brightness**| Ambient Light Sensor (ALS) | ✅ Working | SLPI light sensor over QRTR/IIO, handled by `nabu-autobrightness` / `gsd-power` |
| **Stylus (Input)** | Xiaomi Smart Pen (Drawing & Input) | ✅ Working | `NVTCapacitivePen` (`/dev/input/event4`), 4096 pressure levels, tilt, barrel buttons |
| **Stylus (Charging)**| Wireless Magnetic Pen Charging | ✅ Working | Renesas IDTP9418 wireless charger (`/sys/class/power_supply/idtp9418`), I2C `3-003b` |
| **Accessories** | Magnetic Pogo Keyboard Cover | ✅ Working | Dedicated DWC3 host controller (`&usb_2` / `a800000.usb`), preserved pinctrl across display blank |
| **Battery & Charging**| 33W Fast Charging & PM8150B BMS | ✅ Working | LN8000 33W direct charge pump + PM8150B coordinator, accurate USBIN telemetry & 2:1 ratio, 5s CC sticky window |
| **Sleep** | S2idle Suspend & Resume | ✅ Working | S2idle deep sleep (`0x11`), 300ms power-key hardware grace window, panel/touch wake-up |
| **USB** | USB Type-C 2.0 & OTG | ✅ Working | DWC3 OTG controller (`&usb_1` / `a600000.usb`), high-speed D+/D- role switching |
| **External Display**| USB DisplayLink Output | ✅ Working | Supported via DisplayLink USB docks (`evdi` module + DisplayLink daemon) |
| **External Display**| USB-C DisplayPort Alt-Mode | ❌ Hardware N/A | SoC USB 3.0 lines lack physical board routing to the Type-C port |

---

## Linux 7.3 Architecture & Implementation

The Xiaomi Pad 5 (nabu) hardware stack is integrated onto the pristine upstream **Linux 7.3.3** baseline (branch `nabu/7.3.y`).

### 1. Display, Refresh Rate & Backlight
- **60Hz Touch Sync Across Blank**: Restored vendor `nabu_sync_pen_direct()` DCS sequence on every panel unblank. Prevents NT36523 register `0x23` loss at 60Hz and stops WDT MCU SRAM reflash races during screen sleep.
- **Dynamic Refresh Rate (120Hz ↔ 60Hz DFPS)**:
  - DSI host pins `VACTIVE` to base porch layout, preventing timing drift (172 → 2930 lines).
  - DSI shadow timing flush (`0x1E4`) and line-scheduled DMA delivery timed specifically inside the Vertical Front Porch (VFP).
  - Eliminates frame duplication and visual tearing when switching refresh rates.
- **KTZ8866 Backlight Driver**:
  - Overvoltage Protection (OVP) set to 26.4V and full-scale current configured to 23mA.
  - Implemented the vendor 2048-entry non-linear exponential brightness curve, preventing boost converter low-current ripple and screen flicker at 60Hz.

### 2. Battery & Fast Charging
- **LN8000 33W Direct Fast Charging**: Set `qcom,required-authenticators = <0>` in Device Tree to unlock the LN8000 direct charge pump coordinator.
- **VBUS Oscillation Prevention**: Added a 5-second CC sticky window during direct-charge transitions to eliminate the 3.3-second attach/detach loop caused by load dips.
- **USB-PD Charger Compatibility**: Implemented retry caps and fallback in `qcom_smb5.c` to prevent 0A charging stalls on Type-C chargers skipping legacy BC1.2.
- **Pack IR Drop Compensation**: Current capped under 900mA before declaring battery full, preventing false 100% full reporting at low battery (~12%).
- **Low-Voltage Protection**: Direct charge withheld below 3.2V; PM8150B safely handles trickle/pre-charge until safe voltage is reached.

### 3. Audio & DSP
- **Wired Headphone Playback**: Added dedicated `MultiMedia3` frontend and `SLIMBUS_0_RX <-> AIF1_PB` DAI-link in DTS for the WCD9340 headphone output.
- **AFE Port Recovery**: Intercepts `ADSP_EALREADY` in `q6afe-dai.c` to force port stop and resync, resolving the issue where wired headphones could only play once per boot.
- **Stream Close Handling**: Grouped `ASM_DATA_CMD_WRITE_V2` and `ASM_DATA_CMD_READ_V2` in `q6asm.c` to discard trailing in-flight buffer acknowledgments cleanly without dmesg errors.
- **Speakers & Mic**: Quad CS35L41 amplifiers over I2S/TDM and WCD9341 24-bit mic capture.

### 4. Iris VPU Hardware Video Decode & Encode
- SM8150 Iris VPU5 dual-core architecture ported to native Linux 7.3:
  - Dual GDSC power domains (`mvs0` and `mvs1`) managed with asynchronous power sequencing.
  - Dual clock supplies (`vcodec0_core` and `vcodec1_core`) scaled synchronously with OPP levels.
  - Opp-table voltage ordering deferred until both GDSC power domains are operational.
  - Zeroed HFI command and response ring memory buffers to avoid firmware assertion errors.
  - Runtime PM forbid and ISR interrupt clear resolving `VIDIOC_REQBUFS` failures.
  - Full hardware decode (`/dev/video0`, `/dev/video16`) and encode (`/dev/video1`, `/dev/video17`) verified (H.264 10.6x, HEVC 13.9x).

### 5. Sleep, Power Management & System Stability
- **S2idle Power Button Grace Window**: Restored a 300ms hardware timer grace window (`CONFIG_SUSPEND_S2IDLE_WAKE_GRACE`) in the PM core and hooked `pm_s2idle_set_wake_irq` for the PM8941 power key, eliminating instant wakeups upon button release.
- **Keyboard Cover Pinctrl**: Preserved active pin control across display blanking in `xiaomi_keyboard.c`, eliminating I2C bus stalls when the screen turns off.
- **UFS Storage**: 8-row frequency table (`freq-table-hz`) locking gears `<37500000 300000000>` for Samsung UFS stability; inline crypto engine bypass (`/delete-property/ qcom,ice;`).
- **Zap Shader Early Bundling**: Bundle `/lib/firmware/qcom/sm8150/xiaomi/nabu/a640_zap.mbn` in initramfs via `mkinitcpio.conf` `FILES=`.
- **Sensors**: FastRPC SLPI sensor PDR registration with SDSP high-IOVA alias mapping for auto-rotation and ambient light sensing.

### 6. Packaging & UKI
- **Package**: `linux-nabu-7.3.3-3` (`arch=('aarch64')`, kernel release `7.3.3-3-krjy`).
- **Unified Kernel Image (UKI)**: Automated UKI generation via `ukify` with DTB symlink and early firmware bundling in `packages/linux-nabu-73/linux-nabu.install`.
