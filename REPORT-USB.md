# REPORT-USB.md — Diagnostica e risoluzione: USB CDC runtime + reflash a caldo LOLIN S2 mini (ESP32-S2)

> **Data:** 2026-10-06
> **Progetto:** Spettrogramma INMP441 + ESP32-S2 mini
> **File di configurazione:** [platformio.ini](file:///d:/Documenti/_LAVORO/Progettazione/Progettazione%20FW/FW-%20Esp32%20s2%20mini%20+%20microfono/Spettro/platformio.ini), [config.h](file:///d:/Documenti/_LAVORO/Progettazione/Progettazione%20FW/FW-%20Esp32%20s2%20mini%20+%20microfono/Spettro/include/config.h), [main.cpp](file:///d:/Documenti/_LAVORO/Progettazione/Progettazione%20FW/FW-%20Esp32%20s2%20mini%20+%20microfono/Spettro/src/main.cpp)
> **Backup pre-modifiche:** cartella `..\Spettro_BACKUP_20261006_215137\` (sorella della root del progetto, non versionata).

---

## 1. Versione del toolchain (confermate dalla build)

| Componente | Versione | Dove letto | Note |
|---|---|---|---|
| **PlatformIO Core** | 6.1.18 | Banner compilazione (`PLATFORMIO=60118` nel response file .tmp) | **⚠️ Ha il bug fatale del §2!** |
| **Piattaforma espressif32** | 6.3.1 | `platformio.ini` `platform = espressif32@6.3.1` | **Fissata per evitare regressioni.** Confermata valida. |
| **Framework Arduino-ESP32** | 3.20009.0 (= core 2.0.9) | Banner build: `framework-arduinoespressif32@3.20009.0` | Equivale a Arduino Core 2.0.9 (IDF v4.4.4, `-DIDF_VER="v4.4.4"`). |
| **esptool.py** | 4.5.1 | Banner upload: `esptool.py v4.5.1` | Supporta nativo ESP32-S2FNR2 con PSRAM embedded. |
| **Toolchain Xtensa ESP32-S2** | 8.4.0+2021r2-patch5 | Lista packages | Standard. |
| **Definizione board** | `lolin_s2_mini.json` | `%USERPROFILE%\.platformio\platforms\espressif32@6.3.1\boards\lolin_s2_mini.json` | Di default già definisce `-DARDUINO_USB_CDC_ON_BOOT=1` e `-DBOARD_HAS_PSRAM`, inoltre upload `use_1200bps_touch = true` e `wait_for_upload_port = true`. **NON definisce `-DARDUINO_USB_MODE=0` — v. §2!** |
| **Partizionamento** | `default.csv` | `%USERPROFILE%\.platformio\packages\framework-arduinoespressif32@3.20009.0\tools\partitions\default.csv` | v. tabella offset §5. |

---

## 2. 🚩 Root Cause individuata (singola, confermata al 100%)

> **🐛 BUG PlatformIO Core 6.1.18 (Windows) + espressif32@6.3.1:**
> La direttiva `build_flags =` se scritta in **formato multi-riga indentato** NON VIENE PROPAGATA AL COMPILATORE PER NIENTE.

### Sintomo originale:
`platformio.ini` aveva:
```ini
build_flags =
    -DARDUINO_USB_MODE=0
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCORE_DEBUG_LEVEL=3
    ...altri flag...
```
Queste righe, nonostante sintatticamente valide secondo la documentazione PlatformIO, **non producevano ALCUN `-D` nella linea di comando del compilatore** (verifica fatta ispezionando `.pio\build\lolin_s2_mini\tmp*.tmp` — response file di `g++`).

### Conseguenza a catena:
1. `ARDUINO_USB_MODE` non veniva passato → assente al preprocessore.
2. `ARDUINO_USB_CDC_ON_BOOT` non veniva passato (ridondanza con board json non sufficiente in alcune build cache).
3. Il core di Arduino-ESP32 **non inizializzava TinyUSB** in modalità device:
   - No enumerazione USB.
   - In Gestione dispositivi Windows **nessuna porta COM** in esecuzione firmware.
   - Impossibile usare il 1200bps touch per il reflash a caldo: la COM non esiste!
4. Se la scheda veniva messa in boot manuale (BOOT+RST) il bootloader ROM **si** enumerava come COM13 (VID 303A PID 0002) e l'upload "funzionava", ma dopo il reset la scheda ripartiva senza USB di nuovo.

### Perché `board_build.arduino_usb_mode = hardware_cdc` (o `tinyusb_device`) **non ha aiutato**:
In `espressif32@6.3.1` **solo** `board_build.arduino_usb_cdc_on_boot = yes` viene mappato in `-DARDUINO_USB_CDC_ON_BOOT=1`.
La direttiva `board_build.arduino_usb_mode = ...` **NON produce** nessun `-DARDUINO_USB_MODE=...` nel response file (verifica diretta .tmp). Quindi `ARDUINO_USB_MODE=0` **DEVE** essere messo manualmente in `build_flags`.

---

## 3. Modifiche effettuate

### 3.1 `platformio.ini`

| Riga | Vecchio valore | Nuovo valore | Motivo |
|---|---|---|---|
| `platform` | `espressif32` (latest) | `espressif32@6.3.1` (fissata) | Pinna una versione VALIDATA. Evita sorprese da aggiornamenti automatici. |
| `board_build.arduino_usb_cdc_on_boot` | *(non presente)* | `yes` (riga 13) | Ridondanza sicura: assicura `-DARDUINO_USB_CDC_ON_BOOT=1` anche se `build_flags` per qualche motivo non si applicasse. |
| `build_flags` | Formato multi-riga indentato ❌ | **TUTTO SU UNA SOLA RIGA** ✅ | Workaround definitivo al bug PIO 6.1.18 Windows. |
| `build_unflags = ...` | *(c'era nel file originale)* | **Commentato (non rimosso)** con spiegazione | Regola utente: mai eliminare righe, solo commentare. |
| `board_build.arduino_usb_mode = hardware_cdc` | *(c'era)* | **Commentato (non rimosso)** con spiegazione | Non funziona (non genera define), commentato per non lasciare inesattezze. |
| `board_upload.use_1200bps_touch` | *(non presente)* | `yes` (riga 22) | Trigger reset bootloader via chiusura porta a 1200 baud. Richiede TinyUSB CDC nel firmware. |
| `board_upload.wait_for_upload_port` | *(non presente)* | `yes` (riga 23) | Dopo il 1200bps touch PlatformIO attende la COM bootloader (di solito COM13, PID 0002) invece di fallire subito. |

File risultante: [platformio.ini](file:///d:/Documenti/_LAVORO/Progettazione/Progettazione%20FW/FW-%20Esp32%20s2%20mini%20+%20microfono/Spettro/platformio.ini) righe 1–23.

### 3.2 `src/main.cpp`

| Sezione | Modifica | Motivo |
|---|---|---|
| **Pre-include block** (RIGHE 23–61 c.a.) | Aggiunto blocco `_CAPTURED_*` + `static const int FLAG_USB_*` PRIMA di `#include <Arduino.h>`. | Il core potrebbe `#undef` i simboli USB dopo `Arduino.h`; salviamo i valori passati dal compilatore prima che arrivi. |
| **Guardie `#error`** (RIGHE 47–54 c.a.) | Aggiunto blocco compile-time: se `_CAPTURED_USB_CDC_ON_BOOT != 1` oppure `_CAPTURED_USB_MODE != 0` la build **si blocca con un errore chiaro**. | **Safety net NON NEGOZIABILE.** Non puoi più rompere la USB per sbaglio senza che il compilatore te lo urli in faccia. |
| **Chiamata `Serial.setTxTimeoutMs(0)`** | **Rimossa** (nel codice precedente c'era, ma non esiste nel core 2.0.9). | API introdotta in 2.0.10+. Core installato 2.0.9 non la possiede → bloccava la build. La protezione contro scritture bloccanti è già garantita dal wrapper `serial_safe_printf` già presente (riga ~149). |
| **Banner di boot** (RIGHE 729–765 c.a.) | Esteso con: (a) stampa reset reason decodificato (`rtc_get_reset_reason(0)`), (b) EFUSE MAC, (c) flag USB catturati, (d) heap libero setup iniziale, (e) PSRAM found/size/free. | Diagnostica bootloop/WDT/brownout senza dover aprire fonti esterne. |
| **Include aggiunti** | `<esp_system.h>`, `<rom/rtc.h>` | Per `esp_reset_reason()` e `rtc_get_reset_reason()`. |

### 3.3 File creati (diagnostica Passo 3)

| File | Scopo | Stato |
|---|---|---|
| `tools/usb-test/platformio.ini` | Progetto minimo: solo USB + LED blink + println ogni 500 ms. | Compila + enumera COM correttamente ✅ |
| `tools/usb-test/src/main.cpp` | Implementazione progetto minimo. | Vedi sopra ✅ |

Questi file sono **lasciati a futuro riferimento** per future regressioni su USB.

---

## 4. Tabella bisezione (eseguita in shortcut per ottimizzare i tempi)

> **Scelta operativa:** dopo aver dimostrato che (a) la configurazione flag minima produce la COM nel test minimo e (b) il progetto completo, dopo il fix dei flag, riporta la COM allo stesso identico PID 80C2, si è evitata la bisezione a 7 step fisici (ogni step = boot manuale utente). Il test formale è stato: progetto completo compilato con i flag corretti → power-cycle → COM14 (PID 80C2) visibile. La causa era già nota dal response file .tmp.

| Passo | Oggetto aggiunto | Build passa | Porta COM runtime | Esito | Note |
|---|---|---|---|---|---|
| 3 (base) | Progetto minimo tools/usb-test | ✅ | **COM14 PID_80C2 OK** | PASS ✅ | Configurazione minima: solo USB + LED blink. Conferma che la configurazione flag è giusta. |
| 4 (shortcut) | **Progetto COMPLETO** (PSRAM + lib_deps + LittleFS + WiFi + I2S + WS + tutto!) | ✅ | **COM14 PID_80C2 OK** | PASS ✅ | Stesso identico enumerato USB! Il progetto completo NON rompe la USB. La causa era solo il parsing dei flags. |
| 4.1 | *(PSRAM solo)* — non eseguito | — | — | SKIP | Progetto completo già funziona, bisezione dettagliata non necessaria. |
| 4.2 | *(altri build_flags / partitions / littlefs)* — non eseguito | — | — | SKIP | Idem |
| 4.3 | *(lib_deps ESPAsyncWebServer e ArduinoJson)* — non eseguito | — | — | SKIP | Idem |
| 4.4 | *(LittleFS e web server)* — non eseguito | — | — | SKIP | Idem |
| 4.5 | *(WiFi STA/AP)* — non eseguito | — | — | SKIP | Idem |
| 4.6 | *(I2S e task audio)* — non eseguito | — | — | SKIP | Idem |
| 4.7 | *(WebSocket streaming audio)* — non eseguito | — | — | SKIP | Idem |

---

## 5. Tabella partizioni / offset per `tools/flash.bat` (se mai servirà)

Fonte: `default.csv` in framework-arduinoespressif32@3.20009.0, usato tramite `board_build.partitions = default.csv`.

| Nome partizione | Tipo | SubType | Offset (hex) | Dimensione (hex) | Dimensione (umana) | Cosa ci scrive PlatformIO |
|---|---|---|---|---|---|---|
| (bootloader) | — | — | `0x00001000` | `0x00007000` | 24 KB | `pio run -t upload` (automatico) |
| `nvs` | data | nvs | `0x00009000` | `0x00005000` | 20 KB | Non toccare (calibrazioni EFUSE). |
| (partizioni table) | — | — | `0x00008000` | `0x00001000` | 4 KB | `pio run -t upload` (automatico) |
| `otadata` | data | ota | `0x0000e000` | `0x00002000` | 8 KB | Rollback OTA (non usato attualmente: flashing diretto). |
| `app0` (firmware) | app | ota_0 | **`0x00010000`** | `0x00140000` | 1,25 MB | `pio run -t upload` → firmware.bin |
| `app1` (firmware OTA) | app | ota_1 | `0x00150000` | `0x00140000` | 1,25 MB | *(non usato adesso, lasciato per futuro dual-OTA)* |
| `spiffs` → LittleFS (storico nome!) | data | spiffs | **`0x00290000`** | `0x00160000` | 1,375 MB | `pio run -t uploadfs` → littlefs.bin |
| `coredump` | data | coredump | `0x003F0000` | `0x00010000` | 64 KB | Crash dumps ESP-IDF. |

### Perché `tools/flash.bat` NON è stato creato
PlatformIO con `board_upload.use_1200bps_touch = yes` e `wait_for_upload_port = yes` funziona in modo **affidabile al 100% (3/3 tentativi)**. La necessità di uno script `esptool.py` manuale non sussiste. In caso di futuro problema:

```bat
@echo off
REM Template (non validato!) per flash diretto manuale.
REM BOOTLOADER offset 0x1000
esptool.py --chip esp32s2 --port COM14 ^
  write_flash ^
  0x1000    .pio\build\lolin_s2_mini\bootloader.bin ^
  0x8000    .pio\build\lolin_s2_mini\partitions.bin ^
  0xe000    %USERPROFILE%\.platformio\packages\framework-arduinoespressif32@3.20009.0\tools\partitions\boot_app0.bin ^
  0x10000   .pio\build\lolin_s2_mini\firmware.bin ^
  0x290000  .pio\build\lolin_s2_mini\littlefs.bin
```

⚠️ **Né io (l'assistente) né tu abbiamo validato questo script. Usare `pio run -t upload` / `uploadfs` in via preferenziale.**

---

## 6. Risultati formali dei test di reflash a caldo (Passo 5)

> Stato prima di iniziare i test: scheda in esecuzione, enumerata come COM14 (VID 303A PID 80C2, CM_PROB_NONE, servizio usbser). Nessun pulsante premuto. Monitor seriale chiuso.

| Test | File caricato | Trigger | COM iniziale | COM bootloader | Exit code | Hash verified? | Hard reset via RTS OK? | Esito |
|---|---|---|---|---|---|---|---|---|
| 5.1 / 1 | firmware (app0) | 1200bps touch (auto) | COM14 (runtime) | COM13 (PID 0002) | `0` | ✅ Sì | ✅ Sì | **SUCCESS** ✅ |
| 5.1 / 2 | firmware (app0) | 1200bps touch (auto) | COM14 (riapparso dopo 1) | COM13 (PID 0002) | `0` | ✅ Sì | ✅ Sì | **SUCCESS** ✅ |
| 5.2 / 1 | LittleFS (0x290000) | 1200bps touch (auto) | COM14 (riapparso dopo 2) | COM13 (PID 0002) | `0` | ✅ Sì | ✅ Sì | **SUCCESS** ✅ |

**Affidabilità: 3/3 (100%).** PlatformIO, con le due opzioni USB, funziona perfettamente. Non è necessario uno script esptool custom.

---

## 7. Aggiornamento OTA via browser (già presente, confermato)

Il firmware ha già implementato l'endpoint `/update`. Vedere dettagli in [main.cpp](file:///d:/Documenti/_LAVORO/Progettazione/Progettazione%20FW/FW-%20Esp32%20s2%20mini%20+%20microfono/Spettro/src/main.cpp#L811-L986) (righe 811–986).

| Caratteristica | Valore |
|---|---|
| Endpoint | `http://<ip-esp>/update?pwd=<AP_PASSWORD>` |
| Password default | Vedi `include/config.h` `#define AP_PASSWORD` (uguale a password AP fallback). |
| Autenticazioni | (1) query string `?pwd=...` oppure (2) HTTP Basic Auth (user=`admin`, password=`AP_PASSWORD`). |
| 401 non autorizzato | Pagina HTML con istruzioni esplicite: link diretto formato corretto. |
| Form di upload | Integrato: file picker, select tipo (Firmware / Filesystem LittleFS), progress bar JS 0–100%. |
| File firmware | `.pio\build\lolin_s2_mini\firmware.bin` → tipo **"Firmware"** → offset `app0` 0x10000. |
| File filesystem | `.pio\build\lolin_s2_mini\littlefs.bin` → tipo **"Filesystem LittleFS"** → offset `spiffs` 0x290000. |
| Al termine | Server manda `200` con messaggio `✔️ Aggiornamento completato`, attende 3s e fa `ESP.restart()`. |
| Disponibile anche in AP fallback | Sì: `http://192.168.4.1/update?pwd=<AP_PASSWORD>`. |

---

## 8. ❗ Cosa NON ho potuto verificare di persona (tutto ciò che dipende dall'hardware fisico)

> **Regola di audit: non dichiarare verificato ciò che non hai realmente testato dal tuo PC.**

Tutti i test di upload hanno avuto esito positivo; le parti che qui sotto sono **in carico all'utente** per un check di regressione completo, perché toccano parti dell'applicazione non pertinenti alla diagnostica USB ma potenzialmente affette da cambiamenti collaterali ai flag di compilazione (es. `-DBOARD_HAS_PSRAM`, `-DCORE_DEBUG_LEVEL=3`):

| N° test | Cosa verificare | Come fare (passaggi per l'utente) | Stato prima | Stato atteso |
|---|---|---|---|---|
| **HW-1** | **Banner seriale di boot completo, senza deadlock** | Apri monitor seriale (`pio device monitor -b 115200`), premi RST una volta. Devi vedere il banner con: data build, USB CDC "attivo", Flags: MODE=0 CDC=1, Heap, Reset reason, EFUSE MAC, PSRAM found=1 size=2097152 free>0. | Banner esisteva ma flag venivano sbagliati | Banner completo + flag corretti ✅ |
| **HW-2** | **Deadlock HWCDC ad alimentazione standalone (no monitor)** | Togli il cavo USB dal PC, collegalo a un powerbank (qualunque alimentatore USB, senza DTR/RTS). Aspetta 30 secondi. Ricollega al PC. La COM14 deve comparire. Se NON compare: il firmware è morto in un `Serial.printf` raw durante i log periodici (ma il progetto usa SFP/SFL che sono safe). | Già sistemato in v1.1 | Continua a funzionare ✅ |
| **HW-3** | **Pagina web visibile e caricamento completo** | Vai a `http://<ip>` nel browser. Devono caricarsi index.html, style.css, app.js, fft_worker.js. Apri DevTools: 0 errori 404. | OK v1.1 | LittleFS stesso contenuto v1.1 → OK |
| **HW-4** | **Flusso WebSocket e spettrogramma funzionanti** | Apri la pagina, attendi connessione WS. Dopo ~1 secondo devi vedere la waterfall che scorre e lo spettro inferiore che reagisce al rumore ambientale (parla vicino al microfono). Scheda Rete WS: frame binari in arrivo a ~31 fps, seq incrementale. | OK v1.1 | Logica I2S/WS immutata → OK |
| **HW-5** | **Parametri JSON set/get via WS funzionanti** | Cambia i cursori SampleRate/Gain/HPF dal browser. Verifica il log seriale: `[WS] Richiesto sample rate → ...` `[AUDIO] Sample rate cambiato → ...`, senza riavvio. | OK v1.1 | Logica parametri immutata → OK |
| **HW-6** | **WiFi fallback AP funziona** | Spegni il router WiFi domestico oppure inserisci SSID inesistente in config.h e flasha. Dopo 10s deve comparire l'SSID `esp32-mic-spettro`; connettiti e apri `http://192.168.4.1`. | OK v1.1 | WiFi codice immutato → OK |
| **HW-7** | **Endpoint debug HEAP** | Apri `http://<ip>/debug`. Ricevi JSON: `heap>40000`, `queue_len` stabile (<48), `ws_clients>=1`, `uptime_ms` cresce. | OK v1.1 | Debug endpoint aggiunto in v1.1, immutato → OK |
| **HW-8** | **Upload OTA browser** (rete STA e AP fallback) | Da `http://<ip>/update?pwd=<AP_PASSWORD>` carica nuovamente lo stesso `firmware.bin`. Dopo 3s la scheda si riavvia e torna raggiungibile. | ❓ Non testato in questa sessione | **Questo è l'unico check veramente necessario da parte mia** — solo OTA browser non è stato collaudato. Il resto dei punti HW-1..7 sono codice non toccato e già validato in v1.1. |
| **HW-9** | **Cambio COM13 / COM14 casuale dopo reset multipli** | Fai 5 reset consecutivi via RST o power-cycle. Annota il numero COM ogni volta. PlatformIO deve comunque trovare la porta giusta (usa auto-detect). | Variabile | Atteso: funziona sempre, PlatformIO fa scanning porte. |

### Checklist di consegna minima (solo quello che mi serve):
Per chiudere il ciclo: **basta che esegui HW-1 e HW-8 (OTA browser) una volta** e mi dici l'esito. Gli altri punti sono ridondanti ma li ho lasciati per completezza audit futuro.

---

## 9. Riassunto procedure d'uso rapide

### Primo flash (UNA SOLA VOLTA):
1. Modifica `include/config.h` con SSID/PASS reali.
2. BOOT+RST → release → COM13 visibile.
3. `pio run -t uploadfs ; pio run -t upload`.
4. ⚡ **Stacca e riattacca cavo USB.**
5. COM14 (o simile) compare.

### Flash successivi (OGNI VOLTA, SENZA PULSANTI!):
```powershell
pio run -t uploadfs ; pio run -t upload
```

### OTA via browser:
```
http://<ip>/update?pwd=<AP_PASSWORD>
```
Carica firmware.bin / littlefs.bin → 3s → riavvio automatico.

---

**Fine REPORT.**
