# claude-mon — Design

A standalone Claude usage monitor on the ESP32-2432S028R ("Cheap Yellow Display").

## Goals

1. **Nothing runs on the computer.** The device talks to Anthropic over Wi-Fi by itself. You only need a browser to flash it and set it up once.
2. **Readable from across the desk:** 5-hour session %, weekly %, and the time until each one resets.
3. **Simple and auditable.** Our own small codebase, PlatformIO + Arduino, no cloud relay.

Non-goals for v1: per-project token or cost breakdown (that data only exists in Claude Code's logs on the computer), and Bluetooth.

---

## 1. Hardware assessment: ESP32-2432S028R

| Part | Detail | Notes for us |
|---|---|---|
| MCU | ESP32-WROOM-32, 240 MHz dual core, 520 KB SRAM, **no PSRAM**, 4 MB flash | Enough for TLS plus a hand-drawn UI. A full-screen framebuffer (150 KB) is too big, so we draw in partial sprites. |
| Display | 2.8" 240×320 TFT, **ILI9341** (1 USB port) or **ST7789** (2 USB ports, "CYD2USB", inverted colours) | Driver chosen at build time with a flag. |
| Display SPI (HSPI) | MOSI 13, MISO 12, SCK 14, CS 15, DC 2, backlight **21** (PWM) | |
| Touch | XPT2046 resistive on its own SPI: MOSI 32, MISO 39, CLK 25, CS 33, IRQ 36 | Fine for big buttons and swipes. Needs a one-time calibration saved to NVS. |
| RGB LED | GPIO 4 (R), 16 (G), 17 (B), **active-low** | Status light: green, amber, red. |
| LDR | GPIO 34 (ADC) | Auto-dims the backlight at night. |
| Speaker | GPIO 26 (DAC/I²S amp) | Optional chirp at 80% and 95%. |
| SD slot | VSPI 5/18/19/23 | Not used in v1. |
| USB-serial | CH340 | Built into macOS and Linux. Windows may need the WCH driver; that is the only thing that might ever touch the PC. |
| Buttons | BOOT (GPIO0), RST | BOOT held at power-up opens the setup portal. |

**Verdict:** the board is a good fit. The tight spots are RAM (TLS uses about 40–50 KB of heap) and resistive touch accuracy. Neither blocks a gauge-style dashboard.

---

## 2. Where the usage numbers come from

This is the main design risk, so the options are spelled out.

| Source | What it gives | Works standalone? | Catch |
|---|---|---|---|
| **A. Rate-limit response headers** (`anthropic-ratelimit-unified-*`) on a tiny `/v1/messages` call, authenticated with a `claude setup-token` token | 5h and 7d utilisation, reset times, allowed/throttled status | ✅ Yes. `setup-token` gives a long-lived (about 1 year) token, so no refresh logic. | Each poll is a real (tiny) inference request and counts slightly against your quota. Header names are undocumented. |
| **B. `GET /api/oauth/usage`** (what `/usage` in Claude Code shows) | 5h, 7d, per-model weekly, extra usage, reset times | ⚠️ Not with a setup-token. Those tokens are scoped `user:inference` only and this endpoint needs `user:profile` (returns 403). | The normal login token has the scope, but it expires in hours and refreshing it from the device would fight with Claude Code's own refresh. |
| **C. Claude Code JSONL logs** (`~/.claude/projects/**`) | Exact tokens and cost per session or project | ❌ Needs software on the computer. | This is the path we're deliberately avoiding. |
| **D. Admin API usage/cost reports** | Org-level API spend | ✅ | Only for API (Console) orgs with an admin key, not Pro/Max subscriptions. |

**Decision:** v1 uses **A**, with the poll interval adjustable (default 5 min, 2 min while usage is climbing, 15 min when idle overnight). The API client is behind an interface so B or D can be added later without touching the UI.

**Policy check before we build:** Anthropic's terms on using subscription OAuth tokens outside official clients have changed over time. Read the current Consumer Terms and Claude Code docs first. If personal tools like this aren't allowed, fall back to D (API orgs) or an opt-in bridge.

### Spike (do this before writing UI)
A 30-line sketch on the board that:
1. Connects to Wi-Fi and syncs time with NTP.
2. Sends the smallest valid `/v1/messages` request (smallest model, `max_tokens: 1`) using the setup-token.
3. Prints every response header and the free heap.

Its output tells us the real header names, whether OAuth inference needs particular request fields, roughly how much each poll costs, and the TLS memory headroom.

---

## 3. Firmware architecture

```
src/
  main.cpp            setup/loop, task wiring
  config/             NVS-backed settings (wifi, token, poll, brightness, thresholds, touch cal)
  net/
    wifi_mgr.*        connect, reconnect with backoff, captive setup portal
    anthropic.*       UsageSource interface + RateLimitHeaderSource (option A)
    time_sync.*       NTP + timezone
  model/usage.*       UsageSnapshot {pct5h, pct7d, reset5h, reset7d, status, fetchedAt, error}
                      + 24h ring buffer (one sample/5 min = 288 samples, ~2 KB)
  ui/
    display.*         LovyanGFX config (ILI9341 / ST7789 via build flag)
    touch.*           XPT2046 + calibration
    screens/          gauge, detail, history, settings, setup
  hw/
    led.*  backlight.*  buzzer.*  ldr.*
platformio.ini        envs: cyd (ILI9341), cyd2usb (ST7789)
```

**Tasks (FreeRTOS):**
- **Core 0, net task:** wakes on the poll timer, fetches, writes a `UsageSnapshot` under a mutex, posts an event. All blocking TLS work stays here.
- **Core 1, UI loop:** reads touch, redraws only what changed, runs countdown timers locally every second (no network needed for those).
- **Task watchdog** on both. After repeated failures it shows a "stale" badge instead of rebooting in a loop.

**Graphics library:** LovyanGFX. It's lighter than LVGL on a board without PSRAM, handles both panel variants and the XPT2046, and draws smooth arcs with sprites. LVGL is worth it only if we later want a richer touch UI.

**TLS:** `WiFiClientSecure` with the ESP-IDF certificate bundle, never `setInsecure()`.

---

## 4. Screens (tap or swipe to cycle)

1. **Gauges (home):** two big ring gauges, *Session (5h)* and *Week (7d)*, each with % in the middle and "resets in 2h 14m" underneath. Ring colour: green under 60%, amber 60–85%, red above 85%. The RGB LED matches the worse of the two.
2. **Detail:** exact reset clock times, throttle status, last-updated time, Wi-Fi signal strength.
3. **History:** 24h sparkline of the 5h utilisation, so you can see burn rate. Shows "at this pace you hit 100% at 15:40".
4. **Settings (touch):** brightness / auto-dim, poll interval, LED on/off, sound on/off, re-calibrate touch, "open setup portal".

Status bar on every screen: Wi-Fi icon, last-update age, red dot when data is stale.

---

## 5. Setup and security

- **First boot (or BOOT held):** device starts the access point `claude-mon-XXXX` and a captive portal served from the device. You enter Wi-Fi details, paste the token from `claude setup-token` (run once in any terminal), and set your timezone.
- **Token storage:** NVS, never logged, never sent back to the browser (write-only field). Optional PIN encryption (AES-GCM, key derived from the PIN at boot) for anyone worried about the board walking off. Full ESP32 flash encryption is possible, but it burns eFuses irreversibly, so it's off by default.
- **If the token leaks:** revoke it from your Claude account settings and paste a new one in the portal.
- **Updates:** OTA upload of a `.bin` through the portal. First flash happens in the browser with ESP Web Tools (Web Serial) or `pio run -t upload`.

---

## 6. Milestones

| # | Deliverable | Done when |
|---|---|---|
| M0 | Spike: Wi-Fi, TLS, print rate-limit headers | Real header names and heap numbers recorded in this doc |
| M1 | Board bring-up: display, touch, LED, backlight, LDR on both panel variants | Test pattern and touch dots drawn correctly |
| M2 | Config: NVS settings + captive portal | Fresh board is set up from a phone with no reflash |
| M3 | Usage client: polling, backoff, stale handling, history buffer | 24h soak with no crash; survives Wi-Fi loss |
| M4 | UI: gauge, detail, history, settings | Readable at 1 m; screen updates are flicker-free |
| M5 | Polish: OTA, auto-dim, alert sounds, web flasher page | Fresh user goes from unboxing to working in 10 minutes |

Optional M6: a single-file, opt-in "bridge" script for anyone who *wants* source B or C. It would only run when launched by hand: no service, no drivers, no auto-start.

---

## 7. Open questions

1. Board variant: one USB port (ILI9341) or two (ST7789)?
2. Plan: Pro, Max 5×, Max 20×, or API key? This decides between source A and D.
3. Is a tiny quota cost per poll acceptable? At 5-minute polls that's 288 one-token calls a day.
4. Should alerts beep, or only show on the LED and screen?
