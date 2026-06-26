#include "hw/hw.h"
#include <Arduino.h>
#include <Wire.h>

static void die(const char* what) {
  Serial.printf("hwInit FAIL: %s\n", what);
  while (1) delay(1000);
}

void hwInit() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== claude-buddy waveshare boot ===");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  if (!hwExpanderInit())  die("expander");
#if BOARD_LCD_RST_VIA_PMU
  // Optional PMU reset path retained for compile-time HAL compatibility.
  if (!hwPowerInit())     die("power");
  // ALDO3 power-cycle resets the panel (50 ms low between two highs).
  // s_pmu.enableALDO3() in powerInit left it enabled; toggle it here.
  hwPmuRef()->disableALDO3();
  delay(50);
  hwPmuRef()->enableALDO3();
  delay(50);
#endif
  // Toggles touch and display reset lines as defined by the board header.
  hwExpanderResetSequence();
  if (!hwDisplayInit())   die("display");
#if !BOARD_LCD_RST_VIA_PMU
  if (!hwPowerInit())     die("power");
#endif
  if (!hwInputInit())     die("input");
  if (!hwImuInit())       die("imu");
  if (!hwRtcInit())       die("rtc");
  if (!hwAudioInit())     die("audio");

  Serial.println("hwInit OK");
}
