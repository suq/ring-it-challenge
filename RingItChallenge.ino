//
// UNO R4 WiFi — Ring It! Challenge
// v1.5.1
//
// Changes vs v1.4.5:
// - Removed FREEBIE flow entirely; 2-player behaves like 3+ (Option A).
// - Kept all performance guards (one-count-per-press, release-gated re-count,
//   first-press grace, debounce baselining), timings, 📌 pin, and 🎀 tie overlay.
// - Start Match button remains below name fields; /start still hard-resets to
//   avoid MATCH_END hang.
//
// Input reliability (Golf-aligned):
// - Timings: PRESS=12ms, RELEASE=15ms, BURST_SUSTAIN=200ms, COOLDOWN=350ms, LOCKOUT=1500ms
// - One-count-per-press: countedThisBurst
// - Require a release between counts: seenReleaseSinceCount
// - Re-baseline debouncer + first-press grace (turnFresh) at every startTurn/startRound
//
// Game logic (Option A, unified for 2+ players):
// - FIRST: fail to set within 10 → eliminated.
// - If only one player remains alive in FIRST and they ring, they win immediately.
// - After a bar is set, BEAT_BAR cap = bar value; tie → safe (🎀); fail at cap → eliminated.
// - Round point to sole survivor; ACE still works; match end unchanged.
//

#include <WiFiS3.h>
#include "arduino_secrets.h"

const char* APP_VERSION = "RingChallenge v1.5.1";

// ---------- WiFi (edit these) ----------
char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

// Static IP (optional). Comment out WiFi.config(...) for DHCP.
IPAddress ip(192,168,1,170);
IPAddress dns(192,168,1,1);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);

WiFiServer server(80);

// ---------- Switch & timing ----------
const uint8_t SENSOR_PIN = 2;
const int PRESSED = LOW; // using INPUT_PULLUP

// Performance timings
const unsigned long PRESS_DEBOUNCE_MS        = 12;
const unsigned long RELEASE_DEBOUNCE_MS      = 30;
const unsigned long BURST_SUSTAIN_MS         = 200;
const unsigned long RINGER_LINGER_MS         = 1500;
const unsigned long AFTER_COUNT_COOLDOWN_MS  = 350;
const unsigned long AFTER_RELEASE_LOCKOUT_MS = 1500;

// ---------- Challenge game tuning ----------
const uint8_t MAX_PLAYERS        = 8;   // up to 8
uint8_t      attemptsPerTurn     = 10;
uint8_t      targetScore         = 5;   // "Play to"

// ---------- Debounce / burst state ----------
int lastRawLevel = HIGH;
int stableLevel = HIGH;
int lastStableLevel = HIGH;
unsigned long lastChangeMs = 0;
int pendingTargetLevel = HIGH;

enum BurstPhase { IDLE, BURST, LOCKOUT };
BurstPhase burstPhase = IDLE;
unsigned long burstLastEdgeMs = 0;
unsigned long lockoutUntilMs = 0;
unsigned long pressStartMs = 0;
unsigned long lastReleaseMs = 0;
unsigned long lastCountMs   = 0;
bool          ringerLingerActive  = false;
unsigned long ringerLingerStartMs = 0;

// One-count-per-burst + release-gated re-count
bool countedThisBurst = false;
bool seenReleaseSinceCount = true;

// first-press grace to ensure the first swing of each turn is eligible
bool turnFresh = false;

// ---------- Overlays ----------
/*
  overlayCode:
   0 = none
   1 = ACE (🎯)
   2 = Round win (🎉)
   4 = Fail/elimination (🚫)
   5 = Tie (🎀)
   6 = Match win (🏆)
   8 = Numeric ringer (keycap emoji; uses overlayNum)
*/
uint8_t overlayCode = 0;
unsigned long overlayUntilMs = 0;
int8_t overlayPlayer = -1; // who triggered (if relevant)
uint8_t overlayNum = 0;    // used when overlayCode == 8

inline void triggerOverlay(uint8_t code, unsigned long durMs, int8_t who = -1) {
  overlayCode = code;
  overlayPlayer = who;
  overlayNum = 0;
  overlayUntilMs = millis() + durMs;
}
inline void triggerOverlayNum(uint8_t num, unsigned long durMs) {
  overlayCode = 8;
  overlayPlayer = -1;
  overlayNum = num;
  overlayUntilMs = millis() + durMs;
}

// ---------- Game engine (Challenge) ----------
struct ChallengeBar { bool active=false; uint8_t value=0; uint8_t setter=0; };
struct Flags { bool ace=false; };

enum class GState : uint8_t { IDLE, COUNTDOWN, TURN_ACTIVE, ROUND_END, MATCH_END };
GState gstate = GState::IDLE;

enum class PhaseKind : uint8_t { FIRST, BEAT_BAR };
PhaseKind phaseKind = PhaseKind::FIRST;   // internal only
uint8_t attemptCap = 10;                  // for UI

uint8_t  numPlayers   = 2;
uint8_t  current      = 0;
uint8_t  starter      = 0;
uint8_t  attemptsUsed = 0;
uint16_t scores[MAX_PLAYERS] = {0};
String   names[MAX_PLAYERS]  = {"Player 1","Player 2","Player 3","Player 4","Player 5","Player 6","Player 7","Player 8"};
ChallengeBar bar;
Flags    flags;
uint8_t  lastPointPlayer = 255;
enum class Reason : uint8_t { NONE, ACE, CHALLENGE, DRAW };
Reason   lastPointReason = Reason::NONE;

