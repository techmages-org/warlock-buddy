// subctl-buddy CoreS3 firmware (v0.2.0).
//
// Layout (320×240 landscape):
//   24 px top:        status bar (firmware version, BLE state)
//   176 px middle:    LEFT half  — pet (18 ASCII species @ scale 2)
//                     RIGHT half — thought bubble with live subctl data
//   40 px bottom:     three touch zones: CYCLE / PET / MENU
//                     (or APPROVE / DENY / … when a prompt is in)
//
// Pet's persona reflects subctl posture:
//   pending prompt OR verdict=red   → ATTENTION (alarmed)
//   sessionsRunning≥1 OR convs≥3    → BUSY (typing animation)
//   verdict=yellow                   → DIZZY (woozy spin)
//   recentlyCompleted                → CELEBRATE oneshot
//   not connected OR everything calm → IDLE (slow micro-actions)

#include <M5Unified.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"
#include "character.h"

static const char* FW_VERSION = "0.2.0-cores3";

static const int W = 320, H = 240;
static const int STATUS_H = 24, TOUCH_H = 40;
static const int BODY_Y = STATUS_H;
static const int BODY_H = H - STATUS_H - TOUCH_H;

// Right-side bubble panel — pet occupies the left half.
static const int BUBBLE_X = 160;
static const int BUBBLE_W = 152;
static const int BUBBLE_Y = 32;
static const int BUBBLE_H = 152;

// The pet renderer (buddy.cpp) takes `extern M5Canvas spr;`, so this MUST
// be named `spr`. PSRAM-backed full-frame sprite — no flicker on redraws.
M5Canvas spr(&M5.Display);

enum Persona { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };

static char btName[16] = "subctl";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "subctl-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

static TamaState tama;
static uint8_t activeState = P_IDLE;
static uint8_t baseState   = P_IDLE;
static uint32_t oneShotUntil = 0;
static char lastPromptId[40] = "";
static bool responseSent = false;
static uint32_t promptArrivedMs = 0;
static uint32_t lastDraw = 0;

// Stats page takeover — PET button toggles. While open, the pet is hidden,
// the bubble is hidden, and the body area shows the buddy's profile.
static bool statsOpen = false;

// Pet display mode: GIF character (from LittleFS) vs ASCII species (from
// buddy.cpp). NVS sentinel SPECIES_GIF=0xFF picks the installed GIF; any
// value < N_SPECIES picks an ASCII species. CYCLE button rotates through
// GIF → species 0 → ... → species N-1 → GIF (if available).
static bool buddyMode = false;     // true = ASCII species; false = GIF
static bool gifAvailable = false;  // a character is installed on LittleFS
static const uint8_t SPECIES_GIF = 0xFF;

// Settings menu overlays.
static bool menuOpen  = false;
static bool aboutOpen = false;

// Brightness levels — index into BRIGHT_LEVELS, persisted to NVS.
static const uint8_t BRIGHT_LEVELS[] = {40, 80, 120, 180, 240};
static const uint8_t N_BRIGHT = 5;
static uint8_t brightIdx = 3;   // default 180 — matches the initial setBrightness call

static void applyBrightness() {
  M5.Display.setBrightness(BRIGHT_LEVELS[brightIdx]);
}
static void brightLoad() {
  Preferences p;
  p.begin("buddy", true);
  uint8_t v = p.getUChar("bright", 3);
  p.end();
  if (v < N_BRIGHT) brightIdx = v;
}
static void brightSave() {
  Preferences p;
  p.begin("buddy", false);
  p.putUChar("bright", brightIdx);
  p.end();
}

// Sound-gated beep — every tone() call in the firmware goes through here.
static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Speaker.tone(freq, dur);
}

