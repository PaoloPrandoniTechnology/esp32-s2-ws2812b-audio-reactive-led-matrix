/*
 * main.cpp - Firmware principale ESP32-S2 mini + microfono I2S INMP441
 *
 * Descrizione:
 *   - Acquisisce audio via I2S usando il driver I2S legacy espressif
 *     (compatibile con Arduino Core ESP32 2.0.x / framework installato).
 *   - Connette a WiFi in modalità Station, con fallback ad Access Point
 *     (SSID: esp32-mic-spettro) dopo 10 tentativi falliti.
 *   - Monta LittleFS e serve la pagina web (HTML/CSS/JS) in /data.
 *   - Attraverso WebSocket:
 *       * invia pacchetti BINARI di campioni int16 con header 12 byte
 *         (seq, sr, nsamples, MAGIC 0xDA7A);
 *       * riceve comandi JSON per cambiare sample rate, guadagno,
 *         frequenza HPF.
 *   - La FFT NON è calcolata dall'ESP32: viene eseguita dal browser
 *     per consentire modifiche a finestra, dimensione, overlap, ecc.
 *     senza ricompilare il firmware.
 *
 * Convenzioni: commenti in italiano, variabili e funzioni snake_case,
 * costanti MAIUSCOLE.
 */

// ============================================================
// [PRIMA DI TUTTO] CATTURA FLAG USB PASSATI DAL COMPILATORE
// ------------------------------------------------------------
// Arduino.h (arduino-esp32 core) PUO' fare #undef o ridefinire
// internamente ARDUINO_USB_MODE / ARDUINO_USB_CDC_ON_BOOT in base
// alla configurazione delle periferiche USB.
// Salviamo i valori QUI, PRIMA di qualsiasi include, in modo da:
//   1. poter fare le guardie statiche (#error) in modo affidabile;
//   2. poter stampare i valori REALI nel banner di boot (dopo
//      Arduino.h i simboli potrebbero non essere piu' visibili).
// ============================================================
#if defined(ARDUINO_USB_CDC_ON_BOOT)
#  define _CAPTURED_USB_CDC_ON_BOOT  ARDUINO_USB_CDC_ON_BOOT
#else
#  define _CAPTURED_USB_CDC_ON_BOOT  (-1)
#endif
#if defined(ARDUINO_USB_MODE)
#  define _CAPTURED_USB_MODE         ARDUINO_USB_MODE
#else
#  define _CAPTURED_USB_MODE         (-1)
#endif
static const int  FLAG_USB_CDC_ON_BOOT = _CAPTURED_USB_CDC_ON_BOOT;
static const int  FLAG_USB_MODE        = _CAPTURED_USB_MODE;

#if _CAPTURED_USB_CDC_ON_BOOT != 1
#error "CDC on boot NON attivo! Aggiungi -DARDUINO_USB_CDC_ON_BOOT=1 ai build_flags di platformio.ini"
#endif
#if _CAPTURED_USB_MODE != 0
#error "USB MODE NON e' 0 (TinyUSB nativo)! Aggiungi -DARDUINO_USB_MODE=0 ai build_flags di platformio.ini"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <math.h>
#include <driver/i2s.h>
#include <Update.h>
#include <ArduinoOTA.h>   // Seconda via di aggiornamento OTA (oltre a pagina web /update)
#include <esp_system.h>  // per esp_reset_reason()
#include <rom/rtc.h>     // per RESET_REASON_*
#include <esp_heap_caps.h>  // per heap_caps_get_minimum_free_size() + largest_free_block()
#include "config.h"

static const char *reset_reason_name(uint8_t reason_no) {
    switch (reason_no) {
        case 1:  return "POWERON (ESP_RST_POWERON)";
        case 2:  return "EXT (reset esterno pin RST)";
        case 3:  return "SW (software reset esp_restart)";
        case 4:  return "PANIC (exception/panic)";
        case 5:  return "INT_WDT (watchdog interrupt)";
        case 6:  return "TASK_WDT (task watchdog)";
        case 7:  return "WDT (main watchdog)";
        case 8:  return "DEEPSLEEP (uscita deep sleep)";
        case 9:  return "BROWNOUT (brownout)";
        case 10: return "SDIO (reset SDIO)";
        case 11: return "GROUP0 (group 0)";
        case 12: return "GROUP1 (group 1)";
        default: return "Sconosciuto";
    }
}

// ============================================================
// TIPO DATO PER IL RING BUFFER
// ============================================================
struct audio_frame_t {
    uint32_t seq;          // numero di sequenza progressivo
    uint32_t sample_rate;  // sample rate con cui è stato catturato
    int16_t  samples[AUDIO_FRAME_SAMPLES];
};

// ============================================================
// VARIABILI GLOBALI
// ============================================================
static volatile uint32_t  g_sample_rate         = SAMPLE_RATE_DEFAULT;
static volatile uint32_t  g_sample_rate_pending = SAMPLE_RATE_DEFAULT;
static volatile bool      g_sr_changed          = false;
static volatile uint8_t   g_gain_db             = GAIN_DB_DEFAULT;
static volatile uint16_t  g_hpf_hz              = HPF_HZ_DEFAULT;

// Cache dei parametri audio calcolati (expf / powf sono COSTOSI su S2 senza FPU).
// Vengono ricalcolati SOLAMENTE al cambio parametro (flag dirty), NON 31 volte al secondo.
static volatile bool      g_audio_params_dirty  = true;
static float              g_alpha_cached        = 0.995f;
static float              g_gain_factor_cached  = 1.0f;

// Stato LED STAT asincrono (nessun delay() bloccante nel loop!)
static uint32_t           g_led_off_at_ms       = 0;

static QueueHandle_t      g_ring_queue  = NULL;
static AsyncWebServer    *g_server      = NULL;
static AsyncWebSocket   *g_ws          = NULL;
static volatile uint32_t  g_seq         = 0;
static volatile int32_t   g_audio_boot_peak_s24 = 0;  // Picco MAX misurato nel warm-up di boot (solo diagnosi)

// ============================================================
// BUFFER PING-PONG PREALLOCATI IN PSRAM PER PACCHETTI WS AUDIO
// ============================================================
// Zero allocazioni runtime: 2 buffer statici alternati (ping-pong)
// per evitare frammentazione da audio_malloc 31×/sec.
#define WS_PKT_MAX_SIZE    (WS_HEADER_SIZE + (AUDIO_FRAME_SAMPLES) * 2)
static uint8_t *g_ws_pkt[2]  = { NULL, NULL };
static int       g_ws_pkt_idx = 0;

// ============================================================
// SERIALE USB CDC SICURA: NON si blocca MAI quando la USB è
// collegata all'alimentazione ma NESSUN PC apre il monitor
// seriale. La HWCDC ESP32-S2 senza DTR/RTS dal lato host ha
// un bug gravissimo: Serial.printf() aspetta INDEFINITAMENTE
// finché non c'è spazio nel buffer TX (perché l'host non ha
// aperto la porta e non legge niente).
//
// SINTOMO VISTO DALL'UTENTE: "Al primo lampeggio LED STAT a
// 5 secondi, il grafico si blocca per SEMPRE."
// Perché la STAT printf è la prima che invia dopo il banner di
// boot (stampato quando la USB era forse ancora enumerata).
// ============================================================
// Usiamo sempre serial_safe_printf(...) invece di Serial.printf.
// Per println() semplici: serial_safe_println("...").
static inline int __attribute__((format(printf, 1, 2))) serial_safe_printf(const char *fmt, ...) {
    if (!Serial) return 0;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    // ⚠️ ULTRA-SAFE: anche dopo !Serial, Serial.write() può BLOCCARSI
    // quando la USB CDC non è stata consumata per 10-30+ secondi dal PC
    // (buffer TX ring dell'USB si riempie e l'ISR bloccante aspetta
    // indefinitamente spazio). Guardie:
    //   (a) availableForWrite >= n: spazio disponibile nel buffer TX;
    //   (b) timeout 20 ms: se dopo 20 ms non c'è ancora spazio, ABORT.
    uint32_t t0 = (uint32_t)millis();
    while (Serial.availableForWrite() < n) {
        if ((int32_t)((uint32_t)millis() - t0) > 20) return 0; // 20 ms
        taskYIELD();
    }
    return (int)Serial.write((const uint8_t *)buf, (size_t)n);
}
static inline void serial_safe_println(const char *s) {
    if (!Serial) return;
    size_t n = strlen(s);
    if (n > 240) n = 240;
    uint32_t t0 = (uint32_t)millis();
    while (Serial.availableForWrite() < (int)(n + 2)) {
        if ((int32_t)((uint32_t)millis() - t0) > 20) return;
        taskYIELD();
    }
    Serial.println(s);
}
static inline void serial_safe_print(const String &s) {
    if (!Serial) return;
    size_t n = s.length();
    if (n > 240) n = 240;
    uint32_t t0 = (uint32_t)millis();
    while (Serial.availableForWrite() < (int)n) {
        if ((int32_t)((uint32_t)millis() - t0) > 20) return;
        taskYIELD();
    }
    Serial.print(s);
}
#define SERIAL_SAFE_PRINTF(...)  (void)serial_safe_printf(__VA_ARGS__)
#define SERIAL_SAFE_PRINTLN(s)   (void)serial_safe_println(s)
// Alias più corto per retrocompatibilità durante la migrazione
#define SFP  serial_safe_printf
#define SFL  serial_safe_println

