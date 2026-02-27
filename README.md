# ESP32_Relay_X8_Modbus — WiFi/MQTT Firmware
**Board:** 303E32DC812  
**Framework:** Arduino / PlatformIO

## Setup

```bash
git clone <your-repo>
cd <your-repo>
cp include/config.example.h include/config.h
# Edit include/config.h with your WiFi and MQTT credentials
```

Then open in VS Code with PlatformIO and click **Upload**.

## Configuration

All secrets live in `include/config.h` which is **git-ignored**.  
Never edit `config.example.h` with real values — it's the public template.

| Setting | Description |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Your WiFi network |
| `MQTT_BROKER` | IP or hostname of your MQTT broker |
| `MQTT_USER` / `MQTT_PASSWORD` | MQTT credentials (leave `""` if none) |
| `NTP_UTC_OFFSET` | Seconds from UTC — Hungary winter: `3600`, summer: `7200` |

## MQTT API

| Topic | Payload | Direction |
|---|---|---|
| `home/relayboard/relay/<1-8>/set` | `ON` / `OFF` / `TOGGLE` or `{"state":"ON","pulse_ms":500}` | → board |
| `home/relayboard/relay/all/set` | `ON` / `OFF` | → board |
| `home/relayboard/relay/<1-8>/state` | `ON` / `OFF` | ← board |
| `home/relayboard/input/<1-8>/state` | `ON` / `OFF` | ← board |
| `home/relayboard/schedule/set` | JSON array | → board |
| `home/relayboard/schedule/clear` | any | → board |
| `home/relayboard/status` | `online` / `offline` (LWT) | ← board |

## Schedule JSON example

```json
[
  { "relay": 1, "days": "MTWTF--", "time": "07:00", "action": "ON" },
  { "relay": 1, "days": "MTWTF--", "time": "18:00", "action": "OFF" },
  { "relay": 3, "days": "-------", "time": "12:00", "action": "TOGGLE", "pulse_ms": 5000 }
]
```

`days` = 7 chars Mon–Sun, use any char to enable, `-` to skip.

## Hardware

Board uses a **74HC595 shift register** (confirmed by pin scanner):

| Signal | GPIO |
|---|---|
| LATCH | 25 |
| CLOCK | 26 |
| DATA  | 33 |
| OE    | 13 |
