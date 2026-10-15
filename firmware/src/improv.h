// Improv Wi-Fi over USB serial (https://www.improv-wifi.com/serial/), so the
// web installer can hand the board Wi-Fi credentials right after flashing.
#pragma once
#include <Arduino.h>
#include <functional>

// Tries to join the network; returns true and fills `url` on success.
using ImprovConnectFn = std::function<bool(const char* ssid, const char* pass, String& url)>;

void improvBegin(ImprovConnectFn connect);
void improvSetProvisioned(const String& url);  // already on Wi-Fi
void improvLoop();
