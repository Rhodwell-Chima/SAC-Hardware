#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "portal.h"
#include "rfid.h"
#include "relay.h"

// ---------------------------------------------------------------------------
// Global instances (definitions declared extern in their headers)
// ---------------------------------------------------------------------------
ConfigManager Config;
ConfigPortal Portal;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
bool checkCardAuth(const String &uid);
bool isRFIDConnected();
String getCardUID();
void connectWiFi();
void rfidTask(void *pvParameters);

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  delay(200);

  //    1. Load persisted config from NVS
  Config.load();

  //    2. Check physical button
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  bool buttonHeld = (digitalRead(CONFIG_BUTTON_PIN) == LOW);

  if (buttonHeld)
  {
    Serial.println("[Setup] BOOT button held   entering config portal.");
    Serial.println("[Setup] Release within 3 s to cancel.");
    delay(3000);

    if (digitalRead(CONFIG_BUTTON_PIN) == HIGH)
    {
      Serial.println("[Setup] Button released   continuing normal boot.");
    }
    else
    {
      // AsyncWebServer is self-driven; startAP() returns immediately.
      // We spin here servicing only DNS — the web server needs no polling.
      Portal.startAP();
      while (true)
      {
        Portal.handleDNS();
        delay(10);
      }
    }
  }

  //    3. Hardware init
  initRFID();

  // initialize relay module (replaces direct pin handling + service task)
  initRelay();

  //    4. WiFi   retry forever, no AP fallback
  connectWiFi();

  //    5. Start LAN config server (non-blocking)
  // AsyncWebServer runs on its own FreeRTOS task — no handleClient() needed.
  Portal.startLAN();

  //    6. Start application tasks
  startRelayTask();
  startRFIDTask();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop()
{
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}

// ===========================================================================
// WiFi
// ===========================================================================

void connectWiFi()
{
  if (!Config.isProvisioned())
  {
    Serial.println("[WiFi] No SSID configured. Hold BOOT button on power-on to set up.");
    Serial.println("[WiFi] Halting   door remains locked.");
    while (true)
      delay(1000);
  }

  Serial.printf("[WiFi] Connecting to \"%s\"", Config.cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(Config.cfg.ssid, Config.cfg.password);

  // Retry forever — no AP fallback (closes deauth attack vector).
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");

    // Re-issue WiFi.begin() every 30 s in case the connection stalled
    static uint32_t lastBegin = 0;
    if (millis() - lastBegin > 30000)
    {
      lastBegin = millis();
      WiFi.disconnect();
      WiFi.begin(Config.cfg.ssid, Config.cfg.password);
      Serial.print("\n[WiFi] Retrying...");
    }
  }

  Serial.printf("\n[WiFi] Connected   IP: %s\n", WiFi.localIP().toString().c_str());
}

// ===========================================================================
// Auth
// ===========================================================================

bool checkCardAuth(const String &uid)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[AUTH] WiFi not connected   denying by default.");
    return false;
  }

  StaticJsonDocument<192> reqDoc;
  reqDoc["card_uid"] = uid;
  reqDoc["access_point_id"] = Config.cfg.accessPointId;
  reqDoc["api_key"] = Config.cfg.apiKey; // shared secret

  String reqBody;
  serializeJson(reqDoc, reqBody);

  HTTPClient http;
  http.begin(AUTH_SERVER_URL); // compile-time constant
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(Config.cfg.authTimeoutMs);

  int httpCode = http.POST(reqBody);

  if (httpCode <= 0)
  {
    Serial.printf("[AUTH] Request failed: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }

  String resBody = http.getString();
  http.end();

  Serial.printf("[AUTH] HTTP %d   %s\n", httpCode, resBody.c_str());

  StaticJsonDocument<256> resDoc;
  DeserializationError err = deserializeJson(resDoc, resBody);

  if (err)
  {
    Serial.printf("[AUTH] JSON parse error: %s\n", err.c_str());
    return false;
  }

  bool granted = resDoc["granted"] | false;
  String reason = resDoc["reason"] | "UNKNOWN";

  if (granted)
  {
    String user = resDoc["user"] | "Unknown";
    Serial.printf("[AUTH] GRANTED   %s (%s)\n", user.c_str(), reason.c_str());
  }
  else
  {
    Serial.printf("[AUTH] DENIED    %s\n", reason.c_str());
  }

  return granted;
}