static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {
    // GIF → first ASCII species
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {
    // Last species → back to GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

static void sendCmd(const char* json) {
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

// ─── verdict mapping ─────────────────────────────────────────────────────

static uint16_t verdictColor() {
  if (strcmp(tama.dispatchVerdict, "red")    == 0) return TFT_RED;
  if (strcmp(tama.dispatchVerdict, "yellow") == 0) return TFT_ORANGE;
  return TFT_GREEN;
}

static const char* verdictLabel() {
  if (strcmp(tama.dispatchVerdict, "red")    == 0) return "BLOCKED";
  if (strcmp(tama.dispatchVerdict, "yellow") == 0) return "WARN";
  return "GO";
}

static uint16_t acctDot(const AccountChip& a) {
  if (strcmp(a.color, "red")    == 0) return TFT_RED;
  if (strcmp(a.color, "yellow") == 0) return TFT_ORANGE;
  if (strcmp(a.color, "green")  == 0) return TFT_GREEN;
  return 0x4208;
}

static uint8_t derivePersona() {
  if (tama.promptId[0] && !responseSent)            return P_ATTENTION;
  if (!tama.connected)                              return P_IDLE;
  if (strcmp(tama.dispatchVerdict, "red") == 0)     return P_ATTENTION;
  // BUSY only when a real session is running — `activeConvs` includes
  // idle conversation files on disk (memory observer etc.), so it's
  // misleading as a "busy" signal.
  if (tama.sessionsRunning >= 1)                    return P_BUSY;
  if (strcmp(tama.dispatchVerdict, "yellow") == 0)  return P_DIZZY;
  return P_IDLE;
}

// ─── drawing ──────────────────────────────────────────────────────────────

static void drawStatusBar() {
  spr.fillRect(0, 0, W, STATUS_H, TFT_BLACK);
  spr.drawFastHLine(0, STATUS_H - 1, W, 0x4208);
  spr.setTextSize(1);
  spr.setTextDatum(top_left);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setCursor(6, 8);
  spr.printf("subctl-buddy %s", FW_VERSION);

  bool conn = bleConnected();
  bool sec  = bleSecure();
  uint16_t col = !conn ? TFT_DARKGRAY : (sec ? TFT_GREEN : TFT_ORANGE);
  const char* label = !conn ? "BLE: --" : (sec ? "BLE: secure" : "BLE: open");
  spr.setTextColor(col, TFT_BLACK);
  spr.setTextDatum(top_right);
  spr.drawString(label, W - 6, 8);
  spr.setTextDatum(top_left);
}

static void drawTouchBar() {
  bool inPrompt = tama.promptId[0] && !responseSent;
  spr.fillRect(0, H - TOUCH_H, W, TOUCH_H, 0x1082);
  spr.drawFastHLine(0, H - TOUCH_H, W, 0x4208);

  const int third = W / 3;
  if (inPrompt) {
    spr.fillRect(0,         H - TOUCH_H + 1, third,         TOUCH_H - 1, 0x0440);
    spr.fillRect(third,     H - TOUCH_H + 1, third,         TOUCH_H - 1, 0x4000);
    spr.fillRect(third * 2, H - TOUCH_H + 1, W - third * 2, TOUCH_H - 1, 0x1082);
  }

  spr.setTextDatum(middle_center);
  spr.setTextSize(2);
  if (inPrompt) {
    spr.setTextColor(TFT_WHITE);
    spr.drawString("APPROVE", third / 2,             H - TOUCH_H / 2);
    spr.drawString("DENY",    third + third / 2,     H - TOUCH_H / 2);
    spr.drawString("…",       third * 2 + third / 2, H - TOUCH_H / 2);
  } else if (statsOpen) {
    spr.setTextColor(TFT_LIGHTGRAY);
    spr.drawString("CLOSE", third / 2,             H - TOUCH_H / 2);
    spr.drawString("DONE",  third + third / 2,     H - TOUCH_H / 2);
    spr.drawString("MENU",  third * 2 + third / 2, H - TOUCH_H / 2);
  } else if (menuOpen || aboutOpen) {
    spr.setTextColor(TFT_LIGHTGRAY);
    spr.drawString("CLOSE", third / 2,             H - TOUCH_H / 2);
    spr.drawString("—",     third + third / 2,     H - TOUCH_H / 2);
    spr.drawString("—",     third * 2 + third / 2, H - TOUCH_H / 2);
  } else {
    spr.setTextColor(TFT_LIGHTGRAY);
    spr.drawString("CYCLE", third / 2,             H - TOUCH_H / 2);
    spr.drawString("PET",   third + third / 2,     H - TOUCH_H / 2);
    spr.drawString("MENU",  third * 2 + third / 2, H - TOUCH_H / 2);
  }
  spr.setTextDatum(top_left);
}

static void drawApproval() {
  spr.fillRect(0, BODY_Y, W, BODY_H, 0x2104);

  spr.setTextSize(1);
  spr.setTextColor(TFT_ORANGE, 0x2104);
  spr.setTextDatum(top_left);
  spr.setCursor(12, BODY_Y + 12);
  uint32_t waitedS = (millis() - promptArrivedMs) / 1000;
  spr.printf("approve? %lus", (unsigned long)waitedS);

  spr.setTextColor(TFT_WHITE, 0x2104);
  spr.setTextSize(3);
  spr.setCursor(12, BODY_Y + 36);
  char tool[14];
  strncpy(tool, tama.promptTool, sizeof(tool) - 1);
  tool[sizeof(tool) - 1] = 0;
  spr.print(tool);

  spr.setTextSize(1);
  spr.setTextColor(TFT_LIGHTGRAY, 0x2104);
  int hlen = strlen(tama.promptHint);
  for (int row = 0; row * 45 < hlen && row < 4; row++) {
    int start = row * 45;
    int len = hlen - start; if (len > 45) len = 45;
    char buf[46]; memcpy(buf, tama.promptHint + start, len); buf[len] = 0;
    spr.setCursor(12, BODY_Y + 88 + row * 12);
    spr.print(buf);
  }

  if (responseSent) {
    spr.setTextColor(TFT_DARKGRAY, 0x2104);
    spr.setCursor(12, BODY_Y + BODY_H - 16);
    spr.print("sent. waiting for subctl…");
  }
}

// Right-side thought bubble. The pet on the left is drawn into the same
// sprite by buddyTick(); this paints the right column only.
static void drawBubble() {
  spr.fillRect(BUBBLE_X, BODY_Y, W - BUBBLE_X, BODY_H, TFT_BLACK);

  spr.drawRoundRect(BUBBLE_X, BUBBLE_Y, BUBBLE_W, BUBBLE_H, 8, TFT_LIGHTGRAY);
  // No thought-bubble tail — the 3-circle convention didn't read on a
  // 320×240 LCD; the rounded rect on its own is enough chrome. If we want
  // to re-anchor the bubble to the pet later, prefer a single triangle
  // pointer over a trail of discs.

  const int pad = 8;
  int x = BUBBLE_X + pad;
  int y = BUBBLE_Y + pad;

  // Verdict tile — color-coded dot + big label, ready count to the right.
  uint16_t vcol = verdictColor();
  spr.fillCircle(x + 4, y + 8, 5, vcol);
  spr.setTextSize(2);
  spr.setTextColor(vcol, TFT_BLACK);
  spr.setCursor(x + 14, y + 1);
  spr.print(verdictLabel());

  spr.setTextSize(1);
  spr.setTextColor(TFT_LIGHTGRAY, TFT_BLACK);
  spr.setCursor(x + 90, y + 8);
  spr.printf("%u/%u", tama.dispatchReady, tama.dispatchTotal);
  y += 24;

  // Account chips — up to 5 rows.
  for (uint8_t i = 0; i < tama.nAccounts && i < 5; i++) {
    spr.fillCircle(x + 4, y + 4, 3, acctDot(tama.accounts[i]));
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setCursor(x + 14, y);
    // Strip the "claude-" / "openai-" prefix to fit; ~14 chars max.
    const char* alias = tama.accounts[i].alias;
    const char* shown = alias;
    if (strncmp(alias, "claude-", 7) == 0)  shown = alias + 7;
    if (strncmp(alias, "openai-", 7) == 0)  shown = alias + 7;
    char buf[16]; strncpy(buf, shown, 15); buf[15] = 0;
    spr.print(buf);
    // Provider tag in dim text on the right
    spr.setTextColor(TFT_DARKGRAY, TFT_BLACK);
    spr.setCursor(x + BUBBLE_W - 2 * pad - 24, y);
    if (strncmp(alias, "claude-", 7) == 0)      spr.print("cld");
    else if (strncmp(alias, "openai-", 7) == 0) spr.print("oai");
    y += 12;
  }

  y += 4;

  // Live conversations summary.
  spr.setTextColor(TFT_LIGHTGRAY, TFT_BLACK);
  spr.setCursor(x, y);
  if (tama.activeConvs > 0 && tama.topProject[0]) {
    spr.printf("%u live", tama.activeConvs);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setCursor(x, y + 12);
    spr.printf("%.14s %us", tama.topProject, (unsigned)tama.topAgeS);
  } else {
    spr.printf("%u live conversations", tama.activeConvs);
  }
  y += 26;

  // Cost saved this month — vanity stat in a small font.
  spr.setTextColor(0xFFE0, TFT_BLACK);  // soft yellow
  spr.setCursor(x, y);
  if (tama.costSavedMonth >= 1000) {
    spr.printf("$%lu.%luK saved",
               (unsigned long)(tama.costSavedMonth / 1000),
               (unsigned long)((tama.costSavedMonth / 100) % 10));
  } else {
    spr.printf("$%lu saved", (unsigned long)tama.costSavedMonth);
  }
  y += 11;

  // Heartbeat pulse — confirms the bridge is alive.
  spr.setTextColor(TFT_DARKGRAY, TFT_BLACK);
  spr.setCursor(x, y);
  uint32_t age = tama.lastUpdated ? (millis() - tama.lastUpdated) / 1000 : 0;
  spr.printf("last msg %lus", (unsigned long)age);
}

// Buddy profile page — full body-area takeover. Stats come from stats.h
// (in scope via data.h). Mood/fed/energy fill in as the operator uses
// the buddy; numbers will be small until approvals accumulate.
static void drawStats() {
  spr.fillRect(0, BODY_Y, W, BODY_H, TFT_BLACK);

  // Title row — "buddy profile" + Level chip on the right.
  spr.setTextSize(2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextDatum(top_left);
  spr.setCursor(12, BODY_Y + 8);
  spr.print("buddy profile");

  char lvl[8];
  snprintf(lvl, sizeof(lvl), "Lv %u", stats().level);
  spr.fillRoundRect(W - 64, BODY_Y + 8, 52, 18, 4, 0x3C18);
  spr.setTextColor(TFT_WHITE, 0x3C18);
  spr.setTextDatum(middle_center);
  spr.drawString(lvl, W - 38, BODY_Y + 17);
  spr.setTextDatum(top_left);

  // Three meter rows: mood / fed / energy.
  int y = BODY_Y + 36;
  const int xLabel = 12;
  const int xMeter = 96;

  spr.setTextSize(1);

  // mood — 4 hearts, filled to tier
  spr.setTextColor(TFT_LIGHTGRAY, TFT_BLACK);
  spr.setCursor(xLabel, y);
  spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = mood >= 3 ? 0xF81F : mood >= 2 ? 0xFA20 : 0x8410;
  for (int i = 0; i < 4; i++) {
    int cx = xMeter + i * 18;
    if (i < mood) spr.fillCircle(cx + 5, y + 4, 5, moodCol);
    else          spr.drawCircle(cx + 5, y + 4, 5, moodCol);
  }
  y += 16;

  // fed — 10 pips, drives token level
  spr.setCursor(xLabel, y);
  spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int cx = xMeter + i * 14;
    if (i < fed) spr.fillCircle(cx + 4, y + 4, 4, 0x07FF);
    else         spr.drawCircle(cx + 4, y + 4, 4, 0x4208);
  }
  y += 16;

  // energy — 5 bars, drains over time
  spr.setCursor(xLabel, y);
  spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = en >= 4 ? TFT_GREEN : en >= 2 ? 0xFD20 : TFT_RED;
  for (int i = 0; i < 5; i++) {
    int rx = xMeter + i * 22;
    if (i < en) spr.fillRect(rx, y, 18, 8, enCol);
    else        spr.drawRect(rx, y, 18, 8, 0x4208);
  }
  y += 20;

  // Tally rows
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setCursor(xLabel, y);
  spr.printf("approved   %u", stats().approvals);
  spr.setTextColor(TFT_LIGHTGRAY, TFT_BLACK);
  spr.setCursor(W / 2 + 8, y);
  spr.printf("denied  %u", stats().denials);
  y += 12;

  spr.setCursor(xLabel, y);
  uint32_t nap = stats().napSeconds;
  spr.printf("napped     %luh %02lum",
             (unsigned long)(nap / 3600),
             (unsigned long)((nap / 60) % 60));
  y += 12;

  // Token formatter — same kK/kM pattern the StickC profile uses.
  auto tokFmt = [&](const char* label, uint32_t v) {
    spr.setCursor(xLabel, y);
    if      (v >= 1000000) spr.printf("%s%lu.%luM", label,
                              (unsigned long)(v / 1000000),
                              (unsigned long)((v / 100000) % 10));
    else if (v >= 1000)    spr.printf("%s%lu.%luK", label,
                              (unsigned long)(v / 1000),
                              (unsigned long)((v / 100) % 10));
    else                   spr.printf("%s%lu", label, (unsigned long)v);
    y += 12;
  };
  tokFmt("tokens     ", stats().tokens);
  tokFmt("today      ", tama.tokensToday);

  // Species name footer — reminds the operator which pet they're petting.
  spr.setTextColor(TFT_DARKGRAY, TFT_BLACK);
  spr.setCursor(xLabel, BODY_Y + BODY_H - 12);
  spr.printf("species: %s", buddySpeciesName());
}

// ─── settings menu ───────────────────────────────────────────────────────

static const int MENU_ITEM_H = 38;
static const int MENU_TOP    = BODY_Y + 6;   // first item top edge

struct MenuItem {
  const char* label;
  const char* (*value)();
  void (*activate)();
};

static char _mvBuf[16];
static const char* mvBright() {
  snprintf(_mvBuf, sizeof(_mvBuf), "%u/%u", brightIdx + 1, N_BRIGHT);
  return _mvBuf;
}
static const char* mvSound() { return settings().sound ? "on" : "off"; }
static const char* mvDemo()  { return dataDemo()       ? "on" : "off"; }
static const char* mvAbout() { return ">";          }

static void mActBright() {
  brightIdx = (brightIdx + 1) % N_BRIGHT;
  applyBrightness();
  brightSave();
}
static void mActSound() {
  settings().sound = !settings().sound;
  settingsSave();
}
static void mActDemo()  { dataSetDemo(!dataDemo()); }
static void mActAbout() { aboutOpen = true; menuOpen = false; }

static const MenuItem MENU_ITEMS[] = {
  {"brightness", mvBright, mActBright},
  {"sound",      mvSound,  mActSound},
  {"demo mode",  mvDemo,   mActDemo},
  {"about",      mvAbout,  mActAbout},
};
static const uint8_t N_MENU = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

static void drawMenu() {
  spr.fillRect(0, BODY_Y, W, BODY_H, TFT_BLACK);

  for (uint8_t i = 0; i < N_MENU; i++) {
    int y = MENU_TOP + i * MENU_ITEM_H;
    int rh = MENU_ITEM_H - 6;
    spr.fillRoundRect(8, y, W - 16, rh, 6, 0x1082);
    spr.drawRoundRect(8, y, W - 16, rh, 6, 0x4208);

    spr.setTextSize(2);
    spr.setTextColor(TFT_WHITE, 0x1082);
    spr.setTextDatum(middle_left);
    spr.drawString(MENU_ITEMS[i].label, 22, y + rh / 2);

    spr.setTextSize(1);
    spr.setTextColor(0xFFE0, 0x1082);   // soft yellow values
    spr.setTextDatum(middle_right);
    spr.drawString(MENU_ITEMS[i].value(), W - 22, y + rh / 2);
  }
  spr.setTextDatum(top_left);
}

static void drawAbout() {
  spr.fillRect(0, BODY_Y, W, BODY_H, TFT_BLACK);

  spr.setTextSize(2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextDatum(top_left);
  spr.setCursor(12, BODY_Y + 8);
  spr.print("about");

  int y = BODY_Y + 38;
  const int xL = 12, xV = 110;
  spr.setTextSize(1);

  auto row = [&](const char* label, const char* value) {
    spr.setTextColor(TFT_LIGHTGRAY, TFT_BLACK);
    spr.setCursor(xL, y); spr.print(label);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setCursor(xV, y); spr.print(value);
    y += 14;
  };

  row("firmware", FW_VERSION);
  row("bt name",  btName);

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  char macStr[20];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  row("bt mac",   macStr);

  char buf[32];
  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), "%luh %02lum",
           (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60));
  row("uptime",   buf);

  snprintf(buf, sizeof(buf), "%uKB free", ESP.getFreeHeap() / 1024);
  row("heap",     buf);

  row("owner",    ownerName()[0] ? ownerName() : "(unset)");

  // Credit footer
  spr.setTextColor(TFT_DARKGRAY, TFT_BLACK);
  spr.setCursor(xL, BODY_Y + BODY_H - 22);
  spr.print("subctl-buddy");
  spr.setCursor(xL, BODY_Y + BODY_H - 10);
  spr.print("by Jason Brashear");
}

static void drawSplash() {
  spr.clear(TFT_BLACK);
  spr.setTextDatum(middle_center);
  spr.setTextSize(4);
  spr.setTextColor(TFT_WHITE);
  spr.drawString("subctl", W / 2, H / 2 - 20);
  spr.setTextSize(1);
  spr.setTextColor(TFT_DARKGRAY);
  spr.drawString("buddy awakens", W / 2, H / 2 + 14);
  spr.drawString(FW_VERSION, W / 2, H / 2 + 30);
  spr.drawString(btName, W / 2, H - 18);
  spr.setTextDatum(top_left);
}

static void drawPasskey(uint32_t pk) {
  spr.clear(TFT_BLACK);
  spr.setTextDatum(top_left);
  spr.setTextSize(1);
  spr.setTextColor(TFT_DARKGRAY);
  spr.setCursor(12, 12);
  spr.print("BLUETOOTH PAIRING");

  spr.setTextDatum(middle_center);
  spr.setTextSize(6);
  spr.setTextColor(TFT_WHITE);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)pk);
  spr.drawString(b, W / 2, H / 2);

  spr.setTextSize(1);
  spr.setTextColor(TFT_DARKGRAY);
  spr.drawString("enter on Mac", W / 2, H - 20);
  spr.setTextDatum(top_left);
}

