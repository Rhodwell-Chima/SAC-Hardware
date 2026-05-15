#pragma once

#include <Preferences.h>
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Hardware pin definitions — these are physically wired, never change at runtime
// ---------------------------------------------------------------------------
#define SS_PIN 21
#define RST_PIN 22
#define RELAY_PIN 2
#define CONFIG_BUTTON_PIN 0  // Boot/flash button — hold on power-up to enter portal

// ---------------------------------------------------------------------------
// NVS namespace
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE "access_ctrl"

// ---------------------------------------------------------------------------
// Config struct — holds all runtime-configurable values
// ---------------------------------------------------------------------------
struct DeviceConfig {
  // WiFi
  char ssid[64];
  char password[64];

  // Server
  char authServerUrl[128];
  uint16_t accessPointId;
  uint32_t authTimeoutMs;

  // Behaviour
  uint32_t doorLockDuration;  // ms the relay stays HIGH
};

// ---------------------------------------------------------------------------
// Default values — used on first boot or after a factory reset
// ---------------------------------------------------------------------------
static const DeviceConfig DEFAULT_CONFIG = {
  .ssid = "iPhone",
  .password = "87654321",
  .authServerUrl = "http://nixos.local:8000/api/v1/auth/check/",
  .accessPointId = 2,
  .authTimeoutMs = 5000,
  .doorLockDuration = 2000,
};

// ---------------------------------------------------------------------------
// Config manager
// ---------------------------------------------------------------------------
class ConfigManager {
public:
  DeviceConfig cfg;

  // Load from NVS; populate with defaults for any missing key
  void load() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);  // read-only

    _readStr(prefs, "ssid", cfg.ssid, DEFAULT_CONFIG.ssid, sizeof(cfg.ssid));
    _readStr(prefs, "password", cfg.password, DEFAULT_CONFIG.password, sizeof(cfg.password));
    _readStr(prefs, "serverUrl", cfg.authServerUrl, DEFAULT_CONFIG.authServerUrl, sizeof(cfg.authServerUrl));

    cfg.accessPointId = prefs.getUShort("apId", DEFAULT_CONFIG.accessPointId);
    cfg.authTimeoutMs = prefs.getULong("authTimeout", DEFAULT_CONFIG.authTimeoutMs);
    cfg.doorLockDuration = prefs.getULong("lockDuration", DEFAULT_CONFIG.doorLockDuration);

    prefs.end();
    Serial.println("[Config] Loaded from NVS.");
  }

  // Persist current cfg values to NVS
  void save() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);  // read-write

    prefs.putString("ssid", cfg.ssid);
    prefs.putString("password", cfg.password);
    prefs.putString("serverUrl", cfg.authServerUrl);
    prefs.putUShort("apId", cfg.accessPointId);
    prefs.putULong("authTimeout", cfg.authTimeoutMs);
    prefs.putULong("lockDuration", cfg.doorLockDuration);

    prefs.end();
    Serial.println("[Config] Saved to NVS.");
  }

  // Erase all keys in the namespace → next boot uses defaults
  void factoryReset() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    Serial.println("[Config] Factory reset — NVS cleared.");
  }

private:
  void _readStr(Preferences& p, const char* key, char* dest, const char* fallback, size_t maxLen) {
    if (p.isKey(key)) {
      String val = p.getString(key, fallback);
      strncpy(dest, val.c_str(), maxLen - 1);
      dest[maxLen - 1] = '\0';
    } else {
      strncpy(dest, fallback, maxLen - 1);
      dest[maxLen - 1] = '\0';
    }
  }
};

// Global instance — accessible from all translation units
extern ConfigManager Config;