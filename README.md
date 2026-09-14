# Claude Desktop Buddy for Waveshare 1.85C

> A desk companion for makers using Claude Desktop developer mode: this firmware adapts Anthropic's Hardware Buddy reference implementation to the **Waveshare ESP32-S3-Touch-LCD-1.85C V2 / Rev2.0**, so an ESP32-S3 can display Claude activity and accept on-device approval decisions over encrypted Bluetooth LE.

![Photograph of the Waveshare 1.85C hardware target running this firmware](image.jpg)

This is a **single-board fork**, not a general Hardware Buddy distribution. It retains the upstream Nordic UART Service protocol, pairing flow, and desktop Hardware Buddy integration while replacing the original M5StickCPlus-specific hardware layer for the round 1.85C board.

## At a glance

| Item | Evidence-backed status |
| --- | --- |
| Target | Waveshare ESP32-S3-Touch-LCD-1.85C V2 / Rev2.0 only |
| Desktop integration | Claude Desktop developer-mode Hardware Buddy flow; protocol documented in [REFERENCE.md](REFERENCE.md) |
| Visual evidence | The repository photograph above is `image.jpg` |
| Firmware build | A no-device PlatformIO compile workflow is included in [`.github/workflows/firmware-build.yml`](.github/workflows/firmware-build.yml). Its first default-branch run is still required before displaying a passing badge. |
| Local verification for this documentation update | **Not run**: PlatformIO was not installed in the local workspace; no board was connected. |
| Physical-device verification | **Not claimed**. Use the checklist in [Hardware validation](#hardware-validation) after flashing a real V2 board. |

There is no hosted demo: the useful artefact is the firmware running on the specified physical board.

## What changed for this board

The port is deliberately more than a board name or pin remap. It replaces the target selection with one 1.85C environment and adds a board capability header, then uses those capabilities to route the shared firmware to the correct display, input, power, audio, RTC, and BLE paths.

| Constraint on the 1.85C V2 | Implementation decision |
| --- | --- |
| 360 × 360 **round** ST77916 QSPI panel | Render a 300 × 300 logical canvas into a full physical framebuffer, centred with a 40 px safe inset; draw the attention alert as a circular border rather than relying on corner UI. See [`src/hw/display.cpp`](src/hw/display.cpp). |
| CST816 touch at I2C `0x15` | Use an interrupt-assisted CST816 reader and map physical coordinates back to the logical canvas, including the user-selected UI rotation. See [`src/hw/input.cpp`](src/hw/input.cpp). |
| BOOT is GPIO0; the side control is a latching GPIO6 slide switch | BOOT is the primary action; the side switch is secondary and must be returned before it can be used again. The reset control is a hardware reset line, not a firmware input. |
| LCD and touch reset are behind a TCA9554 I/O expander | Initialise the expander and pulse both reset lines before display/input setup. See [`src/hw/expander.cpp`](src/hw/expander.cpp). |
| The target does not use the AXP2101 path | Compile that path out for this board. The current firmware consequently does not provide board-specific battery/USB telemetry; do not treat battery-dependent screen behaviour as verified. |
| Claude activity may contain sensitive transcript snippets or tool hints | Require LE Secure Connections with MITM bonding and display a six-digit passkey on the device. See [`src/ble_bridge.cpp`](src/ble_bridge.cpp). |

For a traceable change inventory, including the exact baseline and commit range inspected, see [the upstream contribution delta](docs/UPSTREAM_DELTA.md). It is preparation for an upstream discussion, **not** an upstream submission.

## Architecture

```text
Claude Desktop (developer mode)
        |  encrypted BLE Nordic UART Service; newline-delimited JSON
        v
ble_bridge.cpp  <->  data.h / main.cpp state and UI routing
        |                         |
        |                         +-- ASCII buddies or streamed GIF characters
        v
hardware abstraction layer (src/hw/)
        |-- ST77916 QSPI display + PWM backlight
        |-- CST816 touch + GPIO0 BOOT + GPIO6 side switch
        |-- TCA9554 reset expander, PCF85063 RTC, ES8311 audio
        v
Waveshare ESP32-S3-Touch-LCD-1.85C V2
```

The desktop sends snapshots, permission prompts, optional usage data, time, owner information, and character-pack transfers. The device sends status and permission/question responses. `REFERENCE.md` defines the protocol surface; the board layer is the local adaptation.

## Build and flash

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/), connect the intended board by USB, then compile:

```bash
pio run -e waveshare-esp32s3-touch-lcd-1-85c-v2
```

