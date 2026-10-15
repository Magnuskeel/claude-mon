// Fallback Wi-Fi setup: the board opens its own network with a captive page.
#pragma once
#include <Arduino.h>
#include <functional>

static const char* kSetupApName = "claude-mon-setup";

using PortalSaveFn = std::function<void(const String& ssid, const String& pass)>;

void portalStart(PortalSaveFn onSave);
void portalLoop();
void portalStop();
