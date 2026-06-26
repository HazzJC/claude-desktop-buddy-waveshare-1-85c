// Arduino_GFX's font headers reference an undefined U8G2_FONT_SECTION
// macro — provide a no-op so the const array compiles. The font symbol
// itself is gated on U8G2_USE_LARGE_FONTS (set in build_flags).
#define U8G2_FONT_SECTION(name)
#include <Arduino_GFX_Library.h>

#include "hw/hw.h"
#include <LittleFS.h>
#include <stdarg.h>
#include <esp_mac.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

// TFT_eSPI used to define these named colors; Arduino_GFX uses
// RGB565_*. Keep the names so existing UI code compiles unchanged.
#define GREEN  0x07E0
#define RED    0xF800
#define BLUE   0x001F
#define YELLOW 0xFFE0
#define WHITE  0xFFFF
#define BLACK  0x0000

// spr is a thin alias for hwCanvas() — keeps existing UI code unchanged
#define spr (*hwCanvas())

#ifndef BOARD_UI_TEXT_SCALE
#define BOARD_UI_TEXT_SCALE 1
#endif
static constexpr int UI_SCALE = BOARD_UI_TEXT_SCALE;
static constexpr int UI_CHAR_W = 6 * UI_SCALE;
static constexpr int UI_CHAR_H = 8 * UI_SCALE;
static constexpr int UI_LINE_H = 10 * UI_SCALE;

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = HW_W;
const int H = HW_H;
const int CX = W / 2;
const int CY_BASE = 120;
// LED replaced by a small display alert via hwBorderAlert().

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     swallowTouch = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

static void prevPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → last species
    buddyMode = true;
    buddySetSpeciesIdx(n - 1);
    speciesIdxSave(n - 1);
  } else if (buddySpeciesIdx() == 0 && gifAvailable) {      // first species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
    buddyPrevSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS    = 30UL * 1000UL;        // 30s on battery, non-clock idle
const uint32_t CLOCK_OFF_MS_BAT = 5UL  * 60UL * 1000UL; // 5min on battery, clock visible

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;
uint32_t questionArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  hwImuAccel(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { hwDisplayBrightness(brightLevel); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    hwDisplaySleep(false);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}

static void markUserInteraction() {
  wake();
}

bool     responseSent = false;
bool     questionResponseSent = false;
uint8_t  questionSel = 0;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) hwBeep(freq, dur);
}

// Touch hit-test helper (additive: keys still work, touch is a 2nd path).
// Returns true on a fresh tap-down inside the rect; releases don't fire.
static bool tap(int x, int y, int w, int h) {
  const HwTouch& t = hwTouch();
  return t.justPressed && t.x >= x && t.y >= y && t.x < x+w && t.y < y+h;
}

// Press-start snapshot for gesture classification (swipe). Updated on every
// justPressed; read on justReleased to compute Δx/Δy/Δt.
static int16_t  _tpStartX = 0, _tpStartY = 0;
static uint32_t _tpStartMs = 0;

// Rect hit-test against the press-START position. Use on justReleased after
// a gesture has been classified as a stationary tap — so a tap with minor
// finger drift still targets the region the user pressed on.
static bool tappedFrom(int x, int y, int w, int h) {
  return _tpStartX >= x && _tpStartY >= y &&
         _tpStartX <  x + w && _tpStartY <  y + h;
}

struct CircleBand {
  int x;
  int w;
};

static int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static CircleBand circleBand(int y, int h, int pad = 8) {
  const int R = 142;
  int y0 = y - CX;
  int y1 = y + h - 1 - CX;
  int dy = abs(y0) > abs(y1) ? abs(y0) : abs(y1);
  int half = 0;
  if (dy < R) half = (int)sqrtf((float)(R * R - dy * dy));
  int x = CX - half + pad;
  int w = half * 2 - pad * 2;
  if (w < 0) w = 0;
  if (x < 0) { w += x; x = 0; }
  if (x + w > W) w = W - x;
  return { x, w };
}

static int textWidthPx(const char* s, int sz = UI_SCALE) {
  return (int)strlen(s) * 6 * sz;
}

static void drawBandText(int y, const char* s, uint16_t fg, uint16_t bg,
                         int sz = UI_SCALE, int align = 0, int pad = 8) {
  CircleBand b = circleBand(y, 8 * sz, pad);
  int tw = textWidthPx(s, sz);
  int x = b.x;
  if (align == 0) x = b.x + (b.w - tw) / 2;
  else if (align > 0) x = b.x + b.w - tw;
  int right = b.x + b.w - tw;
  if (right < b.x) right = b.x;
  spr.setTextSize(sz);
  spr.setTextColor(fg, bg);
  spr.setCursor(clampi(x, b.x, right), y);
  spr.print(s);
}

static void drawBandPrintf(int y, uint16_t fg, uint16_t bg,
                           int sz, int align, int pad, const char* fmt, ...) {
  char b[48];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  drawBandText(y, b, fg, bg, sz, align, pad);
}

// After a user interaction in clock mode (pet tap or species swipe), keep the
// buddy awake for this long — otherwise the time-of-day logic snaps back to
// P_SLEEP the instant the one-shot animation expires.
static const uint32_t PLAYFUL_MS = 3UL * 60UL * 1000UL;
static uint32_t _playfulUntil = 0;
static uint32_t _petCursorUntil = 0;
static int16_t  _petCursorX = CX;
static int16_t  _petCursorY = 92;
static constexpr uint32_t PET_CURSOR_MS = 520;

static void triggerPetCursor(int16_t x, int16_t y) {
  _petCursorX = x;
  _petCursorY = y;
  _petCursorUntil = millis() + PET_CURSOR_MS;
}

void triggerOneShot(PersonaState s, uint32_t durMs);

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static size_t jsonEscape(char* out, size_t n, const char* in) {
  if (n == 0) return 0;
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 1 < n; i++) {
    char c = in[i];
    if ((c == '"' || c == '\\') && j + 2 < n) {
      out[j++] = '\\';
      out[j++] = c;
    } else if ((uint8_t)c >= 0x20) {
      out[j++] = c;
    }
  }
  out[j] = 0;
  return j;
}

static void sendPermissionDecision(bool approve) {
  if (dataDemo()) {
    responseSent = true;
    beep(approve ? 2400 : 600, 60);
    triggerOneShot(approve ? P_HEART : P_IDLE, approve ? 1600 : 800);
    return;
  }
  char cmd[112];
  snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
           tama.promptId, approve ? "once" : "deny");
  sendCmd(cmd);
  responseSent = true;
  if (approve) {
    uint32_t tookS = (millis() - promptArrivedMs) / 1000;
    statsOnApproval(tookS);
    beep(2400, 60);
    if (tookS < 5) triggerOneShot(P_HEART, 2000);
  } else {
    statsOnDenial();
    beep(600, 60);
  }
}