// ============================================================
// LED DI STATO (GPIO15 - LOLIN S2 mini - attivo basso)
// ============================================================
#ifndef LED_BUILTIN
#define LED_BUILTIN        15
#endif
#define LED_ON()           digitalWrite(LED_BUILTIN, HIGH)
#define LED_OFF()          digitalWrite(LED_BUILTIN, LOW)
#define LED_BLINK_MS       20

static inline void led_setup(void) {
    pinMode(LED_BUILTIN, OUTPUT);
    LED_OFF();
}
static inline void led_pulse(uint32_t ms_on = LED_BLINK_MS) {
    LED_ON(); delay(ms_on); LED_OFF();
}
static inline void led_wait_serial_with_blink(uint32_t timeout_ms) {
    // Attende Serial.primo output per max timeout_ms lampeggiando il LED.
    // Utile perché la USB CDC impiega 500-3000ms per enumerarsi dopo reset.
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < timeout_ms) {
        LED_ON();  delay(60);
        LED_OFF(); delay(60);
    }
}

// ============================================================
// FUNZIONI AUSILIARIE
// ============================================================
static uint32_t clamp_sample_rate(uint32_t sr) {
    if (sr <= SR_ALLOWED[0]) return SR_ALLOWED[0];
    if (sr >= SR_ALLOWED[SR_LIST_COUNT - 1]) return SR_ALLOWED[SR_LIST_COUNT - 1];
    uint32_t best = SR_ALLOWED[0];
    uint32_t best_diff = (uint32_t)abs((int32_t)(best - sr));
    for (int i = 1; i < SR_LIST_COUNT; ++i) {
        uint32_t d = (uint32_t)abs((int32_t)(SR_ALLOWED[i] - sr));
        if (d < best_diff) { best = SR_ALLOWED[i]; best_diff = d; }
    }
    return best;
}
static inline uint8_t clamp_gain(int g) {
    if (g < GAIN_DB_MIN) return GAIN_DB_MIN;
    if (g > GAIN_DB_MAX) return GAIN_DB_MAX;
    return (uint8_t)g;
}
static inline uint16_t clamp_hpf(int h) {
    if (h < HPF_HZ_MIN) return HPF_HZ_MIN;
    if (h > HPF_HZ_MAX) return HPF_HZ_MAX;
    return (uint16_t)h;
}
static float db_to_factor(float db) {
    return powf(10.0f, db / 20.0f);
}

// ============================================================
// HELPER: malloc() preferibilmente in PSRAM (se disponibile),
// altrimenti fallback su heap interno.
// Riduce di MOLTO la frammentazione heap interno (2MB PSRAM libera).
// ============================================================
#ifdef BOARD_HAS_PSRAM
#  include <esp32-hal-psram.h>
static inline void *audio_malloc(size_t size) {
    if (psramFound()) return ps_malloc(size);
    return malloc(size);
}
static inline void audio_free(void *p) {
    free(p);
}
#else
#  define audio_malloc   malloc
#  define audio_free     free
#endif

