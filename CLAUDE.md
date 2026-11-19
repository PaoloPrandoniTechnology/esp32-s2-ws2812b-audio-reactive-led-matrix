# CLAUDE.md — Guida al progetto "Giochi di Luce 8×32" (ESP32-S2 mini + INMP441)

> **NOTA PER ASSISTENTI AI CHE LAVORERANNO SU QUESTO PROGETTO:**
> Prima di apportare QUALSIASI modifica a pin, protocollo, parametri o
> struttura, leggi questo file e applica le regole qui descritte.
> Aggiorna questo file dopo ogni modifica sostanziale.

---

## 1. Descrizione sintetica

Firmware che anima una **matrice LED WS2812B 8×32 (256 pixel)** con una
ricca libreria di **27 effetti luminosi**. Gli effetti funzionano in due
modi:

- **Autonomi** (indipendenti dal suono): arcobaleno, plasma, fuoco,
  pioggia digitale, onde, scintille, comete, coriandoli, interferenza,
  fuochi d'artificio, metaball, palle rimbalzanti, lampada lava, onde
  concentriche, gradiente pulsante, serpenti.
- **Audio-reattivi** (guidati dal microfono INMP441): barre di spettro,
  barre centrali, VU meter, lampo sul beat, organo di colori, fuoco
  reattivo, arcobaleno reattivo, anelli reattivi, blocchi EQ, palla audio.

L'analisi audio avviene **sul microcontrollore** tramite **filtro di
Goertzel** a 16 bande (nessuna FFT esterna): la matrice reagisce a
livello, bassi/medi/alti e impulsi ritmici (beat) in tempo reale.

La **pagina web è volutamente minimale**: serve solo a scegliere
l'effetto, regolare luminosità / velocità / sensibilità / **guadagno del
microfono**, selezionare la palette colori, attivare il cambio casuale e
vedere un VU-meter. È previsto l'aggiornamento firmware via browser (OTA).

---

## 2. Hardware

### 2.1 Scheda
- **LOLIN S2 mini** (ESP32-S2FN4R2, flash 4 MB, PSRAM 2 MB, USB-C nativo CDC).
- **Single-core** a 240 MHz, **senza FPU vettoriale**: evitare funzioni
  trascendentali (`sinf`/`cosf`) nel loop di rendering.
- **Una sola periferica I²S** (`I2S_NUM_0`): impossibile un secondo mic.

### 2.2 Microfono INMP441
- MEMS I²S, uscita digitale mono. Nessun registro interno, nessun I²C.
- Il guadagno e la sensibilità si regolano **solo lato ESP32** (software).

### 2.3 Tabella collegamenti
> **Punto unico di definizione:** `include/config.h`.

| Segnale | GPIO ESP32-S2 mini | Note |
|---------|--------------------|------|
| INMP441 SCK (BCLK) | **7** | Bit clock (master) |
| INMP441 WS (LRCLK) | **9** | Word select |
| INMP441 SD (DIN) | **11** | Dati seriali in ingresso |
| INMP441 L/R | GND | Canale LEFT. Con `I2S_FORCE_CHANNEL 2` (AUTO) il firmware commuta su RIGHT se LEFT è silenzioso |
| INMP441 VDD / GND | 3V3 / GND | Alimentazione |
| Matrice WS2812B DIN | **5** | `FASTLED_DATA_PIN` |
| LED di stato | **15** | `LED_BUILTIN` (attivo LOW) |

### 2.4 Geometria matrice
- 32 colonne × 8 righe = **256 LED**, cablaggio **serpentina**
  (`MATRIX_SERPENTINE true`). Helper `matrix_xy()` gestisce la mappatura.

---

## 3. Architettura

```
[INMP441] --I2S--> [audio_task  (FreeRTOS, prio 1)]
                        │  conversione 24-bit -> float + guadagno (g_gain_dirty)
                        │  RMS / picco, Goertzel 16 bande, beat detection
                        ▼
                 [g_audio  struct AudioState]  (protetta da mutex portMUX)
                        │  snapshot coerente -> g_snap
                        ▼
                  [loop()]  clock animazione (dt * speed)
                        │  clear_all(); g_fx[effect](dt); FastLED.show();
                        ▼
                  [Matrice WS2812B 8x32]

[Browser] <--WS /ws (JSON set/get)--> [Firmware]
    │  stato  : {"type":"status", effect, random, brightness, speed,
    │            sensitivity, gainDb, palette, fxCount, heap}
    │  audio  : {"type":"audio", level, bass, mid, treble, beats, signal}
    │  comandi: {"cmd":"set", ...}  /  {"cmd":"get"}
    └-- HTTP: /effects (liste), /debug (diagnostica), /update (OTA)
```