// helper
inline uint16_t bitFor(uint8_t i){ return (uint16_t)1 << i; }

// BEAT_BAR lap-style elimination state
uint16_t beatAliveMask = 0;  // bit i = player i still alive in this round

// UI badges (left column): 0 none, 1 number, 2 fail; number stored in uiBadgeNum
uint8_t  uiBadgeKind[MAX_PLAYERS] = {0};
uint8_t  uiBadgeNum [MAX_PLAYERS] = {0};

// Round summary (persisted until next change)
String roundSummary = "";

// ---------- HTTP helpers ----------
void handleClient(WiFiClient &c);
void sendHtml(WiFiClient &c);
void sendJsonState(WiFiClient &c);
void send404(WiFiClient &c);
String getQueryParam(const String& url, const String& key);
String urlDecode(const String& s);

// ---------- Engine helpers ----------
void startMatch(uint8_t players);
void resetMatch();
void startRound(uint8_t startIdx);
void startTurn();
void endTurn();
void givePoint(uint8_t player, Reason r);
void applyRoundEnd();
void onSwingEdge();
void onRingerLinger();
void setRoundSummary(const String& s);

// Non-freebie helpers
uint8_t nextAliveSeat(uint8_t from);
int     soleAliveIdx();
void    eliminatePlayer(uint8_t p);

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(120);
  Serial.println(APP_VERSION);

  pinMode(SENSOR_PIN, INPUT_PULLUP);

  // WiFi
  WiFi.config(ip, dns, gateway, subnet);   // comment out for DHCP
  WiFi.begin(ssid, pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 20000) delay(200);
  server.begin();

  // seed debounce
  int cur = digitalRead(SENSOR_PIN);
  lastRawLevel = stableLevel = lastStableLevel = cur;
  pendingTargetLevel = cur;
  lastChangeMs = millis();
  lastReleaseMs = millis();

  // init guards
  burstPhase = IDLE;
  countedThisBurst = false;
  seenReleaseSinceCount = true;
}

