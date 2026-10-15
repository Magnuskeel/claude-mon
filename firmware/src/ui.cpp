#include "ui.h"

#include <math.h>
#include <time.h>

#include "display.h"
#include "version.h"

namespace {

LGFX tft;
LGFX_Sprite band(&tft);
const int kW = 320, kH = 240, kBandH = 40;
int oy = 0;  // top of the band being drawn
int32_t utcOffset = 0;

// Palette (RGB888).
const uint32_t kBg = 0x0E1116, kCard = 0x181D25, kTrack = 0x2A313C;
const uint32_t kText = 0xECEFF4, kDim = 0x8B95A5, kFaint = 0x5C6573;
const uint32_t kAccent = 0xD97757, kGreen = 0x3FB950, kAmber = 0xE3A33B, kRed = 0xF2545B, kBlue = 0x58A6FF;
const uint32_t kAmberBg = 0x2B2210, kGreenBg = 0x16281C;

const lgfx::IFont* const kSmall = &fonts::Font2;
const lgfx::IFont* const kBody = &fonts::FreeSans9pt7b;
const lgfx::IFont* const kBodyBold = &fonts::FreeSansBold9pt7b;
const lgfx::IFont* const kBig = &fonts::FreeSansBold12pt7b;
const lgfx::IFont* const kHuge = &fonts::FreeSansBold18pt7b;

// Drawing helpers in screen coordinates; they translate into the current band.
bool visible(int y, int h) { return y < oy + kBandH && y + h > oy; }
void rect(int x, int y, int w, int h, uint32_t c) {
  if (visible(y, h)) band.fillRect(x, y - oy, w, h, c);
}
void rrect(int x, int y, int w, int h, int r, uint32_t c) {
  if (visible(y, h)) band.fillRoundRect(x, y - oy, w, h, r, c);
}
void rrectOutline(int x, int y, int w, int h, int r, uint32_t c) {
  if (!visible(y, h)) return;
  band.drawRoundRect(x, y - oy, w, h, r, c);
  band.drawRoundRect(x + 1, y + 1 - oy, w - 2, h - 2, r - 1, c);
}
void circle(int x, int y, int r, uint32_t c) {
  if (visible(y - r, 2 * r + 1)) band.fillCircle(x, y - oy, r, c);
}
void text(const char* s, int x, int y, const lgfx::IFont* f, uint32_t c, uint8_t datum) {
  if (!visible(y - 20, 40)) return;
  band.setFont(f);
  band.setTextColor(c);
  band.setTextDatum(datum);
  band.drawString(s, x, y - oy);
}
int textWidth(const char* s, const lgfx::IFont* f) {
  band.setFont(f);
  return band.textWidth(s);
}

// Shortens `s` in place with a trailing ".." so it fits in maxW pixels.
void fit(char* s, const lgfx::IFont* f, int maxW) {
  if (textWidth(s, f) <= maxW) return;
  char tmp[128];
  for (size_t n = strlen(s); n > 0; n--) {
    snprintf(tmp, sizeof(tmp), "%.*s..", (int)n - 1, s);
    if (textWidth(tmp, f) <= maxW) break;
  }
  strcpy(s, tmp);
}

uint32_t colorFor(float pct) {
  if (pct < 60) return kGreen;
  if (pct < 85) return kAmber;
  return kRed;
}

uint32_t stateColor(SessionState st) {
  switch (st) {
    case SessionState::Working: return kBlue;
    case SessionState::Waiting: return kAmber;
    case SessionState::Done: return kGreen;
    default: return kFaint;
  }
}

void duration(char* out, size_t n, int32_t secs) {
  if (secs < 0) secs = 0;
  int d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
  if (d) snprintf(out, n, "%dd %dh", d, h);
  else if (h) snprintf(out, n, "%dh %02dm", h, m);
  else if (m) snprintf(out, n, "%dm", m);
  else snprintf(out, n, "%ds", (int)secs);
}

void clockTime(char* out, size_t n, uint32_t t, bool withDay) {
  static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  time_t tt = (time_t)t + utcOffset;
  struct tm tm;
  gmtime_r(&tt, &tm);
  int h = tm.tm_hour % 12;
  if (!h) h = 12;
  const char* ampm = tm.tm_hour < 12 ? "AM" : "PM";
  if (withDay) snprintf(out, n, "%s %d:%02d %s", kDays[tm.tm_wday], h, tm.tm_min, ampm);
  else snprintf(out, n, "%d:%02d %s", h, tm.tm_min, ampm);
}

// ---------------------------------------------------------------- pieces

void topBar(const AppState& s, uint32_t now) {
  const ServiceStatus& st = s.service;
  uint32_t dot = kFaint;
  char msg[64] = "Checking Anthropic status";
  if (st.known) {
    if (!strcmp(st.indicator, "none")) dot = kGreen;
    else if (!strcmp(st.indicator, "maintenance")) dot = kBlue;
    else if (!strcmp(st.indicator, "minor")) dot = kAmber;
    else dot = kRed;
    strlcpy(msg, st.text, sizeof(msg));
  }
  circle(11, 12, 4, dot);
  fit(msg, kSmall, 205);
  text(msg, 21, 12, kSmall, st.known && dot != kGreen ? kText : kDim, middle_left);

  if (now && s.utcKnown) {
    char clock[16];
    clockTime(clock, sizeof(clock), now, false);
    text(clock, 312, 12, kBodyBold, kText, middle_right);
  }
  rect(8, 24, kW - 16, 1, kCard);
}

void ring(int x0, const char* label, float pct, const char* detail) {
  const int cx = x0 + 53, cy = 74, r1 = 42, r0 = 33;
  if (visible(cy - r1, 2 * r1 + 1)) {
    band.fillArc(cx, cy - oy, r0, r1, 135, 405, kTrack);
    if (pct > 0) {
      float end = 135 + 270 * min(pct, 100.0f) / 100;
      uint32_t c = colorFor(pct);
      band.fillArc(cx, cy - oy, r0, r1, 135, end, c);
      // Rounded caps at both ends of the value arc.
      float rm = (r0 + r1) / 2.0f, rc = (r1 - r0) / 2.0f;
      for (float a : {135.0f, end}) {
        float rad = a * DEG_TO_RAD;
        band.fillCircle(cx + rm * cosf(rad), cy - oy + rm * sinf(rad), rc, c);
      }
    }
  }
  char buf[12];
  if (pct < 0) strcpy(buf, "--");
  else snprintf(buf, sizeof(buf), "%d%%", (int)(pct + 0.5f));
  text(buf, cx, cy, kBig, kText, middle_center);
  text(label, cx, cy + 50, kBody, kText, middle_center);
  text(detail, cx, cy + 68, kSmall, kDim, middle_center);
}

void resetDetail(char* out, size_t n, const Window& w, uint32_t now) {
  if (!w.resetsAt || !now) snprintf(out, n, " ");
  else if (now >= w.resetsAt) snprintf(out, n, "reset");
  else {
    char d[16];
    duration(d, sizeof(d), w.resetsAt - now);
    snprintf(out, n, "resets %s", d);
  }
}

void forecastRow(int y, const char* label, const Forecast& f, uint32_t now, bool week) {
  char msg[48], t[24], d[16];
  uint32_t c = kFaint;
  switch (f.kind) {
    case Forecast::Unknown:
      strcpy(msg, "Collecting data");
      break;
    case Forecast::Reached:
      clockTime(t, sizeof(t), f.resetsAt, week);
      snprintf(msg, sizeof(msg), "Limit hit, back %s", t);
      c = kRed;
      break;
    case Forecast::HitsLimit:
      clockTime(t, sizeof(t), f.limitAt, week);
      duration(d, sizeof(d), f.resetsAt - f.limitAt);
      snprintf(msg, sizeof(msg), "Limit ~%s, %s early", t, d);
      c = f.limitAt - now < 3600 ? kRed : kAmber;
      break;
    case Forecast::OnPace:
      clockTime(t, sizeof(t), f.resetsAt, week);
      snprintf(msg, sizeof(msg), "~%d%% by %s", (int)(f.pctAtReset + 0.5f), t);
      c = colorFor(f.pctAtReset);
      break;
  }
  text(label, 18, y, kSmall, kDim, middle_left);
  fit(msg, kBody, 200);
  text(msg, 304, y, kBody, c, middle_right);
}

void overview(const AppState& s, const char* ip, uint32_t now) {
  topBar(s, now);

  const Session* active = s.activeSession >= 0 && s.sessions[s.activeSession].used ? &s.sessions[s.activeSession] : nullptr;
  char detail[24] = " ";
  if (active && active->ctxSize)
    snprintf(detail, sizeof(detail), "%luk of %luk", (unsigned long)(active->ctxUsed / 1000),
             (unsigned long)(active->ctxSize / 1000));
  ring(0, "Context", active ? active->ctxPct : -1, detail);
  resetDetail(detail, sizeof(detail), s.usage.session, now);
  ring(107, "Session", effectivePct(s.usage.session, now), detail);
  resetDetail(detail, sizeof(detail), s.usage.week, now);
  ring(214, "Week", effectivePct(s.usage.week, now), detail);

  rrect(6, 158, 308, 48, 10, kCard);
  forecastRow(171, "Session pace", forecastSession(s, now), now, false);
  forecastRow(193, "Week pace", forecastWeek(s, now), now, true);

  // Bottom strip: "finished" toast, else a summary of open sessions.
  if (s.toast.until && now < s.toast.until) {
    rrect(6, 210, 308, 26, 8, kGreenBg);
    circle(20, 223, 5, kGreen);
    char msg[64];
    strlcpy(msg, s.toast.text, sizeof(msg));
    fit(msg, kSmall, 270);
    text(msg, 32, 223, kSmall, kText, middle_left);
    return;
  }
  if (!s.usage.receivedAt) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Waiting for Claude Code  -  %s", ip);
    text(msg, 160, 223, kSmall, kFaint, middle_center);
    return;
  }
  int x = 14;
  const struct { SessionState st; const char* word; } kinds[] = {
      {SessionState::Waiting, "waiting"}, {SessionState::Working, "working"}, {SessionState::Done, "done"}};
  bool any = false;
  for (auto& k : kinds) {
    int n = countState(s, k.st);
    if (!n) continue;
    any = true;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d %s", n, k.word);
    circle(x + 4, 223, 4, stateColor(k.st));
    text(buf, x + 13, 223, kSmall, kText, middle_left);
    x += 13 + textWidth(buf, kSmall) + 14;
  }
  if (!any) text("No active sessions", 14, 223, kSmall, kFaint, middle_left);
  char upd[32], d[16];
  duration(d, sizeof(d), now > s.usage.receivedAt ? now - s.usage.receivedAt : 0);
  snprintf(upd, sizeof(upd), "updated %s ago", d);
  text(upd, 308, 223, kSmall, kFaint, middle_right);
}

