// claude-mon: a desk display for Claude Code.
// The PC pushes usage (status line script) and session events (hook script) to
// http://<device>/usage and /hook, authorized by a key handed over at pairing.
// The device itself only reads Anthropic's public status page; it holds no
// Anthropic credentials.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>
#include <time.h>

#include "config.h"
#include "improv.h"
#include "service_status.h"
#include "setup_portal.h"
#include "state.h"
#include "ui.h"
#include "version.h"

// RGB LED, active-low.
static const int kLedR = 4, kLedG = 16, kLedB = 17;
static const int kBootButton = 0;             // hold 5 s to forget Wi-Fi and pairing
static const uint32_t kMinWorkForToast = 20;  // seconds of work before "finished" is announced
static const uint32_t kPairWindowMs = 5 * 60 * 1000;

enum class Mode : uint8_t { Setup, Normal };

static DeviceConfig cfg;
static Mode mode = Mode::Setup;
static WebServer server(80);
static Preferences usagePrefs;
static AppState app;
static ServiceStatus serviceShared;
static SemaphoreHandle_t serviceLock;
static Page page = Page::Overview;
static bool dirty = true;
static char ipText[20] = "";
static uint32_t restartAt = 0;

// Setup mode
static char setupStatus[64] = "";
static bool pendingJoin = false;
static String pendingSsid, pendingPass;

// Pairing: always open until the first PC pairs, then only after a long press.
static char pairCode[7] = "";
static uint32_t pairUntil = 0;
static int pairFails = 0;

static bool pairingOpen() { return !cfg.paired || (pairUntil && millis() < pairUntil); }

static void newPairCode() {
  snprintf(pairCode, sizeof(pairCode), "%06lu", (unsigned long)(esp_random() % 1000000));
  pairFails = 0;
  Serial.printf("[claude-mon] pairing code %s\n", pairCode);
}

// ------------------------------------------------------------------ LED

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(1, 255 - r);
  ledcWrite(2, 255 - g);
  ledcWrite(3, 255 - b);
}

// Pulsing amber while Claude waits on you, else the worst usage colour, kept dim.
static void updateLed(uint32_t now) {
  float phase = (sinf(millis() / 400.0f) + 1) / 2;
  if (mode == Mode::Setup || pairingOpen()) {
    setLed(0, 0, 10 + 40 * phase);
    return;
  }
  if (countState(app, SessionState::Waiting)) {
    setLed(10 + 70 * phase, 4 + 28 * phase, 0);
    return;
  }
  float worst = max(effectivePct(app.usage.session, now), effectivePct(app.usage.week, now));
  if (worst >= 85) setLed(40, 0, 0);
  else if (worst >= 60) setLed(40, 20, 0);
  else if (worst >= 0) setLed(0, 25, 0);
  else setLed(0, 0, 0);
}

// ------------------------------------------------------------------ Wi-Fi

static bool joinWifi(const char* ssid, const char* pass, uint32_t timeoutMs) {
  WiFi.begin(ssid, pass);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(100);
  return WiFi.status() == WL_CONNECTED;
}

static void saveWifi(const char* ssid, const char* pass) {
  strlcpy(cfg.ssid, ssid, sizeof(cfg.ssid));
  strlcpy(cfg.pass, pass, sizeof(cfg.pass));
  configSave(cfg);
}

// ------------------------------------------------------------------ HTTP API