// ================= LOOP =================
void loop() {
  unsigned long now = millis();

  // SENSOR debounced + burst/lockout/linger control
  int raw = digitalRead(SENSOR_PIN);
  if (raw != lastRawLevel) {
    lastRawLevel = raw;
    lastChangeMs = now;
    pendingTargetLevel = raw;
  } else {
    unsigned long need = (pendingTargetLevel == PRESSED) ? PRESS_DEBOUNCE_MS : RELEASE_DEBOUNCE_MS;
    if ((now - lastChangeMs) >= need && stableLevel != pendingTargetLevel) {
      lastStableLevel = stableLevel;
      stableLevel = pendingTargetLevel;

      if (burstPhase == BURST) burstLastEdgeMs = now;

      // PRESS edge
      if (stableLevel == PRESSED && lastStableLevel != PRESSED) {
        pressStartMs = now;
        ringerLingerActive = false;

        if (burstPhase == IDLE) {
          // Golf-tight press gate: one count per burst + must see a release since last count (unless fresh)
          bool allowNew = (turnFresh || (now >= lockoutUntilMs && seenReleaseSinceCount)) && !countedThisBurst;
          if (allowNew && gstate == GState::TURN_ACTIVE) {
            onSwingEdge();                 // counts attempt immediately
            turnFresh = false;             // consume grace
            seenReleaseSinceCount = false; // must see release before next count
            countedThisBurst = true;       // one count per press
            burstPhase = BURST;
            burstLastEdgeMs = now;
          }
        }
      }

      // RELEASE edge
      if (stableLevel != PRESSED && lastStableLevel == PRESSED) {
        lastReleaseMs = now;
        bool burstQuiet = (now - burstLastEdgeMs) >= BURST_SUSTAIN_MS;

        // release seen (enables next count after lockout)
        seenReleaseSinceCount = true;

        // End any linger on release
        ringerLingerActive = false;

        // ===== FIRST: opener hits cap with no ringer -> eliminate, even in 2P =====
        if (gstate == GState::TURN_ACTIVE && phaseKind == PhaseKind::FIRST) {
          if (burstQuiet && attemptsUsed == attemptsPerTurn) {
            uiBadgeKind[current] = 2; uiBadgeNum[current] = 0;
            setRoundSummary(String(names[current]) + " fails to set — eliminated");
            triggerOverlay(4, 1000, current); // 🚫
            eliminatePlayer(current);

            if (beatAliveMask == 0) {
              // Everyone failed; no bar set at all
              setRoundSummary("No bar set — round ends with no points");
              startRound(0); // Player 1 starts next round
            } else {
              // Advance to the next alive seat; even if only one remains, they must still shoot
              current = nextAliveSeat(current);
              startTurn(); // remain in FIRST
            }
          }
        }

        // ===== BEAT_BAR: quiet at bar without ringer -> elimination =====
        if (gstate == GState::TURN_ACTIVE && phaseKind == PhaseKind::BEAT_BAR && bar.active) {
          if (burstQuiet && attemptsUsed == bar.value) {
            setRoundSummary(String(names[current]) + " eliminated at " + String(bar.value));
            triggerOverlay(4, 1200, current); // 🚫
            uiBadgeKind[current] = 2; uiBadgeNum[current] = 0;
            eliminatePlayer(current);

            int sole = soleAliveIdx();
            if (sole != 255) {
              // Only one remains alive -> award them (BEAT_BAR only)
              uint8_t winner = (uint8_t)sole;
              givePoint(winner, Reason::CHALLENGE);
              setRoundSummary(String(names[winner]) + " holds at " + String(bar.value));
              triggerOverlay(2, 1600, winner); // 🎉
              gstate = GState::ROUND_END;
            } else {
              current = nextAliveSeat(current);
              startTurn();
            }
          }
        }
      }
    }
  }

  // BURST / LOCKOUT flow
  if (burstPhase == BURST) {
    bool burstQuiet = (now - burstLastEdgeMs) >= BURST_SUSTAIN_MS;

    if (burstQuiet && stableLevel != PRESSED) {
      // ===== FIRST: quiet at cap without ringer -> eliminate, even in 2P =====
      if (gstate == GState::TURN_ACTIVE && phaseKind == PhaseKind::FIRST) {
        if (attemptsUsed == attemptsPerTurn) {
          uiBadgeKind[current] = 2; uiBadgeNum[current] = 0;
          setRoundSummary(String(names[current]) + " fails to set — eliminated");
          triggerOverlay(4, 1000, current); // 🚫
          eliminatePlayer(current);

          if (beatAliveMask == 0) {
            setRoundSummary("No bar set — round ends with no points");
            startRound(0); // Player 1 next round
          } else {
            current = nextAliveSeat(current);
            startTurn(); // stay in FIRST
          }
        }
      }

      // ===== BEAT_BAR: quiet at bar without ringer -> elimination =====
      if (gstate == GState::TURN_ACTIVE && phaseKind == PhaseKind::BEAT_BAR && bar.active) {
        if (attemptsUsed == bar.value) {
          setRoundSummary(String(names[current]) + " eliminated at " + String(bar.value));
          uiBadgeKind[current] = 2; uiBadgeNum[current]  = 0;
          triggerOverlay(4, 1200, current); // 🚫
          eliminatePlayer(current);

          int sole = soleAliveIdx();
          if (sole != 255) {
            uint8_t winner = (uint8_t)sole;
            givePoint(winner, Reason::CHALLENGE);
            setRoundSummary(String(names[winner]) + " holds at " + String(bar.value));
            triggerOverlay(2, 1600, winner); // 🎉
            gstate = GState::ROUND_END;
          } else {
            current = nextAliveSeat(current);
            startTurn();
          }
        }
      }

      burstPhase = LOCKOUT;
      lockoutUntilMs = now + AFTER_RELEASE_LOCKOUT_MS;
      ringerLingerActive = false;
      countedThisBurst = false;           // ready for the next physical press
    }
    else if (burstQuiet && stableLevel == PRESSED && !ringerLingerActive) {
      // Press sustained after the burst -> start lingering (possible ringer)
      ringerLingerActive = true;
      ringerLingerStartMs = now;
    }
  } else if (burstPhase == LOCKOUT) {
    if (now >= lockoutUntilMs) burstPhase = IDLE;
  }

  // Ringer linger success
  if (ringerLingerActive) {
    if (stableLevel != PRESSED) {
      ringerLingerActive = false;
    } else if ((now - ringerLingerStartMs) >= RINGER_LINGER_MS) {
      ringerLingerActive = false;
      onRingerLinger(); // success
      burstPhase = LOCKOUT;
      lockoutUntilMs = now + AFTER_RELEASE_LOCKOUT_MS;
      countedThisBurst = false;          // next press can count
      seenReleaseSinceCount = false;     // require a clean release after a ringer
    }
  }

  // Advance non-tie round ends
  if (gstate == GState::ROUND_END) {
    applyRoundEnd();
  }

  // HTTP
  WiFiClient client = server.available();
  if (client) {
    client.setTimeout(60);
    handleClient(client);
    client.stop();
  }
}

// ================== ENGINE ==================
void startMatch(uint8_t players) {
  numPlayers = players;
  if (numPlayers < 2) numPlayers = 2;
  if (numPlayers > MAX_PLAYERS) numPlayers = MAX_PLAYERS;

  for (uint8_t i=0;i<MAX_PLAYERS;i++) { scores[i]=0; uiBadgeKind[i]=0; uiBadgeNum[i]=0; }
  lastPointPlayer = 255;
  lastPointReason = Reason::NONE;
  roundSummary = "";
  flags = Flags{};

  // everyone alive in FIRST
  beatAliveMask = 0;
  for (uint8_t i=0;i<numPlayers;i++) beatAliveMask |= bitFor(i);

  startRound(0); // P0 starts
}

void resetMatch() {
  gstate = GState::IDLE;
  for (uint8_t i=0;i<MAX_PLAYERS;i++) { scores[i]=0; uiBadgeKind[i]=0; uiBadgeNum[i]=0; }
  attemptsUsed = 0;
  bar = ChallengeBar{};
  flags = Flags{};
  current = 0; starter = 0;
  phaseKind = PhaseKind::FIRST;
  attemptCap = attemptsPerTurn;
  lastPointReason = Reason::NONE;
  roundSummary = "";
  overlayCode = 0; overlayUntilMs = 0; overlayPlayer = -1; overlayNum = 0;

  beatAliveMask = 0;

  // debounce baseline + guards
  reBaselineDebouncer();
}

static inline void reBaselineDebouncer() {
  int curLevel = digitalRead(SENSOR_PIN);
  lastRawLevel = stableLevel = lastStableLevel = curLevel;
  pendingTargetLevel = curLevel;
  lastChangeMs = millis();
  burstPhase = IDLE;
  ringerLingerActive = false;
  lockoutUntilMs = 0;
  countedThisBurst = false;
  seenReleaseSinceCount = true;
}

