// warlock-buddy CoreS3 firmware (v0.1.0) — "hacker HUD".
//
// A desk-side command surface for the Warlock pentest deck. Mirrors the
// engagement gate and drives it with a tap:
//   • Hero gate banner   SAFE / ATTENTION (staged) / ARMED / OFFLINE
//   • Telemetry grid      scope · sigils (signed AAR) · temp/cpu/mem/mesh/gps/sdr
//   • Tap-twice gate      ARM a staged engagement · END · KILLSWITCH
//
// Aesthetic: black CRT, phosphor green, amber for staged, red for ARMED,
// scanlines + corner brackets. The bridge (BLE NUS JSON) feeds heartbeat.warlock.*
// and the gate commands ({cmd:"arm",id} / {cmd:"end",id} / {cmd:"killswitch"})
// ride back to Warlock's audited endpoints.

#include <M5Unified.h>
#include "ble_bridge.h"
#include "data.h"

static const char* FW_VERSION = "0.1.0-cores3";

static const int W = 320, H = 240;
static const int STATUS_H = 22, TOUCH_H = 40;
static const int BODY_Y = STATUS_H;
static const int BODY_H = H - STATUS_H - TOUCH_H;

// ─── palette (RGB565) ──────────────────────────────────────────────────────
static const uint16_t C_BLACK  = 0x0000;
static const uint16_t C_GREEN  = 0x07E0;  // phosphor
static const uint16_t C_GDIM   = 0x03E0;  // dim green (chrome)
static const uint16_t C_SCAN   = 0x0140;  // scanline green (very faint)
static const uint16_t C_AMBER  = 0xFD20;  // staged / warn
static const uint16_t C_RED     = 0xF800; // armed / kill
static const uint16_t C_REDDIM  = 0x5800;
static const uint16_t C_CYAN    = 0x07FF; // values
static const uint16_t C_WHITE   = 0xFFFF;
static const uint16_t C_GRAY     = 0x8410;
static const uint16_t C_DGRAY    = 0x4208;

// PSRAM-backed full-frame sprite — flicker-free. (buddy.cpp/character.cpp
// also extern this symbol; they are compiled but unused by the HUD.)
M5Canvas spr(&M5.Display);

static TamaState tama;
static uint32_t lastDraw = 0;

// Settings overlays.
static bool menuOpen = false, aboutOpen = false;

// Which staged engagement the ARM zone targets.
static uint8_t draftSel = 0;

// Tap-twice confirm for consequential gate actions.
enum GateAction { GA_NONE, GA_ARM, GA_END, GA_KILL };
static GateAction pending = GA_NONE;
static uint32_t   pendingUntil = 0;
static const uint32_t CONFIRM_MS = 3000;

// BLE advertised name → "warlock-XXXX".
static char btName[16] = "warlock";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "warlock-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

// ─── brightness (persisted) ────────────────────────────────────────────────
static const uint8_t BRIGHT_LEVELS[] = {40, 80, 120, 180, 240};
static const uint8_t N_BRIGHT = 5;
static uint8_t brightIdx = 3;
static void applyBrightness() { M5.Display.setBrightness(BRIGHT_LEVELS[brightIdx]); }
static void brightLoad() {
  Preferences p; p.begin("warlock", true);
  uint8_t v = p.getUChar("bright", 3); p.end();
  if (v < N_BRIGHT) brightIdx = v;
}
static void brightSave() {
  Preferences p; p.begin("warlock", false);
  p.putUChar("bright", brightIdx); p.end();
}

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Speaker.tone(freq, dur);
}

static void sendCmd(const char* json) {
  bleWrite((const uint8_t*)json, strlen(json));
  bleWrite((const uint8_t*)"\n", 1);
}

// persona → accent color
static uint16_t personaColor() {
  if (!tama.online)                          return C_DGRAY;
  if (tama.engaged)                          return C_RED;
  if (tama.nDrafts > 0)                      return C_AMBER;
  return C_GREEN;
}

static bool blink() { return (millis() / 450) % 2; }

// ─── chrome helpers ────────────────────────────────────────────────────────
static void scanlines(int y0, int h) {
  for (int y = y0; y < y0 + h; y += 3) spr.drawFastHLine(0, y, W, C_SCAN);
}

