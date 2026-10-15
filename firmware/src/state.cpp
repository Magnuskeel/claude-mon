#include "state.h"

#include <time.h>

uint32_t nowUnix() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

// A window whose reset time has passed is back to 0% until the next update.
float effectivePct(const Window& w, uint32_t now) {
  if (w.pct < 0) return -1;
  if (now && w.resetsAt && now >= w.resetsAt) return 0;
  return w.pct;
}

Session* findSession(AppState& s, const char* id, bool create) {
  if (!id || !*id) return nullptr;
  for (auto& x : s.sessions)
    if (x.used && strcmp(x.id, id) == 0) return &x;
  if (!create) return nullptr;

  Session* slot = nullptr;
  for (auto& x : s.sessions)
    if (!x.used) { slot = &x; break; }
  if (!slot) {  // full: replace the session heard from least recently
    slot = &s.sessions[0];
    for (auto& x : s.sessions)
      if (x.lastSeen < slot->lastSeen) slot = &x;
  }
  *slot = Session();
  slot->used = true;
  strlcpy(slot->id, id, sizeof(slot->id));
  return slot;
}

void removeSession(AppState& s, const char* id) {
  if (Session* x = findSession(s, id, false)) *x = Session();
}

void recordUsage(AppState& s, uint32_t now) {
  const Window& w = s.usage.session;
  if (w.pct < 0 || !now) return;
  if (s.historyCount && now - s.history[s.historyCount - 1].t < 60) {
    s.history[s.historyCount - 1] = {now, w.pct, w.resetsAt};
    return;
  }
  if (s.historyCount == kHistory) {
    memmove(s.history, s.history + 1, sizeof(Sample) * (kHistory - 1));
    s.historyCount--;
  }
  s.history[s.historyCount++] = {now, w.pct, w.resetsAt};
}

void expireSessions(AppState& s, uint32_t now) {
  if (!now) return;
  for (auto& x : s.sessions) {
    if (!x.used) continue;
    uint32_t quiet = now - x.lastSeen;
    if (quiet > (x.state == SessionState::Waiting ? 6 * 3600u : 2 * 3600u)) x = Session();
    else if (x.state == SessionState::Done && now - x.stateSince > 600) x.state = SessionState::Idle;
  }
}

static bool sameWindow(uint32_t a, uint32_t b) { return (a > b ? a - b : b - a) <= 120; }

// Projects a window forward at a given rate (percent per second).
static Forecast project(const Window& w, float rate, uint32_t from) {
  Forecast f;
  f.resetsAt = w.resetsAt;
  if (w.pct >= 100) {
    f.kind = Forecast::Reached;
    return f;
  }
  if (rate <= 0) {
    f.kind = Forecast::OnPace;
    f.pctAtReset = w.pct;
    return f;
  }
  uint32_t limitAt = from + (uint32_t)((100 - w.pct) / rate);
  if (limitAt < w.resetsAt) {
    f.kind = Forecast::HitsLimit;
    f.limitAt = limitAt;
  } else {
    f.kind = Forecast::OnPace;
    f.pctAtReset = min(100.0f, w.pct + rate * (w.resetsAt - from));
  }
  return f;
}

// Uses the recent burn rate (last hour) when there's enough history, otherwise
// the average since the 5-hour window opened.
Forecast forecastSession(const AppState& s, uint32_t now) {
  const Window& w = s.usage.session;
  if (w.pct < 0 || !w.resetsAt || !now || now >= w.resetsAt) return Forecast();

  if (s.historyCount >= 2) {
    const Sample& last = s.history[s.historyCount - 1];
    for (int i = 0; i < s.historyCount - 1; i++) {
      const Sample& old = s.history[i];
      if (!sameWindow(old.resetsAt, last.resetsAt) || last.t - old.t > 3600) continue;
      if (last.t - old.t < 600) break;
      return project(w, (last.pct - old.pct) / (last.t - old.t), now);
    }
  }
  uint32_t start = w.resetsAt - 5 * 3600;
  if (now <= start + 300) return Forecast();
  return project(w, w.pct / (now - start), now);
}

// Weekly average pace since the 7-day window opened.
Forecast forecastWeek(const AppState& s, uint32_t now) {
  const Window& w = s.usage.week;
  if (w.pct < 0 || !w.resetsAt || !now || now >= w.resetsAt) return Forecast();
  uint32_t start = w.resetsAt - 7 * 86400;
  if (now <= start + 3600) return Forecast();
  return project(w, w.pct / (now - start), now);
}

int countState(const AppState& s, SessionState st) {
  int n = 0;
  for (auto& x : s.sessions)
    if (x.used && x.state == st) n++;
  return n;
}

const Session* firstWaitingAlert(const AppState& s) {
  const Session* best = nullptr;
  for (auto& x : s.sessions)
    if (x.used && x.state == SessionState::Waiting && !x.alertDismissed &&
        (!best || x.stateSince < best->stateSince))
      best = &x;
  return best;
}
