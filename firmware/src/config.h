// Settings kept in flash (NVS): Wi-Fi, the pairing key and the PC's UTC offset.
#pragma once
#include <Arduino.h>

struct DeviceConfig {
  char ssid[33] = "";
  char pass[65] = "";
  char key[33] = "";      // shared with the PC at pairing; authorizes /usage and /hook
  bool paired = false;
  int32_t utcOffset = 0;  // seconds, reported by the PC
  bool utcKnown = false;
  uint8_t displayMode = 0;  // 0 = auto-detect the screen controller
  bool flipped = false;      // screen turned 180 degrees (e.g. in an enclosure)
};

void configLoad(DeviceConfig& c);
void configSave(const DeviceConfig& c);
void configReset();  // forget Wi-Fi and pairing