// ─── input ────────────────────────────────────────────────────────────────

// On CoreS3 the legacy M5.BtnA/B/C aren't auto-mapped to touch zones the way
// they are on M5StickC; we read M5.Touch directly.
//
// Touch surfaces:
//   - Pet area (x<160, body y range) → heart-eyes one-shot (LOVE)
//   - Touch bar (y >= H-60), divided into thirds:
//       zone 0 = APPROVE / CLOSE-stats / CYCLE species
//       zone 1 = DENY / PET (toggle stats page)
//       zone 2 = MENU (placeholder)
static void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;

  bool inPrompt = tama.promptId[0] && !responseSent;

  // About sub-page: any touch in the touch bar returns to dashboard.
  if (aboutOpen) {
    if (t.y >= H - 60) {
      aboutOpen = false;
      buddyInvalidate();
      characterInvalidate();
      lastDraw = 0;
      beep(1500, 40);
    }
    return;
  }

  // Menu overlay: body taps activate items; any touch-bar tap closes.
  if (menuOpen) {
    if (t.y >= MENU_TOP && t.y < MENU_TOP + (int)N_MENU * MENU_ITEM_H && t.y < H - 60) {
      uint8_t idx = (t.y - MENU_TOP) / MENU_ITEM_H;
      if (idx < N_MENU) {
        MENU_ITEMS[idx].activate();
        beep(1800, 40);
        lastDraw = 0;   // force immediate repaint so new value shows
      }
      return;
    }
    if (t.y >= H - 60) {
      menuOpen = false;
      buddyInvalidate();
      characterInvalidate();
      lastDraw = 0;
      beep(1500, 40);
    }
    return;
  }

  // Tap the pet itself → LOVE. Only when nothing else is taking over the
  // body area.
  if (!inPrompt && !statsOpen
      && t.y >= BODY_Y && t.y < H - 60
      && t.x < BUBBLE_X) {
    activeState = P_HEART;
    oneShotUntil = millis() + 2500;
    beep(1800, 60);
    return;
  }

  if (t.y < H - 60) return;   // ignore non-bar body taps that didn't hit the pet

  int third = W / 3;
  int zone = t.x / third;
  if (zone > 2) zone = 2;

  if (zone == 0) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
        tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      beep(2400, 60);
    } else if (statsOpen) {
      // CLOSE stats — both invalidators because either pet mode could be
      // active and the stats overlay painted over the pet column.
      statsOpen = false;
      buddyInvalidate();
      characterInvalidate();
      beep(1800, 40);
    } else {
      // CYCLE — rotates through GIF (if installed) and the 18 ASCII species.
      nextPet();
      beep(1800, 40);
    }
  } else if (zone == 1) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}",
        tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      beep(600, 60);
    } else {
      // PET — toggle stats page
      statsOpen = !statsOpen;
      if (!statsOpen) {
        buddyInvalidate();
        characterInvalidate();
      }
      beep(1500, 60);
    }
  } else {
    // MENU — open settings overlay.
    menuOpen = true;
    lastDraw = 0;
    beep(1200, 40);
  }
}

