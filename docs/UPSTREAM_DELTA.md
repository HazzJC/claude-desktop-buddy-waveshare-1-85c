# Upstream contribution delta: Waveshare 1.85C port

## Purpose and boundary

This is a concise, auditable description of the delta from the upstream Hardware Buddy reference implementation. It prepares a possible future upstream conversation; it does **not** open an issue, create a pull request, or claim that the changes are suitable for upstream acceptance.

The upstream repository's own contribution guidance says board ports belong in forks. That makes this document an explanation of the fork, not a request for Anthropic to support the board.

## Provenance examined

| Item | Recorded evidence |
| --- | --- |
| Upstream source | [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy), also linked by the original `README.md` in this repository history |
| Local history baseline | `8ac960d` — `Initial release` (2026-04-09), authored by Anthropic (Felix Rieseberg) |
| Intermediate multi-board work (not this author's) | `a280c64..61a0ce9` — roughly 100 commits by Yadong Xie, with `eMUQI` and `Wulu`, porting the reference firmware to four AMOLED Waveshare boards (1.8″, 1.75C, 2.16″ S3, 2.16″ C6). None of those targets is the 1.85C V2 round LCD board this fork ships. This range built the `src/boards/` dispatcher pattern, the `src/hw/` abstraction split, and the TCA9554/AXP2101/IMU plumbing this fork's port reuses. |
| This author's commit range | `a278ba4..2975a99` — `Port firmware to Waveshare 1.85C touch LCD`, `Keep Bluetooth always discoverable`, `Add guided demo and interaction wake hold` (2026-06-26) |
| Current source inspected | `2975a99` — `Add guided demo and interaction wake hold` (2026-06-26) |
| Remote configuration at audit | Only `origin` is configured; no separate `upstream` remote was present |
| Licence boundary | Root MIT licence is copyright Anthropic, PBC; bundled libraries and `bufo` art carry separate notices |

At `2975a99`, `git diff --stat 8ac960d..HEAD` records 90 changed paths, 10,072 insertions, and 815 deletions across the *entire* repository history since the initial release. That figure is dominated by the ~100-commit, multi-board AMOLED porting effort by other contributors described above — **it is not a measure of this author's own contribution.** This author's own commits are the much smaller, separately-called-out range below.

## Board-specific implementation delta (this author's own work)

This author's target-port commit range (`61a0ce9..2975a99` — i.e. everything after the last commit not authored by HazzJC) changes 26 paths: 1,524 insertions and 1,132 deletions, entirely within three commits (`a278ba4`, `9824938`, `2975a99`) by this repository's owner. The meaningful board adaptation is:

1. **Single environment and board selector.** `platformio.ini` removes the previous multi-board environments and selects `waveshare-esp32s3-touch-lcd-1-85c-v2` with the ESP32-S3, OPI PSRAM, QIO flash, 8 MB partition layout, LittleFS, and `ble_bridge.cpp` source selection.
2. **Board declaration.** `src/boards/board_waveshare_esp32s3_touch_lcd_1_85c_v2.h` defines the 360 × 360 ST77916 QSPI panel, I2C bus, CST816 interrupt, GPIO0 BOOT input, GPIO6 side switch, I2S audio pins, TCA9554 reset pins, RTC capability, display offsets, and the board-specific capability flags.
3. **Round-screen rendering.** `src/hw/display.cpp` selects `Arduino_ST77916`, uses the ST77916 initialisation table, corrects RGB565 pixel format to `0x55`, controls the PWM backlight, copies the 300 × 300 logical canvas into a centred 360 × 360 physical framebuffer, and renders the attention state as a round border.
4. **Input and interaction remapping.** `src/hw/input.cpp` adds CST816 I2C reads at `0x15`, touch interrupt handling, physical-to-logical coordinate conversion, and UI-rotation mapping. It assigns BOOT as the primary input and the latching side switch as secondary.
5. **Reset/power capability routing.** `src/hw/expander.cpp`, `src/hw/power.cpp`, and `src/hw/hw.cpp` route reset through the TCA9554 and compile out AXP2101-only handling for this board.
6. **Application behaviour adapted to available inputs.** `src/main.cpp`, `src/data.h`, `src/buddy.*`, `src/character.*`, and `src/stats.h` contain the board-port integration and follow-on interaction/wake behaviour. The two post-port commits keep BLE advertising discoverable and add guided demo/interaction wake hold.

## Reusable versus fork-specific work

| Candidate reusable upstream material | Fork-specific material that should remain local |
| --- | --- |
| General protocol documentation corrections, cross-board capability abstractions, or a narrowly applicable BLE compatibility fix—each only after isolated review and verification | Waveshare pin map, panel initialisation, reset sequencing, screen geometry, touch calibration, control choices, specific UI layout, bundled hardware libraries, and 1.85C test evidence |

Before proposing any reusable change, isolate it from the board port, verify it against the upstream supported target(s), check licence/third-party provenance, and get human approval for external communication.

## Evidence still needed before external discussion

- A passing default-branch compile workflow for this exact environment.
- A recorded physical test run on an identified 1.85C V2 / Rev2.0 board.
- A decision about which small, independently valuable change—if any—belongs upstream.
- A manual review of the final diff and attribution obligations.

Until then, the correct status is: **maintained as a board-specific fork; no upstream contribution submitted.**
