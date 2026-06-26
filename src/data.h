#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "hw/rtc.h"
#include "xfer.h"

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint8_t  usageSessionPct;
  uint8_t  usageWeekPct;
  uint32_t usageSessionResetSec;
  uint32_t usageWeekResetSec;
  bool     usageValid;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];
  char     questionId[40];   // pending question request ID; empty = no question
  char     questionText[160];
  char     questionOptions[4][64];
  uint8_t  questionCount;
  uint16_t questionGen;
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoStage  = 0;
static uint32_t _demoStageStart = 0;
static const uint32_t DEMO_STAGE_MS = 7000;
static const uint8_t DEMO_STAGE_COUNT = 6;

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoStage = 0; _demoStageStart = millis(); }
}
inline bool dataDemo() { return _demoMode; }
inline uint8_t dataDemoStage() { return _demoMode ? _demoStage : 0xFF; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) {
    static const char* const names[] = {
      "demo home", "demo pet", "demo approve", "demo choose", "demo usage", "demo level"
    };
    return names[_demoStage % DEMO_STAGE_COUNT];
  }
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

inline void dataClearDemoState(TamaState* out) {
  if (!out) return;
  out->promptId[0] = 0;
  out->promptTool[0] = 0;
  out->promptHint[0] = 0;
  out->questionId[0] = 0;
  out->questionText[0] = 0;
  out->questionCount = 0;
  out->usageValid = false;
  out->recentlyCompleted = false;
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

// Replace non-ASCII bytes (UTF-8 multi-byte sequences for Chinese, etc.)
// with random "Matrix-rain"-flavoured ASCII glyphs. The font is ASCII-only,
// so otherwise these bytes render as garbage. One Chinese char (3 bytes)
// becomes 3 random ASCII chars — fits the digital-rain look.
static void _matrixify(char* s) {
  static const char POOL[] = "01<>{}[]/\\|*~$%#@&=+-_:;.!?ABCDEFGHJKLMNPQRSTVWXYZ";
  static const int  POOL_N = sizeof(POOL) - 1;
  for (; *s; s++) {
    if ((uint8_t)*s > 127) *s = POOL[esp_random() % POOL_N];
  }
}

static void _copyMatrix(char* dst, size_t n, const char* src) {
  if (n == 0) return;
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = 0;
  _matrixify(dst);
}

static const char* _textFromVariant(JsonVariant v) {
  if (v.is<const char*>()) return v.as<const char*>();
  if (v.is<JsonObject>()) {
    JsonObject o = v.as<JsonObject>();
    const char* s = o["label"] | o["text"] | o["title"] | o["value"] | o["question"];
    return s ? s : "";
  }
  return "";
}

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    HwTime ht;
    ht.H  = lt.tm_hour;
    ht.M  = lt.tm_min;
    ht.S  = lt.tm_sec;
    ht.Y  = lt.tm_year + 1900;
    ht.Mo = lt.tm_mon + 1;
    ht.D  = lt.tm_mday;
    ht.dow = lt.tm_wday;
    hwRtcWrite(ht);
    extern uint32_t _clkLastRead;
    _clkLastRead = 0;   // force re-read so _clkDt and _rtcValid agree
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  JsonObject usage = doc["usage"];
  if (!usage.isNull() &&
      usage["session_pct"].is<int>() && usage["session_reset_sec"].is<uint32_t>() &&
      usage["week_pct"].is<int>() && usage["week_reset_sec"].is<uint32_t>()) {
    int sp = usage["session_pct"].as<int>();
    int wp = usage["week_pct"].as<int>();
    if (sp < 0) sp = 0; else if (sp > 100) sp = 100;
    if (wp < 0) wp = 0; else if (wp > 100) wp = 100;
    out->usageSessionPct = (uint8_t)sp;
    out->usageWeekPct = (uint8_t)wp;
    out->usageSessionResetSec = usage["session_reset_sec"].as<uint32_t>();
    out->usageWeekResetSec = usage["week_reset_sec"].as<uint32_t>();
    out->usageValid = true;
  } else {
    out->usageValid = false;
  }
  const char* m = doc["msg"];
  if (m) {
    strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0;
    _matrixify(out->msg);
  }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      strncpy(out->lines[n], s ? s : "", 91); out->lines[n][91]=0;
      // Transcript renders with a CJK u8g2 font (drawHUD), so leave the
      // raw UTF-8 bytes intact — only msg/promptTool/promptHint stay
      // ASCII-rendered and need matrixify.
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    strncpy(out->promptTool, pt  ? pt  : "", sizeof(out->promptTool)-1); out->promptTool[sizeof(out->promptTool)-1]=0;
    strncpy(out->promptHint, ph  ? ph  : "", sizeof(out->promptHint)-1); out->promptHint[sizeof(out->promptHint)-1]=0;
    _matrixify(out->promptTool);
    _matrixify(out->promptHint);
    // Don't matrixify promptId — it's an opaque token, must echo verbatim.
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }

  JsonObject q = doc["question"];
  JsonArray rootQuestions = doc["questions"];
  if (q.isNull() && !rootQuestions.isNull() && rootQuestions.size() > 0 &&
      rootQuestions[0].is<JsonObject>()) {
    JsonObject first = rootQuestions[0].as<JsonObject>();
    if (!first["options"].isNull() || !first["choices"].isNull() || !first["answers"].isNull()) {
      q = first;
    }
  }
  if (!q.isNull() || !rootQuestions.isNull()) {
    char oldId[sizeof(out->questionId)];
    strncpy(oldId, out->questionId, sizeof(oldId) - 1);
    oldId[sizeof(oldId) - 1] = 0;

    const char* qid = nullptr;
    const char* text = nullptr;
    JsonArray opts;
    if (!q.isNull()) {
      qid = q["id"] | q["request_id"] | doc["id"];
      text = q["prompt"] | q["text"] | q["question"] | doc["prompt"];
      opts = q["options"];
      if (opts.isNull()) opts = q["choices"];
      if (opts.isNull()) opts = q["answers"];
      if (opts.isNull()) opts = q["questions"];
    } else {
      qid = doc["id"] | doc["request_id"];
      text = doc["prompt"] | doc["text"] | doc["question"];
      opts = rootQuestions;
    }

    strncpy(out->questionId, qid ? qid : "", sizeof(out->questionId) - 1);
    out->questionId[sizeof(out->questionId) - 1] = 0;
    _copyMatrix(out->questionText, sizeof(out->questionText),
                text ? text : "Choose an option");
    out->questionCount = 0;
    for (JsonVariant v : opts) {
      if (out->questionCount >= 4) break;
      _copyMatrix(out->questionOptions[out->questionCount],
                  sizeof(out->questionOptions[out->questionCount]),
                  _textFromVariant(v));
      if (out->questionOptions[out->questionCount][0]) out->questionCount++;
    }
    if (out->questionCount == 0) out->questionId[0] = 0;
    if (strcmp(oldId, out->questionId) != 0) out->questionGen++;
  } else {
    out->questionId[0] = 0;
    out->questionText[0] = 0;
    out->questionCount = 0;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    static uint8_t lastDemoStage = 0xFF;
    if (now - _demoStageStart >= DEMO_STAGE_MS) {
      _demoStage = (_demoStage + 1) % DEMO_STAGE_COUNT;
      _demoStageStart = now;
    }
    bool stageChanged = (_demoStage != lastDemoStage);
    lastDemoStage = _demoStage;
    dataClearDemoState(out);
    out->sessionsTotal = 2;
    out->sessionsRunning = 0;
    out->sessionsWaiting = 0;
    out->tokensToday = 48200;
    out->lastUpdated = now;
    out->connected = true;
    out->usageValid = true;
    out->usageSessionPct = 38;
    out->usageWeekPct = 62;
    out->usageSessionResetSec = 53 * 60;
    out->usageWeekResetSec = 2UL * 24UL * 3600UL + 6UL * 3600UL;
    out->nLines = 2;

    switch (_demoStage) {
      case 0:
        snprintf(out->msg, sizeof(out->msg), "demo: widgets");
        strncpy(out->lines[0], "Home shows usage when the bridge sends it.", sizeof(out->lines[0]) - 1);
        strncpy(out->lines[1], "Swipe or open menu: demo keeps walking.", sizeof(out->lines[1]) - 1);
        break;
      case 1:
        snprintf(out->msg, sizeof(out->msg), "demo: pet");
        strncpy(out->lines[0], "Tap the buddy to pet it.", sizeof(out->lines[0]) - 1);
        strncpy(out->lines[1], "A quick hand and heart appear.", sizeof(out->lines[1]) - 1);
        break;
      case 2:
        out->sessionsWaiting = 1;
        snprintf(out->msg, sizeof(out->msg), "demo: approve");
        strncpy(out->promptId, "demo-permission", sizeof(out->promptId) - 1);
        strncpy(out->promptTool, "Edit file", sizeof(out->promptTool) - 1);
        strncpy(out->promptHint, "Allow this demo change?", sizeof(out->promptHint) - 1);
        strncpy(out->lines[0], "Permission prompt: tap approve or deny.", sizeof(out->lines[0]) - 1);
        strncpy(out->lines[1], "Demo responses stay on-device.", sizeof(out->lines[1]) - 1);
        break;
      case 3:
        snprintf(out->msg, sizeof(out->msg), "demo: choose");
        strncpy(out->questionId, "demo-question", sizeof(out->questionId) - 1);
        strncpy(out->questionText, "Pick a demo snack for the buddy", sizeof(out->questionText) - 1);
        strncpy(out->questionOptions[0], "Tiny battery cake", sizeof(out->questionOptions[0]) - 1);
        strncpy(out->questionOptions[1], "Pixel sandwich", sizeof(out->questionOptions[1]) - 1);
        strncpy(out->questionOptions[2], "Debug biscuits", sizeof(out->questionOptions[2]) - 1);
        strncpy(out->questionOptions[3], "Fresh tokens", sizeof(out->questionOptions[3]) - 1);
        out->questionCount = 4;
        strncpy(out->lines[0], "Question mode: tap an answer.", sizeof(out->lines[0]) - 1);
        strncpy(out->lines[1], "BOOT cycles; side switch confirms.", sizeof(out->lines[1]) - 1);
        break;
      case 4:
        out->usageSessionPct = 74;
        out->usageWeekPct = 46;
        out->usageSessionResetSec = 12 * 60;
        out->usageWeekResetSec = 4UL * 24UL * 3600UL + 3UL * 3600UL;
        snprintf(out->msg, sizeof(out->msg), "demo: usage");
        strncpy(out->lines[0], "Usage is bridge-provided, not guessed.", sizeof(out->lines[0]) - 1);
        strncpy(out->lines[1], "Settings widgets: off, simple, fun.", sizeof(out->lines[1]) - 1);
        break;
      default:
        out->recentlyCompleted = true;
        out->tokensToday = 50120;
        out->usageSessionPct = 21;
        out->usageWeekPct = 18;
        out->usageSessionResetSec = 3 * 3600UL + 41 * 60UL;
        out->usageWeekResetSec = 6UL * 24UL * 3600UL + 1UL * 3600UL;
        snprintf(out->msg, sizeof(out->msg), "demo: level up");
        strncpy(out->lines[0], "Level up celebrates every 50K tokens.", sizeof(out->lines[0]) - 1);
        strncpy(out->lines[1], "Demo animation does not save fake stats.", sizeof(out->lines[1]) - 1);
        break;
    }
    out->lines[0][sizeof(out->lines[0]) - 1] = 0;
    out->lines[1][sizeof(out->lines[1]) - 1] = 0;
    if (stageChanged) out->lineGen++;
    return;
  }

  _usbLine.feed(Serial, out);
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
