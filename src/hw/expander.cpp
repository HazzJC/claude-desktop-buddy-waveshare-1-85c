// src/hw/expander.cpp
#include "hw/expander.h"
#include "hw/pins.h"
#include <Wire.h>
#include <Arduino.h>

#if BOARD_HAS_TCA9554

Adafruit_XCA9554 g_expander;

bool hwExpanderInit() {
  if (!g_expander.begin(0x20)) return false;
  g_expander.pinMode(EXIO_LCD_RESET,  OUTPUT);
  g_expander.pinMode(EXIO_TP_RESET,   OUTPUT);
#ifdef EXIO_DSI_PWR_EN
  g_expander.pinMode(EXIO_DSI_PWR_EN, OUTPUT);
#endif
#ifdef EXIO_AXP_IRQ
  g_expander.pinMode(EXIO_AXP_IRQ,    INPUT);
#endif
  return true;
}

void hwExpanderResetSequence() {
  g_expander.digitalWrite(EXIO_LCD_RESET,  LOW);
  g_expander.digitalWrite(EXIO_TP_RESET,   LOW);
#ifdef EXIO_DSI_PWR_EN
  g_expander.digitalWrite(EXIO_DSI_PWR_EN, LOW);
#endif
  delay(20);
  g_expander.digitalWrite(EXIO_LCD_RESET,  HIGH);
  g_expander.digitalWrite(EXIO_TP_RESET,   HIGH);
#ifdef EXIO_DSI_PWR_EN
  g_expander.digitalWrite(EXIO_DSI_PWR_EN, HIGH);
#endif
  delay(20);
}

bool hwExpanderAxpIrqLow() {
#ifdef EXIO_AXP_IRQ
  return g_expander.digitalRead(EXIO_AXP_IRQ) == 0;
#else
  return true;
#endif
}

#else  // No TCA9554

bool hwExpanderInit() {
#if !BOARD_LCD_RST_VIA_PMU
  pinMode(PIN_LCD_RESET, OUTPUT);
#endif
  pinMode(PIN_TP_RESET, OUTPUT);
  return true;
}

void hwExpanderResetSequence() {
#if !BOARD_LCD_RST_VIA_PMU
  digitalWrite(PIN_LCD_RESET, LOW);
#endif
  digitalWrite(PIN_TP_RESET, LOW);
  delay(20);
#if !BOARD_LCD_RST_VIA_PMU
  digitalWrite(PIN_LCD_RESET, HIGH);
#endif
  digitalWrite(PIN_TP_RESET, HIGH);
  delay(20);
}

// Returning true lets the optional AXP polling path check PEK registers
// without a dedicated interrupt pin.
bool hwExpanderAxpIrqLow() {
  return true;
}

#endif