void sessionsPage(const AppState& s, const char* ip, uint32_t now) {
  topBar(s, now);
  int y = 30, shown = 0;
  for (auto& x : s.sessions) {
    if (!x.used) continue;
    shown++;
    uint32_t sc = stateColor(x.state);
    rrect(6, y, 308, 46, 8, kCard);
    rrect(6, y, 5, 46, 2, sc);

    char name[32];
    strlcpy(name, x.project[0] ? x.project : "Claude Code", sizeof(name));
    fit(name, kBodyBold, 180);
    text(name, 20, y + 14, kBodyBold, kText, middle_left);
    char model[24];
    strlcpy(model, x.model, sizeof(model));
    fit(model, kSmall, 100);
    text(model, 304, y + 14, kSmall, kDim, middle_right);

    char st[40], d[16];
    duration(d, sizeof(d), now > x.stateSince ? now - x.stateSince : 0);
    switch (x.state) {
      case SessionState::Working: snprintf(st, sizeof(st), "Working  %s", d); break;
      case SessionState::Waiting: snprintf(st, sizeof(st), "Needs you  %s", d); break;
      case SessionState::Done: snprintf(st, sizeof(st), "Finished %s ago", d); break;
      default: strcpy(st, "Idle"); break;
    }
    text(st, 20, y + 33, kSmall, sc, middle_left);

    if (x.ctxPct >= 0) {
      const int bx = 214, bw = 90;
      rrect(bx, y + 30, bw, 6, 3, kTrack);
      int fw = (int)(bw * min(x.ctxPct, 100.0f) / 100);
      if (fw > 0) rrect(bx, y + 30, max(fw, 6), 6, 3, colorFor(x.ctxPct));
      char pct[8];
      snprintf(pct, sizeof(pct), "%d%%", (int)(x.ctxPct + 0.5f));
      text(pct, bx - 6, y + 33, kSmall, kDim, middle_right);
    }
    y += 50;
  }
  if (!shown) {
    text("No active sessions", 160, 110, kBody, kDim, middle_center);
    text("Sessions appear once Claude Code hooks report in", 160, 134, kSmall, kFaint, middle_center);
  }
  if (shown < 4) {
    char foot[48];
    snprintf(foot, sizeof(foot), "%s  -  v%s", ip, FW_VERSION);
    text(foot, 160, 232, kSmall, kFaint, middle_center);
  }
}

