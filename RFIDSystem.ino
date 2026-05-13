#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string>
#include <constants.h>

MFRC522 rfid(SS_PIN, RST_PIN);

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
bool checkCardAuth(const String& uid);
void triggerOutput(int pin, int duration);
bool isRFIDConnected();
String getCardUID();

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    SPI.begin();
    rfid.PCD_Init();

    pinMode(RST_PIN, OUTPUT);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    // Connect to WiFi
    Serial.printf("[WiFi] Connecting to %s", ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
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
        triggerOutput(RELAY_PIN, DOOR_LOCK_DURATION);
        Serial.println("[AUTH] Authorised — door unlocked.");
    } else {
        Serial.println("[AUTH] Unauthorised — access denied.");
    }

    rfid.PICC_HaltA();
    // rfid.PCD_StopCrypto1();
}

// ==========================================================================================
// ===================================== Auth Functions =====================================
// ==========================================================================================

/**
 * checkCardAuth()
 *
 * POSTs UID to Django, and returns true if
 * the server responds with {"granted": true}. Fails closed on any error.
 *
 * @param uid  UID string from getCardUID(), e.g. "53E40031"
 * @return     true = access granted, false = denied or error
 */
bool checkCardAuth(const String& uid) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[AUTH] WiFi not connected — denying by default.");
        return false;
    }

    // Build payload: {"card_uid": "53E40031", "access_point_id": 1}
    StaticJsonDocument<128> reqDoc;
    reqDoc["card_uid"]        = uid;
    reqDoc["access_point_id"] = ACCESS_POINT_ID;

    String reqBody;
    serializeJson(reqDoc, reqBody);

    HTTPClient http;
    http.begin(AUTH_SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(AUTH_TIMEOUT_MS);

    int httpCode = http.POST(reqBody);

    if (httpCode <= 0) {
        Serial.printf("[AUTH] Request failed: %s\n", http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String resBody = http.getString();
    http.end();

    Serial.printf("[AUTH] HTTP %d — %s\n", httpCode, resBody.c_str());

    // Parse: {"granted": true/false, "reason": "SUCCESS", "user": "James Banda"}
    StaticJsonDocument<256> resDoc;
    DeserializationError err = deserializeJson(resDoc, resBody);

    if (err) {
        Serial.printf("[AUTH] JSON parse error: %s\n", err.c_str());
        return false;
    }

    bool   granted = resDoc["granted"] | false;
    String reason  = resDoc["reason"]  | "UNKNOWN";

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
    uid.reserve(rfid.uid.size * 2); // pre-allocate exact size, avoids heap reallocs
    for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    return uid;
}

// ==========================================================================================
// ===================================== WiFi Functions =====================================
// ==========================================================================================