void startRound(uint8_t startIdx) {
  starter = startIdx % numPlayers;
  current = starter;
  attemptsUsed = 0;
  bar = ChallengeBar{};
  flags.ace = false;

  // reset alive mask
  beatAliveMask = 0;
  for (uint8_t i=0;i<numPlayers;i++) beatAliveMask |= bitFor(i);

  // clear UI badges for fresh round
  for (uint8_t i=0;i<numPlayers;i++){ uiBadgeKind[i]=0; uiBadgeNum[i]=0; }

  lastPointReason = Reason::NONE;
  phaseKind = PhaseKind::FIRST;
  attemptCap = attemptsPerTurn;

  // re-baseline + arm first-press grace
  turnFresh = true;
  reBaselineDebouncer();

  gstate = GState::TURN_ACTIVE;
}

void startTurn() {
  attemptsUsed = 0;
  if (phaseKind == PhaseKind::FIRST) {
    attemptCap = attemptsPerTurn;        // UI only
  } else { // BEAT_BAR
    attemptCap = (bar.value > 0) ? (bar.value) : 0; // enforce cap to bar
  }

  // re-baseline + arm first-press grace
  turnFresh = true;
  reBaselineDebouncer();

  gstate = GState::TURN_ACTIVE;
}

void endTurn() {
  current = (current + 1) % numPlayers;
  attemptsUsed = 0;
}

void givePoint(uint8_t player, Reason r) {
  scores[player]++;
  lastPointPlayer = player;
  lastPointReason = r;
}

void onSwingEdge() {
  lastCountMs = millis();

  // Count attempt with cap enforced for BEAT_BAR (cap = attemptCap); FIRST uses 10
  if (phaseKind == PhaseKind::BEAT_BAR) {
    uint8_t maxAllowed = attemptCap;
    if (attemptsUsed < maxAllowed) attemptsUsed++;
  } else {
    attemptsUsed++;
  }
}

void onRingerLinger() {
  if (gstate != GState::TURN_ACTIVE) return;
  if (attemptsUsed == 0) attemptsUsed = 1; // safety

  // ACE
  if (current == starter && attemptsUsed == 1 && phaseKind == PhaseKind::FIRST) {
    flags.ace = true;
    givePoint(current, Reason::ACE);
    setRoundSummary(String(names[current]) + " ACE! — " + String(names[current]) + " to start new round");
    triggerOverlay(1, 2000, current); // 🎯
    gstate = GState::ROUND_END;
    return;
  }

  if (phaseKind == PhaseKind::FIRST) {
    // If only this player remains alive and no bar yet, a ringer wins immediately
    int sole = soleAliveIdx();
    if (!bar.active && sole != 255 && (uint8_t)sole == current) {
      givePoint(current, Reason::CHALLENGE);
      setRoundSummary(String(names[current]) + " sets within 10 — wins round");
      triggerOverlayNum(attemptsUsed, 900);
      triggerOverlay(2, 1600, current);       // 🎉
      gstate = GState::ROUND_END;
      return;
    }

    // Normal FIRST behavior: set the initial bar (non-ACE)
    triggerOverlayNum(attemptsUsed, 1200);
    uiBadgeKind[current] = 1; uiBadgeNum[current] = attemptsUsed;

    bar.active = true;
    bar.value  = attemptsUsed;
    bar.setter = current;

    setRoundSummary(String(names[current]) + " sets " + String(bar.value) + " — challengers to beat");
    phaseKind = PhaseKind::BEAT_BAR;

    // Next alive seat (never same player twice)
    current = nextAliveSeat(current);
    startTurn();
    return;
  }

  if (phaseKind == PhaseKind::BEAT_BAR) {
    const uint8_t m = attemptsUsed;
    if (m < bar.value) {
      triggerOverlayNum(m, 1200);
      uiBadgeKind[current] = 1; uiBadgeNum[current] = m;

      bar.value  = m;
      bar.setter = current;
      setRoundSummary(String(names[current]) + " lowers to " + String(m) + " — challengers to beat");

      current = nextAliveSeat(current);
      startTurn();
    } else {
      // TIE: show 🎀 overlay
      uiBadgeKind[current] = 1; uiBadgeNum[current] = m;
      setRoundSummary(String(names[current]) + " ties at " + String(bar.value) + " — safe");
      triggerOverlay(5, 1200, current); // 🎀
      current = nextAliveSeat(current);
      startTurn();
    }
    return;
  }
}

void applyRoundEnd() {
  int winnerIdx = -1;
  for (uint8_t i=0;i<numPlayers;i++) if (scores[i] >= targetScore) { winnerIdx = i; break; }
  if (winnerIdx >= 0) {
    triggerOverlay(6, 2400, winnerIdx); // 🏆
    gstate = GState::MATCH_END;
    return;
  } else {
    if (lastPointReason == Reason::ACE || lastPointReason == Reason::CHALLENGE) {
      startRound(lastPointPlayer); // scorer starts
    } else {
      startRound((starter + 1) % numPlayers);
    }
  }
}

// --------- Non-freebie helpers ---------
uint8_t nextAliveSeat(uint8_t from) {
  for (uint8_t step=1; step<=numPlayers; ++step) {
    uint8_t i = (from + step) % numPlayers;
    if (beatAliveMask & bitFor(i)) return i;
  }
  return 255; // none
}

