# ESP32 Irrigation Controller

**Board:** 303E32DC812 (ESP32_Relay_X8_Modbus)  
**Framework:** Arduino / PlatformIO  
**8 zóna, szekvenciális vezérlés — egyszerre csak egy zóna aktív**

---

## Hardver

| Komponens | Leírás |
|---|---|
| Board | 303E32DC812 — ESP32 + 8 relé |
| Relé vezérlés | 74HC595 shift register |
| LATCH | GPIO 25 |
| CLOCK | GPIO 26 |
| DATA | GPIO 33 |
| OE | GPIO 13 |
| Digitális bemenetek | IN1–IN8, aktív-LOW, debounce szűréssel |
| IN1–IN8 GPIO | 36, 39, 34, 35, 4, 16, 17, 5 |

**Relé bekötés:**
```
Fázis (L) → COM
            NO  → terhelés (szóró, szelep stb.)
Terhelés másik vége → Nulla (N)
```

**Tápellátás:** 9–24V DC a board GND/VIN kapcsaira. Az USB csak az ESP32-t táplálja, a reléket nem.

---

## Első telepítés

```bash
# 1. Klónozd / másold a projektet
cp include/config.example.h include/config.h

# 2. Töltsd ki a config.h-t (WiFi, MQTT, OTA, AP adatok)

# 3. Hozd létre a secrets.ini-t (OTA jelszó — git-ignored)
echo "[secrets]"              > secrets.ini
echo "ota_password = jelszo" >> secrets.ini

# 4. Firmware feltöltése USB-n
pio run -t upload
```

`config.h` és `secrets.ini` git-ignored — soha nem kerülnek a repository-ba.

---

## Konfiguráció (`include/config.h`)

```cpp
// WiFi — fallback ha nincs NVS-ben tárolva
#define WIFI_SSID        "your_wifi_name"
#define WIFI_PASSWORD    "your_wifi_password"

// MQTT broker
#define MQTT_BROKER      "192.168.x.x"
#define MQTT_PORT        1883
#define MQTT_USER        "user"
#define MQTT_PASSWORD    "pass"
#define MQTT_CLIENT_ID   "irrigation-controller"
#define MQTT_ROOT        "irrigation/controller"

// NTP időzóna (másodpercben UTC-től)
// UTC+1 (téli): 3600 | UTC+2 (nyári): 7200
#define NTP_UTC_OFFSET   3600

// OTA frissítés
#define OTA_HOSTNAME  "irrigationcontroller"
#define OTA_USERNAME  "admin"
#define OTA_PASSWORD  "your_ota_password"

// Access Point
#define AP_SSID      "IrrigationController"
#define AP_PASSWORD  "your_ap_password"
#define AP_CHANNEL   1
#define AP_DEFAULT   false   // true = AP boot-kor automatikusan bekapcsol
```

---

## OTA jelszó — `secrets.ini`

Az OTA jelszó a `secrets.ini` fájlban tárolható, így nem kerül be a `platformio.ini`-be:

```ini
[secrets]
ota_password = your_ota_password
```

A `generate_html.py` automatikusan beolvassa ezt build előtt és átadja a PlatformIO upload folyamatának. Nem kell `upload_flags`-t írni a `platformio.ini`-be.

---

## Webes felület

| URL | Leírás |
|---|---|
| `http://<ip>/` | Dashboard — zóna állapotok, program státusz, mentett szekvencia, log |
| `http://<ip>/wifi` | WiFi beállítás (Basic Auth) |
| `http://<ip>/update` | OTA firmware feltöltés (Basic Auth) |

A WiFi beállítás NVS-be mentődik és felülírja a `config.h` alapértelmezést. Ha az új WiFi nem elérhető, a board AP módba vált.

---

## OTA frissítés

**WiFi-n (PlatformIO)** — a `platformio.ini` OTA módban:
```ini
upload_protocol = espota
upload_port     = irrigationcontroller.local
; upload_flags nem kell — generate_html.py olvassa be secrets.ini-ből
```

**Böngészőből:** `http://<ip>/update` → .bin fájl feltöltése  
A .bin fájl helye: `.pio/build/esp32dev/firmware.bin`

---

## MQTT API

**Root topic:** `irrigation/controller`

### Zóna vezérlés

| Topic | Payload | Leírás |
|---|---|---|
| `zone/N/set` | `ON` | Zóna indítása (alapértelmezett időtartam) |
| `zone/N/set` | `OFF` | Program leállítása |
| `zone/N/set` | `{"state":"ON","duration":5}` | Indítás megadott időtartammal (perc) |
| `zone/N/config` | `{"name":"Előkert","duration":10}` | Zóna neve és alapértelmezett időtartama |
| `zone/N/state` | `ON` / `OFF` *(retained)* | Zóna állapot visszajelzés |

### Program vezérlés

| Topic | Payload | Leírás |
|---|---|---|
| `program/start` | bármilyen | Mentett szekvencia indítása |
| `program/stop` | bármilyen | Azonnali leállítás |
| `program/set` | JSON tömb | Egyedi szekvencia azonnali futtatása (nem menti) |
| `program/running` | `ON` / `OFF` *(retained)* | Fut-e program |
| `program/status` | JSON | Program státusz (10mp-enként, lásd lent) |

**program/set payload:**
```json
[{"zone":1,"duration":10},{"zone":3,"duration":8},{"zone":5,"duration":15}]
```

**program/status — locsolás közben (`phase: watering`):**
```json
{
  "phase": "watering",
  "active_zone": 3,
  "step": 2,
  "total_steps": 4,
  "elapsed_sec": 120,
  "remaining_sec": 480,
  "step_duration_sec": 600
}
```