static void brackets(int x, int y, int w, int h, uint16_t c, int len) {
  spr.drawFastHLine(x, y, len, c);             spr.drawFastVLine(x, y, len, c);
  spr.drawFastHLine(x + w - len, y, len, c);   spr.drawFastVLine(x + w - 1, y, len, c);
  spr.drawFastHLine(x, y + h - 1, len, c);     spr.drawFastVLine(x, y + h - len, len, c);
  spr.drawFastHLine(x + w - len, y + h - 1, len, c); spr.drawFastVLine(x + w - 1, y + h - len, len, c);
}

// ─── status bar ────────────────────────────────────────────────────────────
static void drawStatusBar() {
  spr.fillRect(0, 0, W, STATUS_H, C_BLACK);

  spr.setTextDatum(top_left);
  spr.setTextSize(2);
  spr.setTextColor(C_GREEN, C_BLACK);
  spr.setCursor(6, 4);
  spr.print("WaRL0c");
  // little cursor block after the wordmark — terminal vibe
  if (blink()) spr.fillRect(6 + 6 * 12 + 2, 5, 7, 13, C_GDIM);

  bool conn = bleConnected(), sec = bleSecure();
  uint16_t lc = !conn ? C_DGRAY : (sec ? C_GREEN : C_AMBER);
  const char* ls = !conn ? "LINK --" : (sec ? "LINK SEC" : "LINK OPN");
  spr.setTextSize(1);
  spr.setTextColor(lc, C_BLACK);
  spr.setTextDatum(top_right);
  spr.drawString(ls, W - 6, 3);
  // 3-bar signal block
  for (int i = 0; i < 3; i++) {
    int bx = W - 60 + i * 6, bh = 4 + i * 4;
    uint16_t bc = conn ? lc : C_DGRAY;
    if (conn) spr.fillRect(bx, 16 - bh, 4, bh, bc);
    else      spr.drawRect(bx, 16 - bh, 4, bh, C_DGRAY);
  }
  spr.setTextDatum(top_left);
  spr.drawFastHLine(0, STATUS_H - 1, W, C_GDIM);
}

// ─── hero gate banner ──────────────────────────────────────────────────────
static void drawHero() {
  const int x = 6, y = 28, w = W - 12, h = 84;
  uint16_t c = personaColor();

  // double frame + corner brackets
  spr.drawRect(x, y, w, h, c);
  spr.drawRect(x + 2, y + 2, w - 4, h - 4, (c == C_RED) ? C_REDDIM : C_GDIM);
  brackets(x - 0, y - 0, w, h, c, 12);

  // confirm overlay takes over the hero while a gate action is pending
  if (pending != GA_NONE && (int32_t)(pendingUntil - millis()) > 0) {
    spr.setTextDatum(middle_center);
    spr.setTextColor(blink() ? C_WHITE : c, C_BLACK);
    spr.setTextSize(2);
    const char* l1 = pending == GA_KILL ? "CONFIRM KILLSWITCH"
                   : pending == GA_END  ? "CONFIRM END"
                                        : "CONFIRM ARM";
    spr.drawString(l1, W / 2, y + 30);
    spr.setTextSize(1);
    spr.setTextColor(C_GRAY, C_BLACK);
    char l2[40];
    if (pending == GA_ARM)      snprintf(l2, sizeof(l2), "%.24s", tama.draftName[draftSel]);
    else if (pending == GA_END) snprintf(l2, sizeof(l2), "%.24s", tama.engName);
    else                        snprintf(l2, sizeof(l2), "abort all offensive ops");
    spr.drawString(l2, W / 2, y + 52);
    spr.setTextColor(C_DGRAY, C_BLACK);
    spr.drawString("tap again to confirm  ·  wait to cancel", W / 2, y + 68);
    spr.setTextDatum(top_left);
    return;
  }

  // status glyph (drawn, not font) + big word
  const char* word;
  if (!tama.online)      word = "OFFLINE";
  else if (tama.engaged) word = "ARMED";
  else                   word = "SAFE";

  int gx = x + 18, gy = y + 26;  // glyph center
  if (!tama.online) {
    spr.drawLine(gx - 8, gy - 8, gx + 8, gy + 8, C_DGRAY);
    spr.drawLine(gx - 8, gy + 8, gx + 8, gy - 8, C_DGRAY);
  } else if (tama.engaged) {
    uint16_t tc = blink() ? C_RED : C_REDDIM;     // pulsing warning triangle
    spr.fillTriangle(gx, gy - 10, gx - 10, gy + 8, gx + 10, gy + 8, tc);
    spr.fillRect(gx - 1, gy - 4, 2, 7, C_BLACK);
    spr.fillRect(gx - 1, gy + 5, 2, 2, C_BLACK);
  } else {
    spr.fillCircle(gx, gy, 9, (tama.nDrafts > 0) ? C_AMBER : C_GREEN);
    spr.fillCircle(gx, gy, 4, C_BLACK);
  }

  spr.setTextDatum(middle_left);
  spr.setTextColor(c, C_BLACK);
  spr.setTextSize(4);
  spr.drawString(word, x + 38, y + 26);

  // subtitle / detail
  spr.setTextSize(1);
  spr.setTextDatum(top_left);
  if (!tama.online) {
    spr.setTextColor(C_DGRAY, C_BLACK);
    spr.drawString("deck unreachable — check bridge / link", x + 14, y + 58);
  } else if (tama.engaged) {
    spr.setTextColor(C_WHITE, C_BLACK);
    spr.setTextSize(2);
    char nm[22]; snprintf(nm, sizeof(nm), "%.20s", tama.engName[0] ? tama.engName : "engagement");
    spr.drawString(nm, x + 14, y + 50);
    spr.setTextSize(1);
    char sc[40];
    snprintf(sc, sizeof(sc), "SCOPE  ssid:%u  bssid:%u  cidr:%u",
             tama.scopeSsids, tama.scopeBssids, tama.scopeIps);
    spr.setTextColor(C_GRAY, C_BLACK);
    spr.drawString(sc, x + 14, y + 70);
  } else if (tama.nDrafts > 0) {
    spr.setTextColor(C_AMBER, C_BLACK);
    char st[40];
    snprintf(st, sizeof(st), "%u STAGED — TAP ARM", tama.nDrafts);
    spr.drawString(st, x + 14, y + 56);
    spr.setTextColor(C_GRAY, C_BLACK);
    char dn[40]; snprintf(dn, sizeof(dn), "> %.30s", tama.draftName[draftSel]);
    spr.drawString(dn, x + 14, y + 68);
  } else {
    spr.setTextColor(C_GDIM, C_BLACK);
    spr.drawString("no active engagement", x + 14, y + 60);
  }
}

