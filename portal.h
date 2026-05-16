#pragma once

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_random.h> // hardware RNG for token generation
#include "config.h"

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
#define PORTAL_AP_SSID "AccessCtrl-Setup"
#define PORTAL_AP_PASSWORD ""
#define PORTAL_AP_IP "192.168.4.1"
#define PORTAL_AP_USER "admin"
#define MDNS_NAME "accesscontrol"
#define DNS_PORT 53
#define HTTP_PORT 80

// Session cookie name and lifetime
#define SESSION_COOKIE_NAME "AC_SESSION"
#define SESSION_TTL_MS (30UL * 60UL * 1000UL) // 30 minutes

// ---------------------------------------------------------------------------
// Portal
// ---------------------------------------------------------------------------
class ConfigPortal
{
public:
  // ── SoftAP mode ───────────────────────────────────────────────────────
  void startAP()
  {
    Serial.println("[Portal] Starting SoftAP captive portal...");
    if (!_mountFS())
      return;

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(PORTAL_AP_SSID, PORTAL_AP_PASSWORD);
    delay(500);

    IPAddress apIP;
    apIP.fromString(PORTAL_AP_IP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

    _dns.start(DNS_PORT, "*", apIP);
    _apMode = true;

    _initServer();

    MDNS.begin(MDNS_NAME);
    MDNS.addService("http", "tcp", 80);

    Serial.printf("[Portal] AP up         : SSID: %s\n", PORTAL_AP_SSID);
    Serial.printf("[Portal] AP IP         : http://%s/\n", PORTAL_AP_IP);
    Serial.printf("[Portal] Non IP URL    : http://%s.local/\n", MDNS_NAME);
    Serial.printf("[Portal] Auth          : user=%s\n", PORTAL_AP_USER);
  }

  // ── LAN mode ──────────────────────────────────────────────────────────
  void startLAN()
  {
    if (_server)
      return;
    if (!_mountFS())
      return;

    _apMode = false;
    _initServer();

    MDNS.begin(MDNS_NAME);
    MDNS.addService("http", "tcp", 80);

    Serial.printf("[Portal] LAN server up\n");
    Serial.printf("[Portal] IP URL        : http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("[Portal] Non IP URL    : http://%s.local/\n", MDNS_NAME);
  }

  // ── DNS polling — call from loop() in AP mode only ────────────────────
  void handleDNS()
  {
    if (_apMode)
      _dns.processNextRequest();
  }

private:
  AsyncWebServer *_server = nullptr;
  DNSServer _dns;
  bool _apMode = false;

  // Session state — single active session (sufficient for a device portal)
  String _sessionToken;
  uint32_t _sessionExpiresAt = 0;

  // Async WiFi scan state
  volatile bool _scanPending = false;
  AsyncWebServerRequest *_scanRequest = nullptr;

  // ── LittleFS ──────────────────────────────────────────────────────────
  bool _mountFS()
  {
    if (!LittleFS.begin())
    {
      Serial.println("[Portal] FATAL: LittleFS mount failed -- upload data/ first.");
      return false;
    }
    return true;
  }

  // ── Token generation — 32 hex chars from hardware RNG ─────────────────
  String _generateToken()
  {
    String token = "";
    token.reserve(32);
    for (int i = 0; i < 4; i++)
    {
      uint32_t r = esp_random();
      char buf[9];
      snprintf(buf, sizeof(buf), "%08lx", (unsigned long)r);
      token += buf;
    }
    return token;
  }

  // ── Session validation ────────────────────────────────────────────────
  // Returns true if the request carries a valid, non-expired session cookie.
  bool _isAuthenticated(AsyncWebServerRequest *r)
  {
    if (_sessionToken.isEmpty())
      return false;
    if ((int32_t)(millis() - _sessionExpiresAt) > 0)
    {
      // Session expired — clear it
      _sessionToken = "";
      return false;
    }
    if (!r->hasHeader("Cookie"))
      return false;

    String cookies = r->header("Cookie");
    String needle = String(SESSION_COOKIE_NAME) + "=" + _sessionToken;
    return cookies.indexOf(needle) >= 0;
  }

  // ── Auth gate — redirects to login page rather than issuing a 401 ─────
  bool _requireAuth(AsyncWebServerRequest *r)
  {
    if (_isAuthenticated(r))
      return true;
    r->redirect("/login");
    return false;
  }

  // ── Serve a file from LittleFS ────────────────────────────────────────
  void _serveFile(AsyncWebServerRequest *r, const String &path)
  {
    if (!LittleFS.exists(path))
    {
      r->send(404, "text/plain", "Not found: " + path);
      Serial.printf("[Portal] 404: %s\n", path.c_str());
      return;
    }
    r->send(LittleFS, path);
  }

  // ── Config JSON — secrets excluded ────────────────────────────────────
  String _configJson()
  {
    StaticJsonDocument<256> doc;
    doc["ssid"] = Config.cfg.ssid;
    doc["apId"] = Config.cfg.accessPointId;
    doc["authTimeout"] = Config.cfg.authTimeoutMs;
    doc["lockDuration"] = Config.cfg.doorLockDuration;
    doc["mode"] = _apMode ? "ap" : "lan";
    String out;
    serializeJson(doc, out);
    return out;
  }

  // ── Scan result JSON ──────────────────────────────────────────────────
  String _buildScanJson()
  {
    int n = WiFi.scanComplete();
    if (n < 0)
      return "[]";

    int idx[n];
    for (int i = 0; i < n; i++)
      idx[i] = i;
    for (int i = 1; i < n; i++)
    {
      int key = idx[i], j = i - 1;
      while (j >= 0 && WiFi.RSSI(idx[j]) < WiFi.RSSI(key))
        idx[j + 1] = idx[j--];
      idx[j + 1] = key;
    }

    StaticJsonDocument<4096> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; i++)
    {
      int k = idx[i];
      if (WiFi.SSID(k).length() == 0)
        continue;
      JsonObject net = arr.createNestedObject();
      net["ssid"] = WiFi.SSID(k);
      net["rssi"] = WiFi.RSSI(k);
      net["channel"] = WiFi.channel(k);
      net["secure"] = (WiFi.encryptionType(k) != WIFI_AUTH_OPEN);
    }

    WiFi.scanDelete();
    String out;
    serializeJson(arr, out);
    return out;
  }

  // ── Route registration ────────────────────────────────────────────────
  void _initServer()
  {
    _server = new AsyncWebServer(HTTP_PORT);

    // ── Login page — no auth required ─────────────────────────────────
    _server->on("/login", HTTP_GET, [this](AsyncWebServerRequest *r)
                {
            // Already logged in — skip the login page
            if (_isAuthenticated(r)) { r->redirect("/"); return; }
            _serveFile(r, "/login.html"); });

    // ── Login POST — validate credentials, issue session cookie ────────
    auto *loginHandler = new AsyncCallbackJsonWebHandler(
        "/login",
        [this](AsyncWebServerRequest *r, JsonVariant &body)
        {
          JsonObject doc = body.as<JsonObject>();

          const char *user = doc["username"] | "";
          const char *pass = doc["password"] | "";

          bool userOk = (strcmp(user, PORTAL_AP_USER) == 0);
          bool passOk = (strcmp(pass, Config.cfg.portalPassword) == 0);

          if (!userOk || !passOk)
          {
            // Constant-time-ish delay to slow brute-force attempts
            delay(500);
            Serial.printf("[Portal] Failed login attempt for user: %s\n", user);
            r->send(401, "application/json",
                    "{\"ok\":false,\"error\":\"Invalid username or password.\"}");
            return;
          }

          // Generate and store session token
          _sessionToken = _generateToken();
          _sessionExpiresAt = millis() + SESSION_TTL_MS;

          // Build Set-Cookie header
          // HttpOnly prevents JS from reading the token
          // SameSite=Strict prevents CSRF
          String cookie = String(SESSION_COOKIE_NAME) + "=" + _sessionToken + "; Path=/; HttpOnly; SameSite=Strict";

          AsyncWebServerResponse *resp =
              r->beginResponse(200, "application/json", "{\"ok\":true}");
          resp->addHeader("Set-Cookie", cookie);
          r->send(resp);

          Serial.printf("[Portal] Login successful — session issued.\n");
        });
    _server->addHandler(loginHandler);

    // ── Logout — clears session and redirects to login ─────────────────
    _server->on("/logout", HTTP_GET, [this](AsyncWebServerRequest *r)
                {
            _sessionToken     = "";
            _sessionExpiresAt = 0;

            // Expire the cookie on the client side too
            AsyncWebServerResponse* resp = r->beginResponse(302);
            resp->addHeader("Set-Cookie",
                String(SESSION_COOKIE_NAME) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
            resp->addHeader("Location", "/login");
            r->send(resp);

            Serial.println("[Portal] Session cleared — user logged out."); });

    // ── Config page — session required ────────────────────────────────
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest *r)
                {
            if (!_requireAuth(r)) return;
            _serveFile(r, "/index.html"); });
    _server->on("/index.html", HTTP_GET, [this](AsyncWebServerRequest *r)
                {
            if (!_requireAuth(r)) return;
            _serveFile(r, "/index.html"); });

    // ── Static assets — no auth (Safari-safe, contain no secrets) ─────
    _server->serveStatic("/app.css", LittleFS, "/app.css");
    _server->serveStatic("/app.js", LittleFS, "/app.js");
    _server->serveStatic("/bootstrap.min.css", LittleFS, "/bootstrap.min.css");
    _server->serveStatic("/bootstrap.bundle.min.js", LittleFS, "/bootstrap.bundle.min.js");
    _server->serveStatic("/bootstrap-icons.min.css", LittleFS, "/bootstrap-icons.min.css");
    _server->serveStatic("/fonts/bootstrap-icons.woff2", LittleFS, "/fonts/bootstrap-icons.woff2");

    // ── API: config (session required) ────────────────────────────────
    _server->on("/config", HTTP_GET, [this](AsyncWebServerRequest *r)
                {
            if (!_requireAuth(r)) return;
            r->send(200, "application/json", _configJson()); });

    // ── API: WiFi scan (session required, async) ───────────────────────
    _server->on("/scan", HTTP_GET, [this](AsyncWebServerRequest *r)
                {
            if (!_requireAuth(r)) return;

            if (_scanPending) {
                r->send(202, "application/json", "{\"scanning\":true}");
                return;
            }

            int cached = WiFi.scanComplete();
            if (cached >= 0) {
                r->send(200, "application/json", _buildScanJson());
                return;
            }

            _scanPending = true;
            _scanRequest = r;

            WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
                if (!_scanPending || !_scanRequest) return;
                _scanPending = false;
                String json  = _buildScanJson();
                _scanRequest->send(200, "application/json", json);
                _scanRequest = nullptr;
            }, ARDUINO_EVENT_WIFI_SCAN_DONE);

            WiFi.scanNetworks(true, false); });

    // ── API: save config (session required) ───────────────────────────
    auto *saveHandler = new AsyncCallbackJsonWebHandler(
        "/save",
        [this](AsyncWebServerRequest *r, JsonVariant &body)
        {
          if (!_requireAuth(r))
            return;

          JsonObject doc = body.as<JsonObject>();

          if (!doc.containsKey("ssid"))
          {
            r->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"Missing ssid\"}");
            return;
          }

          strlcpy(Config.cfg.ssid,
                  doc["ssid"] | Config.cfg.ssid,
                  sizeof(Config.cfg.ssid));

          const char *pw = doc["password"] | "";
          if (strlen(pw) > 0)
            strlcpy(Config.cfg.password, pw, sizeof(Config.cfg.password));

          const char *ppw = doc["portalPassword"] | "";
          if (strlen(ppw) > 0)
            strlcpy(Config.cfg.portalPassword, ppw, sizeof(Config.cfg.portalPassword));

          const char *ak = doc["apiKey"] | "";
          if (strlen(ak) > 0)
            strlcpy(Config.cfg.apiKey, ak, sizeof(Config.cfg.apiKey));

          Config.cfg.accessPointId = doc["apId"] | Config.cfg.accessPointId;
          Config.cfg.authTimeoutMs = doc["authTimeout"] | Config.cfg.authTimeoutMs;
          Config.cfg.doorLockDuration = doc["lockDuration"] | Config.cfg.doorLockDuration;

          Config.save();
          r->send(200, "application/json", "{\"ok\":true}");
          delay(800);
          ESP.restart();
        });
    _server->addHandler(saveHandler);

    // ── API: factory reset (session required) ──────────────────────────
    _server->on("/reset", HTTP_POST, [this](AsyncWebServerRequest *r)
                {
            if (!_requireAuth(r)) return;
            Config.factoryReset();
            r->send(200, "application/json", "{\"ok\":true}");
            delay(800);
            ESP.restart(); });

    // ── Captive portal probes — redirect to login ──────────────────────
    auto redir = [](AsyncWebServerRequest *r)
    { r->redirect("/login"); };
    _server->on("/generate_204", HTTP_GET, redir);
    _server->on("/hotspot-detect.html", HTTP_GET, redir);
    _server->on("/ncsi.txt", HTTP_GET, redir);
    _server->on("/connecttest.txt", HTTP_GET, redir);
    _server->onNotFound([](AsyncWebServerRequest *r)
                        { r->redirect("/login"); });

    _server->begin();
  }
};

extern ConfigPortal Portal;