static void sendJson(int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void sendError(int code, const char* msg) {
  JsonDocument doc;
  doc["error"] = msg;
  sendJson(code, doc);
}

static bool authorized() {
  if (server.header("X-Claude-Mon-Key") == cfg.key) return true;
  sendError(401, "bad key");
  return false;
}

static bool parseBody(JsonDocument& doc) {
  if (!deserializeJson(doc, server.arg("plain"))) return true;
  sendError(400, "bad json");
  return false;
}

static void showToast(const char* msg, uint32_t secs) {
  strlcpy(app.toast.text, msg, sizeof(app.toast.text));
  app.toast.until = nowUnix() + secs;
}

// The PC reports its UTC offset so the clock matches it, DST included.
static void readUtcOffset(const JsonDocument& doc) {
  if (!doc["utc_offset"].is<int32_t>()) return;
  int32_t off = doc["utc_offset"].as<int32_t>();
  if (cfg.utcKnown && cfg.utcOffset == off) return;
  cfg.utcOffset = app.utcOffset = off;
  cfg.utcKnown = app.utcKnown = true;
  configSave(cfg);
}

static void readWindow(JsonVariantConst v, Window& w) {
  if (v["pct"].is<float>()) w.pct = constrain(v["pct"].as<float>(), 0.0f, 100.0f);
  if (v["resets_at"].is<uint32_t>()) w.resetsAt = v["resets_at"].as<uint32_t>();
}

static void handleInfo() {
  JsonDocument doc;
  doc["name"] = FW_NAME;
  doc["version"] = FW_VERSION;
  doc["paired"] = cfg.paired;
  doc["pairing"] = pairingOpen();
  sendJson(200, doc);
}

static void handlePair() {
  JsonDocument doc;
  if (!parseBody(doc)) return;
  if (!pairingOpen()) return sendError(403, "not in pairing mode");
  if (strcmp(doc["code"] | "", pairCode) != 0) {
    if (++pairFails >= 5) {
      newPairCode();  // too many guesses: show a fresh code
      dirty = true;
    }
    return sendError(401, "wrong code");
  }
  cfg.paired = true;
  configSave(cfg);
  pairUntil = 0;
  char msg[64];
  snprintf(msg, sizeof(msg), "Paired with %s", doc["pc"] | "your PC");
  showToast(msg, 20);
  dirty = true;

  JsonDocument out;
  out["key"] = cfg.key;
  out["version"] = FW_VERSION;
  sendJson(200, out);
}

// From the status line script: account usage plus this session's context.
static void handleUsage() {
  if (!authorized()) return;
  JsonDocument doc;
  if (!parseBody(doc)) return;
  readUtcOffset(doc);
  uint32_t now = nowUnix();

  bool hasLimits = !doc["five_hour"].isNull() || !doc["seven_day"].isNull();
  readWindow(doc["five_hour"], app.usage.session);
  readWindow(doc["seven_day"], app.usage.week);
  if (hasLimits) {
    app.usage.receivedAt = now;
    recordUsage(app, now);
    usagePrefs.putBytes("usage", &app.usage, sizeof(app.usage));
  }

  if (Session* x = findSession(app, doc["session_id"] | "", true)) {
    strlcpy(x->project, doc["project"] | x->project, sizeof(x->project));
    strlcpy(x->model, doc["model"] | x->model, sizeof(x->model));
    JsonVariantConst ctx = doc["context"];
    if (ctx["pct"].is<float>()) x->ctxPct = constrain(ctx["pct"].as<float>(), 0.0f, 100.0f);
    x->ctxUsed = ctx["used"] | x->ctxUsed;
    x->ctxSize = ctx["size"] | x->ctxSize;
    // Activity after a permission prompt means it was answered.
    if (x->state == SessionState::Waiting && now - x->stateSince >= 3) {
      x->state = SessionState::Working;
      x->stateSince = now;
    }
    if (!x->stateSince) x->stateSince = now;
    x->lastSeen = now;
    app.activeSession = x - app.sessions;
  }
  dirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// From the hook script: session lifecycle events.
static void handleHook() {
  if (!authorized()) return;
  JsonDocument doc;
  if (!parseBody(doc)) return;
  readUtcOffset(doc);
  uint32_t now = nowUnix();
  const char* id = doc["session_id"] | "";
  const char* event = doc["event"] | "";
  Serial.printf("[hook] %s %s\n", event, doc["project"] | "");

  if (!strcmp(event, "Test")) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Connected to %s", doc["project"] | "your PC");
    showToast(msg, 30);
  } else if (!strcmp(event, "SessionEnd")) {
    removeSession(app, id);
  } else if (Session* x = findSession(app, id, true)) {
    strlcpy(x->project, doc["project"] | x->project, sizeof(x->project));
    x->lastSeen = now;
    if (!x->stateSince) x->stateSince = now;

    if (!strcmp(event, "UserPromptSubmit")) {
      x->state = SessionState::Working;
      x->stateSince = now;
    } else if (!strcmp(event, "Notification")) {
      // idle_prompt just means "still waiting for your next prompt" after a finish.
      if (strcmp(doc["notification_type"] | "", "idle_prompt") != 0) {
        x->state = SessionState::Waiting;
        x->stateSince = now;
        x->alertDismissed = false;
        strlcpy(x->message, doc["message"] | "", sizeof(x->message));
      }
    } else if (!strcmp(event, "Stop")) {
      if (x->state == SessionState::Working || x->state == SessionState::Waiting) {
        uint32_t worked = now - x->stateSince;
        if (x->state == SessionState::Working && worked >= kMinWorkForToast) {
          char d[16], msg[64];
          if (worked >= 3600)
            snprintf(d, sizeof(d), "%luh %02lum", (unsigned long)(worked / 3600), (unsigned long)(worked % 3600) / 60);
          else snprintf(d, sizeof(d), "%lum %02lus", (unsigned long)(worked / 60), (unsigned long)(worked % 60));
          snprintf(msg, sizeof(msg), "Finished: %s  (%s)", x->project[0] ? x->project : "Claude Code", d);
          showToast(msg, 20);
        }
        x->state = SessionState::Done;
        x->stateSince = now;
      }
    }
  }
  dirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// ------------------------------------------------------------------ modes

static void startNormal() {
  portalStop();
  mode = Mode::Normal;
  strlcpy(ipText, WiFi.localIP().toString().c_str(), sizeof(ipText));
  Serial.printf("[wifi] ip=%s\n", ipText);
  improvSetProvisioned(String("http://") + ipText + "/");

  configTime(0, 0, "pool.ntp.org", "time.google.com");
  MDNS.begin("claude-mon");
  MDNS.addService("http", "tcp", 80);

  const char* headers[] = {"X-Claude-Mon-Key"};
  server.collectHeaders(headers, 1);
  server.on("/info", HTTP_GET, handleInfo);
  server.on("/pair", HTTP_POST, handlePair);
  server.on("/usage", HTTP_POST, handleUsage);
  server.on("/hook", HTTP_POST, handleHook);
  server.on("/", HTTP_GET, [] {
    server.send(200, "text/plain", "claude-mon " FW_VERSION " is running. Install the PC side from https://magnuskeel.github.io/claude-mon/\n");
  });
  server.begin();

  serviceLock = xSemaphoreCreateMutex();
  startServiceStatusTask(&serviceShared, serviceLock);
  if (!cfg.paired) newPairCode();
  dirty = true;
}

static void startSetup(const char* status) {
  mode = Mode::Setup;
  strlcpy(setupStatus, status, sizeof(setupStatus));
  portalStart([](const String& ssid, const String& pass) {
    pendingSsid = ssid;
    pendingPass = pass;
    pendingJoin = true;
  });
  dirty = true;
}

static void loopSetup() {
  portalLoop();

  if (pendingJoin) {
    pendingJoin = false;
    snprintf(setupStatus, sizeof(setupStatus), "Connecting to %s...", pendingSsid.c_str());
    uiSetup(setupStatus);
    if (joinWifi(pendingSsid.c_str(), pendingPass.c_str(), 20000)) {
      saveWifi(pendingSsid.c_str(), pendingPass.c_str());
      uiMessage("Connected", "Restarting...");
      restartAt = millis() + 1500;
    } else {
      WiFi.disconnect();
      snprintf(setupStatus, sizeof(setupStatus), "Couldn't join %s. Check the password.", pendingSsid.c_str());
    }
    dirty = true;
  }

  // Saved network back (e.g. router rebooted)? Retry now and then while nobody is on the setup page.
  static uint32_t lastRetry = millis();
  if (cfg.ssid[0] && WiFi.softAPgetStationNum() == 0 && millis() - lastRetry > 120000) {
    lastRetry = millis();
    if (joinWifi(cfg.ssid, cfg.pass, 10000)) restartAt = millis() + 500;
    else WiFi.disconnect();
  }

  if (dirty) {
    dirty = false;
    uiSetup(setupStatus);
  }
}

static void loopNormal() {
  server.handleClient();
  uint32_t now = nowUnix();

  Touch ev = uiTouch();
  bool showPairing = pairingOpen();
  if (ev == Touch::LongPress) {
    newPairCode();
    pairUntil = millis() + kPairWindowMs;
    dirty = true;
  } else if (ev == Touch::Tap) {
    if (showPairing && cfg.paired) {
      pairUntil = 0;
    } else if (firstWaitingAlert(app)) {
      for (auto& x : app.sessions)
        if (x.state == SessionState::Waiting) x.alertDismissed = true;
    } else {
      page = page == Page::Overview ? Page::Sessions : Page::Overview;
    }
    dirty = true;
  }

  static uint32_t lastHousekeeping = 0, lastAnnounce = 0;
  if (millis() - lastHousekeeping > 5000) {
    lastHousekeeping = millis();
    // Also print the essentials over USB, readable from the web installer's
    // "Logs & Console" even if the screen shows nothing.
    if (!lastAnnounce || millis() - lastAnnounce > 15000) {
      lastAnnounce = millis();
      if (pairingOpen()) Serial.printf("[claude-mon] address %s  pairing code %s\n", ipText, pairCode);
      else Serial.printf("[claude-mon] address %s  paired\n", ipText);
    }
    expireSessions(app, now);
    if (xSemaphoreTake(serviceLock, 0) == pdTRUE) {
      app.service = serviceShared;
      xSemaphoreGive(serviceLock);
    }
    if (WiFi.status() == WL_CONNECTED) strlcpy(ipText, WiFi.localIP().toString().c_str(), sizeof(ipText));
    if (pairUntil && millis() >= pairUntil) {
      pairUntil = 0;
      dirty = true;
    }
  }

  // Redraw on change, every second for clocks and timers, faster while the alert pulses.
  static uint32_t lastDraw = 0;
  static bool wasPairing = false;
  if (pairingOpen()) {
    if (dirty || !wasPairing) uiPairing(pairCode, ipText, !cfg.paired);
    wasPairing = true;
    dirty = false;
  } else {
    uint32_t interval = firstWaitingAlert(app) ? 300 : 1000;
    if (dirty || wasPairing || millis() - lastDraw > interval) {
      lastDraw = millis();
      uiRender(app, page, ipText, now);
    }
    wasPairing = false;
    dirty = false;
  }
  updateLed(now);
}

// BOOT button: a short press flips the screen 180 degrees; holding 3-5 s switches
// to the next screen mode (shown as a test card, kept after a restart); holding
// longer than 5 s forgets Wi-Fi and pairing.
static void checkBootButton() {
  static uint32_t downSince = 0;
  if (digitalRead(kBootButton) == LOW) {
    if (!downSince) downSince = millis();
    if (millis() - downSince > 5000) {
      uiMessage("Resetting", "Forgetting Wi-Fi and pairing");
      configReset();
      usagePrefs.clear();
      delay(1500);
      ESP.restart();
    }
    return;
  }
  uint32_t held = downSince ? millis() - downSince : 0;
  downSince = 0;
  if (held > 50 && held < 1500) {
    cfg.flipped = !cfg.flipped;
    configSave(cfg);
    uiSetFlipped(cfg.flipped);
    dirty = true;
  } else if (held >= 3000) {
    uint8_t next = cfg.displayMode + 1;
    if (next >= uiDisplayModeCount()) next = 1;  // cycle through the explicit modes
    cfg.displayMode = next;
    configSave(cfg);
    Serial.printf("[display] switching to mode %d (%s)\n", next, uiDisplayModeName(next));
    usagePrefs.putBool("disptest", true);
    delay(200);
    ESP.restart();
  }
}

// ------------------------------------------------------------------ main

void setup() {
  Serial.begin(115200);
  ledcSetup(1, 5000, 8); ledcAttachPin(kLedR, 1);
  ledcSetup(2, 5000, 8); ledcAttachPin(kLedG, 2);
  ledcSetup(3, 5000, 8); ledcAttachPin(kLedB, 3);
  setLed(0, 0, 0);
  pinMode(kBootButton, INPUT_PULLUP);

  configLoad(cfg);
  uint8_t usedMode = uiBegin(cfg.displayMode, cfg.flipped);
  app.utcOffset = cfg.utcOffset;
  app.utcKnown = cfg.utcKnown;
  usagePrefs.begin("claude-mon");
  if (usagePrefs.getBool("disptest", false)) {
    usagePrefs.putBool("disptest", false);
    uiDisplayTest(usedMode);
    for (uint32_t t = millis(); millis() - t < 8000;) {
      checkBootButton();  // lets the user keep pressing BOOT to try the next mode
      delay(10);
    }
  }
  if (usagePrefs.isKey("usage") && usagePrefs.getBytesLength("usage") == sizeof(app.usage)) usagePrefs.getBytes("usage", &app.usage, sizeof(app.usage));

  improvBegin([](const char* ssid, const char* pass, String& url) {
    if (!joinWifi(ssid, pass, 20000)) {
      if (mode == Mode::Setup) WiFi.disconnect();
      return false;
    }
    saveWifi(ssid, pass);
    url = "http://" + WiFi.localIP().toString() + "/";
    restartAt = millis() + 1500;  // come back up cleanly in normal mode
    return true;
  });

  WiFi.setHostname("claude-mon");
  WiFi.mode(WIFI_STA);
  if (cfg.ssid[0]) {
    uiMessage("Connecting to Wi-Fi", cfg.ssid);
    if (joinWifi(cfg.ssid, cfg.pass, 20000)) {
      startNormal();
      return;
    }
    WiFi.disconnect();
    char status[64];
    snprintf(status, sizeof(status), "Couldn't join %s", cfg.ssid);
    startSetup(status);
  } else {
    startSetup("Waiting for Wi-Fi details");
  }
}

void loop() {
  improvLoop();
  checkBootButton();
  if (restartAt && millis() > restartAt) ESP.restart();
  if (mode == Mode::Setup) loopSetup();
  else loopNormal();
  delay(5);
}