// Splits `msg` into up to `maxLines` lines that fit `maxW`.
int wrap(const char* msg, const lgfx::IFont* f, int maxW, char lines[][100], int maxLines) {
  int count = 0;
  char line[100] = "";
  const char* p = msg;
  while (*p && count < maxLines) {
    while (*p == ' ') p++;
    const char* end = p;
    while (*end && *end != ' ') end++;
    char candidate[100];
    snprintf(candidate, sizeof(candidate), "%s%s%.*s", line, line[0] ? " " : "", (int)(end - p), p);
    if (line[0] && textWidth(candidate, f) > maxW) {
      strlcpy(lines[count++], line, 100);
      line[0] = 0;
      continue;  // retry this word on a fresh line
    }
    strlcpy(line, candidate, sizeof(line));
    p = end;
  }
  if (line[0] && count < maxLines) strlcpy(lines[count++], line, 100);
  if (*p && count) {
    // Ran out of lines: mark the last one as cut off.
    size_t n = strlen(lines[count - 1]);
    if (n < 97) strcat(lines[count - 1], "..");
  }
  for (int i = 0; i < count; i++) fit(lines[i], f, maxW);
  return count;
}

void alertPage(const AppState& s, const Session& x, uint32_t now) {
  bool pulse = (millis() / 600) % 2;
  rrect(8, 8, 304, 224, 14, kAmberBg);
  rrectOutline(8, 8, 304, 224, 14, pulse ? kAmber : 0x6B5020);

  circle(26, 32, 5, kAmber);
  text("CLAUDE NEEDS YOU", 38, 32, kSmall, kAmber, middle_left);

  char name[32];
  strlcpy(name, x.project[0] ? x.project : "Claude Code", sizeof(name));
  fit(name, kHuge, 272);
  text(name, 24, 66, kHuge, kText, middle_left);

  char lines[3][100];
  int n = wrap(x.message[0] ? x.message : "Waiting for your input", kBody, 272, lines, 3);
  for (int i = 0; i < n; i++) text(lines[i], 24, 104 + i * 22, kBody, kText, middle_left);

  char d[16], buf[32];
  duration(d, sizeof(d), now > x.stateSince ? now - x.stateSince : 0);
  snprintf(buf, sizeof(buf), "Waiting %s", d);
  text(buf, 24, 186, kBig, kAmber, middle_left);

  int others = countState(s, SessionState::Waiting) - 1;
  if (others > 0) {
    snprintf(buf, sizeof(buf), "+%d more waiting", others);
    text(buf, 24, 214, kSmall, kDim, middle_left);
  }
  text("Tap to dismiss", 296, 214, kSmall, kDim, middle_right);
}

}  // namespace