// ─── telemetry grid ────────────────────────────────────────────────────────
static void cell(int x, int y, const char* label, const char* value, uint16_t vcol) {
  spr.setTextSize(1);
  spr.setTextDatum(top_left);
  spr.setTextColor(C_GDIM, C_BLACK);
  spr.drawString(label, x, y);
  spr.setTextColor(vcol, C_BLACK);
  spr.drawString(value, x + 52, y);
}

static void drawTelemetry() {
  const int yTop = 118;
  spr.drawFastHLine(6, yTop - 4, W - 12, C_GDIM);

  char b[24];
  const int xL = 10, xR = 168;
  int y = yTop;

  // SIGILS = signed AAR attestations (the hero stat)
  snprintf(b, sizeof(b), "%lu", (unsigned long)tama.aarRecords);
  cell(xL, y, "SIGILS", b, C_CYAN);
  cell(xR, y, "AAR", tama.aarEnabled ? "L1 ON" : "off", tama.aarEnabled ? C_GREEN : C_DGRAY);
  y += 15;

  snprintf(b, sizeof(b), "%u/%u/%u", tama.scopeSsids, tama.scopeBssids, tama.scopeIps);
  cell(xL, y, "SCOPE", b, tama.engaged ? C_WHITE : C_DGRAY);
  if (tama.meshNodes >= 0) snprintf(b, sizeof(b), "%d", tama.meshNodes); else snprintf(b, sizeof(b), "--");
  cell(xR, y, "MESH", b, C_WHITE);
  y += 15;

  if (tama.haveTemp) snprintf(b, sizeof(b), "%.0fC", tama.tempC); else snprintf(b, sizeof(b), "--");
  cell(xL, y, "TEMP", b, tama.haveTemp && tama.tempC > 75 ? C_RED : C_WHITE);
  if (tama.haveCpu) snprintf(b, sizeof(b), "%.0f%%", tama.cpuPct); else snprintf(b, sizeof(b), "--");
  cell(xR, y, "CPU", b, C_WHITE);
  y += 15;

  if (tama.haveMem) snprintf(b, sizeof(b), "%.0f%%", tama.memPct); else snprintf(b, sizeof(b), "--");
  cell(xL, y, "MEM", b, C_WHITE);
  if (tama.sdrCount >= 0) snprintf(b, sizeof(b), "%d", tama.sdrCount); else snprintf(b, sizeof(b), "--");
  cell(xR, y, "SDR", b, C_WHITE);
  y += 15;

  // footer: operator + uplink heartbeat
  spr.drawFastHLine(6, y + 1, W - 12, C_GDIM);
  spr.setTextColor(C_GDIM, C_BLACK);
  spr.setTextDatum(top_left);
  char op[40];
  snprintf(op, sizeof(op), "OP %.10s", ownerName()[0] ? ownerName() : "operator");
  spr.drawString(op, xL, y + 6);

  spr.setTextColor(tama.gpsFix ? C_GREEN : C_DGRAY, C_BLACK);
  spr.drawString(tama.gpsFix ? "GPS FIX" : "GPS --", 132, y + 6);

  uint32_t age = tama.lastUpdated ? (millis() - tama.lastUpdated) / 1000 : 0;
  snprintf(op, sizeof(op), "uplink %lus", (unsigned long)age);
  spr.setTextColor(C_DGRAY, C_BLACK);
  spr.setTextDatum(top_right);
  spr.drawString(op, W - 8, y + 6);
  if (tama.online && blink()) spr.fillCircle(W - 8 - 6 * (int)strlen(op) - 6, y + 9, 2, C_GREEN);
  spr.setTextDatum(top_left);
}