int soleAliveIdx() {
  if (beatAliveMask == 0) return 255;
  if ((beatAliveMask & (beatAliveMask - 1)) == 0) {
    for (uint8_t i=0;i<numPlayers;i++) {
      if (beatAliveMask & bitFor(i)) return (int)i;
    }
  }
  return 255;
}

void eliminatePlayer(uint8_t p) {
  beatAliveMask &= ~bitFor(p);
}

// ================== HTTP ==================
void handleClient(WiFiClient &c) {
  String reqLine = c.readStringUntil('\n'); reqLine.trim();
  if (!reqLine.length()) return;
  int sp1 = reqLine.indexOf(' ');
  int sp2 = reqLine.indexOf(' ', sp1 + 1);
  String method = (sp1>0)? reqLine.substring(0,sp1) : "";
  String path   = (sp1>0 && sp2>sp1)? reqLine.substring(sp1+1,sp2) : "";

  // Drain headers
  while (true) {
    String h = c.readStringUntil('\n'); if (!h.length()) break;
    h.trim(); if (!h.length()) break;
  }

  // Normalize path
  String clean = path; int q = clean.indexOf('?'); if (q>=0) clean = clean.substring(0,q);

  if (method=="GET" && clean=="/")      { sendHtml(c); return; }
  if (method=="GET" && clean=="/state") { sendJsonState(c); return; }
  if (method=="GET" && clean=="/reset") { resetMatch(); sendJsonState(c); return; }
  if (method=="GET" && clean=="/start") {
    // players count
    uint8_t p = 2;
    String ps = getQueryParam(path, "p");
    if (ps.length()) {
      int v = ps.toInt();
      if (v >= 2 && v <= MAX_PLAYERS) p = (uint8_t)v;
    }
    // names
    for (uint8_t i=0;i<p;i++) {
      String key = "n" + String(i);
      String v = urlDecode(getQueryParam(path, key));
      if (v.length()) names[i] = v;
      else names[i] = "Player " + String(i+1);
    }
    // Hard reset before starting a new match (prevents any MATCH_END hang)
    resetMatch();
    startMatch(p);
    sendJsonState(c); return;
  }
  if (method=="GET" && clean=="/set") {
    String t = getQueryParam(path, "target");
    if (t.length()) {
      int v = t.toInt();
      if (v >= 1 && v <= 50) targetScore = (uint8_t)v;
    }
    sendJsonState(c); return;
  }

  send404(c);
}

