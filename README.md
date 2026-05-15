# Smart Access Control System

A complete ESP32-based RFID access control solution with web-based configuration portal and remote authentication.

## Features

- **RFID Card Reader**: MFRC522-based card scanning and UID extraction
- **Remote Authentication**: POSTs card UIDs to a configurable auth server for validation
- **Web Configuration Portal**: 
  - Responsive Bootstrap UI with light/dark theme support
  - WiFi network scanning and selection
  - Real-time signal strength visualization
  - Non-volatile storage (NVS) for persistent configuration
- **Dual Operating Modes**:
  - **LAN Mode** (primary): Non-blocking HTTP server for configuration on connected network
  - **SoftAP Mode** (fallback): Captive portal if WiFi connection fails or boot button is held
- **Relay Control**: Configurable door unlock duration with dynamic trigger output
- **Fault Tolerance**: Automatic RFID module health checks and re-initialization
- **Security**: Fails closed on auth failures; password never transmitted back to browser

## Hardware Requirements

- ESP32 microcontroller
- MFRC522 RFID reader module
- 5V relay module
- SPDT door lock actuator or solenoid
- Push button (BOOT/FLASH pin for manual portal entry)

### Pin Configuration

| Component | Pin | Mode |
|-----------|-----|------|
| RFID SS   | 21  | SPI  |
| RFID RST  | 22  | GPIO |
| Relay     | 2   | GPIO |
| Config    | 0   | GPIO (INPUT_PULLUP) |

## Software Architecture

### Core Modules

- **`RFIDSystem.ino`**: Main sketch with setup/loop and WiFi/auth orchestration
- **`config.h`**: Configuration management with NVS persistence ([`ConfigManager`](config.h))
- **`portal.h`**: Web server implementation with captive portal ([`ConfigPortal`](portal.h))
- **`data/`**: Frontend assets (HTML, CSS, JavaScript)

### Configuration Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/config` | Fetch current device config (form pre-fill) |
| GET | `/scan` | Scan available WiFi networks (JSON array) |
| POST | `/save` | Save config and reboot |
| POST | `/reset` | Factory reset and reboot |

## Getting Started

### 1. Upload Firmware

Compile and upload to ESP32 using Arduino IDE or PlatformIO.

### 2. Upload Filesystem

Upload the `data/` folder to the device's LittleFS partition using the Arduino IDE LittleFS upload tool.

### 3. Configure Access

#### Option A: Automatic WiFi Connection
If SSID and password are set, the device connects automatically on boot.

#### Option B: Manual Setup Portal
- Hold the **BOOT/FLASH** button on power-up to force SoftAP mode
- Connect to `AccessCtrl-Setup` WiFi network
- Navigate to `http://192.168.4.1/` or `http://accesscontrol.local/`

#### Option C: LAN Configuration
Once connected to WiFi, visit `http://accesscontrol.local/` to reconfigure.

## Configuration Parameters

| Parameter | Type | Default | Notes |
|-----------|------|---------|-------|
| **SSID** | string (63 char) | "iPhone" | WiFi network name |
| **Password** | string (63 char) | "87654321" | WiFi password |
| **Server URL** | string (127 char) | `http://nixos.local:8000/api/v1/auth/check/` | Auth endpoint |
| **Access Point ID** | uint16 | 2 | Sent with each auth request |
| **Auth Timeout** | uint32 (ms) | 5000 | HTTP request timeout |
| **Door Lock Duration** | uint32 (ms) | 2000 | Relay activation time |

## Authentication Flow

```
Card Scanned (UID extracted)
    ↓
[checkCardAuth]
    ↓
POST /api/v1/auth/check/ (JSON)
{
  "card_uid": "1A2B3C4D",
  "access_point_id": 2
}
    ↓
Server Response (JSON)
{
  "granted": true,
  "user": "John Doe",
  "reason": "Valid card"
}
    ↓
granted ? triggerOutput(RELAY, duration) : deny
```

## Web UI Features