// ─── touch bar ─────────────────────────────────────────────────────────────
static void zoneLabel(int zone, const char* s, uint16_t fg, uint16_t bg) {
  const int third = W / 3;
  int x0 = zone * third;
  int wz = (zone == 2) ? (W - third * 2) : third;
  spr.fillRect(x0, H - TOUCH_H + 1, wz, TOUCH_H - 1, bg);
  spr.setTextDatum(middle_center);
  spr.setTextSize(2);
  spr.setTextColor(fg, bg);
  spr.drawString(s, x0 + wz / 2, H - TOUCH_H / 2 + 1);
  spr.setTextDatum(top_left);
}

static void drawTouchBar() {
  spr.fillRect(0, H - TOUCH_H, W, TOUCH_H, C_BLACK);
  spr.drawFastHLine(0, H - TOUCH_H, W, C_GDIM);

  if (menuOpen || aboutOpen) {
    zoneLabel(0, "CLOSE", C_GREEN, C_BLACK);
    zoneLabel(1, "-", C_DGRAY, C_BLACK);
    zoneLabel(2, "-", C_DGRAY, C_BLACK);
    return;
  }
  if (tama.engaged) {
    zoneLabel(0, "KILL", C_WHITE, blink() ? 0x6000 : 0x3800);  // pulsing red
    zoneLabel(1, "END",  C_AMBER, C_BLACK);
    zoneLabel(2, "MENU", C_GRAY,  C_BLACK);
  } else if (tama.online && tama.nDrafts > 0) {
    zoneLabel(0, "ARM",  C_BLACK, C_GREEN);
    zoneLabel(1, tama.nDrafts > 1 ? "NEXT" : "-", tama.nDrafts > 1 ? C_CYAN : C_DGRAY, C_BLACK);
    zoneLabel(2, "MENU", C_GRAY,  C_BLACK);
  } else {
    zoneLabel(0, "-", C_DGRAY, C_BLACK);
    zoneLabel(1, "-", C_DGRAY, C_BLACK);
    zoneLabel(2, "MENU", C_GRAY, C_BLACK);
  }
}

// ─── settings menu ─────────────────────────────────────────────────────────
static const int MENU_ITEM_H = 38;
static const int MENU_TOP = BODY_Y + 6;
struct MenuItem { const char* label; const char* (*value)(); void (*activate)(); };
static char _mvBuf[16];
static const char* mvBright(){ snprintf(_mvBuf,sizeof(_mvBuf),"%u/%u",brightIdx+1,N_BRIGHT); return _mvBuf; }
static const char* mvSound(){ return settings().sound ? "on":"off"; }
static const char* mvDemo(){  return dataDemo() ? "on":"off"; }
static const char* mvAbout(){ return ">"; }
static void mActBright(){ brightIdx=(brightIdx+1)%N_BRIGHT; applyBrightness(); brightSave(); }
static void mActSound(){ settings().sound=!settings().sound; settingsSave(); }
static void mActDemo(){ dataSetDemo(!dataDemo()); }
static void mActAbout(){ aboutOpen=true; menuOpen=false; }
static const MenuItem MENU_ITEMS[] = {
  {"brightness", mvBright, mActBright},
  {"sound",      mvSound,  mActSound},
  {"demo mode",  mvDemo,   mActDemo},
  {"about",      mvAbout,  mActAbout},
};
static const uint8_t N_MENU = sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0]);

