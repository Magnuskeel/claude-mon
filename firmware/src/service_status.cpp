#include "service_status.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "root_cas.h"

static const char* kUrl = "https://status.claude.com/api/v2/summary.json";
static const uint32_t kPollMs = 5 * 60 * 1000;
static const uint32_t kRetryMs = 60 * 1000;

static ServiceStatus* gOut;
static SemaphoreHandle_t gLock;

static bool containsNoCase(const char* hay, const char* needle) {
  return strcasestr(hay, needle) != nullptr;
}

static bool fetch(ServiceStatus& result) {
  WiFiClientSecure client;
  client.setCACert(kRootCAs);
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be streamed
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, kUrl)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[status] HTTP %d\n", code);
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["status"]["indicator"] = true;
  filter["status"]["description"] = true;
  filter["incidents"][0]["name"] = true;
  filter["components"][0]["name"] = true;
  filter["components"][0]["status"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[status] json: %s\n", err.c_str());
    return false;
  }

  strlcpy(result.indicator, doc["status"]["indicator"] | "none", sizeof(result.indicator));
  strlcpy(result.text, doc["status"]["description"] | "Unknown", sizeof(result.text));

  // Prefer what's relevant to Claude Code: an open incident, then a degraded component.
  JsonArrayConst incidents = doc["incidents"];
  if (incidents.size()) {
    strlcpy(result.text, incidents[0]["name"] | result.text, sizeof(result.text));
  } else {
    for (JsonObjectConst c : doc["components"].as<JsonArrayConst>()) {
      const char* name = c["name"] | "";
      const char* st = c["status"] | "operational";
      if (strcmp(st, "operational") != 0 && containsNoCase(name, "claude code")) {
        snprintf(result.text, sizeof(result.text), "%s: %s", name, st);
        for (char* p = result.text; *p; p++)
          if (*p == '_') *p = ' ';
        if (!strcmp(result.indicator, "none")) strcpy(result.indicator, "minor");
        break;
      }
    }
  }
  result.known = true;
  return true;
}

static void task(void*) {
  for (;;) {
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
      ServiceStatus result;
      ok = fetch(result);
      if (ok) {
        time_t t = time(nullptr);
        result.checkedAt = (uint32_t)t;
        xSemaphoreTake(gLock, portMAX_DELAY);
        *gOut = result;
        xSemaphoreGive(gLock);
        Serial.printf("[status] %s: %s\n", result.indicator, result.text);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(ok ? kPollMs : kRetryMs));
  }
}

void startServiceStatusTask(ServiceStatus* out, SemaphoreHandle_t lock) {
  gOut = out;
  gLock = lock;
  xTaskCreatePinnedToCore(task, "status", 12288, nullptr, 1, nullptr, 0);
}