void sendHtml(WiFiClient &c) {
  c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"));
  c.print(F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1' />"
            "<title>Ring It! Challenge</title>"
            "<style>"
            "body{background:#0d0f12;color:#f5f7fb;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial}"
            ".wrap{padding:14px;display:flex;flex-direction:column;gap:12px}"
            ".card{background:#151720;border:1px solid #24283b;border-radius:14px;padding:12px}"
            "button{padding:10px 14px;border-radius:10px;border:1px solid #2a2f45;background:#1e3a8a;color:#fff;cursor:pointer}"
            "input,select{background:#0f1118;color:#fff;border:1px solid #2a2f45;border-radius:8px;padding:8px 10px}"
            "input[type=number]{width:100px;text-align:center}"
            ".accentGreen{ color:#22c55e; font-weight:700; }"
            ".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}"
            ".pill{display:inline-block;background:#1b1f2d;border:1px solid #2a2f45;border-radius:999px;padding:4px 10px;margin-right:6px}"
            "table{width:100%;border-collapse:collapse}th,td{padding:10px 12px;border-bottom:1px solid #24283b;text-align:center;font-size:18px}"
            "th{font-size:20px;letter-spacing:.3px}"
            ".big{font-size:22px;font-weight:800}"
            ".muted{opacity:.7}"
            ".beeCol{width:42px;}"
            /* attempts banner */
            ".attemptBox{display:flex;align-items:center;justify-content:center;padding:14px 10px;margin-top:6px;margin-bottom:6px;"
              "background:#111421;border:1px solid #2a2f45;border-radius:12px}"
            ".attemptBig{font-size:28px;font-weight:800;letter-spacing:.2px}"
            /* overlay */
            ".overlay{position:fixed;inset:0;display:none;align-items:center;justify-content:center;z-index:9999;background:rgba(0,0,0,0.65);backdrop-filter:blur(2px)}"
            ".overlay.show{display:flex}"
            ".mega{font-size:38vmin;display: block;margin: 0 auto;line-height:1;text-align:center}"
            "@keyframes wiggle{0%{transform:rotate(0) scale(1)}20%{transform:rotate(-8deg) scale(1.06)}40%{transform:rotate(8deg) scale(1.06)}60%{transform:rotate(-6deg) scale(1.04)}80%{transform:rotate(6deg) scale(1.02)}100%{transform:rotate(0) scale(1)}}"
            ".wiggle{animation:wiggle 1.6s ease-in-out 1; transform-origin:center; display:inline-block}"
            "#overlay > div {display: flex; flex-direction: column; align-items: center;   /* horizontal center of emoji + label */ text-align: center;    /* ensure label lines center */}"
            "#overlayLabel{font-size:10vmin;font-weight:800;text-align:center;margin-top:12px}"
            "</style></head><body><div class='wrap'>"

            // Overlay (emoji/number + label)
            "<div id='overlay' class='overlay'><div><div id='overlayEmoji' class='mega wiggle'>🎯</div><div id='overlayLabel'></div></div></div>"

            // Setup card (hidden after start)
            "<div class='card' id='setupCard'>"
              "<div class='row'><div class='big'>Game Setup</div></div>"
              "<div class='row'>"
                "<label>Players "
                  "<input id='playersInput' type='number' min='2' max='8' value='2' style='width:70px'/>"
                "</label>"
                "<label>Play to <input id='tgtInput' type='number' min='1' max='50' value='5'/></label>"
              "</div>"
              "<div id='nameFields' class='row' style='gap:8px; margin-top:6px'></div>"
              "<div class='row' style='margin-top:8px'><button id='startBtn'>Start Match</button></div>"
            "</div>"

            // Controls after start
            "<div class='card' id='ctrlCard' style='display:none'>"
              "<div class='row'>"
                "<button id='editBtn'>Edit Setup</button>"
                "<button id='resetBtn'>Reset Match</button>"
                "<span class='pill'>Play to: <b id='tgt'>5</b></span>"
              "</div>"
            "</div>"

            // Challenge / Attempts banner
            "<div class='card'>"
              "<div class='row'><div class='big' id='challengeLine'>Number to Beat: —</div></div>"
              "<div class='row'><div id='setterLine' class='muted'>(set by —)</div></div>"
              "<div class='attemptBox'><div class='attemptBig' id='attemptLine'>Attempts: —</div></div>"
              "<div class='row'><div class='big' id='currentLine'>Current Player: —</div></div>"
              "<div class='row'><div id='summaryLine' class='pill'>—</div></div>"
            "</div>"

            // Scoreboard (left icon column, blank header)
            "<div class='card' id='boardCard' style='display:none'>"
              "<table>"
                "<thead><tr><th class='beeCol'></th><th>Player</th><th>Score</th></tr></thead>"
                "<tbody id='rows'></tbody>"
              "</table>"
            "</div>"

            // Winner banner
            "<div class='card' id='winBanner' style='display:none'><div class='big' id='winText'>Winner: —</div>"
              "<div class='row'><button id='newBtn'>Start New Match</button></div></div>"

            "</div>"
            "<script>"
            "let editing = {}; let setupVisible = true; let bootstrapped = false;"

            "function clamp(n,minv,maxv){ n=Math.floor(Number(n)||0); if(n<minv) n=minv; if(n>maxv) n=maxv; return n; }"
            "function byId(id){ return document.getElementById(id); }"

            // auto-select helper (focus/click/tap)
            "function selectOnFocus(el){"
            "  if(!el) return;"
            "  el.addEventListener('focus', function(){ var self=this; setTimeout(function(){ try{ self.select(); self.setSelectionRange(0, self.value.length); }catch(e){} }, 0); });"
            "  el.addEventListener('click', function(e){ if(document.activeElement===this){ e.preventDefault(); var self=this; setTimeout(function(){ try{ self.select(); self.setSelectionRange(0, self.value.length); }catch(e){} }, 0);} });"
            "  el.addEventListener('touchend', function(){ var self=this; setTimeout(function(){ try{ self.select(); self.setSelectionRange(0, self.value.length); }catch(e){} }, 0); }, {passive:true});"
            "  el.addEventListener('mouseup', function(e){ e.preventDefault(); });"
            "}"

            // dynamic name inputs
            "function renderNameFields(count, names){"
            "  const nf=byId('nameFields'); nf.innerHTML='';"
            "  for(let i=0;i<count;i++){"
            "    const lab=document.createElement('label');"
            "    const val=(names&&names[i]?names[i]:'Player '+(i+1));"
            "    lab.innerHTML='Player '+(i+1)+' <input id=\"n'+i+'\" type=\"text\" value=\"'+val+'\" maxlength=\"24\"/>';"
            "    nf.appendChild(lab);"
            "  }"
            "  for(let i=0;i<count;i++){ selectOnFocus(byId('n'+i)); }"
            "}"
            "renderNameFields(2);"
            "byId('playersInput').addEventListener('change', function(){"
            "  const p = clamp(this.value,2,8); this.value=p; renderNameFields(p);"
            "});"

            "function showSetup(show){ setupVisible = !!show; byId('setupCard').style.display = show?'block':'none';"
              "byId('ctrlCard').style.display = show?'none':'block';"
              "byId('boardCard').style.display = show?'none':'block'; }"

            "function showOverlay(sym,label){"
              "var o=byId('overlay'); var e=byId('overlayEmoji'); var L=byId('overlayLabel'); if(!o||!e) return;"
              "e.textContent=sym||'🎯'; if(L) L.textContent = label||'';"
              "e.classList.remove('wiggle'); void e.offsetWidth; e.classList.add('wiggle');"
              "o.classList.add('show');"
              "setTimeout(function(){ o.classList.remove('show'); if(L) L.textContent=''; }, 2200);"
            "}"

            "function keycap(n){"
              "n = Number(n)||0;"
              "if(n===10) return '🔟';"
              "const map=['0️⃣','1️⃣','2️⃣','3️⃣','4️⃣','5️⃣','6️⃣','7️⃣','8️⃣','9️⃣'];"
              "return (n>=0 && n<map.length)? map[n] : String(n);"
            "}"

            "async function pull(){ try{ const r=await fetch('/state'); const s=await r.json(); render(s); }catch(e){} }"

            "function render(s){"
              "const inMatch = (s.state==='TURN_ACTIVE' || s.state==='ROUND_END' || s.state==='MATCH_END');"
              "showSetup(!inMatch ? true : setupVisible);"

              "if (setupVisible && !bootstrapped){"
                "const p = (s.numPlayers||2); byId('playersInput').value = p;"
                "renderNameFields(p, s.names);"
                "byId('tgtInput').value = s.targetScore || 5;"
                "bootstrapped = true;"
              "}"

              "selectOnFocus(byId('playersInput'));"
              "selectOnFocus(byId('tgtInput'));"

              "byId('tgt').textContent = s.targetScore || 5;"

              "let chText='Number to Beat: —'; let setText='';"
              "{"
                "const ch = s.challenge || {active:s.barActive, value:s.barValue, setter:s.barSetter};"
                "if (ch && (ch.active===true || ch.active==='true')) {"
                  "chText = 'Number to Beat: ' + String(ch.value||0);"
                  "const setterIdx = (typeof ch.setter==='number')?ch.setter:(s.barSetter||0);"
                  "const nm = (s.names && s.names[setterIdx]) ? s.names[setterIdx] : ('P'+((setterIdx|0)+1));"
                  "setText = '(set by ' + nm + ')';"
                "}"
              "}"
              "byId('challengeLine').textContent = chText;"
              "byId('setterLine').textContent = setText;"

              "const cur = (typeof s.current==='number') ? s.current : 0;"
              "const curName = (s.names && s.names[cur]) ? s.names[cur] : ('P'+((cur|0)+1));"
              "const cl = byId('currentLine'); cl.innerHTML = 'Current Player: ';"
              "const nameSpan = document.createElement('span'); nameSpan.className='accentGreen'; nameSpan.textContent=curName; cl.appendChild(nameSpan);"
              "const capNow = (s.attemptCap || s.attemptsPerTurn || 10);"
              "byId('attemptLine').textContent = 'Attempts: ' + String(s.attemptsUsed||0) + '/' + String(capNow);"

              "byId('summaryLine').textContent = s.roundSummary || '—';"

              // Left column icons:
              "const rows=byId('rows'); rows.innerHTML='';"
              "const scores = s.scores || []; const N = s.numPlayers||2; const starterIdx = (typeof s.starter==='number')?s.starter:0;"
              "for(let i=0;i<N;i++){"
                "const nm=(s.names && s.names[i])? s.names[i] : ('P'+(i+1));"
                "const arrow = (i===cur)?'📌 ':'';"
                "let icon='';"
                "{"
                "  const kind = (s.badgesKind && typeof s.badgesKind[i]==='number') ? s.badgesKind[i] : 0;"
                "  const num  = (s.badgesNum  && typeof s.badgesNum[i]==='number')  ? s.badgesNum[i]  : 0;"
                "  if (kind===1) { icon = (num===10)?'🔟':(['','1️⃣','2️⃣','3️⃣','4️⃣','5️⃣','6️⃣','7️⃣','8️⃣','9️⃣'][num]||String(num)); }"
                "  else if (kind===2) { icon = '🚫'; }"
                "}"
                "const starter = (i===starterIdx)?' (Starter)':'';"
                "const tr=document.createElement('tr');"
                "tr.innerHTML="
                  "'<td class=\"beeCol\">'+icon+'</td>' +"
                  "'<td>'+arrow+nm+starter+'</td>' +"
                  "'<td><b>'+String(scores[i]||0)+'</b> / '+String(s.targetScore||5)+'</td>';"
                "rows.appendChild(tr);"
              "}"

              "const wb=byId('winBanner'); const wt=byId('winText');"
              "if (s.state==='MATCH_END' && typeof s.winner==='number' && s.winner>=0) {"
                "const nm = (s.winnerName) ? s.winnerName : ((s.names && s.names[s.winner]) ? s.names[s.winner] : 'Winner');"
                "wb.style.display='block'; wt.textContent = 'Winner: ' + nm;"
              "} else { wb.style.display='none'; }"

              "if (s.overlayActive) {"
                "var sym='🎯'; var label='';"
                "switch(Number(s.overlayCode||0)){"
                  "case 1: sym='🎯'; label = 'ACE!!'; break;"
                  "case 2: sym='🎉';"
                  "        var p = (typeof s.overlayPlayer==='number')?s.overlayPlayer:-1;"
                  "        if (p>=0 && s.names && s.names[p]) label = s.names[p] + ' Scores!';"
                  "        break;"
                  "case 4: sym='🚫'; break;"
                  "case 5: sym='🎀'; break;"
                  "case 6: sym='⭐️';"
                  "        var p2 = (typeof s.overlayPlayer==='number')?s.overlayPlayer:-1;"
                  "        if (p2>=0 && s.names && s.names[p2]) label = s.names[p2] + ' Wins!';"
                  "        break;"
                  "case 8: sym= keycap(s.overlayNum||''); break;"
                "}"
                "showOverlay(sym,label);"
              "}"
            "}"

            "async function startMatch(){"
              "const p  = clamp(byId('playersInput').value,2,8);"
              "const v  = clamp(byId('tgtInput').value,1,50);"
              "byId('tgtInput').value = v;"
              "let qs = '/start?p='+p;"
              "for(let i=0;i<p;i++){ const el=byId('n'+i); const nm=(el && el.value?el.value.trim():('Player '+(i+1))); qs += '&n'+i+'='+encodeURIComponent(nm); }"
              "try{ await fetch('/set?target='+v); }catch(e){}"
              "try{ await fetch(qs); }catch(e){}"
              "showSetup(false);"
              "pull();"
            "}"
            "async function reset(){ try{ await fetch('/reset'); }catch(e){} showSetup(true); bootstrapped=false; pull(); }"

            "byId('startBtn').addEventListener('click', function(e){ e.preventDefault(); startMatch(); });"
            "byId('resetBtn').addEventListener('click', function(e){ e.preventDefault(); reset(); });"
            "byId('newBtn').addEventListener('click', function(e){ e.preventDefault(); reset(); });"
            "byId('editBtn').addEventListener('click', function(e){ e.preventDefault(); showSetup(true); });"

            "setInterval(pull,700); pull();"
            "</script></body></html>"));
}

