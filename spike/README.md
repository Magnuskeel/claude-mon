# M0 spike

Proves the board can reach Anthropic over TLS and shows exactly which usage
headers come back. Serial output only, no display yet.

1. `cp src/secrets.example.h src/secrets.h` and fill in Wi-Fi + the token from
   `claude setup-token` (run that once in any terminal).
2. Plug in the **micro-USB** port (that one has the CH340 serial chip on
   two-port boards; the USB-C is often power-only).
3. `pio run -t upload && pio device monitor`
4. Paste the serial log back (the token is never printed).

What it does, once at boot then every 5 minutes:
- `GET /api/oauth/usage` — expected to fail with 403 for setup-tokens; confirms it.
- `POST /v1/messages` with `max_tokens: 1` — prints every response header,
  status, body excerpt, timing and free heap. If it's rejected, retries once
  with the Claude Code system prompt.
