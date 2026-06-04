#include "rfid.h"
#include <SPI.h>
#include <MFRC522.h>
#include "config.h"

static MFRC522 rfid(SS_PIN, RST_PIN);

static bool isRFIDConnected() {
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  return (version == 0x91 || version == 0x92);
}

static void recoverRFID() {
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
}

static String getCardUID() {
  String uid;
  uid.reserve(rfid.uid.size * 2);

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10)
      uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();
  return uid;
}

static bool waitForCard(String &uid) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial())
    return false;

  uid = getCardUID();
  return true;
}

static void handleScannedCard(const String &uid) {
  Serial.printf("[RFID] Card scanned: %s\n", uid.c_str());

  bool granted = checkCardAuth(uid);
  if (granted) {
    triggerOutput(RELAY_PIN, Config.cfg.doorLockDuration);
    Serial.println("[AUTH] Authorised   door unlocked.");
  } else {
    Serial.println("[AUTH] Unauthorised   access denied.");
  }
}

void rfidTask(void *pvParameters) {
  (void)pvParameters;

  for (;;) {
    if (!isRFIDConnected()) {
      recoverRFID();
      continue;
    }

    String uid;
    if (!waitForCard(uid)) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    handleScannedCard(uid);
    rfid.PICC_HaltA();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void initRFID() {
  SPI.begin();
  rfid.PCD_Init();
  pinMode(RST_PIN, OUTPUT);
}

void startRFIDTask() {
  xTaskCreate(rfidTask, "RFIDTask", 4096, nullptr, 1, nullptr);
}