void sendJsonState(WiFiClient &c) {
  int winner = -1;
  if (gstate == GState::MATCH_END) {
    for (uint8_t i=0;i<numPlayers;i++) if (scores[i] >= targetScore) { winner = i; break; }
  }

  unsigned long now = millis();
  bool overlayActive = (overlayCode != 0) && (now < overlayUntilMs);
  if (!overlayActive) { overlayCode = 0; overlayPlayer = -1; overlayNum = 0; }

  c.print(F("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"));
  c.print('{');
  c.print("\"version\":\""); c.print(APP_VERSION); c.print("\",");
  c.print("\"state\":\"");   c.print(
    gstate==GState::IDLE?"IDLE":gstate==GState::COUNTDOWN?"COUNTDOWN":gstate==GState::TURN_ACTIVE?"TURN_ACTIVE":gstate==GState::ROUND_END?"ROUND_END":"MATCH_END"
  ); c.print("\",");

  c.print("\"attemptCap\":"); c.print(attemptCap); c.print(',');
  c.print("\"numPlayers\":"); c.print(numPlayers); c.print(',');
  c.print("\"current\":");    c.print(current); c.print(',');
  c.print("\"starter\":");    c.print(starter); c.print(',');
  c.print("\"attemptsUsed\":"); c.print(attemptsUsed); c.print(',');
  c.print("\"barActive\":");  c.print(bar.active?"true":"false"); c.print(',');
  c.print("\"barValue\":");   c.print(bar.value); c.print(',');
  c.print("\"barSetter\":");  c.print(bar.setter); c.print(',');

  c.print("\"scores\":[");
    for (uint8_t i=0;i<numPlayers;i++){ if(i) c.print(','); c.print(scores[i]); }
  c.print("],");
  c.print("\"names\":[");
    for (uint8_t i=0;i<numPlayers;i++){ if(i) c.print(','); c.print('\"'); c.print(names[i]); c.print('\"'); }
  c.print("],");
  c.print("\"attemptsPerTurn\":"); c.print(attemptsPerTurn); c.print(',');
  c.print("\"targetScore\":");     c.print(targetScore); c.print(',');
  c.print("\"winner\":");          c.print(winner); c.print(',');
  c.print("\"winnerName\":");      if (winner>=0) { c.print('\"'); c.print(names[winner]); c.print('\"'); } else c.print("null"); c.print(',');
  c.print("\"roundSummary\":");    c.print('\"'); c.print(roundSummary); c.print('\"'); c.print(',');

  c.print("\"overlayActive\":"); c.print(overlayActive ? "true":"false"); c.print(',');
  c.print("\"overlayCode\":");   c.print(overlayCode); c.print(',');
  c.print("\"overlayPlayer\":"); c.print(overlayPlayer); c.print(',');
  c.print("\"overlayNum\":");    c.print(overlayNum); c.print(',');

  // Non-freebie badge arrays (left column)
  c.print("\"badgesKind\":[");
    for (uint8_t i=0;i<numPlayers;i++){ if(i) c.print(','); c.print(uiBadgeKind[i]); }
  c.print("],");
  c.print("\"badgesNum\":[");
    for (uint8_t i=0;i<numPlayers;i++){ if(i) c.print(','); c.print(uiBadgeNum[i]); }
  c.print("],");

  c.print("\"challenge\":{"); c.print("\"active\":"); c.print(bar.active?"true":"false"); c.print(",\"value\":"); c.print(bar.value); c.print(",\"setter\":"); c.print(bar.setter); c.print("}");
  c.print("}");
}