uint8_t uiBegin(uint8_t displayMode) {
  uint8_t used = tft.begin(displayMode);
  Serial.printf("[display] %s\n", LGFX::modeName(used));
  tft.setRotation(1);
  tft.setBrightness(180);
  band.setColorDepth(16);
  band.createSprite(kW, kBandH);
  return used;
}

const char* uiDisplayModeName(uint8_t mode) { return LGFX::modeName(mode); }
uint8_t uiDisplayModeCount() { return LGFX::ModeCount; }

void uiDisplayTest(uint8_t mode) {
  static const struct { uint32_t color; const char* name; } kBars[] = {
      {0xFF0000, "RED"}, {0x00FF00, "GREEN"}, {0x0000FF, "BLUE"}, {0xFFFFFF, "WHITE"}};
  for (oy = 0; oy < kH; oy += kBandH) {
    band.fillScreen(0x000000);
    char title[48];
    snprintf(title, sizeof(title), "Screen mode %d: %s", mode, LGFX::modeName(mode));
    text(title, 160, 18, kBodyBold, 0xFFFFFF, middle_center);
    for (int i = 0; i < 4; i++) {
      rect(10 + i * 76, 40, 72, 110, kBars[i].color);
      text(kBars[i].name, 46 + i * 76, 162, kSmall, 0xFFFFFF, middle_center);
    }
    text("Black background, bars match their names,", 160, 190, kSmall, 0xFFFFFF, middle_center);
    text("no stripes? Keep it. Else press BOOT again.", 160, 208, kSmall, 0xFFFFFF, middle_center);
    text("Hold BOOT 5 s to reset Wi-Fi and pairing.", 160, 228, kSmall, 0x808080, middle_center);
    band.pushSprite(0, oy);
  }
}

