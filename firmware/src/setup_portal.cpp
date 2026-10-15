#include "setup_portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

namespace {

DNSServer dns;
WebServer web(80);
PortalSaveFn gSave;
bool running = false;

const char kHead[] PROGMEM = R"(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>claude-mon setup</title>
<style>body{font-family:system-ui,sans-serif;background:#0e1116;color:#eceff4;margin:0;padding:24px}
.card{max-width:420px;margin:auto;background:#181d25;border-radius:14px;padding:24px}
h1{font-size:20px;margin:0 0 4px;color:#d97757}p{color:#8b95a5;margin:0 0 20px}
label{display:block;font-size:13px;color:#8b95a5;margin:14px 0 6px}
input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #2a313c;
background:#0e1116;color:#eceff4;font-size:16px}
button{margin-top:20px;width:100%;padding:13px;border:0;border-radius:10px;background:#d97757;
color:#fff;font-size:16px;font-weight:600}</style></head><body><div class="card">)";

String escape(const String& s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

void handleRoot() {
  int found = WiFi.scanNetworks();
  String html = FPSTR(kHead);
  html += "<h1>claude-mon setup</h1><p>Choose your 2.4 GHz Wi-Fi network.</p>"
          "<form method=post action=/save><label>Network</label><input name=ssid list=nets required>"
          "<datalist id=nets>";
  for (int i = 0; i < found; i++) html += "<option value=\"" + escape(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
  html += "</datalist><label>Password</label><input name=pass type=password>"
          "<button>Connect</button></form></div></body></html>";
  web.send(200, "text/html", html);
}

void handleSave() {
  String html = FPSTR(kHead);
  html += "<h1>Connecting...</h1><p>Watch the board's screen. If it can't join, this network "
          "comes back so you can try again.</p></div></body></html>";
  web.send(200, "text/html", html);
  delay(300);
  if (gSave) gSave(web.arg("ssid"), web.arg("pass"));
}

}  // namespace

void portalStart(PortalSaveFn onSave) {
  gSave = onSave;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kSetupApName);
  dns.start(53, "*", WiFi.softAPIP());
  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound([] {
    web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    web.send(302, "text/plain", "");
  });
  web.begin();
  running = true;
}

void portalLoop() {
  if (!running) return;
  dns.processNextRequest();
  web.handleClient();
}

void portalStop() {
  if (!running) return;
  web.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  running = false;
}
