#pragma once

#include <Preferences.h>
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Hardware pins — physically wired, never change at runtime
// ---------------------------------------------------------------------------
#define SS_PIN 21
#define RST_PIN 22
#define RELAY_PIN 2
#define CONFIG_BUTTON_PIN 0 // BOOT button — must be held during power-on to enter portal

// ---------------------------------------------------------------------------
// Auth server URL — compile-time constant, intentionally NOT in the UI
//
// Locking this at compile time closes the deauth → rogue-server attack:
// even if someone accesses the portal they cannot redirect auth requests.
// To change the URL, you must reflash the firmware — which requires
// physical access to the device.
// ---------------------------------------------------------------------------
#define AUTH_SERVER_URL "http://nixos.local:8000/api/v1/auth/check/"

// ---------------------------------------------------------------------------
// NVS namespace
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE "access_ctrl"

// ---------------------------------------------------------------------------
// Config struct — only runtime-safe values live here
// ---------------------------------------------------------------------------
struct DeviceConfig
{
  // WiFi
  char ssid[64];
  char password[64];

  // Server
  uint16_t accessPointId;
  uint32_t authTimeoutMs;

  // Behaviour
  uint32_t doorLockDuration; // ms the relay stays HIGH

  // Security
  char portalUser[32];     // Login username for the config portal
  char portalPassword[32]; // Login password for the config portal
  char apiKey[64];         // Shared secret sent in every auth POST request
};

// ---------------------------------------------------------------------------
// Defaults — first boot or after factory reset
// ---------------------------------------------------------------------------
static const DeviceConfig DEFAULT_CONFIG = {
    .ssid = "",
    .password = "",
    .accessPointId = 1,
    .authTimeoutMs = 5000,
    .doorLockDuration = 2000,

    // IMPORTANT: change all three of these before flashing to production
    .portalUser = "admin",
    .portalPassword = "admin",
    .apiKey = "changeme-api-key-replace-before-flash",
};

// ---------------------------------------------------------------------------
// Config manager
// ---------------------------------------------------------------------------
class ConfigManager
{
public:
  DeviceConfig cfg;

  // Returns true if the device has been configured (has a saved SSID)
  bool isProvisioned() const
  {
    return strlen(cfg.ssid) > 0;
  }

  void load()
  {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);

    _readStr(prefs, "ssid", cfg.ssid, DEFAULT_CONFIG.ssid, sizeof(cfg.ssid));
    _readStr(prefs, "password", cfg.password, DEFAULT_CONFIG.password, sizeof(cfg.password));
    _readStr(prefs, "portalUser", cfg.portalUser, DEFAULT_CONFIG.portalUser, sizeof(cfg.portalUser));
    _readStr(prefs, "portalPassword", cfg.portalPassword, DEFAULT_CONFIG.portalPassword, sizeof(cfg.portalPassword));
    _readStr(prefs, "apiKey", cfg.apiKey, DEFAULT_CONFIG.apiKey, sizeof(cfg.apiKey));

    cfg.accessPointId = prefs.getUShort("apId", DEFAULT_CONFIG.accessPointId);
    cfg.authTimeoutMs = prefs.getULong("authTimeout", DEFAULT_CONFIG.authTimeoutMs);
    cfg.doorLockDuration = prefs.getULong("lockDuration", DEFAULT_CONFIG.doorLockDuration);

    prefs.end();
    Serial.println("[Config] Loaded from NVS.");
  }

  void save()
  {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);

    prefs.putString("ssid", cfg.ssid);
    prefs.putString("password", cfg.password);
    prefs.putString("portalUser", cfg.portalUser);
    prefs.putString("portalPassword", cfg.portalPassword);
    prefs.putString("apiKey", cfg.apiKey);
    prefs.putUShort("apId", cfg.accessPointId);
    prefs.putULong("authTimeout", cfg.authTimeoutMs);
    prefs.putULong("lockDuration", cfg.doorLockDuration);

    prefs.end();
    Serial.println("[Config] Saved to NVS.");
  }

  void factoryReset()
  {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    Serial.println("[Config] Factory reset — NVS cleared.");
  }

private:
  void _readStr(Preferences &p, const char *key, char *dest,
                const char *fallback, size_t maxLen)
  {
    String val = p.isKey(key) ? p.getString(key, fallback) : String(fallback);
    strncpy(dest, val.c_str(), maxLen - 1);
    dest[maxLen - 1] = '\0';
  }
};

extern ConfigManager Config;