// ============================================================
// INIZIALIZZAZIONE DRIVER I2S (RX)
// ============================================================
// L'INMP441 produce slot a 32 bit, 24 bit utili left-aligned,
// canale LEFT (L/R collegato a GND). Modalità master RX.
//
// ⚠️ CONFIGURAZIONE IDENTICA AL FIRMWARE LED STANDALONE CHE FUNZIONA:
//   - channel_format = ONLY_LEFT (canale sinistro solo, come pin L/R=GND)
//   - communication_format = Philips Standard (I2S_COMM_FORMAT_STAND_I2S)
//   - use_apll = false (nessun APLL, clock derivato diretto da REF_TICK)
//   - intr_alloc_flags = 0
//   - memset(pin_config, 0, sizeof) + assegnazione 5 membri per nome
//   - **NESSUNA chiamata gpio_set_direction / gpio_set_pull_mode DOPO
//     i2s_set_pin**: quelle chiamate RIMUOVONO i pin dalla periferica I2S
//     e li riconfigurano in GPIO semplice → BCK/WS non ci sono più
//     → INMP441 non vede clock → 0x0000 per sempre (root cause oggi).
// ============================================================
static bool i2s_init_driver(uint32_t sr) {
    static bool s_installed = false;
    if (s_installed) {
        i2s_stop(I2S_PORT_NUM);
        i2s_driver_uninstall(I2S_PORT_NUM);
        delay(20);
    }

    i2s_config_t i2s_config;
    memset(&i2s_config, 0, sizeof(i2s_config));
    i2s_config.mode               = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    i2s_config.sample_rate        = (uint32_t)sr;
    i2s_config.bits_per_sample    = I2S_BITS_PER_SAMPLE_32BIT;
    i2s_config.channel_format     = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags   = 0;
    i2s_config.dma_buf_count      = I2S_DMA_BUF_COUNT;
    i2s_config.dma_buf_len        = I2S_DMA_BUF_LEN;
    i2s_config.use_apll           = false;
    i2s_config.tx_desc_auto_clear = false;
    i2s_config.fixed_mclk         = 0;

    esp_err_t err = i2s_driver_install(I2S_PORT_NUM, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        SFP("[I2S] ERRORE install driver sr=%lu: %s\n",
            (unsigned long)sr, esp_err_to_name(err));
        return false;
    }

    // ------------------------------------------------------------
    // ⚠️ ORDINE IDENTICO A setupI2S DEL FIRMWARE LED STANDALONE FUNZIONANTE:
    // 1. install
    // 2. set_pin
    // 3. zero_dma_buffer  ← DOPO i2s_set_pin, MAI PRIMA
    // 4. delay 15
    // 5. i2s_start
    // Nel vecchio codice web zero_dma_buffer stava PRIMA di set_pin → stato
    // DMA inconsistente → ZERI per sempre (root cause 16:09 build).
    // ------------------------------------------------------------
    // Pin config: IDENTICA al firmware LED funzionante
    // memset 0, 5 membri assegnati per nome. NO forzature GPIO dopo!
    i2s_pin_config_t pin_cfg;
    memset(&pin_cfg, 0, sizeof(pin_cfg));
    pin_cfg.mck_io_num   = I2S_PIN_NO_CHANGE;
    pin_cfg.bck_io_num   = I2S_BCLK_PIN;
    pin_cfg.ws_io_num    = I2S_WS_PIN;
    pin_cfg.data_out_num = I2S_PIN_NO_CHANGE;
    pin_cfg.data_in_num  = I2S_DATA_IN_PIN;
    err = i2s_set_pin(I2S_PORT_NUM, &pin_cfg);
    if (err != ESP_OK) {
        SFP("[I2S] ERRORE set pin: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_zero_dma_buffer(I2S_PORT_NUM);
    delay(15);

    err = i2s_start(I2S_PORT_NUM);
    if (err != ESP_OK) {
        SFP("[I2S] ERRORE start: %s\n", esp_err_to_name(err));
        return false;
    }

    // ----------------------------------------------------------------
    // WARM-UP + PICCO massimo (diagnostica di boot, COPIATO da firmware LED standalone funzionante)
    // Legge 16 frame di scarto per sincronizzare BCK/WS poi 8 frame misura picco.
    // Senza warm-up i primi frame sono sempre 0x0000 (driver legacy IDF),
    // poi il bus si sincronizza e compaiono i dati veri.
    // Uso AUDIO_FRAME_SAMPLES = 512.
    // ----------------------------------------------------------------
    {
        int32_t discard[AUDIO_FRAME_SAMPLES];
        size_t br = 0;
        for (int i = 0; i < 16; ++i) {
            i2s_read(I2S_PORT_NUM, discard, sizeof(discard), &br, pdMS_TO_TICKS(300));
        }
        int32_t peak = 0;
        for (int k = 0; k < 8; ++k) {
            i2s_read(I2S_PORT_NUM, discard, sizeof(discard), &br, pdMS_TO_TICKS(300));
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
                int32_t s = discard[i] >> 8;
                int32_t a = s < 0 ? -s : s;
                if (a > peak) peak = a;
            }
        }
        g_audio_boot_peak_s24 = (int32_t)peak;
        SFP("[AUDIO][BOOT] Warm-up 16 frame OK. Picco MAX 24-bit su 8 frame = %ld (0x%06lX) %s\n",
            (long)peak, (unsigned long)peak,
            peak > 100 ? "SEGNALE PRESENTE" : "(silenzioso / microfono spento)");
    }

    delay(20);
    const bool was_first_install = !s_installed;
    s_installed = true;

    SFP("[I2S] Driver RX ONLY_LEFT @ %lu Hz (APLL OFF, Philips Standard 32-bit/slot). Prima install=%s\n",
        (unsigned long)sr, was_first_install ? "SI" : "no");
    return true;
}

// ============================================================
// TASK FREERTOS: ACQUISIZIONE + CONVERSIONE + HPF + GAIN
// ============================================================
static void audio_task(void *arg) {
    // Buffer di lettura MONO ONLY_LEFT (512 campioni int32 = 2048 byte)
    // Struttura IDENTICA al firmware LED standalone funzionante.
    static int32_t raw_buf[AUDIO_FRAME_SAMPLES];
    float x_prev = 0.0f, y_prev = 0.0f;

    // Canale LEFT forzato (pin L/R = GND come da cablaggio).
    // Non c'è più auto-detect L/R perché ONLY_LEFT: driver I²S già
    // restituisce solo il canale LEFT corretto.
    static uint32_t next_chk_ms   = 0;
    static uint32_t last_chk_rep  = 0;
    static uint32_t next_rawdump  = 5000UL;  // dump ogni 5s per diagnosi

    SFL("[AUDIO] Task acquisizione avviato. ONLY_LEFT forzato (come LED standalone).");

    for (;;) {
        // --- Cambio SR: reinstalla il driver e resetta stato HPF ---
        if (g_sr_changed) {
            uint32_t sr = g_sample_rate_pending;
            if (i2s_init_driver(sr)) {
                g_sample_rate = sr;
                SFP("[AUDIO] Sample rate cambiato a %lu Hz\n", (unsigned long)sr);
                x_prev = 0.0f; y_prev = 0.0f;
                g_audio_params_dirty = true;
            }
            g_sr_changed = false;
        }

        // --- RICALCOLO PARAMETRI (solo al cambio, NON 31x/sec) ---
        if (g_audio_params_dirty) {
            if (g_hpf_hz == 0 || g_sample_rate == 0) g_alpha_cached = 1.0f;
            else g_alpha_cached = expf(-2.0f * (float)M_PI * (float)g_hpf_hz / (float)g_sample_rate);
            g_gain_factor_cached = db_to_factor((float)g_gain_db);
            SFP("[AUDIO] Cache params: gain=%udB (x%.3f), HPF=%uHz (alpha=%.6f)\n",
                (unsigned)g_gain_db, g_gain_factor_cached,
                (unsigned)g_hpf_hz, g_alpha_cached);
            g_audio_params_dirty = false;
        }

        // Variabili locali veloci: cache dai parametri
        const float alpha       = g_alpha_cached;
        const float gain_factor = g_gain_factor_cached;

        // --- Lettura bloccante MONO ONLY_LEFT (512 campioni = 2048 byte) ---
        size_t bytes_read = 0;
        const size_t bytes_to_read = sizeof(raw_buf);
        esp_err_t err = i2s_read(I2S_PORT_NUM, raw_buf, bytes_to_read,
                                 &bytes_read, portMAX_DELAY);
        if (err != ESP_OK || bytes_read < bytes_to_read) {
            SFP("[AUDIO] WARNING i2s_read: %s (bytes %u/%u)\n",
                esp_err_to_name(err), (unsigned)bytes_read, (unsigned)bytes_to_read);
            continue;
        }

        // ============================================================
        // CHECK LIVELLO CANALE (ogni ~1 secondo, diagnostica visiva)
        // Canale LEFT forzato: somma solo LEFT, RIGHT non esiste (ONLY_LEFT)
        // ============================================================
        if (next_chk_ms == 0UL || (int32_t)(millis() - next_chk_ms) >= 0) {
            uint32_t sumL = 0;
            for (int k = 0; k < 100; ++k) {
                const int idx = (k * 5) % AUDIO_FRAME_SAMPLES;
                int32_t sl = raw_buf[idx] >> 8;
                if (sl < 0) sl = -sl;
                sumL += (uint32_t)sl;
            }
            next_chk_ms = millis() + 1000UL;
            if ((millis() - last_chk_rep) > 10000UL || sumL > 0) {
                last_chk_rep = millis();
                SFP("[AUDIO] Livello canale LEFT (ONLY_LEFT): sumL=%lu -> %s\n",
                    (unsigned long)sumL,
                    sumL > 100000 ? "SEGNALE PRESENTE" : "(silenzioso o microfono spento)");
            }
        }

        // ============================================================
        // DUMP DIAGNOSTICO RAW SAMPLE (ogni 5 secondi)
        // Solo LEFT hex perché ONLY_LEFT.
        // ============================================================
        if ((int32_t)(millis() - next_rawdump) >= 0) {
            next_rawdump = millis() + 5000UL;
            char line[384]; int p = 0;
            p += snprintf(line+p, sizeof(line)-p, "[AUDIO] RAW DUMP #0-7 ONLY_LEFT (int32 hex): ");
            for (int s = 0; s < 8; ++s) {
                int32_t l = raw_buf[s];
                p += snprintf(line+p, sizeof(line)-p, "S%d=0x%08lX ", s, (unsigned long)l);
                if (p > (int)sizeof(line)-48) break;
            }
            SFP("%s\n", line);
        }

        audio_frame_t frame;
        frame.seq         = g_seq++;
        frame.sample_rate = g_sample_rate;

        for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
            // ONLY_LEFT: un solo campione per indice, identico al LED funzionante.
            int32_t s32_L = raw_buf[i];

            // ------------------------------------------------------------
            // Conversione: INMP441 produce 24-bit left-aligned in slot
            // 32-bit (bit 31..8 = dati, bit 7..0 = 0).
            // Per portarlo a float [-1, +1): >> 8 (24-bit signed) poi
            // diviso 8388608.0 = 2^23 (range nominale 24-bit).
            // ------------------------------------------------------------
            int32_t s24 = s32_L >> 8;
            float x = (float)s24 / 8388608.0f;

            // HPF 1° ordine (DC Blocker): y = x - x_prev + α·y_prev
            float y;
            if (alpha >= 0.99999f) {
                y = x;
            } else {
                y = x - x_prev + alpha * y_prev;
                x_prev = x;
                y_prev = y;
            }
            y *= gain_factor;

            if (y >  1.0f) y =  1.0f;
            if (y < -1.0f) y = -1.0f;
            frame.samples[i] = (int16_t)(y * 32767.0f);
        }

        // Invia in coda: se piena butta il più vecchio
        BaseType_t ok = xQueueSend(g_ring_queue, &frame, pdMS_TO_TICKS(1));
        if (ok != pdPASS) {
            audio_frame_t drop;
            xQueueReceive(g_ring_queue, &drop, 0);
            xQueueSend(g_ring_queue, &frame, 0);
        }

        // ------------------------------------------------------------
        // SINGLE CORE YIELD CRITICO:
        // FreeRTOS su ESP32-S2 (single core) NON esegue time-slice tra
        // task a priorità DIFFERENTE. Quindi audio_task PR=2, se non
        // cede il controllo volontariamente, può affamare loopTask()
        // PR=1 per 10ms+ in presenza di carico. Chiamiamo taskYIELD()
        // per forzare un check dello scheduler → LwIP e loop possono
        // eseguire tra un frame audio e l'altro (31 volte/sec = OK).
        // ------------------------------------------------------------
        taskYIELD();
    }
}

// ============================================================
// WEBSOCKET: INVIO FRAME E GESTIONE EVENTI
// ============================================================
// NOTA: ESPAsyncWebServer-esphome v3.3.2 NON rende pubblico AsyncWebSocketClient::queueLength().
// Il controllo di backpressure lato client è delegato al ring buffer (HIGH_WATERMARK 48/64).
// Queste variabili sono mantenute per compatibilità STAT e future integrazioni.
static volatile uint32_t g_ws_qb_drop = 0;
static volatile uint32_t g_ws_qb_peak = 0;

