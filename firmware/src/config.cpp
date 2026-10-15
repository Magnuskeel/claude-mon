#include "config.h"

#include <Preferences.h>
#include <esp_random.h>

#if __has_include("secrets.h")
#include "secrets.h"  // optional developer defaults for Wi-Fi
#endif

static Preferences prefs;

void configLoad(DeviceConfig& c) {
  prefs.begin("cfg", true);
  prefs.getString("ssid", c.ssid, sizeof(c.ssid));
  prefs.getString("pass", c.pass, sizeof(c.pass));
  prefs.getString("key", c.key, sizeof(c.key));
  c.paired = prefs.getBool("paired", false);
  c.utcOffset = prefs.getInt("utcoff", 0);
  c.utcKnown = prefs.getBool("utcknown", false);
  prefs.end();

#ifdef WIFI_SSID
  if (!c.ssid[0]) {
    strlcpy(c.ssid, WIFI_SSID, sizeof(c.ssid));
    strlcpy(c.pass, WIFI_PASS, sizeof(c.pass));
  }
#endif

  if (!c.key[0]) {
    for (int i = 0; i < 16; i++) snprintf(c.key + i * 2, 3, "%02x", (unsigned)(esp_random() & 0xFF));
    configSave(c);
  }
}

void configSave(const DeviceConfig& c) {
  prefs.begin("cfg", false);
  prefs.putString("ssid", c.ssid);
  prefs.putString("pass", c.pass);
  prefs.putString("key", c.key);
  prefs.putBool("paired", c.paired);
  prefs.putInt("utcoff", c.utcOffset);
  prefs.putBool("utcknown", c.utcKnown);
  prefs.end();
}

void configReset() {
  prefs.begin("cfg", false);
  prefs.clear();
  prefs.end();
}
