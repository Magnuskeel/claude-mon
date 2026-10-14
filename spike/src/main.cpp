// M0 spike: confirm TLS to api.anthropic.com works on the ESP32-2432S028R and
// record which anthropic-ratelimit-unified-* headers a setup-token gets back.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "root_cas.h"
#include "secrets.h"

static const char* kHost = "api.anthropic.com";
static const char* kModel = "claude-haiku-5-5";
static const char* kClaudeCodeSystem = "You are Claude Code, Anthropic's official CLI for Claude.";
static const uint32_t kPollMs = 5 * 60 * 1000;

static void printHeap(const char* tag) {
  Serial.printf("[heap] %-14s free=%u min=%u maxblock=%u\n", tag, ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
}

static void connectWifi() {
  Serial.printf("[wifi] connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] FAILED, rebooting in 10s");
    delay(10000);
    ESP.restart();
  }
  Serial.printf("[wifi] ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[ntp] syncing");
  time_t now = 0;
  for (int i = 0; i < 40 && now < 1700000000; i++) {
    delay(250);
    time(&now);
    Serial.print('.');
  }
  Serial.printf("\n[ntp] utc epoch=%ld\n", (long)now);
}

// Sends one raw HTTP/1.1 request and prints every header line, so we learn
// the real header names (HTTPClient only returns headers you name up front).
static int request(const char* method, const char* path, const String& body) {
  WiFiClientSecure client;
  client.setCACert(kRootCAs);
  client.setTimeout(15);

  printHeap("before tls");
  uint32_t t0 = millis();
  if (!client.connect(kHost, 443)) {
    char err[128] = {0};
    client.lastError(err, sizeof(err));
    Serial.printf("[tls] connect failed: %s\n", err);
    return -1;
  }
  Serial.printf("[tls] connected in %lu ms\n", millis() - t0);
  printHeap("after tls");

  client.printf("%s %s HTTP/1.1\r\n", method, path);
  client.printf("Host: %s\r\n", kHost);
  client.print("Authorization: Bearer " CLAUDE_TOKEN "\r\n");
  client.print("anthropic-version: 2023-06-01\r\n");
  client.print("anthropic-beta: oauth-2025-04-20\r\n");
  client.print("User-Agent: claude-mon-spike/0.1\r\n");
  client.print("Connection: close\r\n");
  if (body.length()) {
    client.print("Content-Type: application/json\r\n");
    client.printf("Content-Length: %u\r\n", body.length());
  }
  client.print("\r\n");
  if (body.length()) client.print(body);

  String status = client.readStringUntil('\n');
  status.trim();
  Serial.printf("[http] %s %s -> %s (%lu ms)\n", method, path, status.c_str(), millis() - t0);
  int code = status.length() > 12 ? status.substring(9, 12).toInt() : -1;

  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) break;
    String lower = line;
    lower.toLowerCase();
    bool interesting = lower.startsWith("anthropic-") || lower.startsWith("retry-after") ||
                       lower.startsWith("request-id") || lower.startsWith("x-should-retry");
    Serial.printf("  %s %s\n", interesting ? "*" : " ", line.c_str());
  }

  String excerpt;
  uint32_t deadline = millis() + 5000;
  while ((client.connected() || client.available()) && excerpt.length() < 1500 && millis() < deadline) {
    if (client.available()) excerpt += (char)client.read();
    else delay(5);
  }
  Serial.printf("[body] %s\n", excerpt.c_str());
  client.stop();
  printHeap("after close");
  return code;
}

static String messagesBody(bool withSystem) {
  String b = "{\"model\":\"";
  b += kModel;
  b += "\",\"max_tokens\":1,";
  if (withSystem) {
    b += "\"system\":\"";
    b += kClaudeCodeSystem;
    b += "\",";
  }
  b += "\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}";
  return b;
}

static void runChecks() {
  Serial.println("\n===== usage endpoint (expect 403 for setup-token) =====");
  request("GET", "/api/oauth/usage", "");

  Serial.println("\n===== messages, plain =====");
  int code = request("POST", "/v1/messages", messagesBody(false));
  if (code >= 400 && code < 500) {
    Serial.println("\n===== messages, with Claude Code system prompt =====");
    request("POST", "/v1/messages", messagesBody(true));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nclaude-mon M0 spike");
  printHeap("boot");
  connectWifi();
  syncTime();
  runChecks();
}

void loop() {
  static uint32_t last = millis();
  if (WiFi.status() != WL_CONNECTED) connectWifi();
  if (millis() - last >= kPollMs) {
    last = millis();
    runChecks();
  }
  delay(100);
}
