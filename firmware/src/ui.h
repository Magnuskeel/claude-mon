// Screen rendering. Draws in horizontal bands through a small sprite so the
// whole 320x240 frame updates without flicker on a board without PSRAM.
#pragma once
#include "state.h"

enum class Page : uint8_t { Overview, Sessions };

void uiBegin();
void uiMessage(const char* line1, const char* line2);
void uiRender(const AppState& s, Page page, const char* ip, uint32_t now);
bool uiTapped();  // true once per new touch
