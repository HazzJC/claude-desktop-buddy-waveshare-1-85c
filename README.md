# claude-desktop-buddy for Waveshare ESP32-S3-Touch-LCD-1.85C V2

<img src="image.jpg" width="400" />

This fork targets the
[Waveshare ESP32-S3-Touch-LCD-1.85C V2 / Rev2.0](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C)
only. It adapts the Claude Hardware Buddy firmware to the board's 1.85"
round 360x360 ST77916 QSPI LCD, CST816 touch controller, BOOT button, and
side slide switch.

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so the device can show permission prompts, recent
messages, status, and a little desktop buddy.

The BLE wire protocol is unchanged from
[anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy):
same pairing flow, same desktop apps, and same Hardware Buddy window.

## Board Target

- Board: Waveshare ESP32-S3-Touch-LCD-1.85C V2 / Rev2.0
- MCU: ESP32-S3
- Display: 360x360 round ST77916 QSPI LCD with PWM backlight
- Touch: CST816 at I2C address `0x15`
- Build env: `waveshare-esp32s3-touch-lcd-1-85c-v2`
- Logical UI canvas: 300x300, centered in the physical panel with a safe
  inset for the round bezel

The reset button is wired to the ESP32 reset line, so firmware cannot use it
as an app control. The side slide switch is wired to GPIO6 and is treated as
the secondary control when it is moved into its active position.

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then build and upload:

```bash
pio run -e waveshare-esp32s3-touch-lcd-1-85c-v2 -t upload
```

If the board has unrelated firmware on it, erase it first:

```bash
pio run -e waveshare-esp32s3-touch-lcd-1-85c-v2 -t erase
pio run -e waveshare-esp32s3-touch-lcd-1-85c-v2 -t upload
```

LittleFS auto-formats on first boot if the partition is empty or from a
different firmware.

## Pairing

Enable developer mode in Claude Desktop with **Help -> Troubleshooting ->
Enable Developer Mode**. Then open **Developer -> Open Hardware Buddy...**,
click **Connect**, and select the device from the list. It advertises as
`Claude-XXXX`.

The device shows a 6-digit passkey on screen. Type it into the desktop prompt
to complete Bluetooth pairing. Once paired, the bridge reconnects whenever
both sides are awake.

## Controls

| Control | Normal | Pet | Info | Approval |
| --- | --- | --- | --- | --- |
| BOOT tap | next screen | next screen | next screen | approve |
| BOOT hold | open menu | open menu | open menu | open menu |
| Side switch | scroll transcript | next page | next page | deny |
| Touch | tap buddy / scroll area | tap top-right for page | tap top-right for page | tap upper half to approve, lower half to deny |

Swipe right-to-left advances through the flat page chain: Normal, Pet pages,
then Info pages. Swipe left-to-right goes back through the same chain.

For question prompts, the screen shows the prompt plus up to four large
touch-selectable answers. Tap an answer to choose it. As a fallback, BOOT
cycles the highlighted answer and the side switch confirms it.

The menu and settings panels also have touch controls at the bottom of the screen:

- `^` selects the previous row
- `v` selects the next row
- `OK` confirms the highlighted row

Settings rows are direct tap actions. The `rotate` row cycles the whole UI and
touch mapping through `0`, `90`, `180`, and `270` degrees. The `widgets` row
cycles home usage widgets through `off`, `simple`, and `fun`. `cancel` closes
Settings without undoing any changes already made.

Because the side switch is latching, move it into the active position to send
the secondary action, then move it back before using it again.

The reset button performs a hardware reset only. It is not available to the
firmware as a readable button.

If Bluetooth pairing gets stuck, use **Settings -> reset -> pair reset** to
clear the device's stored BLE bonds, then forget the old `Claude-XXXX` entry
from the desktop Bluetooth settings before pairing again.

## Sleep And Wake

- USB plugged in: screen stays awake
- Battery + clock visible: screen sleeps after 5 minutes
- Battery + other screens: screen sleeps after 30 seconds
- Approval prompt visible: screen stays awake

Any BOOT press, side-switch action, or screen tap wakes the display.

## Per-State Animations

| State | Trigger | Feel |
| --- | --- | --- |
| `sleep` | bridge not connected | eyes closed, slow breathing |
| `idle` | connected, nothing urgent | blinking, looking around |
| `busy` | sessions actively running | sweating, working |
| `attention` | approval pending | alert, red top indicator |
| `celebrate` | level up every 50K tokens | confetti, bouncing |
| `dizzy` | motion event | spiral eyes, wobbling |
| `heart` | approved quickly or tapped | floating hearts |

Eighteen ASCII species are included. **Settings -> pet** cycles through
them and persists the choice in NVS.

## Home Usage Widgets

If the desktop bridge sends optional `usage` data, the home screen can show
4-hour session and weekly usage. Simple mode shows compact percentage bars
with reset countdowns. Fun mode shows small water bottles whose fill indicates
usage remaining. Missing usage data hides the widgets.

## Pet Stats

- `level`: cumulative output tokens divided into 50K-token levels.
- `fed`: progress through the current 50K-token level.
- `mood`: based on median approval response speed; frequent denials reduce it.
- `energy`: refills after a face-down nap, then drains by one tier every 2 hours.
- `day`: bridge-provided `tokens_today`, output tokens since local midnight.

## Custom GIF Characters

If you want a custom GIF character instead of an ASCII buddy, drag a character
pack folder onto the drop target in the Hardware Buddy window. The desktop app
streams it over BLE and the device switches to GIF mode live. **Settings ->
reset -> delete char** returns to ASCII mode.

A character pack is a folder with `manifest.json` and 96 px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. GIFs should be 96 px wide;
up to about 140 px tall keeps the character above the HUD. The whole pack must
fit in the LittleFS partition.

## Project Layout

```text
src/
  main.cpp           -- loop, state machine, UI screens
  buddy.{cpp,h}      -- ASCII species dispatch and render helpers
  buddies/           -- one file per species
  character.{cpp,h}  -- GIF decode and render
  ble_bridge.{cpp,h} -- Nordic UART service and line-buffered TX/RX
  data.h             -- wire protocol, JSON parse, CJK matrixifier
  stats.h            -- NVS-backed settings and buddy stats
  boards/            -- Waveshare 1.85C V2 pin and capability header
  hw/                -- display, input, power, imu, rtc, audio, expander
lib/
  ES8311/            -- vendored Espressif codec driver
  Adafruit_XCA9554/  -- vendored TCA9554 expander driver
characters/          -- example GIF character packs
tools/               -- generators and converters
docs/                -- design notes and implementation plans
```

## Availability

The BLE API is only available when the Claude desktop apps are in developer
mode. It is intended for makers and developers and is not an officially
supported product feature.