// ─── setup / loop ─────────────────────────────────────────────────────────

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(180);
  // Volume 0..255; 32 is a polite ~12% — beeps audible but not jarring.
  M5.Speaker.setVolume(32);

  spr.setPsram(true);
  spr.createSprite(W, H);
  spr.fillSprite(TFT_BLACK);

  // Seed verdict so first frame isn't blank before the bridge connects.
  strncpy(tama.dispatchVerdict, "green", sizeof(tama.dispatchVerdict) - 1);

  statsLoad();
  settingsLoad();
  brightLoad();
  applyBrightness();   // override the 180 set above with the persisted level

  buddyInit();
  // Scale 2 — the pet's "home screen" size. Without this it defaults to
  // peek mode (scale 1) which is sized for the StickC's 135px portrait,
  // not the CoreS3's 320px landscape.
  buddySetPeek(false);

  // Look for an installed GIF character on LittleFS (scans /characters/).
  // If found, default to GIF mode; otherwise stay in ASCII species mode.
  characterInit(nullptr);
  gifAvailable = characterLoaded();
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);

  startBt();

  drawSplash();
  spr.pushSprite(0, 0);
  delay(1500);

  spr.fillSprite(TFT_BLACK);
  drawStatusBar();
  drawBubble();
  drawTouchBar();
  spr.pushSprite(0, 0);
}

