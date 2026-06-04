#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "portal.h"

// ---------------------------------------------------------------------------
// Global instances (definitions   declared extern in their headers)
// ---------------------------------------------------------------------------
ConfigManager Config;
ConfigPortal Portal;

MFRC522 rfid(SS_PIN, RST_PIN);

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
bool checkCardAuth(const String &uid);
void triggerOutput(int32_t pin, uint32_t durationMs);
void serviceRelay();
bool isRFIDConnected();
String getCardUID();
void connectWiFi();
void relayTask(void *pvParameters);
void rfidTask(void *pvParameters);

// ---------------------------------------------------------------------------
// Relay runtime state (non-blocking)
// ---------------------------------------------------------------------------
// These variables are only touched from the main loop today, but a critical
// section keeps the code safe if an ISR or another task is added later.
portMUX_TYPE gRelayMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool gRelayActive = false;
volatile int32_t gRelayPin = -1;
volatile uint32_t gRelayOffAt = 0;

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
      // We spin here servicing only DNS   the web server needs no polling.
      Portal.startAP();
      while (true)
      {
        Portal.handleDNS();
        delay(10);
      }
    }
  }

  //    3. Hardware init
  SPI.begin();
  rfid.PCD_Init();

  pinMode(RST_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  //    4. WiFi   retry forever, no AP fallback
  connectWiFi();

  //    5. Start LAN config server (non-blocking)
  // AsyncWebServer runs on its own FreeRTOS task   no handleClient() needed.
  Portal.startLAN();

  //    6. Start application tasks
  xTaskCreate(relayTask, "RelayTask", 2048, nullptr, 1, nullptr);
  xTaskCreate(rfidTask, "RFIDTask", 4096, nullptr, 1, nullptr);
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop()
{
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}

// ---------------------------------------------------------------------------
// FreeRTOS tasks
// ---------------------------------------------------------------------------
void relayTask(void *pvParameters)
{
  (void)pvParameters;

  for (;;)
  {
    serviceRelay();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void rfidTask(void *pvParameters)
{
  (void)pvParameters;

  for (;;)
  {
    if (!isRFIDConnected())
    {
      SPI.end();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      digitalWrite(RST_PIN, LOW);
      vTaskDelay(100 / portTICK_PERIOD_MS);
      digitalWrite(RST_PIN, HIGH);
      vTaskDelay(100 / portTICK_PERIOD_MS);
      Serial.println("[Warning] RFID unhealthy   attempting re-initialisation.");
      SPI.begin();
      rfid.PCD_Init();
      rfid.PCD_WriteRegister(MFRC522::FIFOLevelReg, 0x80);
      rfid.PCD_AntennaOff();
      vTaskDelay(100 / portTICK_PERIOD_MS);
      rfid.PCD_AntennaOn();
      rfid.PCD_SetAntennaGain(rfid.RxGain_max);
      vTaskDelay(500 / portTICK_PERIOD_MS);
      continue;
    }

    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    String uid = getCardUID();
    Serial.printf("[RFID] Card scanned: %s\n", uid.c_str());

    bool granted = checkCardAuth(uid);

    if (granted)
    {
      triggerOutput(RELAY_PIN, Config.cfg.doorLockDuration);
      Serial.println("[AUTH] Authorised   door unlocked.");
    }
    else
    {
      Serial.println("[AUTH] Unauthorised   access denied.");
    }

    rfid.PICC_HaltA();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ===========================================================================
// WiFi
// ===========================================================================

/**
 * connectWiFi()
 *
 * Retries indefinitely   never falls back to SoftAP.
 * A deauth attack causes the device to keep retrying silently.
 * The door fails CLOSED (relay stays LOW) while disconnected.
 */
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

  // Retry forever   no AP fallback (closes deauth attack vector).
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

/**
 * checkCardAuth()
 *
 * POSTs UID + access_point_id + api_key to the compile-time AUTH_SERVER_URL.
 * The server URL is hardcoded and cannot be changed at runtime.
 * The api_key is a shared secret the server validates.
 * Fails closed on any error.
 */
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

// ===========================================================================
// Relay   non-blocking
// ===========================================================================

/**
 * triggerOutput()
 *
 * Arms the relay and records the deadline. Returns immediately.
 * If the relay is already active the new trigger is ignored   the door
 * is already open and re-triggering mid-cycle would extend unpredictably.
 */
void triggerOutput(int32_t pin, uint32_t durationMs)
{
  portENTER_CRITICAL(&gRelayMux);

  if (gRelayActive)
  {
    portEXIT_CRITICAL(&gRelayMux);
    Serial.println("[Relay] Trigger ignored   relay already active.");
    return;
  }

  gRelayPin = pin;
  gRelayOffAt = millis() + durationMs;
  gRelayActive = true;
  digitalWrite((uint8_t)gRelayPin, HIGH);

  portEXIT_CRITICAL(&gRelayMux);
}

/**
 * serviceRelay()
 *
 * Called every loop iteration. Turns the relay off once the deadline passes.
 * Uses a rollover-safe signed comparison so it handles millis() wraparound
 * correctly at ~49 days of uptime.
 */
void serviceRelay()
{
  bool shouldTurnOff = false;
  int32_t pinToClear = -1;

  portENTER_CRITICAL(&gRelayMux);

  if (gRelayActive)
  {
    const uint32_t now = millis();

    // Rollover-safe deadline check:
    // (deadline - now) goes negative in signed space once the deadline passes.
    if ((int32_t)(gRelayOffAt - now) <= 0)
    {
      pinToClear = gRelayPin;
      gRelayActive = false;
      gRelayPin = -1;
      shouldTurnOff = true;
    }
  }

  portEXIT_CRITICAL(&gRelayMux);

  if (shouldTurnOff && pinToClear >= 0)
  {
    digitalWrite((uint8_t)pinToClear, LOW);
  }
}

// ===========================================================================
// RFID helpers
// ===========================================================================

bool isRFIDConnected()
{
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  return (version == 0x91 || version == 0x92);
}

String getCardUID()
{
  String uid = "";
  uid.reserve(rfid.uid.size * 2);
  for (byte i = 0; i < rfid.uid.size; i++)
  {
    if (rfid.uid.uidByte[i] < 0x10)
      uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}