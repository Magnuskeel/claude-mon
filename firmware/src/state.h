// Everything the screen shows, plus the rules that update it from PC events.
#pragma once
#include <Arduino.h>

struct Window {
  float pct = -1;         // 0-100, -1 = unknown
  uint32_t resetsAt = 0;  // unix seconds
};

struct Usage {
  Window session, week;
  uint32_t receivedAt = 0;
};

enum class SessionState : uint8_t { Idle, Working, Waiting, Done };

struct Session {
  bool used = false;
  char id[40] = "";
  char project[32] = "";
  char model[24] = "";
  char message[100] = "";  // what Claude is waiting on
  float ctxPct = -1;
  uint32_t ctxUsed = 0, ctxSize = 0;
  SessionState state = SessionState::Idle;
  uint32_t stateSince = 0;
  uint32_t lastSeen = 0;
  bool alertDismissed = false;
};

struct ServiceStatus {
  bool known = false;
  char indicator[12] = "";  // none | minor | major | critical | maintenance
  char text[64] = "";
  uint32_t checkedAt = 0;
};

struct Toast {
  char text[64] = "";
  uint32_t until = 0;
};

struct Forecast {
  enum Kind : uint8_t { Unknown, OnPace, HitsLimit, Reached } kind = Unknown;
  float pctAtReset = 0;
  uint32_t limitAt = 0;
  uint32_t resetsAt = 0;
};

static const int kMaxSessions = 4;
static const int kHistory = 32;

struct Sample {
  uint32_t t;
  float pct;
  uint32_t resetsAt;
};

struct AppState {
  Usage usage;
  Session sessions[kMaxSessions];
  int activeSession = -1;  // last session that reported context
  Sample history[kHistory];
  int historyCount = 0;
  ServiceStatus service;
  Toast toast;
  int32_t utcOffset = 0;  // seconds; reported by the PC
  bool utcKnown = false;
};

uint32_t nowUnix();
float effectivePct(const Window& w, uint32_t now);

Session* findSession(AppState& s, const char* id, bool create);
void removeSession(AppState& s, const char* id);
void recordUsage(AppState& s, uint32_t now);
void expireSessions(AppState& s, uint32_t now);

Forecast forecastSession(const AppState& s, uint32_t now);
Forecast forecastWeek(const AppState& s, uint32_t now);

int countState(const AppState& s, SessionState st);
const Session* firstWaitingAlert(const AppState& s);
