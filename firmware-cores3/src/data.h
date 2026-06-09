#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
// xfer.h omitted on CoreS3 for v1 — status/file-push commands deferred.
#include "stats.h"

// Per-account chip data — mirrors the bridge's heartbeat.subctl.accounts[].
struct AccountChip {
  char    alias[20];     // "claude-jason"
  char    color[8];      // "grey" | "green" | "yellow" | "red"
  uint8_t hits;          // rl_hits_today
  uint8_t sessions;      // active_sessions
};

struct TamaState {
  // Legacy fields — populated from top-level heartbeat keys, used by glance UI
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[8][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];

  // Rich subctl.* extension — what makes the CoreS3 dashboard sing.
  char     dispatchVerdict[8];   // "green" | "yellow" | "red"
  uint8_t  dispatchReady;        // count of accounts in "ready"
  uint8_t  dispatchTotal;        // total accounts
  uint8_t  activeConvs;          // # of live conversations
  char     topProject[20];       // freshest conv's project name
  uint16_t topAgeS;              // seconds since freshest conv's last activity
  uint32_t costSavedMonth;       // USD saved vs API pricing this month
  uint16_t rlTodayTotal;         // rate-limit hits today across all accounts
  char     activeProfile[16];    // "chat" | "auto" | etc
  bool     promptInFlight;
  AccountChip accounts[8];       // up to 8 accounts; v3.2.0 has 5
  uint8_t  nAccounts;