static void ws_broadcast_audio_frame(const audio_frame_t &frame) {
    if (g_ws == NULL || g_ws->count() == 0) return;
    // ⚠️ NOTA 2026-10-07: WiFi.status() NON viene più chiamata qui internamente!
    //   Era la causa di una DOPPIA chiamata WiFi.status() PER loop (una in
    //   ws_broadcast, una in loop OTA guard) = 2 contese mutex LwIP/33ms,
    //   che su S2 single-core causavano deadlock permanente dopo 40s.
    //   ORA il controllo WL_CONNECTED viene fatto UNA SOLA VOLTA nel loop
    //   prima di chiamare questa funzione, su valore CACHATO 100ms.
    // ⚠️ HEAP SAFETY GUARD vedi sotto:
    if (ESP.getFreeHeap() < 12288) return;
    const size_t nsamples = AUDIO_FRAME_SAMPLES;
    const size_t pkt_size = WS_HEADER_SIZE + nsamples * sizeof(int16_t);

    // ---- PING-PONG BUFFER PSRAM: zero allocazioni runtime ----
    // Alterna g_ws_pkt[0] e [1] (entrambi preallocati in setup()).
    if (g_ws_pkt[0] == NULL || g_ws_pkt[1] == NULL) {
        // Fallback: buffer statico su BSS (non dovrebbe mai servire)
        static uint8_t fallback[WS_PKT_MAX_SIZE];
        if (pkt_size > sizeof(fallback)) return;
        uint16_t magic = WS_MAGIC; uint16_t ns16 = (uint16_t)nsamples;
        memcpy(fallback + 0,  &frame.seq,         4);
        memcpy(fallback + 4,  &frame.sample_rate, 4);
        memcpy(fallback + 8,  &ns16,              2);
        memcpy(fallback + 10, &magic,             2);
        memcpy(fallback + 12, frame.samples,      nsamples * sizeof(int16_t));
        AsyncWebSocketMessageBuffer *buf = g_ws->makeBuffer(fallback, pkt_size);
        if (buf) g_ws->binaryAll(buf);
        return;
    }
    uint8_t *pkt = g_ws_pkt[g_ws_pkt_idx];
    g_ws_pkt_idx ^= 1;

    uint16_t magic = WS_MAGIC;
    uint16_t ns16  = (uint16_t)nsamples;
    memcpy(pkt + 0,  &frame.seq,         4);
    memcpy(pkt + 4,  &frame.sample_rate, 4);
    memcpy(pkt + 8,  &ns16,              2);
    memcpy(pkt + 10, &magic,             2);
    memcpy(pkt + 12, frame.samples,      nsamples * sizeof(int16_t));
    AsyncWebSocketMessageBuffer *buf = g_ws->makeBuffer(pkt, pkt_size);
    if (buf == NULL) return;
    g_ws->binaryAll(buf);
}

static void ws_send_status(AsyncWebSocketClient *client) {
    if (g_ws == NULL) return;
    // HEAP GUARD: stessa soglia 8 KB SOLO heap INTERNO (vedi sopra commento ws_broadcast).
    if (ESP.getFreeHeap() < 12288) return;
    JsonDocument doc;
    doc["type"]       = JSON_TYPE_STATUS;
    doc["sampleRate"] = g_sample_rate;
    doc["gainDb"]     = (int)g_gain_db;
    doc["hpfHz"]      = (int)g_hpf_hz;
    doc["seq"]        = (uint32_t)g_seq;
    doc["heap"]       = (uint32_t)ESP.getFreeHeap();
    char buf[256];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (client != NULL)      client->text(buf, n);
    else if (g_ws != NULL)   g_ws->textAll(buf, n);
}

// Timestamp per invio status NON bloccante dopo un comando set.
// Sostituisce delay(80) di prima: evita freeze del loopTask.
static volatile uint32_t g_status_sched_ms = 0;

static void ws_handle_set(const JsonObjectConst &payload) {
    bool modified = false;
    if (!payload["sampleRate"].isNull()) {
        uint32_t sr_new = clamp_sample_rate(payload["sampleRate"].as<uint32_t>());
        if (sr_new != g_sample_rate) {
            g_sample_rate_pending = sr_new;
            g_sr_changed = true;
            g_audio_params_dirty = true;
            modified = true;
            SFP("[WS] Richiesto sample rate -> %lu\n", (unsigned long)sr_new);
        }
    }
    if (!payload["gainDb"].isNull()) {
        uint8_t g_new = clamp_gain(payload["gainDb"].as<uint8_t>());
        if (g_new != g_gain_db) {
            g_gain_db = g_new; modified = true;
            g_audio_params_dirty = true;
            SFP("[WS] Guadagno digitale -> %u dB\n", (unsigned)g_new);
        }
    }
    if (!payload["hpfHz"].isNull()) {
        uint16_t h_new = clamp_hpf(payload["hpfHz"].as<uint16_t>());
        if (h_new != g_hpf_hz) {
            g_hpf_hz = h_new; modified = true;
            g_audio_params_dirty = true;
            SFP("[WS] Filtro HPF -> %u Hz\n", (unsigned)h_new);
        }
    }
    if (modified) {
        // Schedulazione NON bloccante: invio status dopo ~80ms nel loop()
        g_status_sched_ms = millis() + 80UL;
    }
}

static void ws_on_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            SFP("[WS] Client #%u connesso (ora %u connessi)\n",
                client->id(), (unsigned)server->count());
            // 🟢 MAX 1 client. Se arriva secondo, chiudi il PRIMO (vecchio)
            //   invece del nuovo, così funziona refresh browser: nuovo client
            //   valido sostituisce quello vecchio zombie.
            while (server->count() > MAX_WS_CLIENTS) {
                SFP("[WS] Troppi client → chiudo #%u per limite MAX=%u\n",
                    client->id(), (unsigned)MAX_WS_CLIENTS);
                server->closeAll();
                break;
            }
            ws_send_status(client);
            break;
        case WS_EVT_DISCONNECT:
            SFP("[WS] Client #%u disconnesso\n", client->id());
            break;
        case WS_EVT_DATA: {
            AwsFrameInfo *info = (AwsFrameInfo *)arg;
            if (info->opcode == WS_TEXT && len > 0) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, data, len);
                if (err) {
                    SFP("[WS] JSON invalido: %s\n", err.c_str());
                    return;
                }
                const char *cmd = doc["cmd"] | "";
                if (strcmp(cmd, JSON_CMD_SET) == 0) {
                    ws_handle_set(doc.as<JsonObjectConst>());
                } else if (strcmp(cmd, JSON_CMD_GET) == 0) {
                    ws_send_status(client);
                } else {
                    SFP("[WS] Comando sconosciuto: '%s'\n", cmd);
                }
            }
            break;
        }
        default: break;
    }
}

// ============================================================
// SETUP WIFI: 10 TENTATIVI STA, POI FALLBACK AP
// ============================================================
static bool wifi_connect_sta(void) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setHostname("esp32-mic-spettro");
    // Forza WPA2/WPA3 per la GreenApple (alcuni router moderni rifiutano associazioni WPA-only)
    WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
    WiFi.setAutoReconnect(false);
    // Calibrazione TX power: la LOLIN S2 mini ha antenna PCB INTEGRATA PICCOLA
    // e senza matching ottimale: con potenza > 8 dBm il PHY va in saturazione
    // quando trasmette pacchetti lunghi (WS frame 1 KB), causando PERDITA TOTALE
    // ACK TCP dal router. Sintomo: "dopo 1s stop trasmissione".
    // Valore sicuro e testato: 8 dBm.
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    SFP("[WIFI] Scan passivo per '%s' ... ", WIFI_SSID);
    int nets = WiFi.scanNetworks(false, true, false, 300);
    bool ssid_found = false;
    int target_channel = 0;
    for (int i = 0; i < nets; ++i) {
        if (WiFi.SSID(i) == WIFI_SSID) {
            ssid_found = true;
            target_channel = WiFi.channel(i);
            SFP("OK (canale %d, RSSI %d dBm)\n", target_channel, WiFi.RSSI(i));
            break;
        }
    }
    WiFi.scanDelete();
    if (!ssid_found) SFL(" NON TROVATO in scan (provo lo stesso)");
    SFP("[WIFI] Connessione a '%s' (max %d tentativi)\n",
        WIFI_SSID, WIFI_MAX_RETRIES);

    for (int attempt = 1; attempt <= WIFI_MAX_RETRIES; ++attempt) {
        SFP("[WIFI] Tentativo %d/%d ... ", attempt, WIFI_MAX_RETRIES);
        if (target_channel > 0) {
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD, target_channel, NULL, true);
        } else {
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }

        // ATTESA PIU' LUNGA (8s) per autenticazione WPA2 / DHCP
        // ⚠️ IMPORTANTE: vTaskDelay(1) SEMPRE e taskYIELD su ESP32-S2 single-core:
        //   senza yield al scheduler la LwIP (priorità 4) non riesce a schedularsi tra
        //   un delay(80) e l'altro e la connessione rimane "appesa" per 80 secondi.
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED &&
               (millis() - start) < 8000UL) {
            delay(40);
            led_pulse(15);
            // ⚠️ NON CONDIZIONALE! Ogni tick cedi a LwIP.
            vTaskDelay(pdMS_TO_TICKS(1));
            taskYIELD();
        }

        if (WiFi.status() == WL_CONNECTED) {
            SFL(" OK");
            SFP("[WIFI] IP: %s (dopo %d tentativi, RSSI %d dBm)\n",
                WiFi.localIP().toString().c_str(), attempt, WiFi.RSSI());
            WiFi.setAutoReconnect(true);
            return true;
        }
        switch (WiFi.status()) {
            case WL_IDLE_STATUS:     SFL(" IDLE"); break;
            case WL_NO_SSID_AVAIL:   SFL(" SSID NON TROVATO"); break;
            case WL_CONNECT_FAILED:  SFL(" FALLITA (pwd errata / auth)"); break;
            case WL_CONNECTION_LOST: SFL(" PERSA"); break;
            case WL_DISCONNECTED:    SFL(" DISCONNESSO"); break;
            default:                 SFP(" STATUS=%d\n", WiFi.status()); break;
        }
        WiFi.disconnect(true);
        // Backoff: 200 ms + accumulo progressivo per non saturare il router
        delay(120 + (uint32_t)attempt * 40);
        vTaskDelay(pdMS_TO_TICKS(1));
        taskYIELD();
    }

    SFP("[WIFI] Impossibile connettersi dopo %d tentativi, avvio AP fallback\n",
        WIFI_MAX_RETRIES);
    WiFi.disconnect(true);
    return false;
}