static void drawMenu() {
  spr.fillRect(0, BODY_Y, W, BODY_H, C_BLACK);
  scanlines(BODY_Y, BODY_H);
  for (uint8_t i = 0; i < N_MENU; i++) {
    int y = MENU_TOP + i * MENU_ITEM_H, rh = MENU_ITEM_H - 6;
    spr.drawRect(8, y, W - 16, rh, C_GDIM);
    brackets(8, y, W - 16, rh, C_GREEN, 8);
    spr.setTextSize(2);
    spr.setTextColor(C_GREEN, C_BLACK);
    spr.setTextDatum(middle_left);
    spr.drawString(MENU_ITEMS[i].label, 22, y + rh/2);
    spr.setTextSize(1);
    spr.setTextColor(C_CYAN, C_BLACK);
    spr.setTextDatum(middle_right);
    spr.drawString(MENU_ITEMS[i].value(), W - 22, y + rh/2);
  }
  spr.setTextDatum(top_left);
}

static void drawAbout() {
  spr.fillRect(0, BODY_Y, W, BODY_H, C_BLACK);
  scanlines(BODY_Y, BODY_H);
  spr.setTextSize(2); spr.setTextColor(C_GREEN, C_BLACK);
  spr.setTextDatum(top_left); spr.setCursor(12, BODY_Y + 8);
  spr.print("ABOUT");
  int y = BODY_Y + 36; const int xL = 12, xV = 96;
  spr.setTextSize(1);
  auto row = [&](const char* l, const char* v){
    spr.setTextColor(C_GDIM, C_BLACK); spr.setCursor(xL,y); spr.print(l);
    spr.setTextColor(C_WHITE, C_BLACK); spr.setCursor(xV,y); spr.print(v);
    y += 14;
  };
  row("firmware", FW_VERSION);
  row("bt name", btName);
  uint8_t mac[6]={0}; esp_read_mac(mac, ESP_MAC_BT);
  char m[20]; snprintf(m,sizeof(m),"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  row("bt mac", m);
  char b[32]; uint32_t up = millis()/1000;
  snprintf(b,sizeof(b),"%luh %02lum",(unsigned long)(up/3600),(unsigned long)((up/60)%60));
  row("uptime", b);
  snprintf(b,sizeof(b),"%uKB free",ESP.getFreeHeap()/1024); row("heap", b);
  if (tama.aarSubject[0]) { spr.setTextColor(C_DGRAY,C_BLACK); spr.setCursor(xL, BODY_Y+BODY_H-28);
    char s[40]; snprintf(s,sizeof(s),"%.38s",tama.aarSubject); spr.print(s); }
  spr.setTextColor(C_DGRAY, C_BLACK);
  spr.setCursor(xL, BODY_Y + BODY_H - 12);
  spr.print("warlock-buddy · for the Warlock deck");
}

// ─── splash / passkey ──────────────────────────────────────────────────────
static void drawSplash() {
  spr.clear(C_BLACK);
  scanlines(0, H);
  spr.setTextDatum(middle_center);
  spr.setTextColor(C_GREEN);
  spr.setTextSize(5);
  spr.drawString("WaRL0c", W/2, H/2 - 16);
  spr.setTextSize(1);
  spr.setTextColor(C_GDIM);
  spr.drawString("buddy // deck command surface", W/2, H/2 + 20);
  spr.setTextColor(C_DGRAY);
  spr.drawString(FW_VERSION, W/2, H/2 + 36);
  spr.drawString(btName, W/2, H - 16);
  spr.setTextDatum(top_left);
}

static void drawPasskey(uint32_t pk) {
  spr.clear(C_BLACK);
  scanlines(0, H);
  spr.setTextDatum(top_left); spr.setTextSize(1); spr.setTextColor(C_GDIM);
  spr.setCursor(12,12); spr.print("BLUETOOTH PAIRING");
  spr.setTextDatum(middle_center); spr.setTextSize(6); spr.setTextColor(C_GREEN);
  char b[8]; snprintf(b,sizeof(b),"%06lu",(unsigned long)pk);
  spr.drawString(b, W/2, H/2);
  spr.setTextSize(1); spr.setTextColor(C_DGRAY);
  spr.drawString("enter on host", W/2, H-20);
  spr.setTextDatum(top_left);
}

// ─── input ─────────────────────────────────────────────────────────────────
static void clearPending() { pending = GA_NONE; pendingUntil = 0; }

// returns true if this tap CONFIRMED the action `a` (i.e. it was already pending)
static bool armConfirm(GateAction a) {
  if (pending == a && (int32_t)(pendingUntil - millis()) > 0) { clearPending(); return true; }
  pending = a; pendingUntil = millis() + CONFIRM_MS; beep(1400, 50);
  return false;
}

static void handleTouch() {
  auto t = M5.Touch.getDetail();
  if (!t.wasPressed()) return;

  // overlays: any touch-bar tap closes
  if (menuOpen) {
    if (t.y >= MENU_TOP && t.y < MENU_TOP + (int)N_MENU * MENU_ITEM_H && t.y < H - TOUCH_H) {
      uint8_t idx = (t.y - MENU_TOP) / MENU_ITEM_H;
      if (idx < N_MENU) { MENU_ITEMS[idx].activate(); beep(1800,40); lastDraw=0; }
      return;
    }
    if (t.y >= H - TOUCH_H) { menuOpen=false; lastDraw=0; beep(1500,40); }
    return;
  }
  if (aboutOpen) {
    if (t.y >= H - TOUCH_H) { aboutOpen=false; lastDraw=0; beep(1500,40); }
    return;
  }

  if (t.y < H - TOUCH_H) { clearPending(); return; }  // body taps cancel a pending confirm

  int third = W / 3;
  int zone = t.x / third; if (zone > 2) zone = 2;

  if (zone == 2) { menuOpen = true; clearPending(); lastDraw = 0; beep(1200,40); return; }

  if (tama.engaged) {
    if (zone == 0) {                 // KILLSWITCH
      if (armConfirm(GA_KILL)) { sendCmd("{\"cmd\":\"killswitch\"}"); beep(400,120); }
    } else {                         // END
      if (armConfirm(GA_END)) {
        char c[80]; snprintf(c,sizeof(c),"{\"cmd\":\"end\",\"id\":\"%s\"}", tama.engId);
        sendCmd(c); beep(900,90);
      }
    }
  } else if (tama.online && tama.nDrafts > 0) {
    if (zone == 0) {                 // ARM selected draft
      if (armConfirm(GA_ARM)) {
        char c[80]; snprintf(c,sizeof(c),"{\"cmd\":\"arm\",\"id\":\"%s\"}", tama.draftId[draftSel]);
        sendCmd(c); beep(2400,90);
      }
    } else if (zone == 1 && tama.nDrafts > 1) {   // NEXT draft
      draftSel = (draftSel + 1) % tama.nDrafts; clearPending(); beep(1800,40);
    }
  }
}

// ─── setup / loop ──────────────────────────────────────────────────────────
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(180);
  M5.Speaker.setVolume(32);

  spr.setPsram(true);
  spr.createSprite(W, H);
  spr.fillSprite(C_BLACK);

  strncpy(tama.persona, "OFFLINE", sizeof(tama.persona)-1);
  tama.meshNodes = -1; tama.sdrCount = -1;

  statsLoad();
  settingsLoad();
  petNameLoad();        // also loads owner name
  brightLoad();
  applyBrightness();

  startBt();

  drawSplash();
  spr.pushSprite(0, 0);
  delay(1500);
}