void loop() {
  M5.update();
  uint32_t now = millis();

  dataPoll(&tama);

  // Passkey takeover — drives all other UI off the screen until pairing
  // completes. Same pattern as the StickC.
  static uint32_t lastPk = 0;
  uint32_t pk = blePasskey();
  if (pk) {
    if (pk != lastPk) {
      lastPk = pk;
      drawPasskey(pk);
      spr.pushSprite(0, 0);
      beep(1800, 60);
    }
    delay(50);
    return;
  } else if (lastPk) {
    lastPk = 0;
    spr.fillSprite(TFT_BLACK);
    drawStatusBar();
    drawBubble();
    drawTouchBar();
    spr.pushSprite(0, 0);
  }

  // Prompt arrival edge — beep + force-close any overlay so the approval
  // panel is unambiguously on top.
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = now;
      statsOpen = false;
      menuOpen = false;
      aboutOpen = false;
      beep(1200, 80);
    }
  }

  handleTouch();

  // Level-up celebration — triggered from stats.h when cumulative tokens
  // crosses a TOKENS_PER_LEVEL boundary. Overrides whatever derive() says
  // for ~4s, then base state takes over again.
  if (statsPollLevelUp()) {
    activeState = P_CELEBRATE;
    oneShotUntil = now + 4000;
    beep(2200, 80);
  }

  baseState = derivePersona();
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Render order:
  //   1. Pet renders into left column (buddyTick self-clears its strip).
  //      Skipped during prompt or stats overlays — they take over the body.
  //   2. Bubble / approval / stats + status bar redraw on state change or
  //      every 1s heartbeat.
  //   3. Single pushSprite blits the full frame.
  if (!inPrompt && !statsOpen && !menuOpen && !aboutOpen) {
    if (buddyMode) {
      buddyTick(activeState);
    } else if (characterLoaded()) {
      characterSetState(activeState);
      characterTick();
    }
  }

  static uint8_t lastVerdictHash = 0xFF;
  static uint8_t lastNAccts = 0xFF;
  static uint8_t lastActiveConvs = 0xFF;
  static uint32_t lastCost = 0xFFFFFFFF;
  static bool lastInPrompt = false;
  static bool lastStatsOpen = false;
  static bool lastMenuOpen = false;
  static bool lastAboutOpen = false;
  uint8_t verdictHash = tama.dispatchVerdict[0]
                     ^ (tama.dispatchReady << 4)
                     ^ (tama.dispatchTotal << 1);
  bool dataChanged =
       verdictHash != lastVerdictHash
    || tama.nAccounts != lastNAccts
    || tama.activeConvs != lastActiveConvs
    || tama.costSavedMonth != lastCost
    || inPrompt != lastInPrompt
    || statsOpen != lastStatsOpen
    || menuOpen != lastMenuOpen
    || aboutOpen != lastAboutOpen;

  if (dataChanged || now - lastDraw > 1000) {
    lastDraw = now;
    lastVerdictHash = verdictHash;
    lastNAccts = tama.nAccounts;
    lastActiveConvs = tama.activeConvs;
    lastCost = tama.costSavedMonth;
    lastInPrompt = inPrompt;
    lastStatsOpen = statsOpen;
    lastMenuOpen = menuOpen;
    lastAboutOpen = aboutOpen;
    drawStatusBar();
    if (inPrompt) {
      drawApproval();
    } else if (aboutOpen) {
      drawAbout();
    } else if (menuOpen) {
      drawMenu();
    } else if (statsOpen) {
      drawStats();
    } else {
      drawBubble();
    }
    drawTouchBar();
  }

  spr.pushSprite(0, 0);
  delay(20);
}