  // ─── Warlock extension (heartbeat.warlock.*) — drives the deck HUD ────────
  bool     online;               // deck API reachable
  bool     engaged;              // an engagement is ARMED (live)
  char     persona[12];          // OFFLINE | SAFE | ATTENTION | ARMED
  char     engId[40];            // active engagement id (for END)
  char     engName[24];          // active engagement name
  uint8_t  scopeSsids, scopeBssids, scopeIps;  // active scope cardinality
  bool     aarEnabled;
  uint32_t aarRecords;           // signed attestations ("sigils")
  char     aarSubject[64];       // did:web subject
  bool     haveTemp;
  float    tempC;                // deck CPU temp
  bool     haveCpu, haveMem;
  float    cpuPct, memPct;
  bool     gpsFix;               // gps.ok && mode>=2
  int16_t  meshNodes;            // -1 = unknown/null
  int16_t  sdrCount;             // -1 = none/null
  uint8_t  nDrafts;              // staged engagements awaiting ARM
  char     draftId[4][40];
  char     draftName[4][24];
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "subctl unreachable"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

// Warlock demo scenarios — cycle SAFE → staged → ARMED for screenshots/booth.
struct _Fake { const char* persona; const char* eng; uint8_t drafts; uint32_t sigils; bool armed; };
static const _Fake _FAKES[] = {
  {"SAFE",      "",                0, 3,  false},
  {"ATTENTION", "",                2, 3,  false},
  {"ARMED",     "Acme HQ Wi-Fi",   0, 7,  true},
  {"ARMED",     "10.0.0.0/24 sweep",1, 11, true},
  {"SAFE",      "",                0, 12, false},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].persona;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  // (xferCommand call omitted on CoreS3 — no xfer.h in this firmware yet)

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components. M5Unified exposes setDateTime
  // taking a std::tm pointer (different API from M5StickCPlus library).
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    M5.Rtc.setDateTime(&lt);
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
  const char* m = doc["msg"];
  if (m) { strncpy(out->msg, m, sizeof(out->msg)-1); out->msg[sizeof(out->msg)-1]=0; }
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      strncpy(out->lines[n], s ? s : "", 91); out->lines[n][91]=0;
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
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }

  // Rich subctl.* extension — bridge populates this for the CoreS3 dashboard.
  JsonObject sx = doc["subctl"];
  if (!sx.isNull()) {
    JsonObject disp = sx["dispatch"];
    if (!disp.isNull()) {
      const char* v = disp["verdict"];
      strncpy(out->dispatchVerdict, v ? v : "green", sizeof(out->dispatchVerdict)-1);
      out->dispatchVerdict[sizeof(out->dispatchVerdict)-1] = 0;
      out->dispatchReady = disp["ready"] | 0;
      out->dispatchTotal = disp["total"] | 0;
    }
    JsonObject cv = sx["convs"];
    if (!cv.isNull()) {
      out->activeConvs = cv["count"] | 0;
      const char* tp = cv["top_project"];
      strncpy(out->topProject, tp ? tp : "", sizeof(out->topProject)-1);
      out->topProject[sizeof(out->topProject)-1] = 0;
      out->topAgeS = cv["top_age_s"] | 0;
    }
    out->costSavedMonth = sx["cost_saved_month_usd"] | out->costSavedMonth;
    out->rlTodayTotal   = sx["rl_today_total"] | 0;
    out->promptInFlight = sx["prompt_in_flight"] | false;
    const char* ap = sx["active_profile"];
    if (ap) {
      strncpy(out->activeProfile, ap, sizeof(out->activeProfile)-1);
      out->activeProfile[sizeof(out->activeProfile)-1] = 0;
    }
    JsonArray accts = sx["accounts"];
    if (!accts.isNull()) {
      uint8_t n = 0;
      for (JsonObject a : accts) {
        if (n >= 8) break;
        const char* al = a["alias"];
        const char* co = a["color"];
        strncpy(out->accounts[n].alias, al ? al : "", sizeof(out->accounts[n].alias)-1);
        out->accounts[n].alias[sizeof(out->accounts[n].alias)-1] = 0;
        strncpy(out->accounts[n].color, co ? co : "grey", sizeof(out->accounts[n].color)-1);
        out->accounts[n].color[sizeof(out->accounts[n].color)-1] = 0;
        out->accounts[n].hits     = a["hits"]     | 0;
        out->accounts[n].sessions = a["sessions"] | 0;
        n++;
      }
      out->nAccounts = n;
    }
  }

  // Owner name push: {"cmd":"owner","name":"jason"} — shown as OP: in the HUD.
  if (strcmp(doc["cmd"] | "", "owner") == 0) {
    const char* nm = doc["name"] | "";
    if (nm[0] && strcmp(nm, ownerName()) != 0) ownerSet(nm);  // NVS write only on change
  }

  // Rich warlock.* extension — the deck HUD's data source.
  JsonObject wx = doc["warlock"];
  if (!wx.isNull()) {
    out->online  = wx["online"]  | false;
    out->engaged = wx["engaged"] | false;
    const char* ps = wx["persona"] | "SAFE";
    strncpy(out->persona, ps, sizeof(out->persona)-1); out->persona[sizeof(out->persona)-1]=0;

    JsonObject e = wx["engagement"];
    const char* eid = e["id"]   | "";
    const char* enm = e["name"] | "";
    strncpy(out->engId,   eid, sizeof(out->engId)-1);   out->engId[sizeof(out->engId)-1]=0;
    strncpy(out->engName, enm, sizeof(out->engName)-1); out->engName[sizeof(out->engName)-1]=0;
    JsonObject sc = e["scope"];
    out->scopeSsids  = sc["ssids"]     | 0;
    out->scopeBssids = sc["bssids"]    | 0;
    out->scopeIps    = sc["ip_ranges"] | 0;

    JsonObject aar = wx["aar"];
    out->aarEnabled = aar["enabled"] | false;
    out->aarRecords = aar["records"] | 0UL;
    const char* sub = aar["subject"] | "";
    strncpy(out->aarSubject, sub, sizeof(out->aarSubject)-1); out->aarSubject[sizeof(out->aarSubject)-1]=0;

    JsonObject sys = wx["sys"];
    out->haveTemp = sys["temp_c"].is<float>();
    out->tempC    = sys["temp_c"] | 0.0f;
    out->haveCpu  = sys["cpu_pct"].is<float>();
    out->cpuPct   = sys["cpu_pct"] | 0.0f;
    out->haveMem  = sys["mem_pct"].is<float>();
    out->memPct   = sys["mem_pct"] | 0.0f;
    out->gpsFix   = sys["gps_fix"] | false;
    out->meshNodes = sys["mesh_nodes"].is<int>() ? (int16_t)(sys["mesh_nodes"] | 0) : -1;
    out->sdrCount  = sys["sdr"].is<int>()        ? (int16_t)(sys["sdr"] | 0)        : -1;

    JsonArray dr = wx["drafts"];
    uint8_t n = 0;
    if (!dr.isNull()) {
      for (JsonObject d : dr) {
        if (n >= 4) break;
        const char* di = d["id"]   | "";
        const char* dn = d["name"] | "";
        strncpy(out->draftId[n],   di, sizeof(out->draftId[n])-1);   out->draftId[n][sizeof(out->draftId[n])-1]=0;
        strncpy(out->draftName[n], dn, sizeof(out->draftName[n])-1); out->draftName[n][sizeof(out->draftName[n])-1]=0;
        n++;
      }
    }
    out->nDrafts = n;
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
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->lastUpdated = now;
    out->connected = true;
    out->online = true;
    out->engaged = s.armed;
    strncpy(out->persona, s.persona, sizeof(out->persona)-1); out->persona[sizeof(out->persona)-1]=0;
    strncpy(out->engName, s.eng, sizeof(out->engName)-1); out->engName[sizeof(out->engName)-1]=0;
    strncpy(out->engId, s.armed ? "demo-engagement-id" : "", sizeof(out->engId)-1); out->engId[sizeof(out->engId)-1]=0;
    out->scopeSsids = s.armed ? 1 : 0; out->scopeBssids = 0; out->scopeIps = s.armed ? 2 : 0;
    out->aarEnabled = true; out->aarRecords = s.sigils;
    out->nDrafts = s.drafts;
    for (uint8_t i = 0; i < s.drafts && i < 4; i++) {
      snprintf(out->draftId[i], sizeof(out->draftId[i]), "demo-draft-%u", i);
      snprintf(out->draftName[i], sizeof(out->draftName[i]), i ? "Guest VLAN" : "Floor-3 sweep");
    }
    out->haveTemp = true; out->tempC = 47.5f;
    out->haveCpu = true; out->cpuPct = 23.0f;
    out->haveMem = true; out->memPct = 41.0f;
    out->gpsFix = true; out->meshNodes = 4; out->sdrCount = 3;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.persona);
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
    out->online = false; out->engaged = false; out->nDrafts = 0;
    strncpy(out->persona, "OFFLINE", sizeof(out->persona)-1); out->persona[sizeof(out->persona)-1]=0;
    strncpy(out->msg, "deck offline", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
