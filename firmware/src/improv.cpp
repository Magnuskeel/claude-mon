#include "improv.h"

#include <WiFi.h>

#include "version.h"

namespace {

enum : uint8_t { kTypeState = 0x01, kTypeError = 0x02, kTypeRpc = 0x03, kTypeRpcResult = 0x04 };
enum : uint8_t { kStateReady = 0x02, kStateProvisioning = 0x03, kStateProvisioned = 0x04 };
enum : uint8_t { kErrNone = 0x00, kErrInvalid = 0x01, kErrUnknownCmd = 0x02, kErrConnect = 0x03 };
enum : uint8_t { kCmdWifi = 0x01, kCmdState = 0x02, kCmdInfo = 0x03, kCmdScan = 0x04 };

ImprovConnectFn gConnect;
uint8_t gState = kStateReady;
String gUrl;
uint8_t buf[256];
size_t len = 0;

void send(uint8_t type, const uint8_t* data, uint8_t n) {
  uint8_t pkt[9 + 256 + 1];
  memcpy(pkt, "IMPROV", 6);
  pkt[6] = 1;  // protocol version
  pkt[7] = type;
  pkt[8] = n;
  memcpy(pkt + 9, data, n);
  uint8_t sum = 0;
  for (size_t i = 0; i < 9u + n; i++) sum += pkt[i];
  pkt[9 + n] = sum;
  Serial.write(pkt, 10 + n);
  Serial.write('\n');
}

void sendState(uint8_t s) {
  gState = s;
  send(kTypeState, &s, 1);
}

void sendError(uint8_t e) { send(kTypeError, &e, 1); }

// RPC result: command, total length, then length-prefixed strings.
void sendResult(uint8_t cmd, std::initializer_list<String> strings) {
  uint8_t data[250];
  size_t n = 2;
  for (const String& s : strings) {
    if (n + 1 + s.length() > sizeof(data)) break;
    data[n++] = s.length();
    memcpy(data + n, s.c_str(), s.length());
    n += s.length();
  }
  data[0] = cmd;
  data[1] = n - 2;
  send(kTypeRpcResult, data, n);
}

void handleRpc(const uint8_t* d, uint8_t n) {
  if (n < 2 || d[1] != n - 2) return sendError(kErrInvalid);
  uint8_t cmd = d[0];
  const uint8_t* p = d + 2;
  switch (cmd) {
    case kCmdWifi: {
      uint8_t sl = p[0];
      if (2 + sl > n - 2) return sendError(kErrInvalid);
      uint8_t pl = p[1 + sl];
      if (2 + sl + pl > n - 2) return sendError(kErrInvalid);
      String ssid((const char*)p + 1, sl), pass((const char*)p + 2 + sl, pl);
      sendState(kStateProvisioning);
      String url;
      if (gConnect && gConnect(ssid.c_str(), pass.c_str(), url)) {
        gUrl = url;
        sendState(kStateProvisioned);
        sendResult(kCmdWifi, {url});
      } else {
        sendError(kErrConnect);
        sendState(kStateReady);
      }
      break;
    }
    case kCmdState:
      sendState(gState);
      if (gState == kStateProvisioned) sendResult(kCmdState, {gUrl});
      break;
    case kCmdInfo:
      sendResult(kCmdInfo, {FW_NAME, FW_VERSION, "ESP32", "claude-mon"});
      break;
    case kCmdScan: {
      int found = WiFi.scanNetworks();
      for (int i = 0; i < found; i++)
        sendResult(kCmdScan, {WiFi.SSID(i), String(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"});
      WiFi.scanDelete();
      sendResult(kCmdScan, {});
      break;
    }
    default:
      sendError(kErrUnknownCmd);
  }
}

// Feeds one byte; resets on anything that isn't a well-formed packet.
void feed(uint8_t b) {
  static const char kHeader[] = "IMPROV";
  if (len < 6) {
    if (b == (uint8_t)kHeader[len]) buf[len++] = b;
    else len = (b == 'I') ? (buf[0] = b, 1) : 0;
    return;
  }
  buf[len++] = b;
  if (len == 7 && b != 1) { len = 0; return; }  // version
  if (len < 9) return;
  size_t total = 9 + buf[8] + 1;
  if (len < total) return;
  uint8_t sum = 0;
  for (size_t i = 0; i < total - 1; i++) sum += buf[i];
  uint8_t type = buf[7], n = buf[8];
  bool ok = sum == buf[total - 1];
  len = 0;
  if (!ok) return sendError(kErrInvalid);
  if (type == kTypeRpc) handleRpc(buf + 9, n);
}

}  // namespace

void improvBegin(ImprovConnectFn connect) { gConnect = connect; }

void improvSetProvisioned(const String& url) {
  gUrl = url;
  gState = kStateProvisioned;
}

void improvLoop() {
  while (Serial.available()) feed(Serial.read());
}
