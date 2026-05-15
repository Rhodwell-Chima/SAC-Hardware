#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
#define PORTAL_AP_SSID "AccessCtrl-Setup"
#define PORTAL_AP_PASSWORD ""
#define PORTAL_AP_IP "192.168.4.1"
#define MDNS_NAME "accesscontrol"  // → accesscontrol.local
#define DNS_PORT 53
#define HTTP_PORT 80

// ---------------------------------------------------------------------------
// MIME type helper
// ---------------------------------------------------------------------------
static const char* _mimeFor(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".woff2")) return "font/woff2";
  if (path.endsWith(".woff")) return "font/woff";
  if (path.endsWith(".ico")) return "image/x-icon";
  return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// Portal
// ---------------------------------------------------------------------------
class ConfigPortal {
public:

  // ── SoftAP mode — blocks until saved + reboots ────────────────────────
  void startAP() {
    Serial.println("[Portal] Starting SoftAP captive portal...");

    if (!LittleFS.begin()) {
      Serial.println("[Portal] FATAL: LittleFS mount failed -- did you upload the data/ folder?");
      return;
    }

    // AP_STA so WiFi.scanNetworks() still works in AP mode
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(PORTAL_AP_SSID, PORTAL_AP_PASSWORD);
    delay(500);

    IPAddress apIP;
    apIP.fromString(PORTAL_AP_IP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

    _dns.start(DNS_PORT, "*", apIP);

    _server = new WebServer(HTTP_PORT);
    _registerRoutes(true);
    _server->begin();

    MDNS.begin(MDNS_NAME);
    MDNS.addService("http", "tcp", 80);

    Serial.printf("[Portal] AP up         : SSID: %s\n", PORTAL_AP_SSID);
    Serial.printf("[Portal] AP IP         : http://%s/\n", PORTAL_AP_IP);
    Serial.printf("[Portal] Non IP URL    : http://%s.local/\n", MDNS_NAME);

    while (true) {
      _dns.processNextRequest();
      _server->handleClient();
    }
    // Never exits -- /save handler calls ESP.restart()
  }

  // ── LAN mode — non-blocking ───────────────────────────────────────────
  void startLAN() {
    if (_server) return;

    if (!LittleFS.begin()) {
      Serial.println("[Portal] FATAL: LittleFS mount failed -- did you upload the data/ folder?");
      return;
    }

    _server = new WebServer(HTTP_PORT);
    _registerRoutes(false);
    _server->begin();

    MDNS.begin(MDNS_NAME);
    MDNS.addService("http", "tcp", 80);

    Serial.printf("[Portal] LAN server up\n");
    Serial.printf("[Portal] IP URL        : http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("[Portal] Non IP URL    : http://%s.local/\n", MDNS_NAME);
  }

  void handleClient() {
    if (_server) _server->handleClient();
  }

private:
  WebServer* _server = nullptr;
  DNSServer _dns;

  // ── Serve a file straight from LittleFS ──────────────────────────────
  void _serveFile(const String& path) {
    if (!LittleFS.exists(path)) {
      _server->send(404, "text/plain", "Not found: " + path);
      Serial.printf("[Portal] 404: %s\n", path.c_str());
      return;
    }
    File f = LittleFS.open(path, "r");
    _server->streamFile(f, _mimeFor(path));
    f.close();
  }

  // ── Current config as JSON (app.js fetches this on page load) ─────────
  String _configJson(bool apMode) {
    StaticJsonDocument<256> doc;
    doc["ssid"] = Config.cfg.ssid;
    // Password intentionally omitted -- never sent back to the browser
    doc["serverUrl"] = Config.cfg.authServerUrl;
    doc["apId"] = Config.cfg.accessPointId;
    doc["authTimeout"] = Config.cfg.authTimeoutMs;
    doc["lockDuration"] = Config.cfg.doorLockDuration;
    doc["mode"] = apMode ? "ap" : "lan";

    String out;
    serializeJson(doc, out);
    return out;
  }

  // ── WiFi scan as JSON array, sorted by RSSI descending ────────────────
  String _scanJson() {
    int n = WiFi.scanNetworks(false, false);

    StaticJsonDocument<4096> doc;
    JsonArray arr = doc.to<JsonArray>();

    if (n > 0) {
      // Insertion sort descending by RSSI (n is small, ~20 max)
      int idx[n];
      for (int i = 0; i < n; i++) idx[i] = i;
      for (int i = 1; i < n; i++) {
        int key = idx[i], j = i - 1;
        while (j >= 0 && WiFi.RSSI(idx[j]) < WiFi.RSSI(key)) {
          idx[j + 1] = idx[j--];
        }
        idx[j + 1] = key;
      }

      for (int i = 0; i < n; i++) {
        int k = idx[i];
        if (WiFi.SSID(k).length() == 0) continue;  // skip hidden

        JsonObject net = arr.createNestedObject();
        net["ssid"] = WiFi.SSID(k);
        net["rssi"] = WiFi.RSSI(k);
        net["channel"] = WiFi.channel(k);
        net["secure"] = (WiFi.encryptionType(k) != WIFI_AUTH_OPEN);
      }
    }

    WiFi.scanDelete();

    String out;
    serializeJson(arr, out);
    return out;
  }

  // ── Route registration ────────────────────────────────────────────────
  void _registerRoutes(bool apMode) {

    // Static files -- each route maps directly to a LittleFS path
    _server->on("/", HTTP_GET, [this]() {
      _serveFile("/index.html");
    });
    _server->on("/index.html", HTTP_GET, [this]() {
      _serveFile("/index.html");
    });
    _server->on("/app.css", HTTP_GET, [this]() {
      _serveFile("/app.css");
    });
    _server->on("/app.js", HTTP_GET, [this]() {
      _serveFile("/app.js");
    });
    _server->on("/bootstrap.min.css", HTTP_GET, [this]() {
      _serveFile("/bootstrap.min.css");
    });
    _server->on("/bootstrap.bundle.min.js", HTTP_GET, [this]() {
      _serveFile("/bootstrap.bundle.min.js");
    });
    _server->on("/bootstrap-icons.min.css", HTTP_GET, [this]() {
      _serveFile("/bootstrap-icons.min.css");
    });
    _server->on("/fonts/bootstrap-icons.woff2", HTTP_GET, [this]() {
      _serveFile("/fonts/bootstrap-icons.woff2");
    });

    // API: current config values for form pre-fill
    _server->on("/config", HTTP_GET, [this, apMode]() {
      _server->send(200, "application/json", _configJson(apMode));
    });

    // API: WiFi scan
    _server->on("/scan", HTTP_GET, [this]() {
      _server->send(200, "application/json", _scanJson());
    });

    // API: save and reboot
    _server->on("/save", HTTP_POST, [this]() {
      if (!_server->hasArg("plain")) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"No body\"}");
        return;
      }

      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, _server->arg("plain"))) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Bad JSON\"}");
        return;
      }

      if (!doc.containsKey("ssid") || !doc.containsKey("serverUrl")) {
        _server->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing fields\"}");
        return;
      }

      strlcpy(Config.cfg.ssid, doc["ssid"] | Config.cfg.ssid, sizeof(Config.cfg.ssid));
      strlcpy(Config.cfg.authServerUrl, doc["serverUrl"] | Config.cfg.authServerUrl, sizeof(Config.cfg.authServerUrl));

      const char* pw = doc["password"] | "";
      if (strlen(pw) > 0)
        strlcpy(Config.cfg.password, pw, sizeof(Config.cfg.password));

      Config.cfg.accessPointId = doc["apId"] | Config.cfg.accessPointId;
      Config.cfg.authTimeoutMs = doc["authTimeout"] | Config.cfg.authTimeoutMs;
      Config.cfg.doorLockDuration = doc["lockDuration"] | Config.cfg.doorLockDuration;

      Config.save();
      _server->send(200, "application/json", "{\"ok\":true}");
      delay(800);
      ESP.restart();
    });

    // API: factory reset
    _server->on("/reset", HTTP_POST, [this]() {
      Config.factoryReset();
      _server->send(200, "application/json", "{\"ok\":true}");
      delay(800);
      ESP.restart();
    });

    // Captive portal redirect probes (Android / iOS / Windows)
    auto redirect = [this]() {
      _server->sendHeader("Location", "/");
      _server->send(302);
    };
    _server->on("/generate_204", HTTP_GET, redirect);
    _server->on("/hotspot-detect.html", HTTP_GET, redirect);
    _server->on("/ncsi.txt", HTTP_GET, redirect);
    _server->on("/connecttest.txt", HTTP_GET, redirect);
    _server->onNotFound(redirect);
  }
};

extern ConfigPortal Portal;