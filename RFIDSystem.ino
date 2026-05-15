#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "portal.h"

// ---------------------------------------------------------------------------
// Global instances (definitions — declared extern in their headers)
// ---------------------------------------------------------------------------
ConfigManager Config;
ConfigPortal Portal;

MFRC522 rfid(SS_PIN, RST_PIN);

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
bool checkCardAuth(const String& uid);
void triggerOutput(int pin, int duration);
bool isRFIDConnected();
String getCardUID();
void connectWiFi();

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  // ── 1. Load persisted config from NVS ──────────────────────────────────
  Config.load();

  // ── 2. Check if user wants to force config portal (hold BOOT button) ───
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    Serial.println("[Setup] Config button held — entering setup portal.");
    Portal.startAP();  // blocks until saved + reboots
  }

  // ── 3. Hardware init ───────────────────────────────────────────────────
  SPI.begin();
  rfid.PCD_Init();

  pinMode(RST_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // ── 4. WiFi ────────────────────────────────────────────────────────────
  connectWiFi();  // falls through to AP portal if connection fails

  // ── 5. Start LAN config server (non-blocking) ─────────────────────────
  Portal.startLAN();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  // Let the LAN config portal handle any incoming HTTP requests
  Portal.handleClient();

  // Check RFID health and attempt re-initialisation if unhealthy
  if (!isRFIDConnected()) {
    SPI.end();
    delay(100);
    digitalWrite(RST_PIN, LOW);
    delay(100);
    digitalWrite(RST_PIN, HIGH);
    delay(100);
    Serial.println("[Warning] RFID unhealthy — attempting re-initialisation.");
    SPI.begin();
    rfid.PCD_Init();
    rfid.PCD_WriteRegister(MFRC522::FIFOLevelReg, 0x80);
    rfid.PCD_AntennaOff();
    delay(100);
    rfid.PCD_AntennaOn();
    rfid.PCD_SetAntennaGain(rfid.RxGain_max);
    delay(500);
    return;
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }

  String uid = getCardUID();
  Serial.printf("[RFID] Card scanned: %s\n", uid.c_str());

  bool granted = checkCardAuth(uid);

  if (granted) {
    triggerOutput(RELAY_PIN, Config.cfg.doorLockDuration);  // ← dynamic
    Serial.println("[AUTH] Authorised — door unlocked.");
  } else {
    Serial.println("[AUTH] Unauthorised — access denied.");
  }

  rfid.PICC_HaltA();
}

// ==========================================================================================
// ===================================== WiFi ===============================================
// ==========================================================================================

/**
 * connectWiFi()
 *
 * Tries to connect using stored credentials. If it can't connect within 20 s
 * it falls back to the SoftAP portal so the user can enter new credentials.
 */
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to \"%s\"", Config.cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(Config.cfg.ssid, Config.cfg.password);

  const uint8_t MAX_ATTEMPTS = 40;  // 40 × 500 ms = 20 s
  uint8_t attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < MAX_ATTEMPTS) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Connection failed — launching setup portal.");
    Portal.startAP();  // blocks until saved + reboots
  }
}

// ==========================================================================================
// ===================================== Auth Functions =====================================
// ==========================================================================================

/**
 * checkCardAuth()
 *
 * POSTs UID to the configured server URL, returns true if
 * the server responds with {"granted": true}. Fails closed on any error.
 */
bool checkCardAuth(const String& uid) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AUTH] WiFi not connected — denying by default.");
    return false;
  }

  StaticJsonDocument<128> reqDoc;
  reqDoc["card_uid"] = uid;
  reqDoc["access_point_id"] = Config.cfg.accessPointId;  // ← dynamic

  String reqBody;
  serializeJson(reqDoc, reqBody);

  HTTPClient http;
  http.begin(Config.cfg.authServerUrl);  // ← dynamic
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(Config.cfg.authTimeoutMs);  // ← dynamic

  int httpCode = http.POST(reqBody);

  if (httpCode <= 0) {
    Serial.printf("[AUTH] Request failed: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }

  String resBody = http.getString();
  http.end();

  Serial.printf("[AUTH] HTTP %d — %s\n", httpCode, resBody.c_str());

  StaticJsonDocument<256> resDoc;
  DeserializationError err = deserializeJson(resDoc, resBody);

  if (err) {
    Serial.printf("[AUTH] JSON parse error: %s\n", err.c_str());
    return false;
  }

  bool granted = resDoc["granted"] | false;
  String reason = resDoc["reason"] | "UNKNOWN";

  if (granted) {
    String user = resDoc["user"] | "Unknown";
    Serial.printf("[AUTH] GRANTED — %s (%s)\n", user.c_str(), reason.c_str());
  } else {
    Serial.printf("[AUTH] DENIED  — %s\n", reason.c_str());
  }

  return granted;
}

// ==========================================================================================
// ===================================== RFID Functions =====================================
// ==========================================================================================

void triggerOutput(int pin, int duration) {
  digitalWrite(pin, HIGH);
  delay(duration);
  digitalWrite(pin, LOW);
}

bool isRFIDConnected() {
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  return (version == 0x91 || version == 0x92);
}

String getCardUID() {
  String uid = "";
  uid.reserve(rfid.uid.size * 2);
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}