void loop() {
  M5.update();
  uint32_t now = millis();

  dataPoll(&tama);

  // expire a stale confirm
  if (pending != GA_NONE && (int32_t)(pendingUntil - now) <= 0) clearPending();
  // keep draftSel in range as drafts change
  if (draftSel >= tama.nDrafts) draftSel = 0;

  // passkey takeover
  static uint32_t lastPk = 0;
  uint32_t pk = blePasskey();
  if (pk) {
    if (pk != lastPk) { lastPk = pk; drawPasskey(pk); spr.pushSprite(0,0); beep(1800,60); }
    delay(50); return;
  } else if (lastPk) { lastPk = 0; lastDraw = 0; }

  handleTouch();

  // compose the frame ~25fps (cheap; sprite double-buffers). Always redraw so
  // blink/uplink animate; lastDraw only forces an immediate repaint after taps.
  if (now - lastDraw >= 40 || lastDraw == 0) {
    lastDraw = now;
    spr.fillSprite(C_BLACK);
    drawStatusBar();
    if (aboutOpen)      { drawAbout(); }
    else if (menuOpen)  { drawMenu();  }
    else {
      scanlines(BODY_Y, BODY_H);
      drawHero();
      drawTelemetry();
    }
    drawTouchBar();
    spr.pushSprite(0, 0);
  }
  delay(8);
}