static void wifi_start_ap(void) {
    WiFi.mode(WIFI_AP);
    IPAddress ip(AP_IP);
    IPAddress netmask(255, 255, 255, 0);
    WiFi.softAPConfig(ip, ip, netmask);
    if (strlen(AP_PASSWORD) >= 8) {
        WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
    } else {
        WiFi.softAP(AP_SSID, NULL, AP_CHANNEL, 0, AP_MAX_CONN);
    }
    SFP("[WIFI] AP avviato SSID='%s' PSW='%s' -> IP: %s\n",
        AP_SSID, AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

// ============================================================
// SETUP GENERALE
// ============================================================
void setup() {
    delay(300);
    led_setup();
    LED_ON();      // Feedback visivo: setup iniziato
    Serial.begin(115200);
    // Attesa della USB CDC: fino a 3s con lampeggio ~8 Hz
    led_wait_serial_with_blink(3000);
    LED_OFF();
    // ⚠️ BANNER BOOT: SOLO wrapper SFP/SFL safe (no Serial.raw!).
    // Se DTR/RTS NON asserito (monitor seriale chiuso, alimentazione standalone
    // USB powerbank), qualsiasi Serial.printf/println raw BLOCCO PER SEMPRE
    // la CPU (bug HWCDC ESP32-S2). Nessuna eccezione.
    SFL("\n==========================================");
    SFL(" ESP32-S2 mini + INMP441 - Spettrogramma   ");
    SFP(" Build: %s %s\n", __DATE__, __TIME__);
    SFL(" USB CDC: attivo (flag verificati PRIMA di Arduino.h)");
    SFP(" Flags  : ARDUINO_USB_MODE=%d  ARDUINO_USB_CDC_ON_BOOT=%d\n",
        FLAG_USB_MODE, FLAG_USB_CDC_ON_BOOT);
    SFP(" Heap   : free=%u B (setup iniziale)\n", (unsigned)ESP.getFreeHeap());
    // Motivo dell'ultimo reset: serve a distinguere bootloop, WDT, brownout
    uint8_t rr = (uint8_t)rtc_get_reset_reason(0);
    SFP(" Reset  : codice %u -> %s\n", rr, reset_reason_name(rr));
    uint64_t mac = ESP.getEfuseMac();
    SFP(" EFUSE  : MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        (unsigned)((mac >> 40) & 0xFF), (unsigned)((mac >> 32) & 0xFF),
        (unsigned)((mac >> 24) & 0xFF), (unsigned)((mac >> 16) & 0xFF),
        (unsigned)((mac >>  8) & 0xFF), (unsigned)((mac      ) & 0xFF));
#ifdef BOARD_HAS_PSRAM
    SFP(" PSRAM  : BOARD_HAS_PSRAM define presente, found=%d size=%u B free=%u B\n",
        (int)psramFound(),
        (unsigned)(psramFound() ? ESP.getPsramSize() : 0U),
        (unsigned)(psramFound() ? ESP.getFreePsram() : 0U));
#else
    SFL(" PSRAM  : BOARD_HAS_PSRAM define ASSENTE (usa solo RAM interna)");
#endif
    SFL("==========================================");

    if (!LittleFS.begin(true)) {
        SFL("[FS] ERRORE LittleFS: carica il filesystem con pio run -t uploadfs");
    } else {
        SFL("[FS] LittleFS montato");
    }

    if (strlen(WIFI_SSID) > 0 && strlen(WIFI_SSID) < 32 &&
        strcmp(WIFI_SSID, "LaTuaReteWiFi") != 0) {
        if (!wifi_connect_sta()) wifi_start_ap();
    } else {
        SFL("[WIFI] Credenziali STA non configurate, avvio AP");
        wifi_start_ap();
    }

    // ============================================================
    // ARDUINOOTA: seconda via di aggiornamento (oltre pagina web /update)
    //   Protocollo standard PlatformIO espota.py su porta 3232.
    //   Password = AP_PASSWORD (come OTA web, già in config.h).
    //   Nome host esp32-mic-spettro.local (mDNS).
    // ============================================================
    ArduinoOTA.setHostname("esp32-mic-spettro");
    ArduinoOTA.setPassword(AP_PASSWORD);
    ArduinoOTA.setPort(3232);
    ArduinoOTA
        .onStart([]() {
            const char *tipo = (ArduinoOTA.getCommand() == U_FLASH) ? "FIRMWARE" : "FILESYSTEM";
            SFL("");
            SFP("[OTA-Arduino] Inizio aggiornamento: %s\n", tipo);
        })
        .onEnd([]() {
            SFL("\n[OTA-Arduino] Completato. Riavvio...");
            delay(500);
        })
        .onProgress([](unsigned int progress, unsigned int total) {
            static uint32_t last_pct_show = 999;
            unsigned pct = (total > 0) ? (progress / (total / 100)) : 0;
            if (pct != last_pct_show) {
                last_pct_show = pct;
                SFP("[OTA-Arduino] Progresso: %3u%% (%u/%u)\r", pct, progress, total);
            }
        })
        .onError([](ota_error_t err) {
            SFP("\n[OTA-Arduino] ERRORE codice %u: ", (unsigned)err);
            if (err == OTA_AUTH_ERROR)       SFL("Auth Failed");
            else if (err == OTA_BEGIN_ERROR) SFL("Begin Failed");
            else if (err == OTA_CONNECT_ERROR) SFL("Connect Failed");
            else if (err == OTA_RECEIVE_ERROR) SFL("Receive Failed");
            else if (err == OTA_END_ERROR)   SFL("End Failed");
            else                              SFL("Sconosciuto");
        });
    ArduinoOTA.begin();
    SFP("[OTA-Arduino] Pronto host='esp32-mic-spettro' porta 3232 pwd=*** (stessa AP)\n");

    g_ring_queue = xQueueCreate(RING_BUFFER_FRAMES, sizeof(audio_frame_t));
    if (g_ring_queue == NULL) {
        SFL("[FATAL] Impossibile creare la coda audio");
        while (1) delay(1000);
    }

    // ---- ALLOCAZIONE SINGOLA BUFFER PING-PONG PACCHETTI WS in RAM INTERNA ----
    // 2 × ~1KB = ~2KB. NON usiamo PSRAM per questi buffer: quando I²S DMA
    // e la CPU accedono simultaneamente a PSRAM può avvenire un BUS STALL
    // / DEADLOCK intermittente dopo ~100-200 trasmissioni (sintomo: scheda
    // si blocca e non risponde più a HTTP/PING dopo 1-3 secondi).
    // 2KB è irrisorio per 320KB di RAM interna. Zero allocazioni runtime.
    g_ws_pkt[0] = (uint8_t *)malloc(WS_PKT_MAX_SIZE);
    g_ws_pkt[1] = (uint8_t *)malloc(WS_PKT_MAX_SIZE);
    if (!g_ws_pkt[0] || !g_ws_pkt[1]) {
        SFP("[MEM] ATTENZIONE: malloc WS ping-pong fallito (got %d/2); uso fallback statico 1KB stack interno\n",
            (int)(g_ws_pkt[0] != NULL) + (int)(g_ws_pkt[1] != NULL));
    } else {
        SFP("[MEM] WS ping-pong buffer allocati in RAM INTERNA: %u B × 2 = %u B (no PSRAM per evitare DMA stall)\n",
            (unsigned)WS_PKT_MAX_SIZE, (unsigned)(WS_PKT_MAX_SIZE * 2));
    }

    if (!i2s_init_driver(g_sample_rate)) {
        SFL("[FATAL] Inizializzazione I2S fallita");
    }

    g_server = new AsyncWebServer(80);
    g_ws     = new AsyncWebSocket("/ws");
    g_ws->onEvent(ws_on_event);
    g_server->addHandler(g_ws);

    // ============================================================
    // AGGIORNAMENTO OTA VIA WEB (nessun pulsante, nessun BOOT mode)
    // ============================================================
    // Password uguale alla AP per semplicità.
    const char *OTA_PASS = AP_PASSWORD;
    const uint8_t OTA_PART_FW  = U_FLASH;
    const uint8_t OTA_PART_FS  = U_SPIFFS; // Update.h usa U_SPIFFS anche per LittleFS

    // --- Pagina /update (form HTML semplice) ---
    g_server->on("/update", HTTP_GET, [OTA_PASS](AsyncWebServerRequest *req) {
        // Basic Auth semplice: richiedi la password come parametro pwd=...
        // Accetta anche le credenziali dell'header Basic (se vuoi).
        bool auth = false;
        AsyncWebParameter *pwd_get  = req->getParam("pwd", true);
        AsyncWebParameter *pwd_post = req->getParam("pwd", false);
        if ((pwd_get  && strcmp(pwd_get->value().c_str(),  OTA_PASS) == 0) ||
            (pwd_post && strcmp(pwd_post->value().c_str(), OTA_PASS) == 0)) {
            auth = true;
        }
        // Fallback: header Authorization: Basic (user qualunque, pw = AP_PASSWORD)
        if (!auth && req->hasHeader("Authorization")) {
            String hdr = req->header("Authorization");
            if (hdr.startsWith("Basic ")) {
                String b64 = hdr.substring(6);
                // Decode base64 minimo: solo formato "user:password"
                // (non ci affidiamo a librerie esterne). Fallo tramite confronto:
                // generiamo la stringa "any:<pw>" e la confrontiamo in base64.
                String ref_plain = "admin:";
                ref_plain += OTA_PASS;
                String ref_b64 = "";
                // Semplice encoder base64 inline per stringa piccola (< 60 char).
                static const char B64[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                const uint8_t *s = (const uint8_t *)ref_plain.c_str();
                int n = ref_plain.length();
                ref_b64.reserve(((n + 2) / 3) * 4);
                for (int i = 0; i < n; i += 3) {
                    uint32_t v = (uint32_t)s[i] << 16;
                    if (i+1 < n) v |= (uint32_t)s[i+1] << 8;
                    if (i+2 < n) v |= (uint32_t)s[i+2];
                    ref_b64 += B64[(v >> 18) & 0x3F];
                    ref_b64 += B64[(v >> 12) & 0x3F];
                    ref_b64 += (i+1 < n) ? B64[(v >> 6) & 0x3F] : '=';
                    ref_b64 += (i+2 < n) ? B64[(v) & 0x3F] : '=';
                }
                if (b64 == ref_b64) auth = true;
            }
        }
        if (!auth) {
            AsyncWebServerResponse *r = req->beginResponse(401, "text/html; charset=utf-8",
                "<!doctype html><html><head><title>401 OTA</title></head><body style=\"background:#111;color:#eee;font-family:system-ui\">"
                "<h2>⚠️ Accesso OTA non autorizzato</h2>"
                "<p>Password richiesta. Usa:</p>"
                "<pre>http://" + WiFi.localIP().toString() + "/update?pwd=<b style=\"color:#4ade80\">" + String(OTA_PASS) + "</b></pre>"
                "<p>Oppure Basic Auth: user <b>admin</b>, password uguale a quella dell'AP.</p>"
                "</body></html>");
            req->send(r);
            return;
        }
        String html = "<!doctype html><html lang=\"it\"><head><meta charset=\"utf-8\"><title>Aggiornamento OTA</title>";
        html += "<style>"
                "body{background:#0b0f1a;color:#e6ecf6;font-family:system-ui,Segoe UI,Arial;margin:2rem auto;max-width:760px;padding:1rem}"
                "h1{margin-top:0}a{color:#60a5fa}.glass{background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.08);"
                "padding:1.2rem 1.4rem;border-radius:14px;backdrop-filter:blur(6px);margin-bottom:1rem}"
                "input[type=file]{margin:.6rem 0;padding:.5rem;background:#111827;border:1px solid #374151;border-radius:8px;color:#e5e7eb}"
                "button{background:#2563eb;color:#fff;border:0;padding:.7rem 1.1rem;border-radius:10px;font-weight:600;cursor:pointer}"
                "button:hover{background:#1d4ed8}.warn{color:#f59e0b}.ok{color:#4ade80}.bar{height:14px;background:#111827;border-radius:999px;"
                "margin:.6rem 0;overflow:hidden}.bar>span{display:block;height:100%;background:linear-gradient(90deg,#22d3ee,#2563eb);width:0%}</style></head>";
        html += "<body><h1>🔧 Aggiornamento OTA — ESP32-MIC-SPETTRO</h1>"
                "<div class=\"glass\"><p><b>💡 Passaggi:</b></p>"
                "<ol><li>Scegli un file <code>.bin</code>:</li><ul>"
                "<li><b>firmware.bin</b> (<code>.pio/build/lolin_s2_mini/firmware.bin</code>) → Tipo: <b>Firmware</b></li>"
                "<li><b>littlefs.bin</b> (<code>.pio/build/lolin_s2_mini/littlefs.bin</code>) → Tipo: <b>Filesystem (LittleFS)</b></li>"
                "</ul><li>Clicca <b>Carica e Installa</b>: attendi il messaggio <span class=ok>✔️ OK</span>, la scheda si riavvierà da sola.</li>"
                "</ol></div>"
                "<form class=glass id=f method=POST enctype=multipart/form-data action=/update?pwd=" + String(OTA_PASS) + ">"
                "<label><b>Tipo di file:</b><br>"
                "<select id=t name=type style=margin:.4rem 0;padding:.4rem;background:#111827;color:#e5e7eb;border:1px solid #374151;border-radius:8px>"
                "<option value=firmware selected>Firmware (.bin)</option>"
                "<option value=fs>Filesystem LittleFS (.bin)</option>"
                "</select></label><br><br>"
                "<label><b>File:</b><br><input type=file name=update id=file accept=\".bin\"></label><br><br>"
                "<button type=submit>⬆️  Carica e Installa</button>&nbsp;&nbsp;"
                "<a href=\"/\">← Torna allo spettrogramma</a>"
                "<div class=bar><span id=p></span></div>"
                "<div id=st>Attesa file...</div>"
                "</form>"
                "<script>"
                "const f=document.getElementById('f');const st=document.getElementById('st');const p=document.getElementById('p');"
                "f.addEventListener('submit',e=>{"
                "  e.preventDefault();"
                "  const fd=new FormData(f);"
                "  const fl=document.getElementById('file').files[0];"
                "  if(!fl){st.textContent='⚠️ Seleziona un file .bin';return}"
                "  p.style.width='0%';st.textContent='Upload '+fl.name+' ('+fl.size+' byte)...';"
                "  const xhr=new XMLHttpRequest();xhr.open('POST',f.action);"
                "  xhr.upload.onprogress=e=>{if(e.lengthComputable){const w=(100*e.loaded/e.size)|0;p.style.width=w+'%';st.textContent='Upload '+w+'%...'}};"
                "  xhr.onload=()=>{p.style.width='100%';st.innerHTML='Risposta server: <code>'+xhr.responseText+'</code> dopo riavvio automatico (3s)...';"
                "    setTimeout(()=>location.reload(),3000)};"
                "  xhr.onerror=()=>st.textContent='❌ Errore di rete';"
                "  xhr.send(fd);"
                "});<\\/script></body></html>";
        req->send(200, "text/html; charset=utf-8", html);
    });

    // --- Handler upload /update (multipart/form-data) ---
    struct OtaCtx {
        bool     auth_ok;
        uint8_t  partition;  // U_FLASH o U_SPIFFS
        bool     update_started;
        size_t   total_written;
        size_t   expected_size;
    };
    static OtaCtx ota_ctx;
    g_server->on("/update", HTTP_POST,
        [OTA_PASS](AsyncWebServerRequest *req) {
            // Request completa (dopo body).
            // 🟢 LEGGI SIA query-string ?pwd=... (true=GET param)
            //    SIA form field "pwd" (false=POST param).
            //    Prima bug: solo getParam("pwd") → solo POST form, query ignorata.
            AsyncWebParameter *pwd_q = req->getParam("pwd", true);
            AsyncWebParameter *pwd_f = req->getParam("pwd", false);
            ota_ctx.auth_ok =
                (pwd_q && strcmp(pwd_q->value().c_str(), OTA_PASS) == 0) ||
                (pwd_f && strcmp(pwd_f->value().c_str(), OTA_PASS) == 0);
            if (!ota_ctx.auth_ok) {
                req->send(401, "text/plain", "401 password errata. Usa ?pwd=" + String(OTA_PASS) + " o campo form 'pwd'.");
                return;
            }
            if (!ota_ctx.update_started) {
                req->send(400, "text/plain", "❌ Nessun dato ricevuto o Update.begin() fallito.");
                return;
            }
            bool end_ok = Update.end(true);
            SFP("[OTA] Update.end() -> %s, scritti %u/%u byte\n",
                end_ok ? "OK" : "FALLITO",
                (unsigned)ota_ctx.total_written,
                (unsigned)ota_ctx.expected_size);
            if (end_ok) {
                req->send(200, "text/plain; charset=utf-8",
                          "✔️ Aggiornamento completato. Riavvio in 3s...\n"
                          "Partizione: " + String((ota_ctx.partition == U_FLASH) ? "FIRMWARE" : "FILESYSTEM") +
                          "\nScritti " + String(ota_ctx.total_written) + " byte.");
                SFL("[OTA] OK -> ESP.restart() tra poco");
                delay(3000);
                ESP.restart();
            } else {
                req->send(500, "text/plain",
                          "❌ Errore Update.end(): " + String(Update.errorString()) +
                          "\nScritti " + String(ota_ctx.total_written) + "/" + String(ota_ctx.expected_size));
                Update.abort();
            }
        },
        [OTA_PASS, OTA_PART_FW, OTA_PART_FS](AsyncWebServerRequest *req, const String &filename,
                                              size_t index, uint8_t *data, size_t len, bool final) {
            // Chunk upload
            if (index == 0) {
                // Inizio upload: valida pwd e tipo
                // 🟢 LEGGI SIA query-string ?pwd=... SIA form field "pwd".
                AsyncWebParameter *pwdParamQ = req->getParam("pwd", true);
                AsyncWebParameter *pwdParamF = req->getParam("pwd", false);
                ota_ctx.auth_ok =
                    (pwdParamQ && strcmp(pwdParamQ->value().c_str(), OTA_PASS) == 0) ||
                    (pwdParamF && strcmp(pwdParamF->value().c_str(), OTA_PASS) == 0);
                if (!ota_ctx.auth_ok) { req->send(401, "text/plain", "401 password errata"); return; }
                // Tipo partizione
                ota_ctx.partition = OTA_PART_FW;
                AsyncWebParameter *t = req->getParam("type", false);
                if (t && (t->value() == "fs" || filename.endsWith("littlefs.bin") || filename.endsWith("spiffs.bin"))) {
                    ota_ctx.partition = OTA_PART_FS;
                }
                size_t up_size = req->contentLength();
                ota_ctx.expected_size = up_size;
                // Update.begin(0, part) auto-size NON è affidabile su espressif32 core 2.0.9:
                // spesso ritorna false silenziosamente. Passiamo dimensione reale.
                if (up_size == 0) up_size = 1400 * 1024; // fallback max 1.4 MB
                ota_ctx.update_started = Update.begin(up_size, ota_ctx.partition);
                ota_ctx.total_written = 0;
                SFP("[OTA] Inizio upload '%s' tipo=%s Update.begin=%d, err=%s\n",
                    filename.c_str(),
                    (ota_ctx.partition == OTA_PART_FW) ? "FIRMWARE" : "FILESYSTEM",
                    ota_ctx.update_started,
                    ota_ctx.update_started ? "OK" : Update.errorString());
                if (!ota_ctx.update_started) return;
            }
            if (!ota_ctx.update_started) return;
            if (!ota_ctx.auth_ok) return;
            if (len > 0) {
                size_t w = Update.write(data, len);
                if (w != len) {
                    SFP("[OTA] Write fallito: chiesti %u, scritti %u -> %s\n",
                        (unsigned)len, (unsigned)w, Update.errorString());
                } else {
                    ota_ctx.total_written += w;
                }
            }
        }
    );
    // Fine OTA --------------------------------------------------

    g_server->serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("max-age=60");
    // --- Endpoint diagnostico HEAP e Queue length (GET /debug) ---
    // Utile a vedere in tempo reale: heap libero, lunghezza coda audio,
    // numero client WS, PSRAM free, senza aprire monitor seriale o WS.
    g_server->on("/debug", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["heap"]         = (uint32_t)ESP.getFreeHeap();
        doc["heap_min"]     = (uint32_t)ESP.getMinFreeHeap();
        doc["heap_largest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        doc["psram"]        = (uint32_t)ESP.getFreePsram();
        doc["queue_len"]    = (uint32_t)uxQueueMessagesWaiting(g_ring_queue);
        doc["queue_max"]    = (uint32_t)RING_BUFFER_FRAMES;
        doc["ws_clients"]   = (uint32_t)(g_ws ? g_ws->count() : 0);
        doc["seq"]          = (uint32_t)g_seq;
        doc["sample_rate"]  = (uint32_t)g_sample_rate;
        doc["gain_db"]      = (uint8_t)g_gain_db;
        doc["hpf_hz"]       = (uint16_t)g_hpf_hz;
        doc["uptime_ms"]    = (uint32_t)millis();
        doc["rssi"]         = (int8_t)WiFi.RSSI();
        doc["ip"]           = WiFi.localIP().toString();
        char buf[384];
        size_t n = serializeJson(doc, buf, sizeof(buf));
        (void)n;
        req->send(200, "application/json", String(buf));
    });
    g_server->onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "404: risorsa non trovata. Carica il filesystem LittleFS.");
    });
    g_server->begin();
    SFL("[HTTP] Server web avviato sulla porta 80");

    xTaskCreatePinnedToCore(
        audio_task, "audio_acq", AUDIO_TASK_STACK, NULL,
        AUDIO_TASK_PRIORITY, NULL, 0);
    SFP("[BOOT] Setup completato. Picco audio di boot (s24) = %ld (0x%06lX) %s\n",
        (long)g_audio_boot_peak_s24, (unsigned long)g_audio_boot_peak_s24,
        g_audio_boot_peak_s24 > 100 ? "-> MICROFONO OK ✅" : "-> SEGNALE ZERO / MICROFONO SPENTO ❌");
}

