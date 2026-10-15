# claude-mon

A desk display for Claude Code on the ESP32-2432S028R ("Cheap Yellow Display").

- **Usage rings:** context, 5-hour session and weekly limits, with reset countdowns.
- **Pace forecast:** "limit ~3:40 PM, 50m early" or "~72% by 5:10 PM".
- **"Claude needs you" alert:** a full-screen card and a pulsing LED when Claude waits for permission or input.
- **Sessions page:** every open Claude Code window with its state and context.
- **Anthropic service status** from the public status page.

The board holds no Anthropic credentials. Claude Code's own status line and hooks push data
to it over your local network. Each one runs for a moment and exits, so nothing stays running on your PC.
An optional "claude-mon usage" scheduled task refreshes usage every 5 minutes, for when you only use
cloud sessions (hidden, low priority, stopped by Windows after 1 minute).

## Install

Go to **https://magnuskeel.github.io/claude-mon/** and follow the three steps:

1. **Flash the board from the browser** (Chrome or Edge) and give it your Wi-Fi.
2. **Note the pairing code** the board shows.
3. **Run the installer** in PowerShell, then restart Claude Code:
   ```powershell
   irm https://magnuskeel.github.io/claude-mon/install.ps1 | iex
   ```

**Uninstall:** `powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\claude-mon\uninstall.ps1"`.
**Upside down in an enclosure:** press BOOT briefly to turn the picture 180°.
**Wrong screen colours or stripes:** hold BOOT about 3 seconds to cycle screen settings (a test card shows each).
**Reset the board:** hold BOOT for 5 seconds.

## Layout

| Path | What |
|---|---|
| `firmware/` | PlatformIO project (`pio run -t upload`) |
| `host/` | PowerShell installer, status line and hook scripts |
| `site/` | Web installer page (ESP Web Tools) |
| `.github/workflows/site.yml` | Builds firmware and publishes the site to GitHub Pages |
| `spike/` | The first network experiment (kept for reference) |
| `DESIGN.md` | Design notes and decisions |

## Board HTTP API

| Endpoint | Auth | Purpose |
|---|---|---|
| `GET /info` | none | name, version, paired, pairing |
| `POST /pair` `{code, pc}` | the code on screen | returns `{key}` |
| `POST /usage` | `X-Claude-Mon-Key` | from the status line: rate limits, context, model, project |
| `POST /hook` | `X-Claude-Mon-Key` | from hooks: `UserPromptSubmit`, `Notification`, `Stop`, `SessionStart`, `SessionEnd`, `Test` |
