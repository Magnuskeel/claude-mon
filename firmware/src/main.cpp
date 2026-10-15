// claude-mon: a desk display for Claude Code.
// The PC pushes usage (status line script) and session events (hook script) to
// http://<device>/usage and /hook. The device itself only reads Anthropic's
// public status page; it holds no Anthropic credentials.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"
#include "service_status.h"
#include "state.h"
#include "ui.h"

#ifndef TZ_INFO
#define TZ_INFO "EST5EDT,M3.2.0,M11.1.0"  // POSIX TZ string; see secrets.example.h
#endif

// RGB LED, active-low.
static const int kLedR = 4, kLedG = 16, kLedB = 17;
static const uint32_t kMinWorkForToast = 20;  // seconds of work before "finished" is announced

static WebServer server(80);
static Preferences prefs;
static AppState app;
static ServiceStatus serviceShared;
static SemaphoreHandle_t serviceLock;
static Page page = Page::Overview;
static volatile bool dirty = true;
static char ipText[20] = "";

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(1, 255 - r);
  ledcWrite(2, 255 - g);
  ledcWrite(3, 255 - b);
}

// Pulsing amber while Claude waits on you, else the worst usage colour, kept dim.
static void updateLed(uint32_t now) {
  if (countState(app, SessionState::Waiting)) {
    float phase = (sinf(millis() / 400.0f) + 1) / 2;
    setLed(10 + 70 * phase, 4 + 28 * phase, 0);
    return;
  }
  float worst = max(effectivePct(app.usage.session, now), effectivePct(app.usage.week, now));
  if (worst >= 85) setLed(40, 0, 0);
  else if (worst >= 60) setLed(40, 20, 0);
  else if (worst >= 0) setLed(0, 25, 0);
  else setLed(0, 0, 0);
}

static bool authorized() {
  if (server.header("X-Claude-Mon-Key") == MON_KEY) return true;
  server.send(401, "application/json", "{\"error\":\"bad key\"}");
  return false;
}

static bool parseBody(JsonDocument& doc) {
  if (!deserializeJson(doc, server.arg("plain"))) return true;
  server.send(400, "application/json", "{\"error\":\"bad json\"}");
  return false;
}

static void readWindow(JsonVariantConst v, Window& w) {
  if (v["pct"].is<float>()) w.pct = constrain(v["pct"].as<float>(), 0.0f, 100.0f);
  if (v["resets_at"].is<uint32_t>()) w.resetsAt = v["resets_at"].as<uint32_t>();
}

// From the status line script: account usage plus this session's context.
static void handleUsage() {
  if (!authorized()) return;
  JsonDocument doc;
  if (!parseBody(doc)) return;
  uint32_t now = nowUnix();

  bool hasLimits = !doc["five_hour"].isNull() || !doc["seven_day"].isNull();
  readWindow(doc["five_hour"], app.usage.session);
  readWindow(doc["seven_day"], app.usage.week);
  if (hasLimits) {
    app.usage.receivedAt = now;
    recordUsage(app, now);
    prefs.putBytes("usage", &app.usage, sizeof(app.usage));
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
  uint32_t now = nowUnix();
  const char* id = doc["session_id"] | "";
  const char* event = doc["event"] | "";
  Serial.printf("[hook] %s %s\n", event, doc["project"] | "");

  if (!strcmp(event, "SessionEnd")) {
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
          char d[16];
          if (worked >= 3600) snprintf(d, sizeof(d), "%luh %02lum", (unsigned long)(worked / 3600), (unsigned long)(worked % 3600) / 60);
          else snprintf(d, sizeof(d), "%lum %02lus", (unsigned long)(worked / 60), (unsigned long)(worked % 60));
          snprintf(app.toast.text, sizeof(app.toast.text), "Finished: %s  (%s)",
                   x->project[0] ? x->project : "Claude Code", d);
          app.toast.until = now + 20;
        }
        x->state = SessionState::Done;
        x->stateSince = now;
      }
    }
  }
  dirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("claude-mon");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uiMessage("Connecting to Wi-Fi", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 30000) {
      uiMessage("Wi-Fi failed", "Restarting...");
      delay(3000);
      ESP.restart();
    }
    delay(250);
  }
  strlcpy(ipText, WiFi.localIP().toString().c_str(), sizeof(ipText));
  Serial.printf("[wifi] ip=%s\n", ipText);
}

void setup() {
  Serial.begin(115200);
  ledcSetup(1, 5000, 8); ledcAttachPin(kLedR, 1);
  ledcSetup(2, 5000, 8); ledcAttachPin(kLedG, 2);
  ledcSetup(3, 5000, 8); ledcAttachPin(kLedB, 3);
  setLed(0, 0, 0);

  uiBegin();
  prefs.begin("claude-mon");
  if (prefs.getBytesLength("usage") == sizeof(app.usage)) prefs.getBytes("usage", &app.usage, sizeof(app.usage));

  connectWifi();
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  MDNS.begin("claude-mon");
  MDNS.addService("http", "tcp", 80);

  const char* headers[] = {"X-Claude-Mon-Key"};
  server.collectHeaders(headers, 1);
  server.on("/usage", HTTP_POST, handleUsage);
  server.on("/hook", HTTP_POST, handleHook);
  server.on("/", HTTP_GET, [] { server.send(200, "text/plain", "claude-mon ok\n"); });
  server.begin();

  serviceLock = xSemaphoreCreateMutex();
  startServiceStatusTask(&serviceShared, serviceLock);
}

void loop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  uint32_t now = nowUnix();

  if (uiTapped()) {
    if (firstWaitingAlert(app)) {
      for (auto& x : app.sessions)
        if (x.state == SessionState::Waiting) x.alertDismissed = true;
    } else {
      page = page == Page::Overview ? Page::Sessions : Page::Overview;
    }
    dirty = true;
  }

  static uint32_t lastHousekeeping = 0;
  if (millis() - lastHousekeeping > 5000) {
    lastHousekeeping = millis();
    expireSessions(app, now);
    if (xSemaphoreTake(serviceLock, 0) == pdTRUE) {
      app.service = serviceShared;
      xSemaphoreGive(serviceLock);
    }
  }

  // Redraw on change, every second for clocks and timers, faster while the alert pulses.
  static uint32_t lastDraw = 0;
  uint32_t interval = firstWaitingAlert(app) ? 300 : 1000;
  if (dirty || millis() - lastDraw > interval) {
    dirty = false;
    lastDraw = millis();
    uiRender(app, page, ipText, now);
  }
  updateLed(now);
  delay(5);
}
