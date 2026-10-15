// claude-mon: shows Claude usage pushed from Claude Code's status line over the LAN.
// The PC posts JSON to http://<device>/usage; the device never talks to Anthropic.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

#include "display.h"
#include "secrets.h"

// RGB LED, active-low.
static const int kLedR = 4, kLedG = 16, kLedB = 17;
static const uint32_t kBg = 0x101418, kTrack = 0x2A3038, kText = 0xE8E8E8, kDim = 0x808890;

struct Window {
  float pct = -1;       // 0-100, -1 = unknown
  uint32_t resetsAt = 0;  // unix seconds
};
struct Context {
  float pct = -1;     // 0-100 of the current session's context window, -1 = unknown
  uint32_t used = 0;  // tokens
  uint32_t size = 0;  // tokens
};
struct Snapshot {
  Window session, week;
  Context context;
  char model[24] = "";
  char project[32] = "";
  uint32_t receivedAt = 0;  // unix seconds
};

static LGFX tft;
static LGFX_Sprite gauge(&tft);
static WebServer server(80);
static Preferences prefs;
static Snapshot snap;
static volatile bool dirty = true;

static uint32_t nowUnix() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

// A window whose reset time has passed is back to 0% until the next update.
static float effectivePct(const Window& w, uint32_t now) {
  if (w.pct < 0) return -1;
  if (now && w.resetsAt && now >= w.resetsAt) return 0;
  return w.pct;
}

static uint32_t colorFor(float pct) {
  if (pct < 60) return 0x3FB950;
  if (pct < 85) return 0xD29922;
  return 0xF85149;
}

static void formatDuration(char* out, size_t n, int32_t secs) {
  if (secs < 0) secs = 0;
  int d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
  if (d) snprintf(out, n, "%dd %dh", d, h);
  else if (h) snprintf(out, n, "%dh %02dm", h, m);
  else snprintf(out, n, "%dm", m);
}

static void setLed(float worst) {
  // PWM duty is inverted because the LED is active-low; keep it dim.
  uint8_t r = 0, g = 0;
  if (worst >= 85) r = 40;
  else if (worst >= 60) r = 40, g = 20;
  else if (worst >= 0) g = 25;
  ledcWrite(1, 255 - r);
  ledcWrite(2, 255 - g);
  ledcWrite(3, 255);
}

// One ring in a 106-px column: percentage in the middle, label and detail below.
static void drawGauge(int x, const char* label, float pct, const char* detail) {
  const int cx = 53, cy = 64, r1 = 48, r0 = 38;
  gauge.fillSprite(kBg);
  gauge.fillArc(cx, cy, r0, r1, 135, 405, kTrack);
  if (pct > 0) gauge.fillArc(cx, cy, r0, r1, 135, 135 + 270 * min(pct, 100.0f) / 100, colorFor(pct));

  gauge.setTextDatum(middle_center);
  gauge.setTextColor(kText);
  gauge.setFont(&fonts::FreeSansBold12pt7b);
  char buf[16];
  if (pct < 0) strcpy(buf, "--");
  else snprintf(buf, sizeof(buf), "%d%%", (int)(pct + 0.5f));
  gauge.drawString(buf, cx, cy);

  gauge.setFont(&fonts::FreeSans9pt7b);
  gauge.drawString(label, cx, cy + 62);
  gauge.setFont(&fonts::Font2);
  gauge.setTextColor(kDim);
  gauge.drawString(detail, cx, cy + 84);
  gauge.pushSprite(x, 26);
}

static void resetDetail(char* out, size_t n, const Window& w, uint32_t now) {
  if (!w.resetsAt || !now) snprintf(out, n, " ");
  else if (now >= w.resetsAt) snprintf(out, n, "reset");
  else {
    char dur[16];
    formatDuration(dur, sizeof(dur), w.resetsAt - now);
    snprintf(out, n, "resets %s", dur);
  }
}