static void sendQuestionAnswer(uint8_t idx) {
  if (idx >= tama.questionCount) return;
  if (dataDemo()) {
    questionResponseSent = true;
    beep(2400, 60);
    triggerOneShot(P_HEART, 1200);
    return;
  }
  char id[88], answer[140], cmd[280];
  jsonEscape(id, sizeof(id), tama.questionId);
  jsonEscape(answer, sizeof(answer), tama.questionOptions[idx]);
  snprintf(cmd, sizeof(cmd),
           "{\"cmd\":\"question\",\"id\":\"%s\",\"choice\":%u,\"answer\":\"%s\"}",
           id, idx, answer);
  sendCmd(cmd);
  questionResponseSent = true;
  beep(2400, 60);
  triggerOneShot(P_HEART, 1200);
}
const uint8_t INFO_PAGES = 5;
const uint8_t INFO_PG_BUTTONS = 0;
const uint8_t INFO_PG_CREDITS = 4;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillScreen(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

// Swipe cycles through all 8 pages as a flat list:
//   Normal → Pet 1/2 → Pet 2/2 → Info 1/5 → … → Info 5/5 → (wrap to Normal)
// BOOT short-press keeps the coarser 3-mode cycle; these helpers are only
// wired into the release-based gesture classifier below.
// applyDisplayMode() fires on mode transitions and Pet sub-page (matches
// existing BtnB behaviour). Info sub-page skips it because drawInfo() clears
// its own region — also matches existing BtnB behaviour.
static void swipeNextPage() {
  if (displayMode == DISP_NORMAL) {
    displayMode = DISP_PET; petPage = 0;                  applyDisplayMode();
  } else if (displayMode == DISP_PET) {
    if (petPage + 1 < PET_PAGES)    { petPage++;          applyDisplayMode(); }
    else { displayMode = DISP_INFO; infoPage = 0;         applyDisplayMode(); }
  } else { /* DISP_INFO */
    if (infoPage + 1 < INFO_PAGES)  { infoPage++; }
    else { displayMode = DISP_NORMAL;                     applyDisplayMode(); }
  }
}

static void swipePrevPage() {
  if (displayMode == DISP_NORMAL) {
    displayMode = DISP_INFO; infoPage = INFO_PAGES - 1;   applyDisplayMode();
  } else if (displayMode == DISP_PET) {
    if (petPage > 0)                { petPage--;          applyDisplayMode(); }
    else { displayMode = DISP_NORMAL;                     applyDisplayMode(); }
  } else { /* DISP_INFO */
    if (infoPage > 0)               { infoPage--; }
    else { displayMode = DISP_PET; petPage = PET_PAGES - 1; applyDisplayMode(); }
  }
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "rotate", "widgets", "bright", "sound", "pet", "hud", "alert", "reset", "cancel" };
const uint8_t SETTINGS_N = 9;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "pair reset", "factory", "back" };
const uint8_t RESET_N = 4;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      s.uiRot = (s.uiRot + 1) & 0x03;
      hwDisplaySetRotation(s.uiRot);
      hwInputSetRotation(s.uiRot);
      settingsSave();
      spr.fillScreen(0x0000);
      return;
    case 1:
      s.widgetMode = (s.widgetMode + 1) % 3;
      break;
    case 2:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 3: s.sound = !s.sound; break;
    case 4: nextPet(); return;
    case 5: s.hud = !s.hud; break;
    case 6: s.led = !s.led; break;
    case 7: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 8: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 3) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else if (idx == 1) {
    // pair reset: clear stored BLE bonds only, then reboot so advertising
    // restarts from a clean security state. Desktop/macOS may also need the
    // old Claude-XXXX device forgotten from Bluetooth settings.
    bleClearBonds();
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel. Panels add MENU_HINT_H to height
// and call this at bottom.
const int MENU_ROW_H  = 9 * UI_SCALE;
const int MENU_HINT_H = 21 * UI_SCALE;
const int MENU_PAD_X  = 7 * UI_SCALE;

static void menuLayout(int n, int& mx, int& my, int& mw, int& mh) {
  mw = 96 * UI_SCALE;
  mh = 14 * UI_SCALE + n * MENU_ROW_H + MENU_HINT_H;
  mx = (W - mw) / 2;
  my = (H - mh) / 2;
}

static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + MENU_PAD_X, hy - 4 * UI_SCALE, mw - MENU_PAD_X * 2, p.textDim);
  spr.setTextSize(UI_SCALE);
  const char* labels[3] = { "^", "v", rightLbl };
  int gap = 2 * UI_SCALE;
  int bw = (mw - 2 * gap - MENU_PAD_X * 2) / 3;
  int bh = 16 * UI_SCALE;
  int x = mx + MENU_PAD_X;
  for (int i = 0; i < 3; i++) {
    spr.drawRoundRect(x, hy, bw, bh, 2 * UI_SCALE, p.textDim);
    spr.setTextColor(i == 2 ? p.text : p.textDim, PANEL);
    int tw = strlen(labels[i]) * UI_CHAR_W;
    spr.setCursor(x + (bw - tw) / 2, hy + 4 * UI_SCALE);
    spr.print(labels[i]);
    x += bw + gap;
  }
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mx, my, mw, mh;
  menuLayout(SETTINGS_N, mx, my, mw, mh);
  spr.fillRoundRect(mx, my, mw, mh, 3 * UI_SCALE, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 3 * UI_SCALE, p.textDim);
  spr.setTextSize(UI_SCALE);
  Settings& s = settings();
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + MENU_PAD_X, my + 7 * UI_SCALE + i * MENU_ROW_H);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 32 * UI_SCALE, my + 7 * UI_SCALE + i * MENU_ROW_H);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      static const char* const ROT[] = { "0", "90", "180", "270" };
      spr.print(ROT[s.uiRot & 0x03]);
    } else if (i == 1) {
      static const char* const WM[] = { "off", "simp", "fun" };
      spr.print(WM[s.widgetMode]);
    } else if (i == 2) {
      spr.printf("%u/4", brightLevel);
    } else if (i == 3 || i == 5 || i == 6) {
      bool on = (i == 3) ? s.sound : (i == 5) ? s.hud : s.led;
      spr.setTextColor(on ? GREEN : p.textDim, PANEL);
      spr.print(on ? " on" : "off");
    } else if (i == 4) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 18 * UI_SCALE, "Next", "OK");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mx, my, mw, mh;
  menuLayout(RESET_N, mx, my, mw, mh);
  spr.fillRoundRect(mx, my, mw, mh, 3 * UI_SCALE, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 3 * UI_SCALE, HOT);
  spr.setTextSize(UI_SCALE);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + MENU_PAD_X, my + 7 * UI_SCALE + i * MENU_ROW_H);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 18 * UI_SCALE, "Next", "OK");
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: hwPowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: {
      bool on = !dataDemo();
      dataSetDemo(on);
      if (!on) dataClearDemoState(&tama);
      responseSent = false;
      questionResponseSent = false;
      menuOpen = false;
      displayMode = DISP_NORMAL;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      break;
    }
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mx, my, mw, mh;
  menuLayout(MENU_N, mx, my, mw, mh);
  spr.fillRoundRect(mx, my, mw, mh, 3 * UI_SCALE, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 3 * UI_SCALE, p.textDim);
  spr.setTextSize(UI_SCALE);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + MENU_PAD_X, my + 7 * UI_SCALE + i * MENU_ROW_H);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 18 * UI_SCALE, "Next", "OK");
}

