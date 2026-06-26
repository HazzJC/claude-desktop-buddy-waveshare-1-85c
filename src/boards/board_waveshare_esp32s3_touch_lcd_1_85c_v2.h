#pragma once

// Waveshare ESP32-S3-Touch-LCD-1.85C V2 / Rev2.0.
// Display: 360x360 round ST77916 LCD over QSPI.
#define LCD_W_PHYS  360
#define LCD_H_PHYS  360

// Native render size for the round LCD. This avoids scaling/interpolating
// the finished framebuffer, keeping text and pixel art crisp.
#define BOARD_HW_W        300
#define BOARD_HW_H        300
#define BOARD_SAFE_INSET  40

// QSPI to ST77916, from Waveshare's V2 Arduino example.
#define PIN_LCD_SDIO0  46
#define PIN_LCD_SDIO1  45
#define PIN_LCD_SDIO2  42
#define PIN_LCD_SDIO3  41
#define PIN_LCD_SCLK   40
#define PIN_LCD_CS     21
#define PIN_LCD_TE     18
#define PIN_LCD_BL     5
// LCD reset and touch reset are driven through the TCA9554 expander.

// I2C bus (shared: TCA9554, CST816, PCF85063, ES8311, ES7210, QMI8658)
#define PIN_I2C_SDA   11
#define PIN_I2C_SCL   10

// CST816 touch controller.
#define PIN_TP_INT    4

// I2S to ES8311 codec (V2 pinout).
#define PIN_I2S_MCLK  2
#define PIN_I2S_BCLK  48
#define PIN_I2S_WS    38
#define PIN_I2S_DI    39
#define PIN_I2S_DO    47
#define PIN_PA_CTRL   15

// Buttons
#define PIN_KEY1      0   // BOOT key, active-low
#define PIN_KEY2      6   // Side slide switch, active-low on one throw.

// TCA9554 I2C GPIO expander pin assignments.
#define BOARD_HAS_TCA9554  1
#define EXIO_TP_RESET      0
#define EXIO_LCD_RESET     1

// Capability flags
#define BOARD_HAS_PSRAM            1
#define BOARD_HAS_PCF85063         1
#define BOARD_HAS_QMI8658          0
#define BOARD_HAS_PA_CTRL          1
#define BOARD_HAS_AXP2101          0
#define BOARD_LCD_RST_VIA_PMU      0
#define BOARD_AXP_PWRON_4S_OFF     0
#define BOARD_AXP_ENABLE_AUX_LDOS  0
#define BOARD_DISPLAY_CO5300       0
#define BOARD_DISPLAY_ST77916      1
#define BOARD_DISPLAY_LETTERBOX    0
#define BOARD_DISPLAY_OFFSET_X     ((LCD_W_PHYS - BOARD_HW_W) / 2)
#define BOARD_DISPLAY_OFFSET_Y     ((LCD_H_PHYS - BOARD_HW_H) / 2)
#define BOARD_DISPLAY_SCALE        1
#define BOARD_DISPLAY_PUSH_STREAMED 0
#define BOARD_DISPLAY_FULL_FRAME_1X 1
#define BOARD_DISPLAY_PWM_BACKLIGHT 1
#define BOARD_TOUCH_CST92XX        0
#define BOARD_TOUCH_CST816         1
#define BOARD_BTN_SWAP_AB          0
#define BOARD_BTN_THIRD            0
#define BOARD_KEY1_ACTIVE_HIGH     0
#define BOARD_HAS_KEY2             1
#define BOARD_SINGLE_BUTTON_MENU_CONFIRM 0
#define BOARD_BUDDY_HOME_SCALE 3
#define BOARD_UI_TEXT_SCALE 2

// Defaults required by shared display/input code.
#define BOARD_DISPLAY_SH8601_VENDOR_INIT  0
#define BOARD_CO5300_COL_OFFSET    0
#define BOARD_DISPLAY_ROTATION     0
#define BOARD_CO5300_MADCTL        0

// Credits page
#define BOARD_MODEL_LINE1  "Waveshare ESP32-S3"
#define BOARD_MODEL_LINE2  "Touch LCD 1.85C V2"