static void drawScreen() {
  uint32_t now = nowUnix();
  tft.startWrite();
  tft.fillRect(0, 0, 320, 26, kBg);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(kText, kBg);
  tft.setTextDatum(top_left);
  tft.drawString(snap.project[0] ? snap.project : "Claude usage", 6, 4);
  tft.setTextDatum(top_right);
  tft.setTextColor(kDim, kBg);
  tft.drawString(snap.model, 314, 4);

  char detail[24];
  const Context& c = snap.context;
  if (c.size) snprintf(detail, sizeof(detail), "%luk / %luk", (unsigned long)(c.used / 1000), (unsigned long)(c.size / 1000));
  else snprintf(detail, sizeof(detail), " ");
  drawGauge(0, "Context", c.pct, detail);
  resetDetail(detail, sizeof(detail), snap.session, now);
  drawGauge(107, "Session", effectivePct(snap.session, now), detail);
  resetDetail(detail, sizeof(detail), snap.week, now);
  drawGauge(214, "Week", effectivePct(snap.week, now), detail);

  char buf[64];
  if (!snap.receivedAt) snprintf(buf, sizeof(buf), "waiting for Claude Code - %s", WiFi.localIP().toString().c_str());
  else if (now) {
    char dur[16];
    formatDuration(dur, sizeof(dur), now - snap.receivedAt);
    snprintf(buf, sizeof(buf), "updated %s ago - %s", dur, WiFi.localIP().toString().c_str());
  } else snprintf(buf, sizeof(buf), "%s", WiFi.localIP().toString().c_str());
  tft.fillRect(0, 222, 320, 18, kBg);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(kDim, kBg);
  tft.setTextDatum(bottom_center);
  tft.drawString(buf, 160, 238);
  tft.endWrite();

  float worst = max(effectivePct(snap.session, now), effectivePct(snap.week, now));
  setLed(worst);
}

static void readWindow(JsonVariantConst v, Window& w) {
  if (v["pct"].is<float>()) w.pct = constrain(v["pct"].as<float>(), 0.0f, 100.0f);
  if (v["resets_at"].is<uint32_t>()) w.resetsAt = v["resets_at"].as<uint32_t>();
}

static void handleUsage() {
  if (server.header("X-Claude-Mon-Key") != MON_KEY) {
    server.send(401, "application/json", "{\"error\":\"bad key\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  readWindow(doc["five_hour"], snap.session);
  readWindow(doc["seven_day"], snap.week);
  JsonVariantConst ctx = doc["context"];
  if (ctx["pct"].is<float>()) snap.context.pct = constrain(ctx["pct"].as<float>(), 0.0f, 100.0f);
  snap.context.used = ctx["used"] | 0u;
  snap.context.size = ctx["size"] | 0u;
  strlcpy(snap.model, doc["model"] | "", sizeof(snap.model));
  strlcpy(snap.project, doc["project"] | "", sizeof(snap.project));
  snap.receivedAt = nowUnix();
  prefs.putBytes("snap", &snap, sizeof(snap));
  dirty = true;
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.printf("[usage] ctx=%.1f%% 5h=%.1f%% 7d=%.1f%%\n", snap.context.pct, snap.session.pct, snap.week.pct);
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("claude-mon");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  tft.fillScreen(kBg);
  tft.setTextColor(kText, kBg);
  tft.setTextDatum(middle_center);
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.drawString("Connecting to " WIFI_SSID, 160, 120);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 30000) {
      tft.drawString("Wi-Fi failed, retrying...", 160, 145);
      delay(5000);
      ESP.restart();
    }
    delay(250);
  }
  Serial.printf("[wifi] ip=%s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  ledcSetup(1, 5000, 8); ledcAttachPin(kLedR, 1);
  ledcSetup(2, 5000, 8); ledcAttachPin(kLedG, 2);
  ledcSetup(3, 5000, 8); ledcAttachPin(kLedB, 3);
  setLed(-1);

  tft.init();
  tft.setRotation(1);
  tft.setBrightness(180);
  gauge.setColorDepth(16);
  gauge.createSprite(106, 192);

  prefs.begin("claude-mon");
  if (prefs.getBytesLength("snap") == sizeof(snap)) prefs.getBytes("snap", &snap, sizeof(snap));

  connectWifi();
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  MDNS.begin("claude-mon");
  MDNS.addService("http", "tcp", 80);

  const char* headers[] = {"X-Claude-Mon-Key"};
  server.collectHeaders(headers, 1);
  server.on("/usage", HTTP_POST, handleUsage);
  server.on("/", HTTP_GET, [] { server.send(200, "text/plain", "claude-mon ok\n"); });
  server.begin();
  tft.fillScreen(kBg);
}

void loop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED) connectWifi();

  static uint32_t lastDraw = 0;
  if (dirty || millis() - lastDraw > 30000) {
    dirty = false;
    lastDraw = millis();
    drawScreen();
  }
  delay(5);
}