**program/status — tartálytöltés közben (`phase: filling`):**
```json
{
  "phase": "filling",
  "next_zone": 4,
  "step": 3,
  "total_steps": 4,
  "elapsed_sec": 25,
  "remaining_sec": 35,
  "delay_sec": 60
}
```

### Szekvencia

| Topic | Payload | Leírás |
|---|---|---|
| `sequence/set` | JSON tömb | Szekvencia mentése flash-be |
| `sequence/state` | JSON tömb *(retained)* | Mentett szekvencia visszajelzés |
| `sequence/delay/set` | szám (mp) | Zónák közötti tartálytöltési szünet |
| `sequence/delay/state` | szám *(retained)* | Beállított késleltetés visszajelzés |

**sequence/set payload:**
```json
[{"zone":1,"duration":20},{"zone":2,"duration":5},{"zone":3,"duration":5}]
```

**Tartálytöltési késleltetés:**
```
topic:   irrigation/controller/sequence/delay/set
payload: 60
```

A zónák közötti szünet alatt a dashboard `💧 TANK FILLING` állapotot jelenít meg visszaszámlálással. `0` = nincs késleltetés.

### Napi ütemezés

| Topic | Payload | Leírás |
|---|---|---|
| `schedule/set` | JSON | Ütemezés beállítása |

**schedule/set payload:**
```json
{"enabled":true,"days":"MTWTFSS","time":"06:00"}
```

- `days`: 7 karakter, Hétfő–Vasárnap, `-` = kihagyás (pl. `"MTWTF--"` = csak hétköznap)
- A mentett szekvenciát futtatja a beállított késleltetéssel
- Ha nincs szekvencia mentve, nem indul semmi

### Digitális bemenetek

| Topic | Payload | Leírás |
|---|---|---|
| `input/N/state` | `ON` / `OFF` | Bemenet állapotváltozáskor |

### Rendszer

| Topic | Payload | Leírás |
|---|---|---|
| `status` | `online` / `offline` / `rebooting` *(retained, LWT)* | Board elérhetőség |
| `ap/set` | `ON` / `OFF` | Access Point be/kikapcsolás |
| `ap/state` | `ON` / `OFF` *(retained)* | AP állapot |
| `reboot` | bármilyen | Távoli újraindítás |

> ⚠️ A `reboot` topicot **ne küldd retained-ként** — különben minden reconnect-kor újraindul a board.

> ⚠️ A `zone/+/set`, `program/+`, `ap/set` topicokat **ne küldd retained-ként** — a board connect-kor törli ezeket, de elővigyázatosságból kerüld.

---

## NVS perzisztencia

Reboot után automatikusan visszaállnak:

| Adat | NVS namespace | Kulcsok |
|---|---|---|
| WiFi SSID + jelszó | `wifi` | `ssid`, `pass` |
| Mentett szekvencia | `seq` | `count`, `z0`…`z7`, `d0`…`d7` |
| Zónák közötti késleltetés | `seq` | `delay` |
| Napi ütemezés | `sched` | `enabled`, `hour`, `minute`, `days` |

---

## Access Point

Bekapcsolható MQTT-n (`ap/set → ON`) vagy a dashboard fejlécéből.  
AP IP: `192.168.4.1` — a dashboard és WiFi beállítás itt is elérhető.

Ha a fő WiFi nem elérhető és `AP_DEFAULT = true`, boot-kor automatikusan elindul.  
WiFi reconnect után az AP automatikusan visszaáll ha be volt kapcsolva.

---

## Fájlstruktúra

```
├── platformio.ini
├── generate_html.py       # 1. HTML → gzip PROGMEM  2. OTA jelszó betöltés secrets.ini-ből
├── secrets.ini            # ← git-ignored, OTA jelszó
├── data/
│   └── index.html         # Dashboard forrás — itt szerkeszd
├── include/
│   ├── config.h           # ← git-ignored, saját adatok
│   ├── config.example.h   # sablon, commitolható
│   ├── web_ui.h
│   └── web_pages.h
└── src/
    ├── main.cpp           # WiFi, MQTT, relék, szekvenszer, ütemező, NVS
    ├── web_ui.cpp         # Web szerver routes, OTA handler, REST API
    └── web_pages.cpp      # HTML tartalom (auto-generált gzip PROGMEM)
```

### Weboldal szerkesztése

1. Szerkeszd a `data/index.html`-t
2. `pio run -t upload` — a `generate_html.py` automatikusan tömöríti és befordítja

Nincs szükség külön `uploadfs`-re — minden a firmware-be kerül.

---

## Node-RED flow

A `nodered_irrigation_flow.json` importálható Node-RED-be:  
**Hamburger menü → Import → JSON beillesztése**

Tartalmaz inject node-okat minden MQTT parancshoz (zóna, program, szekvencia, késleltetés, ütemezés, AP, reboot), debug és function node-okat.

**Első beállítás:** az `mqtt-broker` config node-ban állítsd be a broker IP-t és hitelesítő adatokat.

---

## Tipikus hibák

| Hiba | Megoldás |
|---|---|
| `No section: 'secrets'` | Hozd létre a `secrets.ini` fájlt a projekt gyökerében |
| OTA 31%-nál megakad | WiFi jel gyenge — menj közelebb a routerhez |
| COM port nem található | Driver telepítés (CP2102 vagy CH340), vagy csak töltő USB kábel |
| Dupla MQTT üzenet | A `set` topicokat ne küldd `retain: true`-val |
| Reboot loop | A `reboot` topicot valaki retained-ként küldte — töröld: `mosquitto_pub -t "irrigation/controller/reboot" -r -n` |
