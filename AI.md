# AI.md

This file provides guidance to AI assistants (Claude, Codex, Cursor, Copilot, etc.) when working with code in this repository.

## Project Overview

SoundPanel 7 is an ESP32-S3 connected wall panel for sound monitoring, NTP clock display, and home automation integration (MQTT, Home Assistant). Built with Arduino framework and PlatformIO.

## Build Commands

### Compilation
```bash
pio run                              # Default env: soundpanel7_usb
pio run -e soundpanel7_usb           # Waveshare 7" with screen
pio run -e soundpanel7_headless_usb  # ESP32-S3 without screen
```

### Flashing
```bash
# USB
pio run -e soundpanel7_usb -t upload
pio run -e soundpanel7_headless_usb -t upload

# OTA (port 3232, password: SoundPanel7)
pio run -e soundpanel7_ota -t upload --upload-port 192.168.X.X
pio run -e soundpanel7_headless_ota -t upload --upload-port 192.168.X.X
```

### Monitoring
```bash
pio device monitor -b 115200
```

## Local Configuration

**Never commit local values to `platformio.ini`**. Use `platformio.override.ini` (gitignored) for:
- USB/serial ports
- OTA IP addresses
- Machine-specific settings

Example `platformio.override.ini`:
```ini
[env:soundpanel7_ota]
upload_port = 192.168.X.X
```

## Core Architecture

### Dual Core Design
```
┌─────────────────────────────────────────────────┐
│ ESP32-S3 Dual Core Architecture                 │
├─────────────────────┬───────────────────────────┤
│ CORE 0 (loop)       │ CORE 1 (audioTask)        │
├─────────────────────┼───────────────────────────┤
│ • WiFi/NetManager   │ • AudioEngine.update()    │
│ • MQTT client       │ • I2S/ADC acquisition     │
│ • Web server/SSE    │ • dB/RMS/Peak calc        │
│ • UI LVGL (screen)  │ • SharedHistory.update()  │
│ • OTA manager       │                           │
│                     │ Priority: 5 (HIGH)        │
│ Priority: 1         │ Period: 80ms fixed        │
└─────────────────────┴───────────────────────────┘
```

### Data Flow
```
AudioEngine (Core 1) → Mutex → SharedHistory → UI/Web/MQTT (Core 0)
```

The firmware follows a strict boot sequence:
1. Settings storage (NVS)
2. Display/touch (if `SOUNDPANEL7_HAS_SCREEN=1`)
3. Network (Wi-Fi)
4. OTA
5. MQTT
6. Web server
7. Audio engine
8. Audio FreeRTOS task (Core 1)

### Key Components

- **AudioEngine**: Audio acquisition with 4 sources (Demo, Analog, PDM MEMS, INMP441). Computes dB instant, Leq, Peak. Manages calibration per source profile (3 profiles stored in SettingsV1).
- **SharedHistory**: Circular buffer managing 96 audio measurement points with dynamic sample period based on `historyMinutes` setting.
- **SettingsStore**: NVS persistence with magic number `0x53503730` and version `8`. Handles config export/import/backup/restore.
- **WebManager**: AsyncWebServer with REST API, SSE live stream on port 81, web authentication (bootstrap + multi-user).
- **MqttManager**: MQTT publishing + Discovery for Home Assistant auto-configuration.
- **OtaManager**: Network OTA via espota (port 3232).
- **ReleaseUpdateManager**: GitHub release OTA with SHA-256 verification.

### Conditional Compilation

Two hardware profiles:
- **Waveshare** (`SOUNDPANEL7_HAS_SCREEN=1`, `SOUNDPANEL7_AUDIO_BOARD_PROFILE=1`): 7" touchscreen, LVGL UI, local PIN protection, GPIO6 analog, GPIO11/12/13 digital
- **Headless** (`SOUNDPANEL7_HAS_SCREEN=0`, `SOUNDPANEL7_AUDIO_BOARD_PROFILE=2`): No screen, web-only, GPIO4 analog, GPIO11/12/13 digital, optional RGB LED for TARDIS mode

Audio mock mode enabled by default: `-DSOUNDPANEL7_MOCK_AUDIO=1` in `platformio.ini` line 50.

## Audio System

4 sources (enum `AudioSource` in `AudioEngine.h`):
- Demo (0): Simulated values
- SensorAnalog (1): Analog mic (MAX4466), uses GPIO6 (Waveshare) or GPIO4 (headless)
- PdmMems (2): PDM digital mic
- Inmp441 (3): I2S digital mic

Calibration system: 3 or 5 points per source profile. Points stored in `SettingsV1::calibrationPoints[CALIBRATION_PROFILE_COUNT][CALIBRATION_POINT_MAX]`. Typical 3-point values: 45, 65, 85 dB.

### Audio Task Architecture (Dual Core)

Audio acquisition runs on **dedicated FreeRTOS task** pinned to **Core 1** with high priority (5):
- Isolated from WiFi/MQTT/Web (Core 0)
- Fixed 80ms measurement period
- Thread-safe metrics sharing via mutex
- Guarantees reliable measurements even during network activity

### ⚠️ CRITICAL: Analog Mic Hardware Limitation

**WiFi AP scan blocks analog ADC on ESP32-S3** due to shared hardware resources. During WiFi scanning operations (AP portal mode, reconnection attempts, channel scan), analog audio acquisition freezes for 2-4 seconds.

**This is a hardware constraint, not a software bug.**

**Impact:**
- SensorAnalog (1): Affected - measurements freeze during WiFi scan
- PdmMems (2): NOT affected - I2S operates independently
- Inmp441 (3): NOT affected - I2S operates independently

**Mitigation in firmware:**
- NetManager stops WiFi scanning after 3 failed attempts when portal is active
- Audio task runs on isolated Core 1 with high priority
- However, Core 1 isolation **cannot override hardware ADC conflicts**

**For production sound measurement instruments: always use digital audio sources (PDM MEMS or INMP441) to guarantee uninterrupted measurements.**

## Web API

Interface: `http://soundpanel7.local/` or `http://IP/`
Live SSE: `http://IP:81/api/events`

Critical endpoints:
```
GET  /api/status          # Full system state
POST /api/reboot
POST /api/shutdown
POST /api/factory_reset
GET  /api/config/export   # JSON export
POST /api/config/import
POST /api/config/backup   # Local NVS backup
POST /api/config/restore
GET  /api/release
POST /api/release/check   # GitHub release check
POST /api/release/install # OTA from GitHub
POST /api/calibrate
```

## Development Workflow

1. **USB flash and validate**: `pio run -e soundpanel7_usb -t upload && pio device monitor -b 115200`
2. **Test on device**: Verify display (if applicable), web UI, and modified features
3. **OTA deployment**: `pio run -e soundpanel7_ota -t upload` (ensure same network)

## Home Assistant Integration

Two methods:
1. **MQTT Discovery**: Enable MQTT, entities auto-created
2. **Custom integration**: `custom_components/soundpanel7/`, Zeroconf service `_soundpanel7._tcp.local.`, requires HA token configured in panel