### WiFi Network Selection
- Real-time scanning with signal strength indicators (dBm, channel)
- Color-coded signal quality (green → red)
- Lock/unlock icons for encryption status
- Click to auto-fill SSID field

### Dark/Light Theme
- Respects system preference on first visit
- Toggle via navbar button
- Bootstrap dark mode integration

### Factory Reset
- One-click configuration reset to defaults
- Triggers device reboot

### Status Indicators
- Connected/Setup Mode badge in navbar
- Scan status messages and error handling

## Dependencies

```cpp
#include <SPI.h>              // SPI communication
#include <MFRC522.h>          // RFID reader library
#include <WiFi.h>             // WiFi stack
#include <HTTPClient.h>       // HTTP requests
#include <ArduinoJson.h>      // JSON serialization
#include <WebServer.h>        // HTTP server
#include <DNSServer.h>        // Captive portal DNS
#include <ESPmDNS.h>          // mDNS/Bonjour
#include <LittleFS.h>         // Filesystem (data/)
#include <Preferences.h>      // NVS key-value storage
```

## API Reference

### GET `/config`

**Response (200 OK):**
```json
{
  "ssid": "iPhone",
  "serverUrl": "http://nixos.local:8000/api/v1/auth/check/",
  "apId": 2,
  "authTimeout": 5000,
  "lockDuration": 2000,
  "mode": "lan"
}
```

### GET `/scan`

**Response (200 OK):**
```json
[
  {
    "ssid": "MyNetwork",
    "rssi": -45,
    "channel": 6,
    "secure": true
  },
  {
    "ssid": "WeakSignal",
    "rssi": -85,
    "channel": 11,
    "secure": false
  }
]
```

*Sorted descending by signal strength (RSSI).*

### POST `/save`

**Request Body:**
```json
{
  "ssid": "iPhone",
  "password": "newpass123",
  "serverUrl": "http://auth.example.com/api/check/",
  "apId": 2,
  "authTimeout": 5000,
  "lockDuration": 2000
}
```

**Response (200 OK):**
```json
{ "ok": true }
```

*Device reboots after 800ms.*

### POST `/reset`

**Response (200 OK):**
```json
{ "ok": true }
```

*Clears all NVS values and reboots.*

## Serial Debugging

Monitor serial output at **115200 baud** for detailed logs:

```
[Config] Loaded from NVS.
[WiFi] Connecting to "iPhone"....
[WiFi] Connected — IP: 192.168.1.42
[Portal] LAN server up
[Portal] IP URL        : http://192.168.1.42/
[Portal] Non IP URL    : http://accesscontrol.local/

[RFID] Card scanned: 1A2B3C4D
[AUTH] HTTP 200 — {"granted":true,"user":"John Doe","reason":"Valid card"}
[AUTH] GRANTED — John Doe (Valid card)
[AUTH] Authorised — door unlocked.
```

## Security Considerations

- ✅ WiFi password never returned to browser
- ✅ Auth failures default to **deny** (fail closed)
- ✅ HTTP timeout prevents hanging on bad auth server
- ✅ NVS encryption available (not currently enabled)
- ⚠️ Authentication uses HTTP — use HTTPS on local network or add certificate pinning for production

## Troubleshooting

| Issue | Solution |
|-------|----------|
| LittleFS mount failed | Ensure `data/` folder uploaded via Arduino IDE LittleFS tool |
| WiFi connection fails | Check SSID/password in `/config` endpoint; enter setup portal |
| RFID not detected | Power-cycle device; check SPI pin connections |
| Relay always on | Verify `RELAY_PIN` (GPIO 2) is set LOW in setup |
| Cannot reach `accesscontrol.local` | Install mDNS browser or use IP address directly |

## Future Enhancements

- [ ] HTTPS/mDNS certificate support
- [ ] User management dashboard
- [ ] Local card whitelist with cloud sync
- [ ] Event logging and statistics
- [ ] Multi-card support and expiration dates
- [ ] Buzzer/LED feedback for auth results

## License

MIT

## Author

Rhodwell — Smart Access Control System