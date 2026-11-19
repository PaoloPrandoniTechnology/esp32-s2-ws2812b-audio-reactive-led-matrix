# Debug Session: no-spectrum-visible
Status: [OPEN]
Session ID: no-spectrum-visible
Data: 2026-10-07
Utente segnala: "non vedo lo spettro" (dopo flash firmware web v1.5 con patch I²S Philips Standard)

## Stato attuale delle conoscenze
- Firmware LED standalone (backup) funziona con microfono (confermato da utente).
- Firmware web appena compilato e patchato: MSB → Philips Standard.
- Utente dice "non vedo lo spettro" → la pagina si carica ma il grafico è vuoto/nero.
- Possibili layer: HW I²S → FW I²S driver → Ring Buffer → WebSocket → Browser app.js → FFT Worker → Canvas.

## 5 Ipotesi Falsificabili (H1-H5)

| ID | Ipotesi | Come confermare / smentire |
|---|---|---|
| **H1** | Il flash del firmware web NON è andato a buon fine (power-cycle dimenticato, uptime ancora alto, firmware LED standalone ancora in esecuzione) | GET `/debug` → IP raggiungibile? JSON restituito? Se ritorna, controlla campi `heap`, `seq`, `sample_rate`, `uptime_ms` |
| **H2** | WebSocket non riesce a connettersi (AP mode sbagliato, IP sbagliato, WS path `/ws` non mappato, firewall) | Console browser: "WS open" stampato? F12 Network tab → WS 101 Switching Protocols? |
| **H3** | I²S produce campioni TUTTI ZERO nonostante patch Philips Standard → canale MIX L+R sempre 0, seq cresce ma audio=0 | `/debug` JSON `seq` cresce? Monitor seriale: raw dump L/R hex ogni 5s → TUTTI 0x00000000? |
| **H4** | WS frame arrivano ma FFT browser non visualizza → campioni OK ma soglie `minDb` / `maxDb` o `gainDb` sbagliate nel browser (es. minDb=-20 maxDb=-10 e segnale a -80 dBFS = tagliato fuori) | F12 Console: "received frame" con seq? Valore peak calcolato nei campioni? Slider gainDb al max 36 dB? |
| **H5** | WiFi.status WL_CONNECTED caching a WL_DISCONNECTED → WS broadcast saltato tutti i tick, seq cresce in audio_task ma loop non trasmette | `/debug` JSON → `ws_clients` = 1 quando connesso? Console seriale `[WS] Client #0 connesso`? |

## Strumenti di verifica disponibili (senza modifiche codice)
1. Monitor seriale (`pio device monitor -b 115200`) → banner boot, detect L/R, raw dump L/R hex.
2. Browser → `http://<ip>/debug` → JSON diagnostico.
3. Browser → F12 → Network → WS `/ws` → tab Messages: frame binari arrivano?
4. Browser → F12 → Console: errori JS, messaggi `WS opened / received N bytes`

## Analisi Post-Evidenze (2026-10-07 15:28)

Evidenze raccolte da utente:
  - Banner seriale OK, Build Oct 7 15:11:32, USB CDC flag verificati ✅
  - GET /debug → IP 192.168.0.71, rssi=-40 ✅
  - 🔴 sample_rate: 44100 Hz → SR NON SUPPORTATO su ESP32-S2 IDF 4.4.4 driver I2S legacy (APLL bug)
  - 🔴 heap_min = 9640 B (SOTTO guardia <12288 → TUTTI i frame audio WS sono saltati)
  - 🔴 heap_largest = 7668 B (grave frammentazione da init driver I2S 44k)
  - queue_len = 37/64 → frame si accumulano, audio_task vivo ma loop non trasmette
  - ws_clients = 1 ✅, seq=1167 crescente ✅, uptime 36s
  - Browser WS aperto ma "a..." troncato → nessun frame ricevuto dopo handshake

Conclusione: localStorage browser precedente conteneva sr=44100. All'apertura la UI
manda il comando set → firmware "valida" 44100 (SR_ALLOWED lo conteneva) →
driver I2S tenta init 44k ma APLL non riesce a generarlo correttamente.
Conseguenze: heap ridotta da ~30K a 9.6K (12K guardia scatta sempre) →
0 frame audio sul WS → spettro invisibile.

### Fix applicati (minimi, 2 file)
1. include/config.h SR_ALLOWED: rimosso 22050 e 44100 (ora 4 valori: 8k/16k/32k/48k), SR_LIST_COUNT 6→4, commento con warning.
2. data/app.js PARAM_CONFIG.sampleRate.steps: rimosso 22050/44100 (4 valori), commento warning.
3. Rebuild pulito: firmware 881.898 B (67.3% flash), LittleFS OK /app.js etc.

## Analisi Post-Evidenze 2 (2026-10-07 16:05) — ROOT CAUSE REALE TROVATA