### 3.1 Perché l'analisi audio è minima?
Il **filtro di Goertzel** calcola l'energia di 16 bande logaritmiche
(80 Hz → 7 kHz) con poche operazioni per campione, senza FFT e senza
allocazioni. È sufficiente per pilotare gli effetti e lascia CPU libera
al rendering LED e al web server.

### 3.2 Zero allocazioni runtime
Tutto lo stato degli effetti (particelle, code, anelli, buffer) è
**statico/preallocato**. Nessun `malloc` durante il rendering.

### 3.3 Tabella seno (LUT)
`g_sin_lut` (512 voci) + `fsin()`: sostituisce `sinf()` evitando la FPU
assente. Usare sempre `fsin()`/`fclamp()` negli effetti.

---

## 4. Struttura delle cartelle

```
Spettro/
├── platformio.ini         Configurazione PlatformIO (scheda, USB CDC, libs)
├── CLAUDE.md              ← QUESTO FILE
├── include/
│   └── config.h           PUNTO UNICO DI VERITA': pin, WiFi, limiti
│                          parametri, task audio.
├── src/
│   └── main.cpp           Firmware: guardie USB, LUT, palette, 27 effetti,
│                          I2S + Goertzel (audio_task), WiFi STA/AP,
│                          WebSocket, endpoint /effects /debug /update.
├── data/                  Filesystem LittleFS -> caricato con uploadfs
│   ├── index.html         Pagina web minimale
│   ├── style.css          Tema scuro industriale (flat, angoli 3px)
│   └── app.js             Client WS: griglia effetti, slider, palette,
│                          random, VU-meter, localStorage.
└── backup_*/ release_*/   Backup storici (NON modificare)
```

---

## 5. Protocollo

### 5.1 WebSocket `ws://<ip>/ws`
Massimo **1 client** consigliato (la S2 non ha margine per più flussi).

**Firmware → Browser**
```json
{ "type":"status", "effect":1, "random":false, "brightness":110,
  "speed":50, "sensitivity":50, "gainDb":12, "palette":0,
  "fxCount":27, "heap":123456 }

{ "type":"audio", "level":42, "bass":60, "mid":30, "treble":20,
  "beats":157, "signal":true }
```
`level/bass/mid/treble` sono interi **0-100** (percento).

**Browser → Firmware**
```json
{ "cmd":"set", "effect":5, "brightness":140, "gainDb":18 }
{ "cmd":"get" }
```
Tutti i campi di `set` sono opzionali; il firmware riclampa sempre.

### 5.2 Endpoint HTTP
| Endpoint | Descrizione |
|----------|-------------|
| `GET /effects` | `{effects:[...], palettes:[...], firstAudioFx, fxCount, paletteCount}` |
| `GET /debug` | Diagnostica: heap, heap_min, heap_largest, psram, ws_clients, parametri, level, beats, uptime_ms, rssi, ip |
| `GET /update` | Form OTA (auth `?pwd=<AP_PASSWORD>` o Basic `admin:<AP_PASSWORD>`) per firmware `.bin` e `littlefs.bin` |

`firstAudioFx` (=17) indica il primo indice di effetto audio-reattivo,
usato dalla UI per etichettare i pulsanti con il tag "audio".

---

## 6. Tabella dei parametri

> **⚠️ PUNTO UNICO DI CONFIGURAZIONE DEI LIMITI:**
> 1. **Firmware:** `include/config.h` (`*_MIN / *_MAX / *_DEFAULT`)
> 2. **Browser:** `data/app.js` → oggetto `PARAM_CONFIG`
>
> Ogni variazione va fatta in **ENTRAMBI i file**. Il firmware valida
> comunque sempre i valori ricevuti come safety net.

| Nome | Min | Max | Default | Dove | Note |
|------|-----|-----|---------|------|------|
| `effect` | 0 | 26 | 1 | Firmware | Indice in `g_fx[]`. 0 = Spento. |
| `random` | — | — | false | Firmware | Cambio casuale ogni `RANDOM_PERIOD_MS` (30 s). |
| `brightness` | 5 | 255 | 110 | Firmware | `FastLED.setBrightness()`. |
| `speed` | 1 | 100 | 50 | Firmware | Moltiplicatore `dt` (50 = nominale). |
| `sensitivity` | 0 | 100 | 50 | Firmware | Amplifica la reazione audio. |
| `gainDb` | 0 | 36 | 12 | Firmware | Guadagno digitale microfono. |
| `palette` | 0 | 8 | 0 | Firmware | 9 palette: Arcobaleno, Festa, Oceano, Lava, Foresta, Neon, Tramonto, Ghiaccio, Pastello. |

---

## 7. Comandi di build

Dalla root del progetto:

