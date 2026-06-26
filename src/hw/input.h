#pragma once
#include <stdint.h>

struct HwBtn {
  bool isPressed;
  bool wasPressed;
  bool wasReleased;
  uint32_t pressedAt;
  bool pressedFor(uint32_t ms);
};

struct HwTouch {
  bool down;
  int16_t x, y;
  bool justPressed;
  bool justReleased;
};

bool hwInputInit();
void hwInputUpdate();
void hwInputSetRotation(uint8_t quarterTurns);

HwBtn& hwBtnA();          // primary control: BOOT button on GPIO0
HwBtn& hwBtnB();          // secondary control: side slide switch on GPIO6
uint8_t hwAxpBtnEvent();  // 0 / 0x02 / 0x04 — caller consumes 0x04

const HwTouch& hwTouch();
bool hwTouchIrqPending();  // peek at IRQ flag without consuming; for break-early sleep
