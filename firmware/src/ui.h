// Screen rendering. Draws in horizontal bands through a small sprite so the
// whole 320x240 frame updates without flicker on a board without PSRAM.
#pragma once
#include "state.h"

enum class Page : uint8_t { Overview, Sessions };
enum class Touch : uint8_t { None, Tap, LongPress };

void uiBegin();
void uiMessage(const char* line1, const char* line2);
void uiRender(const AppState& s, Page page, const char* ip, uint32_t now);
// First-run screen while waiting for Wi-Fi credentials.
void uiSetup(const char* status);
// Shows the code the PC installer asks for.
void uiPairing(const char* code, const char* ip, bool firstTime);
Touch uiTouch();  // reports each tap or 2.5 s hold once