void uiMessage(const char* line1, const char* line2) {
  for (oy = 0; oy < kH; oy += kBandH) {
    band.fillScreen(kBg);
    text("claude-mon", 160, 96, kBig, kAccent, middle_center);
    text(line1, 160, 132, kBody, kText, middle_center);
    if (line2) text(line2, 160, 156, kSmall, kDim, middle_center);
    band.pushSprite(0, oy);
  }
}

void uiRender(const AppState& s, Page page, const char* ip, uint32_t now) {
  utcOffset = s.utcOffset;
  const Session* alert = firstWaitingAlert(s);
  tft.startWrite();
  for (oy = 0; oy < kH; oy += kBandH) {
    band.fillScreen(kBg);
    if (alert) alertPage(s, *alert, now);
    else if (page == Page::Sessions) sessionsPage(s, ip, now);
    else overview(s, ip, now);
    band.pushSprite(0, oy);
  }
  tft.endWrite();
}

void uiSetup(const char* status) {
  for (oy = 0; oy < kH; oy += kBandH) {
    band.fillScreen(kBg);
    text("claude-mon", 160, 26, kBig, kAccent, middle_center);
    text("Let's get you online", 160, 54, kBody, kText, middle_center);

    rrect(10, 72, 300, 60, 10, kCard);
    circle(26, 92, 9, kAccent);
    text("1", 26, 92, kSmall, kBg, middle_center);
    text("Using the web installer?", 42, 92, kBodyBold, kText, middle_left);
    text("Click \"Connect to Wi-Fi\" on the page.", 42, 114, kSmall, kDim, middle_left);

    rrect(10, 140, 300, 60, 10, kCard);
    circle(26, 160, 9, kAccent);
    text("2", 26, 160, kSmall, kBg, middle_center);
    text("Or use your phone", 42, 160, kBodyBold, kText, middle_left);
    char join[48];
    snprintf(join, sizeof(join), "Join Wi-Fi \"%s\"", "claude-mon-setup");
    text(join, 42, 182, kSmall, kDim, middle_left);

    char msg[64];
    strlcpy(msg, status, sizeof(msg));
    fit(msg, kSmall, 300);
    text(msg, 160, 222, kSmall, kFaint, middle_center);
    band.pushSprite(0, oy);
  }
}

void uiPairing(const char* code, const char* ip, bool firstTime) {
  for (oy = 0; oy < kH; oy += kBandH) {
    band.fillScreen(kBg);
    text(firstTime ? "Connected! Now pair your PC" : "Pair a PC", 160, 26, kBody, kText, middle_center);
    rrect(30, 48, 260, 76, 14, kCard);
    char spaced[8];
    snprintf(spaced, sizeof(spaced), "%.3s %.3s", code, code + 3);
    text(spaced, 160, 86, kHuge, kAccent, middle_center);
    text("Run the claude-mon installer on your PC", 160, 146, kSmall, kText, middle_center);
    text("and enter this code when it asks.", 160, 164, kSmall, kText, middle_center);
    char foot[48];
    snprintf(foot, sizeof(foot), "Board address: %s", ip);
    text(foot, 160, 196, kSmall, kDim, middle_center);
    text(firstTime ? "Hold the screen 3 s later to pair again" : "Tap to close", 160, 222, kSmall, kFaint, middle_center);
    band.pushSprite(0, oy);
  }
}

Touch uiTouch() {
  static bool wasDown = false, longFired = false;
  static uint32_t downAt = 0;
  int32_t x, y;
  bool down = tft.getTouch(&x, &y);
  Touch ev = Touch::None;
  if (down && !wasDown) {
    downAt = millis();
    longFired = false;
  } else if (down && !longFired && millis() - downAt > 2500) {
    longFired = true;
    ev = Touch::LongPress;
  } else if (!down && wasDown && !longFired && millis() - downAt > 30) {
    ev = Touch::Tap;
  }
  wasDown = down;
  return ev;
}