// ============================================================
// LOOP: trasmette frame audio via WebSocket e statistiche
// ============================================================
// Strategia anti-blocchi e anti-latenza:
//   • Skip oldest semplice (Nessun malloc loop): se la coda supera
//     HIGH_WATERMARK, si scartano i frame più vecchi finché non si
//     arriva a LOW_WATERMARK e si trasmette solo l'ultimo frame
//     disponibile. Zero allocazioni, zero copie massive.
//   • Schedulazione status NON bloccante per i comandi `set`.
//   • Controllo coda WS interna (AsyncWebSocket) soglia 4KB.
//   • Heartbeat JSON status ogni 3s (no cleanupClients).
void loop() {
    // ==================================================================
    // 🟢 CACHING WiFi.status() 100ms (10 volte/s invece di 66 volte/s!)
    // ==================================================================
    //   ROOT CAUSE FREEZE 40s RISOLTA:
    //   Nel loop erano presenti DUE chiamate WiFi.status() AD OGNI ITERAZIONE
    //   (una in ws_broadcast_audio_frame() e una nella guardia ArduinoOTA):
    //   2 × 33 volte/s = 66 chiamate/s! WiFi.status() ACQUISISCE MUTEX LwIP
    //   GLOBALE. Contesa ripetuta per 40s = ~80000 volte → deadlock mutex.
    //   Risoluzione: UNA SOLA chiamata ogni 100ms (10/s), valore CACHATO.
    static uint32_t wifi_status_last_ms = 0;
    static int wifi_status_cached       = WL_DISCONNECTED;
    if (millis() - wifi_status_last_ms > 100UL) {
        wifi_status_last_ms = millis();
        wifi_status_cached = WiFi.status();
    }

    const UBaseType_t HIGH_WATERMARK = (RING_BUFFER_FRAMES * 9) / 16;  // 36/64
    const UBaseType_t LOW_WATERMARK  = RING_BUFFER_FRAMES / 4;         // 16/64
    static uint32_t dropped = 0;
    static uint32_t last_ws_heartbeat = 0;
    static uint32_t last_cleanup_clients = 0;
    int frames_broadcasted_this_tick = 0;  // reset OGNI tick (NO static!)

    UBaseType_t qlen = uxQueueMessagesWaiting(g_ring_queue);

    // ------------------------------------------------------------------
    // A) BACKPRESSURE ESTREMA: troppi frame in coda → salta i più vecchi
    // ------------------------------------------------------------------
    if (qlen > HIGH_WATERMARK) {
        audio_frame_t tmp;
        // Strip: butta finché queue non scende a LOW_WATERMARK.
        while (uxQueueMessagesWaiting(g_ring_queue) > LOW_WATERMARK &&
               xQueueReceive(g_ring_queue, &tmp, 0) == pdPASS) {
            dropped++;
        }
        // Trasmetti i 4 frames più recenti disponibili (non solo ultimo!):
        // se trasmettiamo SOLO 1 frame, quando il ring ha 32 frame a 16kHz
        // sono 1 secondo di audio, perdere 28/32 = troppi salti visibili.
        int sent = 0;
        audio_frame_t last;
        while (sent < 4 && xQueueReceive(g_ring_queue, &last, 0) == pdPASS) {
            // 🟢 Stessa guardia CACHATA del ramo B: evita WiFi.status() doppio.
            if (wifi_status_cached == WL_CONNECTED) {
                ws_broadcast_audio_frame(last);
                frames_broadcasted_this_tick++;
            }
            sent++;
            if ((frames_broadcasted_this_tick & 1) == 0) taskYIELD();
        }
        // Dopo backpressure estrema: UN TICK DI RIPOSO.
        // Garantisce 1ms di respiro a LwIP prima di nuovo loop.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    // ------------------------------------------------------------------
    // B) Andamento normale: trasmette max 4 frame per tick
    // ------------------------------------------------------------------
    else {
        // B) Andamento NORMALE: 1 SOL0 frame per tick (non 4!).
        //   Root cause crash WiFi: 4 frame/tick × 1000 loop/sec = 4000 frame/sec
        //   di 1036 byte = 4.1 MB/sec teorico >> throughput massimo WiFi ~600KB/s.
        //   Conseguenza: pbuf LwIP saturi, loop 100% task CPU S2 single core,
        //   AsyncWebServer non riesce a rispondere a /debug (Timeout), poi
        //   LwIP perde la connessione e non riconnette (host unreachable).
        //   Soluzione: 1 frame solo per tick, pausa 30ms → 33 trasmissioni/sec
        //   (~34 KB/s) = 1/20 del throughput massimo, MOLTO sicuro.
        audio_frame_t frame;
        if (xQueueReceive(g_ring_queue, &frame, 0) == pdPASS) {
            // 🟢 GUARDIA WL_CONNECTED CACHATA: evita 2° chiamata WiFi.status()
            // dentro ws_broadcast_audio_frame() → vedi inizio loop().
            if (wifi_status_cached == WL_CONNECTED) {
                ws_broadcast_audio_frame(frame);
                frames_broadcasted_this_tick = 1;
            }
        }
        taskYIELD();
        // 🟢 PAUSA 100 ms invece 30ms = FRAME RATE WS 10 fps invece 33 fps!
        //   Root cause freeze 40s CON 1 CLIENT WS: 33 trasmissioni/s × 1036B
        //   = 34 KB/s teorico, ma insieme alle risposte TCP ACK, ai JSON heartbeat,
        //   alla HTTP risposte JS/CSS iniziali saturavano pbuf LwIP della S2
        //   single-core senza PSRAM. 10 fps = 10 trasmissioni/s = 10 KB/s è
        //   MOLTO sotto soglia e per un grafico waterfall/spettro è PIÙ CHE
        //   SUFFICIENTE (occhio umano non distingue oltre 15 fps per grafici
        //   lenti come spettrogramma). Produzione audio: 32ms × 3 = ~100ms,
        //   ogni 3 frame audio = 1 frame WS → lieve jitter invisibile.
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // ------------------------------------------------------------------
    // C) Invio status SCHEDULATO (non bloccante) dopo un comando set
    // ------------------------------------------------------------------
    if (g_status_sched_ms && (int32_t)(millis() - g_status_sched_ms) >= 0) {
        g_status_sched_ms = 0;
        ws_send_status(NULL);
    }

    // ------------------------------------------------------------------
    // D) HEARTBEAT WS: JSON status ogni 10s (era 6s, troppo carico!)
    //   Insieme a 10 fps WS, 10s heartbeat = ~300B JSON /10s negligibile.
    // ------------------------------------------------------------------
    if (g_ws != NULL) {
        const size_t ws_count = g_ws->count();
        if (ws_count > 0 && millis() - last_ws_heartbeat > 10000UL) {
            last_ws_heartbeat = millis();
            ws_send_status(NULL);
        }
        // --- CLEANUP ASYNCTCPSERVER: ogni 15s chiude zombie client ---
        // Senza cleanupClients() la AsyncWebSocket accumula client vecchi
        // e buffer TCP non rilasciati → Heap scende piano piano a 0,
        // poi lampeggio WDT, freeze e dopo refresh riparte (perché new
        // client ne approfitta per rilasciare un po' di memoria).
        if (millis() - last_cleanup_clients > 15000UL) {
            last_cleanup_clients = millis();
            g_ws->cleanupClients();
        }
    }

    // ------------------------------------------------------------------
    // E) LED STAT asincrono (nessun delay bloccante: 60ms di ON)
    // ------------------------------------------------------------------
    if (g_led_off_at_ms != 0UL && (int32_t)(millis() - g_led_off_at_ms) >= 0) {
        LED_OFF();
        g_led_off_at_ms = 0UL;
    }

    // ------------------------------------------------------------------
    // F) STAT periodico (ogni 5s): metriche di debug
    // ------------------------------------------------------------------
    static uint32_t last_stat = 0;
    if (millis() - last_stat > 5000) {
        last_stat = millis();
        UBaseType_t queued = uxQueueMessagesWaiting(g_ring_queue);
        uint32_t heap_free = (uint32_t)ESP.getFreeHeap();
        uint32_t heap_min  = (uint32_t)ESP.getMinFreeHeap();
        uint32_t heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        SFP("[STAT] heap_free=%u heap_min=%u heap_largest=%u psram_free=%u, queue=%u/%u, drop_rb=%lu, ws_qb_drop=%lu, ws_qb_peak=%uB, ws_clients=%u, seq=%lu\n",
            (unsigned)heap_free,
            (unsigned)heap_min,
            (unsigned)heap_largest,
            (unsigned)ESP.getFreePsram(),
            (unsigned)queued, (unsigned)RING_BUFFER_FRAMES,
            (unsigned long)dropped,
            (unsigned long)g_ws_qb_drop,
            (unsigned)g_ws_qb_peak,
            (unsigned)(g_ws ? g_ws->count() : 0),
            (unsigned long)g_seq);
        // Reset picco osservato per prossima finestra STAT
        g_ws_qb_peak = 0;
        // Lampeggio LED: 60ms ON asincrono
        LED_ON();
        g_led_off_at_ms = millis() + 60UL;
    }

    // --- GESTORE ArduinoOTA (protocollo standard espota.py) ---
    //   ⚠️ RATE-LIMIT 250 ms (4 volte/s MAX):
    //   Prima era chiamato OGNI loop (33 volte/s!) e internamente ArduinoOTA
    //   apre/chiude socket UDP porta 3232 AD OGNI CHIAMATA, consumando pbuf
    //   LwIP della ESP32-S2. Ora 4/s MAX + WiFi.status() CACHATO 100ms.
    static uint32_t last_ota_handle = 0;
    if (wifi_status_cached == WL_CONNECTED && (millis() - last_ota_handle) > 250UL) {
        last_ota_handle = millis();
        ArduinoOTA.handle();
    }

    // --- YIELD FINALE + PAUSA 1 ms GARANTITA a LwIP (task PR 4) ---
    //   S2 single-core: senza una pausa minima garantita, tra loop task e
    //   audio task (entrambi PR 1) lo scheduler FreeRTOS può non schedulare
    //   regolarmente LwIP in presenza di carico. 1 ms su 33 ms totali = 3%.
    taskYIELD();
    vTaskDelay(pdMS_TO_TICKS(1));
}