| Operazione | Comando |
|------------|---------|
| **Compilazione** | `pio run` |
| **Upload firmware (USB)** | `pio run -t upload` |
| **Upload LittleFS (pagina web)** | `pio run -t uploadfs` |
| **Entrambi** | `pio run -t uploadfs ; pio run -t upload` |
| **Solo immagine FS** | `pio run -t buildfs` |
| **Monitor seriale** | `pio device monitor -b 115200` |
| **Pulizia build** | `pio run -t clean` |

> La pagina web richiede `pio run -t uploadfs` **prima** di aprirla, altrimenti
> `/` risponde 404 (LittleFS vuoto).

### 7.1 🔴 `build_flags` su UNA SOLA RIGA (bug PlatformIO Core 6.1.18 Windows)
In `platformio.ini` i `build_flags` **DEVONO** stare tutti su una sola riga.
La forma multi-riga indentata viene **silenziosamente scartata**: i flag USB
non arrivano al compilatore e la USB CDC non compare a runtime.
Verifica: `pio run -v 2>&1 | Select-String "ARDUINO_USB"`.
Safety net: le guardie `#error` in cima a `src/main.cpp` (PRIMA di
`Arduino.h`). **NON rimuoverle.**

### 7.2 Flag USB minimi per LOLIN S2 mini
| Impostazione | Valore | Dove |
|---|---|---|
| `platform` | `espressif32@6.3.1` | `platformio.ini` — **fissata, non toccare** |
| `board_build.arduino_usb_cdc_on_boot` | `yes` | `platformio.ini` |
| `ARDUINO_USB_MODE` | `0` (TinyUSB) | `build_flags` (MANUALE) |
| `ARDUINO_USB_CDC_ON_BOOT` | `1` | `build_flags` (MANUALE) |
| `board_upload.use_1200bps_touch` | `yes` | `platformio.ini` |
| `board_upload.wait_for_upload_port` | `yes` | `platformio.ini` |

| Stato | PID Windows | Esempio COM |
|---|---|---|
| Firmware RUNTIME | `VID_303A&PID_80C2` | COM14… |
| ROM BOOTLOADER | `VID_303A&PID_0002` | COM13 |

### 7.3 Primo flash (una sola volta, boot manuale)
1. Imposta SSID/PASSWORD in `include/config.h`.
2. Tieni premuto **BOOT**, premi **RST**, rilascia entrambi (compare COM13/PID 0002).
3. `pio run -t uploadfs` poi `pio run -t upload`.
4. ⚡ **Stacca e riattacca il cavo USB** (power-cycle; non premere RST).
5. Attendi 5 s (compare COM14/PID 80C2), apri `http://<ip>`.

### 7.4 Flash successivi — SENZA PULSANTI
`pio run -t upload` / `pio run -t uploadfs` usano il **1200 bps touch** per
il reset automatico in bootloader.

> ⚠️ Dopo ogni upload **fai comunque un power-cycle USB**: il reset via
> pin RTS non è affidabile su questo hardware.

### 7.5 OTA via browser
`http://<ip>/update?pwd=<AP_PASSWORD>` (default `12345678`).
Caricare `firmware.bin` (tipo *Firmware*) o `littlefs.bin` (tipo *Filesystem*).
Dopo l'OTA fare un power-cycle USB per un avvio pulito.

WiFi fallback AP: se la rete non viene trovata, cerca `esp32-luci` e apri
`http://192.168.4.1`.

---

## 8. Convenzioni di codice

### ✅ SEMPRE
- **Commenti in italiano** in tutti i file sorgente (firmware e JS).
- **Costanti MAIUSCOLE**, C `snake_case`, JS `camelCase`.
- **Un solo punto di verità:** pin/limiti/credenziali solo in
  `include/config.h` e `PARAM_CONFIG` in `data/app.js`.
- **Validazione lato server:** ogni comando `set` ripassa da `apply_params()`
  (clamp di sicurezza).
- **Nessuna dipendenza esterna/CDN:** tutto locale in `data/` (funziona anche
  col solo AP, senza internet).
- **Zero reboot dopo cambio parametri:** si applicano a runtime
  (`apply_params()`); il cambio effetto azzera lo stato con `fx_reset_state()`.
- **API I²S legacy** (`driver/i2s.h`, ONLY_LEFT, Philips Standard,
  `I2S_COMM_FORMAT_STAND_I2S`, 32-bit/slot, APLL off).
- **Ordine init I²S obbligatorio:**
  `i2s_driver_install()` → `i2s_set_pin()` → `i2s_zero_dma_buffer()` → delay →
  `i2s_start()`. MAI `gpio_set_direction/pull_mode` dopo `set_pin`.