// Portrait-only clock on the round LCD target.
static HwTime  _clkTm;
uint32_t       _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool    _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = hwBattery().usbPresent;
  hwRtcRead(&_clkTm);
}

// Clock face: shown when charging on USB with nothing else going on.
// Paints the upper ~110px to the canvas; pet renders below.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkTm.dow % 7; }
// Manual centered-text helper (Arduino_GFX has no setTextDatum). Default
// font is 6 px wide × 8 px tall; multiply by textSize for placement.
static void drawCenteredText(const char* s, int cx, int cy, int sz, uint16_t fg, uint16_t bg) {
  int w = (int)strlen(s) * 6 * sz;
  int h = 8 * sz;
  spr.setTextSize(sz);
  spr.setTextColor(fg, bg);
  spr.setCursor(cx - w/2, cy - h/2);
  spr.print(s);
}
static void drawClock() {
  const Palette& p = characterPalette();
  char hms[12]; snprintf(hms, sizeof(hms), "%02u:%02u:%02u", _clkTm.H, _clkTm.M, _clkTm.S);
  uint8_t mi = (_clkTm.Mo >= 1 && _clkTm.Mo <= 12) ? _clkTm.Mo - 1 : 0;
  char dl[16]; snprintf(dl, sizeof(dl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkTm.D);

  // Compact clock: single-line HH:MM:SS plus date below. Clears only
  // y >= 140 so the buddy at full home scale (reaches y≈126) fits
  // entirely above. Wider canvas + portrait orientation has plenty of
  // horizontal room for HH:MM:SS at size 3 (8 chars × 18 = 144 px).
  spr.fillRect(0, 140, W, H - 140, p.bg);
  drawCenteredText(hms, CX, 160, 3, p.text,    p.bg);
  drawBandText(SAFE_B - 26, dl, p.textDim, p.bg, 1, 0, 18);
  spr.setTextSize(1);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.promptId[0] || s.questionId[0]) return P_ATTENTION;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  hwImuAccel(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




static void drawPageHeader(const Palette& p, int y, const char* title,
                           uint8_t page, uint8_t pages) {
  spr.setTextSize(UI_SCALE);
  CircleBand b = circleBand(y, UI_CHAR_H, 16);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(b.x, y);
  spr.print(title);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(b.x + b.w - 24 * UI_SCALE, y);
  spr.printf("%u/%u", page + 1, pages);
}

// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  drawPageHeader(p, y, "Info", page, INFO_PAGES);
  y += 19;
  drawBandText(y, section, p.body, p.bg, UI_SCALE, 0, 20);
  y += 22;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillScreen(p.bg);
  drawBandText(58, "PAIRING", p.textDim, p.bg, UI_SCALE, 0, 20);
  drawBandText(SAFE_B - 34, "enter on desktop:", p.textDim, p.bg, UI_SCALE, 0, 16);
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, H / 2 - 12);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 66;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(UI_SCALE);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    CircleBand cb = circleBand(y, UI_CHAR_H, 12);
    spr.setTextColor(c, p.bg);
    spr.setCursor(cb.x, y);
    spr.print(b);
    y += 18;
  };
  auto gap = [&]() { y += 4; };

  if (infoPage == 0) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    ln(p.text,    "BOOT tap");
    ln(p.textDim, "next / approve");
    gap();
    ln(p.text,    "BOOT hold");
    ln(p.textDim, "open menu");
    gap();
    ln(p.text,    "Side switch");
    ln(p.textDim, "page / deny / OK");
    gap();
    ln(p.textDim, "touch rows + ^ v OK");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    ln(p.textDim, "sessions %u", tama.sessionsTotal);
    ln(p.textDim, "running  %u", tama.sessionsRunning);
    ln(p.textDim, "waiting  %u", tama.sessionsWaiting);
    gap();
    ln(p.text,    "LINK");
    ln(p.textDim, "via %s", dataScenarioName());
    ln(p.textDim, "ble %s", !bleConnected() ? "-" : bleSecure() ? "secure" : "open");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln(p.textDim, "age %lus", (unsigned long)age);
    ln(p.textDim, "state %s", stateNames[activeState]);

  } else if (infoPage == 2) {
    _infoHeader(p, y, "DEVICE", infoPage);

    HwBattery hb = hwBattery();
    int vBat_mV  = hb.mV;
    int vBus_mV  = hb.usbPresent ? 5000 : 0;
    int pct      = hb.pct;
    bool usb     = hb.usbPresent;
    bool charging = hb.charging;
    bool full    = usb && vBat_mV > 4100 && !charging;

    drawBandPrintf(y, p.text, p.bg, UI_SCALE, 0, 16, "%d%% %s", pct,
                   full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 22;
    ln(p.textDim, "bat %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    if (usb) ln(p.textDim, "usb %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    else     ln(p.textDim, "usb none");
    gap();
    uint32_t up = millis() / 1000;
    ln(p.textDim, "up %luh %02lum", up / 3600, (up / 60) % 60);
    ln(p.textDim, "heap %uKB", ESP.getFreeHeap() / 1024);
    ln(p.textDim, "bright %u/4", brightLevel);
    ln(p.textDim, "bt %s", dataBtActive() ? "linked" : "on");

  } else if (infoPage == 3) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = dataBtActive();

    ln(linked ? GREEN : HOT, "%s", linked ? "linked" : "discover");
    gap();
    ln(p.text, "%s", btName);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln(p.textDim, "%02X:%02X:%02X", mac[0],mac[1],mac[2]);
    ln(p.textDim, "%02X:%02X:%02X", mac[3],mac[4],mac[5]);
    gap();

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln(p.textDim, "last msg %lus", (unsigned long)age);
    } else {
      ln(p.text, "TO PAIR");
      ln(p.textDim, "Claude desktop");
      ln(p.textDim, "Developer");
      ln(p.textDim, "Hardware Buddy");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    ln(p.textDim, "originally by");
    ln(p.text,    "Felix Rieseberg");
    gap();
    ln(p.textDim, "forked ESP32");
    ln(p.text,    "yadong");
    gap();
    ln(p.textDim, "refined 1.85C");
    ln(p.text,    "HazzJC");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
// UTF-8 continuation byte = 0b10xxxxxx. Pull `take` back so we never
// land mid-codepoint when hard-breaking long Chinese sentences.
static uint8_t _utf8SafeTake(const char* w, uint8_t take, uint8_t wlen) {
  if (take == 0 || take >= wlen) return take;
  while (take > 0 && ((uint8_t)w[take] & 0xC0) == 0x80) take--;
  return take;
}

static uint8_t wrapInto(const char* in, char out[][48], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit, on UTF-8 char boundaries
    while (wlen > width - col) {
      uint8_t take = _utf8SafeTake(w, width - col, wlen);
      if (take == 0) take = 1;                 // safety: avoid infinite loop
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

static void fitLabel(char* out, size_t n, const char* prefix, const char* s, int maxChars) {
  if (n == 0) return;
  if (maxChars < 1) maxChars = 1;
  if (maxChars > (int)n - 1) maxChars = (int)n - 1;
  int pre = strlen(prefix);
  int room = maxChars - pre;
  if (room < 1) room = 1;
  snprintf(out, n, "%s%.*s", prefix, room, s ? s : "");
  if ((int)strlen(out) >= maxChars && maxChars > 1) {
    out[maxChars - 1] = '.';
    out[maxChars] = 0;
  }
}

static bool questionRowRect(uint8_t idx, int& x, int& y, int& w, int& h) {
  if (idx >= tama.questionCount || idx >= 4) return false;
  y = 156 + idx * 28;
  h = 24;
  CircleBand b = circleBand(y, h, 14);
  x = b.x;
  w = b.w;
  return w > 20;
}

static void drawQuestion() {
  const Palette& p = characterPalette();
  const int TOP = 64;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  drawPageHeader(p, 70, "Question", questionSel, tama.questionCount ? tama.questionCount : 1);

  char lines[3][48];
  uint8_t n = wrapInto(tama.questionText, lines, 3, 18);
  int y = 94;
  for (uint8_t i = 0; i < n; i++) {
    drawBandText(y + i * 18, lines[i], i == 0 ? p.text : p.textDim, p.bg, UI_SCALE, 0, 16);
  }

  for (uint8_t i = 0; i < tama.questionCount && i < 4; i++) {
    int rx, ry, rw, rh;
    if (!questionRowRect(i, rx, ry, rw, rh)) continue;
    bool sel = (i == questionSel);
    uint16_t fill = sel ? p.body : PANEL;
    uint16_t ink  = sel ? p.bg : p.text;
    spr.fillRoundRect(rx, ry, rw, rh, 3 * UI_SCALE, fill);
    spr.drawRoundRect(rx, ry, rw, rh, 3 * UI_SCALE, sel ? p.text : p.textDim);
    spr.setTextSize(UI_SCALE);
    spr.setTextColor(ink, fill);
    char prefix[4];
    snprintf(prefix, sizeof(prefix), "%u ", i + 1);
    char label[36];
    int maxChars = (rw - 10 * UI_SCALE) / UI_CHAR_W;
    fitLabel(label, sizeof(label), prefix, tama.questionOptions[i], maxChars);
    spr.setCursor(rx + 5 * UI_SCALE, ry + 4 * UI_SCALE);
    spr.print(label);
  }
}

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 112;
  const int TOP = H - AREA;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(circleBand(TOP, 1, 12).x, TOP, circleBand(TOP, 1, 12).w, p.textDim);

  spr.setTextSize(UI_SCALE);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  drawBandPrintf(TOP + 9, waited >= 10 ? HOT : p.textDim, p.bg,
                 UI_SCALE, 0, 14, "approve? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  drawBandText(TOP + 31, tama.promptTool, p.text, p.bg, toolLen <= 10 ? 2 : UI_SCALE, 0, 14);
  spr.setTextSize(UI_SCALE);

  int hlen = strlen(tama.promptHint);
  char hint[18];
  snprintf(hint, sizeof(hint), hlen > 15 ? "%.14s." : "%.15s", tama.promptHint);
  drawBandText(TOP + 57, hint, p.textDim, p.bg, UI_SCALE, 0, 14);

  if (responseSent) {
    drawBandText(SAFE_B - 21, "sent...", p.textDim, p.bg, UI_SCALE, 0, 18);
  } else {
    CircleBand b = circleBand(SAFE_B - 22, UI_CHAR_H, 18);
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(b.x, SAFE_B - 22);
    spr.print("BOOT/tap");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(b.x + b.w - textWidthPx("slide no"), SAFE_B - 22);
    spr.print("slide no");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  int r = 2 * UI_SCALE;
  if (filled) {
    spr.fillCircle(x - r, y, r, col);
    spr.fillCircle(x + r, y, r, col);
    spr.fillTriangle(x - 2 * r, y + UI_SCALE, x + 2 * r, y + UI_SCALE, x, y + 5 * UI_SCALE, col);
  } else {
    spr.drawCircle(x - r, y, r, col);
    spr.drawCircle(x + r, y, r, col);
    spr.drawLine(x - 2 * r, y + UI_SCALE, x, y + 5 * UI_SCALE, col);
    spr.drawLine(x + 2 * r, y + UI_SCALE, x, y + 5 * UI_SCALE, col);
  }
}

static void drawPetCursor() {
  int32_t left = (int32_t)_petCursorUntil - (int32_t)millis();
  if (left <= 0) return;

  int age = PET_CURSOR_MS - left;
  int frame = (age / 170) % 3;
  int dx = (frame == 1) ? 1 : (frame == 2) ? -1 : 0;
  int dy = (frame == 1) ? 2 : 0;
  int x = _petCursorX - 16 + dx;
  int y = _petCursorY - 28 + dy;
  x = clampi(x, SAFE_L + 8, SAFE_R - 38);
  y = clampi(y, SAFE_T + 8, SAFE_B - 46);

  auto palm = [&](int ox, int oy, uint16_t c) {
    int px = x + ox;
    int py = y + oy;
    spr.fillRoundRect(px + 8,  py + 0,  6, 22, 2, c);
    spr.fillRoundRect(px + 14, py + 0,  6, 23, 2, c);
    spr.fillRoundRect(px + 20, py + 1,  6, 21, 2, c);
    spr.fillRoundRect(px + 26, py + 4,  6, 18, 2, c);
    spr.fillRoundRect(px + 8,  py + 17, 24, 18, 4, c);
    spr.fillRoundRect(px + 12, py + 31, 15, 9, 2, c);
    spr.fillTriangle(px + 9, py + 21, px + 1, py + 25, px + 10, py + 31, c);
    spr.fillRoundRect(px + 1, py + 23, 11, 8, 2, c);
  };

  palm(1, 1, BLACK);
  palm(0, 0, WHITE);
  spr.drawFastVLine(x + 14, y + 4, 13, BLACK);
  spr.drawFastVLine(x + 20, y + 5, 12, BLACK);
  spr.drawFastVLine(x + 26, y + 8, 9, BLACK);
  spr.drawPixel(x + 5, y + 29, BLACK);

  if (left > 90) tinyHeart(x + 36, y + 10, true, HOT);
}

static void drawPetStats(const Palette& p) {
  const int TOP = 76;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(UI_SCALE);
  int y = 106;
  const int rowGap = 24;

  auto statRow = [&](const char* label, int yy) -> CircleBand {
    CircleBand b = circleBand(yy - 8, 16, 14);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(b.x, yy - 6);
    spr.print(label);
    return b;
  };

  CircleBand b = statRow("mood", y);
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  int gx = b.x + b.w - 94;
  for (int i = 0; i < 4; i++) {
    tinyHeart(gx + i * 22, y - 2, i < mood, moodCol);
  }

  y += rowGap;
  b = statRow("fed", y);
  uint8_t fed = statsFedProgress();
  uint8_t fedBars = (fed + 1) / 2;
  gx = b.x + b.w - 114;
  for (int i = 0; i < 5; i++) {
    int px = gx + i * 23;
    if (i < fedBars) spr.fillRoundRect(px, y - 8, 17, 10, 2, p.body);
    else spr.drawRoundRect(px, y - 8, 17, 10, 2, p.textDim);
  }

  y += rowGap;
  b = statRow("energy", y);
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  gx = b.x + b.w - 114;
  for (int i = 0; i < 5; i++) {
    int px = gx + i * 23;
    if (i < en) spr.fillRoundRect(px, y - 8, 17, 10, 2, enCol);
    else spr.drawRoundRect(px, y - 8, 17, 10, 2, p.textDim);
  }

  y += 27;
  b = circleBand(y - 10, 20, 18);
  int pillW = 56;
  spr.fillRoundRect(CX - pillW / 2, y - 10, pillW, 18, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(CX - 22, y - 6);
  spr.printf("Lv %u", stats().level);

  auto statFmt = [&](char* out, size_t n, const char* label, uint32_t v) {
    if (v >= 1000000)   snprintf(out, n, "%s %lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) snprintf(out, n, "%s %lu.%luK", label, v/1000, (v/100)%10);
    else                snprintf(out, n, "%s %lu", label, v);
  };
  char left[18], right[18];
  statFmt(left, sizeof(left), "tok", stats().tokens);
  statFmt(right, sizeof(right), "day", tama.tokensToday);
  b = circleBand(y + 22, UI_CHAR_H, 18);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(b.x, y + 22);
  spr.print(left);
  spr.setCursor(b.x + b.w - textWidthPx(right), y + 22);
  spr.print(right);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 76;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(UI_SCALE);
  int y = 106;
  auto ln = [&](uint16_t c, const char* s) {
    CircleBand b = circleBand(y, UI_CHAR_H, 14);
    spr.setTextColor(c, p.bg);
    spr.setCursor(b.x, y);
    spr.print(s);
    y += 18;
  };
  auto gap = [&]() { y += 5; };

  ln(p.body,    "MOOD");
  ln(p.textDim, "fast approve helps"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, "50K tokens = level"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, "face-down to nap"); gap();

  ln(p.textDim, "BOOT screens");
  ln(p.textDim, "hold menu");
  ln(p.textDim, "slide page");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  spr.setTextSize(UI_SCALE);
  char title[22];
  if (ownerName()[0]) {
    snprintf(title, sizeof(title), "%s's %s", ownerName(), petName());
  } else {
    snprintf(title, sizeof(title), "%s", petName());
  }
  if (strlen(title) > 15) strcpy(title + 12, "...");
  drawPageHeader(p, y + 2 * UI_SCALE, title, petPage, PET_PAGES);
}

static void formatCountdown(char* out, size_t n, uint32_t sec) {
  if (sec >= 86400) snprintf(out, n, "%lud", (unsigned long)((sec + 86399) / 86400));
  else if (sec >= 3600) snprintf(out, n, "%luh", (unsigned long)((sec + 3599) / 3600));
  else snprintf(out, n, "%lum", (unsigned long)((sec + 59) / 60));
}

static void drawUsageBar(const Palette& p, int y, const char* label, uint8_t pct, uint32_t resetSec) {
  char cd[8];
  formatCountdown(cd, sizeof(cd), resetSec);
  CircleBand b = circleBand(y, 18, 18);
  int labelW = 4 * UI_CHAR_W;
  int pctW = 4 * UI_CHAR_W;
  int cdW = textWidthPx(cd, UI_SCALE);
  int barX = b.x + labelW;
  int barW = b.w - labelW - pctW - cdW - 12 * UI_SCALE;
  if (barW < 34) return;
  int fill = (barW - 2) * pct / 100;
  spr.setTextSize(UI_SCALE);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(b.x, y + 2);
  spr.print(label);
  spr.drawRoundRect(barX, y + 5, barW, 8, 2, p.textDim);
  if (fill > 0) spr.fillRoundRect(barX + 1, y + 6, fill, 6, 2, p.body);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(barX + barW + 4 * UI_SCALE, y + 2);
  spr.printf("%u%%", pct);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(b.x + b.w - cdW, y + 2);
  spr.print(cd);
}

static void drawBottleWidget(const Palette& p, int x, int y, const char* label,
                             uint8_t usedPct, uint32_t resetSec) {
  uint8_t remain = 100 - usedPct;
  int bw = 28, bh = 46;
  int fillH = (bh - 10) * remain / 100;
  spr.drawRoundRect(x + 7, y + 6, bw - 14, 7, 2, p.textDim);
  spr.drawRoundRect(x + 3, y + 12, bw - 6, bh - 12, 4, p.textDim);
  if (fillH > 0) {
    spr.fillRoundRect(x + 5, y + bh - 2 - fillH, bw - 10, fillH, 3, p.body);
  }
  spr.drawFastHLine(x + 6, y + 25, bw - 12, p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(x + 2, y + bh + 3);
  spr.print(label);
  char cd[8];
  formatCountdown(cd, sizeof(cd), resetSec);
  spr.setCursor(x + 2, y + bh + 13);
  spr.print(cd);
}

static void drawHomeWidgets() {
  if (!tama.usageValid || settings().widgetMode == 0) return;
  const Palette& p = characterPalette();
  if (settings().widgetMode == 1) {
    spr.fillRect(0, 142, W, 50, p.bg);
    drawUsageBar(p, 148, "4h", tama.usageSessionPct, tama.usageSessionResetSec);
    drawUsageBar(p, 172, "week", tama.usageWeekPct, tama.usageWeekResetSec);
  } else {
    drawBottleWidget(p, 52, 126, "4h", tama.usageSessionPct, tama.usageSessionResetSec);
    drawBottleWidget(p, W - 80, 126, "wk", tama.usageWeekPct, tama.usageWeekResetSec);
  }
}

void drawHUD() {
  if (tama.promptId[0] && !responseSent) { drawApproval(); return; }
  if (tama.questionId[0] && !questionResponseSent) { drawQuestion(); return; }
  const Palette& p = characterPalette();
  // chill7 font: glyphs ~7 px tall but baseline-positioned (setCursor
  // is the baseline, not the top). Allow ~10 px line spacing, ~22 byte
  // budget per line — Chinese chars are ~7 px wide, ASCII ~5 px, so a
  // mixed line of 22 bytes (~7 Chinese OR 22 ASCII) fits W=184.
  const int SHOW = 3, LH = UI_LINE_H, WIDTH = 18;
  const int HUD_TOP = SAFE_B - SHOW * LH - 4;
  spr.fillRect(0, HUD_TOP, W, H - HUD_TOP, p.bg);

  // Menu/settings/reset should hide the HUD strip underneath — panels are
  // centered and don't cover the bottom 34 px on their own.
  if (menuOpen || settingsOpen || resetOpen) return;

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  // buddy/character ticks leave textsize at 2 (home scale); without
  // pinning it here the CJK font alternates between 1× and 2× every tick.
  spr.setTextSize(UI_SCALE);
  spr.setFont((const uint8_t*)u8g2_font_chill7_h_cjk);

  if (tama.nLines == 0) {
    CircleBand b = circleBand(SAFE_B - 20, UI_CHAR_H, 16);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(b.x, SAFE_B - 20);
    spr.print(tama.msg);
    spr.setFont((const GFXfont*)NULL);
    return;
  }

  static char disp[32][48];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    int yy = HUD_TOP + UI_CHAR_H + i * LH;
    CircleBand b = circleBand(yy, UI_CHAR_H, 14);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(b.x, yy);
    spr.print(disp[row]);
  }

  spr.setFont((const GFXfont*)NULL);

  if (msgScroll > 0) {
    spr.setTextSize(UI_SCALE);
    spr.setTextColor(p.body, p.bg);
    CircleBand b = circleBand(SAFE_B - 20, UI_CHAR_H, 14);
    spr.setCursor(b.x + b.w - 18 * UI_SCALE, SAFE_B - 20);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  hwInit();                  // Wire + expander + display + power + input + IMU + RTC + audio
  startBt();                 // BLE stays always-on
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  hwDisplaySetRotation(settings().uiRot);
  hwInputSetRotation(settings().uiRot);
  petNameLoad();
  buddyInit();

  characterInit(nullptr);    // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF).
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillScreen(p.bg);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      drawCenteredText(line,      W/2, H/2 - 12, 2, p.text, p.bg);
      drawCenteredText(petName(), W/2, H/2 + 12, 2, p.body, p.bg);
    } else {
      drawCenteredText("Hello!",          W/2, H/2 - 12, 2, p.body,    p.bg);
      drawCenteredText("a buddy appears", W/2, H/2 + 12, 1, p.textDim, p.bg);
    }
    spr.setTextSize(1);
    hwDisplayPush();
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  hwInputUpdate();
  ;
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  static uint8_t lastDemoStage = 0xFF;
  uint8_t demoStage = dataDemoStage();
  if (demoStage != lastDemoStage) {
    lastDemoStage = demoStage;
    if (dataDemo()) {
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      if (demoStage == 1) {
        triggerOneShot(P_HEART, 1800);
        triggerPetCursor(CX + 16, 104);
        _playfulUntil = millis() + PLAYFUL_MS;
      } else if (demoStage == 5) {
        triggerOneShot(P_CELEBRATE, 3200);
      }
    }
  }
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Attention indicator: display alert in place of a hardware LED.
  hwBorderAlert(activeState == P_ATTENTION && settings().led
                && (now / 400) % 2 == 0);

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // Primary control: step through fake scenarios.
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  static char lastQuestionId[40] = "";
  if (strcmp(tama.questionId, lastQuestionId) != 0) {
    strncpy(lastQuestionId, tama.questionId, sizeof(lastQuestionId)-1);
    lastQuestionId[sizeof(lastQuestionId)-1] = 0;
    questionResponseSent = false;
    questionSel = 0;
    if (tama.questionId[0]) {
      questionArrivedMs = millis();
      wake();
      beep(1400, 80);
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  if (questionSel >= tama.questionCount) questionSel = 0;
  bool inPermission = tama.promptId[0] && !responseSent;
  bool inQuestion = tama.questionId[0] && !questionResponseSent;
  bool inPrompt = inPermission || inQuestion;

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (hwBtnA().isPressed || hwBtnB().isPressed) {
    if (screenOff) {
      if (hwBtnA().isPressed) swallowBtnA = true;
      if (hwBtnB().isPressed) swallowBtnB = true;
    }
    markUserInteraction();
  }

  // Optional power-key path retained for HAL compatibility.
  // Very-long-press (6s) still powers off via AXP hardware.
  if (hwAxpBtnEvent() == 0x04) {
    bool wasOff = screenOff;
    markUserInteraction();
    if (!wasOff) {
      hwDisplaySleep(true);
      screenOff = true;
    }
  }

  if (hwBtnA().pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
#if defined(BOARD_SINGLE_BUTTON_MENU_CONFIRM) && BOARD_SINGLE_BUTTON_MENU_CONFIRM
    if (inPermission) {
      sendPermissionDecision(false);
    } else if (inQuestion) {
      sendQuestionAnswer(questionSel);
    } else if (resetOpen) { applyReset(resetSel); }
    else if (settingsOpen) { applySetting(settingsSel); }
    else if (menuOpen) { menuConfirm(); }
    else {
      menuOpen = true;
      menuSel = 0;
    }
#else
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
#endif
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (hwBtnA().wasReleased) {
    if (!btnALong && !swallowBtnA) {
      if (inPermission) {
        sendPermissionDecision(true);
      } else if (inQuestion) {
        beep(1800, 30);
        questionSel = (questionSel + 1) % tama.questionCount;
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // Secondary control: pet -> heart.
  if (hwBtnB().wasPressed) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPermission) {
      sendPermissionDecision(false);
    } else if (inQuestion) {
      sendQuestionAnswer(questionSel);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // ─── Touch (additive — buttons above already handled) ──────────────
  // Clocking = idle home screen with RTC synced; drives gesture routing:
  // HUD (!clocking) gets tap-to-pet, clocking gets horizontal-swipe-to-switch-species.
  bool tpClocking = displayMode == DISP_NORMAL
                 && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
                 && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
                 && dataRtcValid();

  const HwTouch& tp = hwTouch();
  if (tp.justPressed) {
    bool wasOff = screenOff || dimmed;
    markUserInteraction();
    if (wasOff) swallowTouch = true;
    _tpStartX = tp.x; _tpStartY = tp.y; _tpStartMs = millis();
  }

  // Approval: tap upper half of the approval area = approve,
  //           tap lower half = deny.
  if (swallowTouch && tp.justReleased) {
    swallowTouch = false;
  } else if (!swallowTouch && inPermission) {
    const int APPROVAL_TOP = H - 112;
    if (tap(0, APPROVAL_TOP,      W, 56)) {
      sendPermissionDecision(true);
    }
    if (tap(0, APPROVAL_TOP + 56, W, 56)) {
      sendPermissionDecision(false);
    }
  } else if (!swallowTouch && inQuestion) {
    const HwTouch& t = hwTouch();
    if (t.justPressed) {
      for (uint8_t i = 0; i < tama.questionCount && i < 4; i++) {
        int rx, ry, rw, rh;
        if (questionRowRect(i, rx, ry, rw, rh) &&
            t.x >= rx && t.x < rx + rw && t.y >= ry && t.y < ry + rh) {
          questionSel = i;
          sendQuestionAnswer(i);
          break;
        }
      }
    }
  } else if (!swallowTouch && (menuOpen || settingsOpen || resetOpen)) {
    // Tap a menu row → directly select + confirm. Reuses the layout
    // constants from drawMenu/drawSettings/drawReset.
    int n = menuOpen ? MENU_N : settingsOpen ? SETTINGS_N : RESET_N;
    int mx, my, mw, mh;
    menuLayout(n, mx, my, mw, mh);
    int rowH   = MENU_ROW_H;
    int rowsTop = my + 7 * UI_SCALE;
    const HwTouch& t = hwTouch();
    if (t.justPressed && t.x >= mx && t.x < mx + mw &&
        t.y >= rowsTop && t.y < rowsTop + n * rowH) {
      int hit = (t.y - rowsTop) / rowH;
      if (hit >= 0 && hit < n) {
        beep(2400, 30);
        if (menuOpen)         { menuSel     = hit; menuConfirm(); }
        else if (settingsOpen){ settingsSel = hit; applySetting(hit); }
        else /* resetOpen */  { resetSel    = hit; applyReset(hit); }
      }
    }
    int footerY = my + mh - 18 * UI_SCALE;
    int gap = 2 * UI_SCALE;
    int bw = (mw - 2 * gap - MENU_PAD_X * 2) / 3;
    int bh = 16 * UI_SCALE;
    int bx = mx + MENU_PAD_X;
    if (t.justPressed && t.y >= footerY && t.y < footerY + bh) {
      int action = -1;
      for (int i = 0; i < 3; i++) {
        int x0 = bx + i * (bw + gap);
        if (t.x >= x0 && t.x < x0 + bw) action = i;
      }
      if (action >= 0) {
        beep(1800, 30);
        uint8_t* sel = menuOpen ? &menuSel : settingsOpen ? &settingsSel : &resetSel;
        if (action == 0) {
          *sel = (*sel + n - 1) % n;
          if (resetOpen) resetConfirmIdx = 0xFF;
        } else if (action == 1) {
          *sel = (*sel + 1) % n;
          if (resetOpen) resetConfirmIdx = 0xFF;
        } else {
          if (menuOpen) menuConfirm();
          else if (settingsOpen) applySetting(settingsSel);
          else applyReset(resetSel);
        }
      }
    }
  }
  // END of press-based approval/overlay taps. Below: release-based classifier
  // for DISP_NORMAL / DISP_PET / DISP_INFO. Horizontal swipes page through
  // the whole flat chain. Approval and overlay menus are excluded so an
  // accidental drag can't mis-decide.

  if (tp.justReleased
      && !swallowTouch && !inPrompt && !menuOpen && !settingsOpen && !resetOpen
      && !napping && !screenOff) {
    int dx = (int)tp.x - _tpStartX;
    int dy = (int)tp.y - _tpStartY;
    uint32_t dt = millis() - _tpStartMs;

    if (abs(dx) >= 40 && abs(dx) > abs(dy) * 2 && dt < 500) {
      // Horizontal swipe through the flat page chain: right-to-left = next,
      // left-to-right = previous.
      beep(1800, 30);
      if (dx < 0) swipeNextPage();
      else        swipePrevPage();
    }
    else if (abs(dx) < 12 && abs(dy) < 12 && dt < 800) {
      // Stationary tap → route by press-start position.
      if (displayMode == DISP_INFO && tappedFrom(W - 60, 0, 60, 70)) {
        beep(2400, 30);
        infoPage = (infoPage + 1) % INFO_PAGES;
      }
      else if (displayMode == DISP_PET && tappedFrom(W - 60, 0, 60, 70)) {
        beep(2400, 30);
        petPage = (petPage + 1) % PET_PAGES;
        applyDisplayMode();
      }
      else if (displayMode == DISP_PET && tappedFrom(CX - 70, 66, 140, 84)) {
        triggerOneShot(P_HEART, 2000);
        triggerPetCursor(_tpStartX, _tpStartY);
        characterInvalidate();
        if (buddyMode) buddyInvalidate();
        beep(2400, 50);
      }
      else if (displayMode == DISP_NORMAL && !tpClocking && tappedFrom(0, 0, W, (H * 2) / 3)) {
        // Top two-thirds → pet buddy. Bottom strip remains transcript scroll.
        triggerOneShot(P_HEART, 2000);
        triggerPetCursor(_tpStartX, _tpStartY);
        _playfulUntil = millis() + PLAYFUL_MS;
        characterInvalidate();
        if (buddyMode) buddyInvalidate();
        beep(2400, 50);
      }
      else if (displayMode == DISP_NORMAL && !tpClocking && tappedFrom(0, H - 32, W, 32)) {
        // Bottom strip → scroll transcript back (mirrors BtnB short-press).
        beep(2400, 30);
        msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
      }
      else if (tpClocking && _tpStartY < 130) {
        // Clock mode upper half = buddy region (lower half is clock digits).
        triggerOneShot(P_HEART, 2000);
        triggerPetCursor(_tpStartX, _tpStartY);
        _playfulUntil = millis() + PLAYFUL_MS;
        characterInvalidate();
        if (buddyMode) buddyInvalidate();
        beep(2400, 50);
      }
    }
  }
  if (tp.justReleased) swallowTouch = false;

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  // Clock shows when Claude is idle and the RTC is synced — regardless
  // of USB power. On battery the screen still auto-offs after a longer
  // timeout (CLOCK_OFF_MS_BAT) so it doesn't drain forever.
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid();
  // Portrait-only clock.
  static bool wasClocking = false;
  if (clocking != wasClocking) {
    if (clocking) {
      // GIFs are tall (up to 140 px) — must shrink to fit above clock.
      // ASCII buddy at scale 2 reaches y≈126; clock starts at y=140
      // (compact single-line layout) so peek isn't needed and the pet
      // gets to keep its full size.
      characterSetPeek(true);
      buddySetPeek(false);
      // Clear the full canvas once on entry: buddy/clock both update
      // partial regions every frame, so any stale ink left behind from
      // the previous mode would persist forever.
      const Palette& cp = characterPalette();
      spr.fillScreen(cp.bg);
    } else {
      applyDisplayMode();
    }
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  // Skip the time-of-day mood logic while a one-shot animation
  // (shake → dizzy, level-up → celebrate, fast-approve → heart) is
  // active — otherwise it would overwrite activeState immediately.
  if (clocking && (int32_t)(now - oneShotUntil) >= 0) {
    if ((int32_t)(now - _playfulUntil) < 0) {
      // Recently interacted with (pet tap / species swipe) — rotate through
      // awake animations instead of falling back to the time-of-day logic
      // that mostly picks P_SLEEP. Decays to normal after PLAYFUL_MS.
      static const PersonaState PLAYFUL[] = {
        P_IDLE, P_IDLE, P_HEART, P_IDLE, P_CELEBRATE, P_IDLE
      };
      activeState = PLAYFUL[(now / 5000) % 6];
    } else {
      // Ambient rhythm is SLEEP↔IDLE only. Emotional states (HEART, CELEBRATE,
      // DIZZY) are reactions — they fire from real events (shake, fast-approve,
      // level-up, pet tap, species swipe) via triggerOneShot / playful window,
      // never spontaneously from wall-clock mood.
      uint8_t h = _clkTm.H;
      if (h < 7 || h >= 22) activeState = (now/15000 % 8 == 0) ? P_IDLE  : P_SLEEP;
      else                  activeState = (now/12000 % 6 == 0) ? P_SLEEP : P_IDLE;
    }
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff) {
    // skip canvas render — face-down or powered off
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillScreen(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      drawBandText(90, "installing", p.textDim, p.bg, UI_SCALE, 0, 18);
      drawBandPrintf(112, p.textDim, p.bg, UI_SCALE, 0, 18,
                     "%luK / %luK", done/1024, total/1024);
      CircleBand b = circleBand(136, 8, 24);
      spr.drawRect(b.x, 136, b.w, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)b.w * done / total);
        if (fill > 1) spr.fillRect(b.x + 1, 137, fill - 1, 6, p.body);
      }
    } else {
      drawBandText(112, "no character", p.textDim, p.bg, UI_SCALE, 0, 18);
      drawBandText(134, "loaded", p.textDim, p.bg, UI_SCALE, 0, 18);
    }
  }
  if (!napping && !screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (inPermission) drawApproval();
    else if (inQuestion) drawQuestion();
    else {
      drawHomeWidgets();
      if (settings().hud) drawHUD();
    }
    drawPetCursor();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    hwDisplayPush();
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    hwDisplayBrightness(0);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // Auto-off rules:
  //   USB plugged: never (clock can stay visible indefinitely)
  //   Battery + clock visible: 5 min (CLOCK_OFF_MS_BAT)
  //   Battery + non-clock idle: 30 s (SCREEN_OFF_MS)
  if (!screenOff && !inPrompt && !_onUsb) {
    uint32_t idleMs    = millis() - lastInteractMs;
    uint32_t threshold = clocking ? CLOCK_OFF_MS_BAT : SCREEN_OFF_MS;
    if (idleMs >= SCREEN_OFF_MS && idleMs > threshold) {
      hwDisplaySleep(true);
      screenOff = true;
    }
  }

  // Periodic full redraw.
  // OLED pixels degrade where they stay lit at constant value; redrawing
  // (rather than incremental updates) at least exercises every pixel for
  // a frame. A more aggressive 1-px shimmy could shift the whole canvas
  // each cycle, but this minimum is a safe baseline.
  static uint32_t lastShimmy = 0;
  if (millis() - lastShimmy > 5UL * 60UL * 1000UL) {
    lastShimmy = millis();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
  }

  // LTPO-lite: vary loop cadence by what's happening. Animations tick on
  // wall-clock (buddy.cpp TICK_MS=200) and redraws are gated, so slowing the
  // loop during ambient SLEEP↔IDLE costs no frames — just fewer MCU wakes.
  // Fast rate only where latency is felt: input, interactive UI, one-shots,
  // nap-exit, transfer progress, BLE pairing.
  uint32_t loopMs;
  if (screenOff) {
    loopMs = 200;
  } else if (napping
          || hwTouch().down
          || hwBtnA().isPressed || hwBtnB().isPressed
          || inPrompt || menuOpen || settingsOpen || resetOpen
          || (int32_t)(now - oneShotUntil) < 0
          || xferActive()
          || blePasskey()) {
    loopMs = 16;
  } else {
    loopMs = 100;
  }
  // Slice the idle sleep so a touch-down IRQ (edge-triggered) or a button
  // press breaks out within ~8ms instead of waiting the full loopMs. Without
  // this, first-tap latency during idle felt sluggish.
  if (loopMs <= 16) {
    delay(loopMs);
  } else {
    uint32_t slept = 0;
    while (slept < loopMs) {
      uint32_t slice = (loopMs - slept > 8) ? 8 : (loopMs - slept);
      delay(slice);
      slept += slice;
      if (hwTouchIrqPending()) break;
      if (digitalRead(PIN_KEY1) == LOW) break;
    }
  }
}
