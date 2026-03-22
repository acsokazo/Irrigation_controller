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

# 3. Firmware feltöltése USB-n
pio run -t upload
```

`config.h` git-ignored — soha nem kerül a repository-ba.

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

## Webes felület

| URL | Leírás |
|---|---|
| `http://<ip>/` | Dashboard — zóna állapotok, program státusz, log |
| `http://<ip>/wifi` | WiFi beállítás (Basic Auth) |
| `http://<ip>/update` | OTA firmware feltöltés (Basic Auth) |

A WiFi beállítás NVS-be mentődik és felülírja a `config.h` alapértelmezést. Ha az új WiFi nem elérhető, a board AP módba vált.

---

## OTA frissítés

**WiFi-n (PlatformIO):** kommenteld ki a `platformio.ini`-ben:
```ini
upload_protocol = espota
upload_port     = irrigationcontroller.local
upload_flags    = --auth=your_ota_password
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
| `program/status` | JSON | Aktív zóna, hátralévő idő (10mp-enként) |

**program/set payload:**
```json
[{"zone":1,"duration":10},{"zone":3,"duration":8},{"zone":5,"duration":15}]
```

### Szekvencia mentése

| Topic | Payload | Leírás |
|---|---|---|
| `sequence/set` | JSON tömb | Szekvencia mentése flash-be (ütemező ezt futtatja) |
| `sequence/state` | JSON tömb *(retained)* | Aktuálisan mentett szekvencia visszajelzés |

**sequence/set payload:**
```json
[{"zone":1,"duration":20},{"zone":2,"duration":5},{"zone":3,"duration":5}]
```

Mentés után reboot-kor automatikusan visszatöltődik.

### Napi ütemezés

| Topic | Payload | Leírás |
|---|---|---|
| `schedule/set` | JSON | Ütemezés beállítása |

**schedule/set payload:**
```json
{"enabled":true,"days":"MTWTFSS","time":"06:00"}
```

- `days`: 7 karakter, Hétfő–Vasárnap. `-` = kihagyás (pl. `"MTWTF--"` = csak hétköznap)
- Az ütemező a mentett szekvenciát futtatja. Ha nincs szekvencia mentve, nem indul semmi.
- Mentés után reboot-kor automatikusan visszatöltődik.

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

---

## NVS perzisztencia

A következő beállítások reboot után automatikusan visszaállnak (ESP32 NVS flash):

| Adat | NVS namespace |
|---|---|
| WiFi SSID + jelszó | `wifi` |
| Mentett szekvencia | `seq` |
| Napi ütemezés | `sched` |

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
├── generate_html.py          # HTML → gzip PROGMEM (auto-fut build előtt)
├── data/
│   └── index.html            # Dashboard forrás — itt szerkeszd
├── include/
│   ├── config.h              # ← git-ignored, saját adatok
│   ├── config.example.h      # sablon, commitolható
│   ├── web_ui.h
│   └── web_pages.h
└── src/
    ├── main.cpp              # WiFi, MQTT, relék, szekvenszer, ütemező
    ├── web_ui.cpp            # Web szerver routes, OTA handler, REST API
    └── web_pages.cpp         # HTML tartalom (auto-generált gzip PROGMEM)
```

### Weboldal szerkesztése

1. Szerkeszd a `data/index.html`-t
2. `pio run -t upload` — a `generate_html.py` automatikusan tömöríti és befordítja

Nincs szükség külön `uploadfs`-re — minden a firmware-be kerül.

---

## Node-RED flow

A `nodered_irrigation_flow.json` importálható Node-RED-be:  
**Hamburger menü → Import → JSON beillesztése**

Tartalmaz inject node-okat minden MQTT parancshoz, debug node-okat az állapotkövetéshez, és function node-okat az üzenetek formázásához.

**Első beállítás:** az `mqtt-broker` config node-ban állítsd be a broker IP-t és hitelesítő adatokat.

---

## Tipikus hibák

| Hiba | Megoldás |
|---|---|
| OTA 31%-nál megakad | WiFi jel gyenge, vagy firmware túl nagy — a dashboard gzip tömörítve van, ez nem lehet az ok |
| COM port nem található | Driver telepítés (CP2102 vagy CH340), vagy csak töltő USB kábel |
| `uploadfs` sikertelen | Használj `pio run -e ota -t uploadfs` WiFi-n, vagy a board gomb-kombinációval tartsd bootloader módban |
| Dupla MQTT üzenet | A `zone/+/set` topicot ne küld `retain: true`-val — a board connect-kor törli a retained set üzeneteket |
| Retained parancsok újrainduláskor | Lásd fent — a board connect-kor üres payloaddal törli az összes `set` retained üzenetet |