- **Stampe seriali SOLO con wrapper safe `SFP()` / `SFL()`** (mai `Serial`
  raw fuori dal banner boot). Vedi §9.1.
- **Guardia heap 12 KB** (`ESP.getFreeHeap() < 12288`): prima di inviare
  frame/JSON via WebSocket.
- **Sample rate audio:** 16 kHz, blocchi da 256 campioni (`config.h`).

### ❌ MAI
- **NON** usare FFT pesanti o `sinf`/`cosf` nel loop: usare `fsin()`.
- **NON** usare `float` nell'ISR o in routine critiche.
- **NON** allocare memoria a runtime nel rendering.
- **NON** inserire credenziali WiFi hardcoded versionate (`config.h`
  idealmente in `.gitignore`).
- **NON** trasmettere WS con WiFi disconnesso o con un solo pbuf libero.
- **NON** modificare `platform` (deve restare `espressif32@6.3.1`).

---

## 9. Problemi noti e note

### 9.1 Deadlock HWCDC ESP32-S2 (critico)
Se il PC non ha il monitor seriale aperto (DTR/RTS non asserito),
`Serial.printf/println/print` **bloccano indefinitamente**.
Tutte le stampe applicative DEVONO usare i wrapper sicuri:
- `SFP(format, ...)` → printf con `if (!Serial) return;`
- `SFL(string)`      → println con `if (!Serial) return;`

Nessuna eccezione (nemmeno nel banner boot).

### 9.2 Stabilità WiFi / LwIP
- Caching di `WiFi.status()` ogni **100 ms** (`wifi_status_cached`): evita il
  deadlock del mutex LwIP per chiamate troppo frequenti.
- `WiFi.setTxPower(WIFI_POWER_8_5dBm)` per contenere i picchi TX.
- Rate-limit WebSocket e telemetria per non saturare i pbuf della S2.
- `vTaskDelay(pdMS_TO_TICKS(1))` garantito a fine `loop()` per cedere a LwIP.

### 9.3 FastLED
- **Versione fissata 3.5.0** (la 3.6.0 rompe su ESP32-S2 per
  `ADC_WIDTH_BIT_12`).
- Il messaggio `No hardware SPI pins defined` è **normale** (la matrice è
  pilotata in bit-bang sul GPIO 5).

### 9.4 Clamp dei parametri
I parametri sono `uint8_t` (0-255). I confronti di clamp che coincidono con
gli estremi del tipo (es. `brightness > 255`) sono omessi in
`apply_params()` perché GCC li segnala con `-Wtype-limits` e sono no-op.
Restano attivi tutti i clamp significativi.

### 9.5 Microfono sempre a zero (RISOLTO 2026-10-17)
Il pin **L/R** di molti moduli INMP441 è **flottante**: il mic può emettere
sul canale LEFT o RIGHT a seconda del lotto. Il firmware forzava
`I2S_CHANNEL_FMT_ONLY_LEFT` → campioni sempre a zero. Fix:
- `I2S_FORCE_CHANNEL 2` = **AUTO**: `i2s_init_driver()` misura il picco su
  LEFT e, se silenzioso (< 100), reinstalla il driver su RIGHT (helper
  `i2s_reinstall_channel()`); se anche RIGHT è peggiore, torna a LEFT.
  Valori: `0`=forza LEFT, `1`=forza RIGHT, `2`=AUTO.
- `i2s_read()` usa un **timeout finito** (`AUDIO_READ_TIMEOUT_MS` = 200 ms),
  MAI `portMAX_DELAY`.
- Il task audio parte **subito dopo** `i2s_init_driver()` e **prima** del
  WiFi (nessun intervallo lungo tra `i2s_start()` e la prima lettura).
- Diagnostica su `/debug`: `raw_peak`, `raw0`, `rd_ok`, `rd_fail`
  (distingue causa software da causa hardware). Dump seriale ogni 5 s.

---

**Ultimo aggiornamento:** 2026-10-17 — Riscrittura completa del progetto da
"spettrogramma web" a **"Giochi di Luce 8×32"**: firmware con 27 effetti
(16 autonomi + 10 audio-reattivi) basati su Goertzel a 16 bande; pagina web
minimale (effetti, luminosità, velocità, sensibilità, guadagno mic, palette,
random, VU-meter) + OTA; rimossa la FFT nel browser e `fft_worker.js`.
Build: `pio run` SUCCESS — RAM 17.8%, Flash 64.8%.
**Fix microfono a zero:** auto-rilevazione canale I2S LEFT/RIGHT
(`I2S_FORCE_CHANNEL 2`) + timeout finito `i2s_read` + task audio avviato
prima del WiFi. Verificato su 192.168.0.71: `level`/`beats` > 0,
`raw_peak` fino a ~1.1M, `rd_fail` = 0.