void setRoundSummary(const String& s) {
  roundSummary = s;
}

void send404(WiFiClient &c) {
  c.print(F("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
}

// ---- query helpers ----
String getQueryParam(const String& url, const String& key) {
  int q = url.indexOf('?');
  if (q < 0) return "";
  String qs = url.substring(q+1);
  int start = 0;
  while (start >= 0) {
    int amp = qs.indexOf('&', start);
    String pair = (amp >= 0) ? qs.substring(start, amp) : qs.substring(start);
    int eq = pair.indexOf('=');
    if (eq >= 0) {
      String k = pair.substring(0, eq);
      if (k == key) return pair.substring(eq+1);
    }
    if (amp < 0) break;
    start = amp + 1;
  }
  return "";
}

String urlDecode(const String& s) {
  String out; out.reserve(s.length());
  for (int i=0; i<(int)s.length(); i++) {
    char c = s[i];
    if (c == '+') { out += ' '; }
    else if (c == '%' && i+2 < (int)s.length()) {
      char h1 = s[i+1], h2 = s[i+2];
      auto hex = [](char ch)->int{
        if (ch>='0'&&ch<='9') return ch-'0';
        if (ch>='A'&&ch<='F') return 10+(ch-'A');
        if (ch>='a'&&ch<='f') return 10+(ch-'a');
        return -1;
      };
      int v1 = hex(h1), v2 = hex(h2);
      if (v1>=0 && v2>=0) { out += char((v1<<4)|v2); i+=2; }
      else { out += c; }
    } else { out += c; }
  }
  return out;
}