Upload only after the compile succeeds and the connected device has been identified as the target board:

```bash
pio run -e waveshare-esp32s3-touch-lcd-1-85c-v2 -t upload
```

If the device contains unrelated firmware, erase it before uploading:

```bash
pio run -e waveshare-esp32s3-touch-lcd-1-85c-v2 -t erase
pio run -e waveshare-esp32s3-touch-lcd-1-85c-v2 -t upload
```

`LittleFS` may format itself on first boot when its partition is empty or belongs to different firmware. That can remove stored character files; it is expected behaviour, not a recovery process for arbitrary data.

## Pair and use

1. In Claude Desktop, enable **Help → Troubleshooting → Enable Developer Mode**.
2. Open **Developer → Open Hardware Buddy…**, choose **Connect**, then select the advertised `Claude-XXXX` device.
3. Type the six-digit code shown on the board into the desktop pairing prompt.

The firmware exposes the same protocol family as the upstream reference implementation. The desktop feature is developer-only and is not represented here as a supported Anthropic product feature.

### Controls

| Control | Normal view | Approval/question view |
| --- | --- | --- |
| BOOT tap | move through screens; select the next question option | approve a permission / move question selection |
| BOOT hold | open or close the menu | menu action according to the current overlay |
| Side slide switch | scroll or change page | deny a permission / confirm the selected question option |
| Touch | pet, scroll, page, or menu interaction depending on the view | upper/lower approval region, or a visible question option |

The slide switch is latching. Move it back out of its active position after use. The board reset button resets hardware; the firmware cannot read it as an application control.

## Quality and validation

### Automated compile gate

The GitHub Actions workflow runs `pio run -e waveshare-esp32s3-touch-lcd-1-85c-v2` on pushes and pull requests. It compiles firmware only—there is no upload, serial port, or physical-device dependency. PlatformIO Core is pinned in the workflow and the platform URL in `platformio.ini` identifies the PIO Arduino platform release used by this project.

The library declarations still use compatible version ranges, so this is a compile regression gate rather than a byte-for-byte dependency lock. A passing workflow proves the selected source and resolved toolchain compile; it does **not** prove hardware, pairing, display timing, touch calibration, or desktop compatibility.

### Hardware validation

No physical run was performed for this README/CI change. Before calling a release or demo ready, record the board revision, firmware commit, PlatformIO output, and outcome for:

- clean boot after upload, including the round display, backlight, and reset-expander sequence;
- CST816 tap, drag, release, and rotated-coordinate behaviour;
- BOOT and side-switch input, including switch re-arm behaviour;
- pairing with a real Claude Desktop developer-mode session, six-digit passkey display, reconnect, and bond reset;
- permission approve/deny and question-answer messages end-to-end;
- LittleFS first-boot/character-pack behaviour; and
- the board-specific power, sleep, RTC, and audio paths that the intended deployment uses.

## Current limitations

- This firmware is scoped to the 1.85C V2 / Rev2.0 board. Other Waveshare boards need their own port and validation.
- It is a maker/developer integration, not an official supported desktop product workflow.
- The code requires a real board for functional testing; GitHub Actions cannot exercise display, touch, BLE pairing, radio range, or connected desktop behaviour.
- Current 1.85C configuration disables the AXP2101 path, so battery/USB telemetry is not provided by `hwBattery()` for this target.
- Bluetooth payloads can include activity summaries and tool hints. Treat the board as a local, trusted device and do not rely on it for a security boundary beyond the implemented BLE pairing model.

## Project layout

```text
src/
  boards/       board capabilities and pin mapping for the 1.85C V2
  hw/           display, input, reset expander, audio, RTC, power abstractions
  main.cpp      application state machine and on-device interaction routing
  ble_bridge.*  encrypted Nordic UART Service bridge
  buddies/      ASCII companion animations
  character.*   GIF character playback from LittleFS
lib/            vendored board-support libraries
characters/     example character pack
docs/           manual, upstream-delta record, and validation context
```

## Attribution and licensing

This repository is a fork of [Anthropic's `claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy), whose initial commit is present in this repository history. The root [MIT license](LICENSE) retains Anthropic's copyright notice.

Vendored libraries and artwork retain their own terms: [`lib/Arduino_DriveBus/LICENSE`](lib/Arduino_DriveBus/LICENSE) covers the Arduino_DriveBus copy, and [`characters/bufo/README.md`](characters/bufo/README.md) documents the separate third-party artwork attribution. Do not assume those assets inherit the root MIT licence.