Nuove evidenze da utente dopo primo fix (sr=16000 heap OK ora):
  - heap: 23708 B ✅, heap_min: 22668 B ✅ (>12K, guardia non scatta più)
  - queue_len 2 ✅, seq=3 (piccolo), ws_clients=0 (al momento del check)
  - sample_rate 16000 ✅, uptime 15s
  - **Seriale `[AUDIO] Detect L/R: sumL=0 sumR=0 -> canale LEFT` ripetuto 4 volte in 4 secondi** → AUDIO SEMPRE ZERO
  - Banner boot, WiFi IP 192.168.0.71 ✅, WS client connesso ✅

Firmware LED standalone FUNZIONA (conferma utente "il programma che mostra lo spettro sulla matrice led funziona correttamente").

### Confronto LED funzionante ↔ Web NON funzionante — 2 DIFFERENZE CRITICHE

| Impostazione I2S | LED (backup_fw_led_working_20261007/src_main.cpp) | Web Pre-fix | FIX |
|---|---|---|---|
| 1. `gpio_set_direction/pull_mode DOPO i2s_set_pin` | NESSUNA chiamata | 9 righe (BCLK OUT PULLUP, WS OUT PULLUP, DIN IN PULLDOWN, ecc.) L361-L369 | ❌ RIMOSSE COMPLETAMENTE → quelle chiamate TOGLIEVANO i pin GPIO 7/9/11 dalla periferica I2S mux function SPECIAL e li riconfiguravano in GPIO Semplice OUT/IN → BCK/WS non più generati, DIN non letto → 0x0000 sempre |
| 2. `channel_format` | `I2S_CHANNEL_FMT_ONLY_LEFT` (stesso di L/R=GND) | `I2S_CHANNEL_FMT_RIGHT_LEFT` STEREO + chan0/chan1 slot mask | Cambiato a ONLY_LEFT (stesso LED) |
| 3. `memset(pin_cfg,` | `0, sizeof` + 5 membri per nome (mck, bck, ws, dout, din) | `0xFF, sizeof` + 3 membri (bck, ws, din) | 0 + 5 membri (stesso LED) |
| 4. `use_apll` | `false` | `true` | false (stesso LED) |
| 5. `intr_alloc_flags` | `0` | `ESP_INTR_FLAG_LEVEL1` | 0 (stesso LED) |
| 6. `i2s_set_clk` DOPO set_pin | NO | YES + STEREO | Rimosso, usa `i2s_start` dopo `i2s_zero_dma_buffer` (LED) |
| 7. Lettura buffer | `int32_t raw_buf[AUDIO_FRAME_SAMPLES]` (MONO ONLY_LEFT) | `int32_t raw_buf[AUDIO_FRAME_SAMPLES*2]` (STEREO L+R idx*2) | MONO 512 sample (LED) |
| 8. Comms format | `I2S_COMM_FORMAT_STAND_I2S` Philips Standard | Stesso dopo patch L307 ✅ | Stesso ✅ |

### Fix applicati (minimi, LED-aligned)
1. **RIMOSSE 9 righe** gpio_set_direction + gpio_set_pull_mode + gpio_wakeup_disable DOPO i2s_set_pin (ROOT CAUSE #1 PRINCIPALE)
2. **channel_format ONLY_LEFT** come pin L/R=GND e LED
3. use_apll=false, intr=0, memset(pin_cfg,0,sizeof) + 5 membri per nome
4. Aggiunte `i2s_zero_dma_buffer + delay15 + i2s_start()` (LED schema) invece di `i2s_set_clk`
5. audio_task: raw_buf MONO 512 campioni, i2s_read MONO bytes_to_read, i loop corretti idx non moltiplicati * 2
6. Rimosso auto-detect L/R (sostituito con check livello LEFT forzato), dump RAW solo LEFT hex
7. Inseriti define mancanti `I2S_DMA_BUF_COUNT=8`, `I2S_DMA_BUF_LEN=AUDIO_FRAME_SAMPLES` in include/config.h (nel backup LED erano define locali nel cpp)

### Build OK
firmware.bin = 881.722 B (67.3% flash, 0 errori), RAM 17.8%

### Prossimi passi utente (stesso format)
1. pio run -t uploadfs  (LittleFS)
2. pio run -t upload      (firmware nuovo)
3. ⚠️ POWER-CYCLE USB 5s
4. pio device monitor -b 115200 → atteso:
   `[AUDIO] Livello canale LEFT (ONLY_LEFT): sumL=1234567 -> SEGNALE PRESENTE`
   (se parli vicino al microfono)
   `[AUDIO] RAW DUMP #0-7 ONLY_LEFT (int32 hex): S0=0xFFF3xxxx S1=0x00045200 ...` (diversi da 0x00000000)
5. Apri UI http://192.168.0.71/ → pulsante Reset → clicca → attendi
6. Parla e muovi Gain → spettro visibile
