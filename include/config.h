/*
 * config.h - Configurazione centrale del progetto "Giochi di Luce"
 *
 * Questo file e' il PUNTO UNICO DI VERITA' per:
 *   - Pin del microfono INMP441 (I2S)
 *   - Pin e geometria della matrice WS2812B 8x32
 *   - Credenziali WiFi (modalita' Station + fallback Access Point)
 *   - Limiti e default dei parametri degli effetti (luminosita',
 *     velocita', sensibilita', guadagno microfono)
 *
 * ATTENZIONE: se modifichi i limiti qui, aggiorna anche l'oggetto
 * PARAM_CONFIG in data/app.js per mantenere la consistenza.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
// PIN I2S - MICROFONO INMP441 (ESP32-S2 mini ha UNA sola I2S)
// ============================================================
// Collegamento:
//   INMP441 SCK  -> GPIO 7  (BCLK)
//   INMP441 WS   -> GPIO 9  (LRCLK / Word Select)
//   INMP441 SD   -> GPIO 11 (Dati seriali)
//   INMP441 L/R  -> GND     (solo canale LEFT)
//   INMP441 VDD  -> 3V3
//   INMP441 GND  -> GND
#define I2S_BCLK_PIN      7
#define I2S_WS_PIN        9
#define I2S_DATA_IN_PIN   11
#define I2S_PORT_NUM      I2S_NUM_0   // Unica periferica disponibile su S2
// Canale fisico I2S:
//   0 = forza LEFT  (INMP441 con L/R saldato a GND)
//   1 = forza RIGHT (INMP441 con L/R saldato a 3V3)
//   2 = AUTO: prova LEFT e, se silenzioso, commuta su RIGHT (consigliato:
//       il pin L/R di molti moduli INMP441 e' flottante e il mic puo'
//       emettere sull'uno o sull'altro canale a seconda del lotto).
#define I2S_FORCE_CHANNEL 2

// Campioni per blocco audio. 256 @ 16 kHz = 16 ms -> ~60 blocchi/s.
#define AUDIO_BLOCK_SAMPLES  256
#define AUDIO_SAMPLE_RATE    16000
// DMA I2S: 8 buffer x 256 campioni = 2048 campioni = 128 ms di margine.
#define I2S_DMA_BUF_COUNT    8
#define I2S_DMA_BUF_LEN      AUDIO_BLOCK_SAMPLES

// Timeout di i2s_read espresso in millisecondi. DEVE essere finito, NON
// portMAX_DELAY: se la catena DMA si incastra, un'attesa infinita congela
// per sempre lo stato audio a zero. Con un timeout finito il task riprova.
#define AUDIO_READ_TIMEOUT_MS 200

// ============================================================
// FASTLED - Matrice WS2812B 8 righe x 32 colonne (GPIO5)
// ============================================================
#define FASTLED_DATA_PIN   5      // DIN della matrice -> GPIO5
#define FASTLED_MATRIX_W   32     // colonne (larghezza)
#define FASTLED_MATRIX_H   8      // righe (altezza)
#define FASTLED_NUM_LEDS   (FASTLED_MATRIX_W * FASTLED_MATRIX_H)  // 256 pixel
#define MATRIX_SERPENTINE  true   // cablaggio a serpentina (righe alternate)
#define MATRIX_FLIP_X      false
#define MATRIX_FLIP_Y      false

// ============================================================
// WIFI - Modalita' Station (rete domestica)
// ------------------------------------------------------------
// Le credenziali REALI vivono in include/config_secrets.h, che non e'
// versionato (vedi .gitignore): cosi' il repository pubblico non
// contiene la password di casa. Per configurarle:
//   cp include/config_secrets.example.h include/config_secrets.h
// Se il file non esiste, valgono i valori fittizi qui sotto e la
// scheda parte direttamente in modalita' Access Point.
// ============================================================
#if __has_include("config_secrets.h")
  #include "config_secrets.h"
#endif

#ifndef WIFI_SSID
  #define WIFI_SSID     "NOME_DELLA_TUA_RETE"
#endif
#ifndef WIFI_PASSWORD
  #define WIFI_PASSWORD "PASSWORD_DELLA_TUA_RETE"
#endif

#define WIFI_MAX_RETRIES  10           // Numero massimo di tentativi in STA
#define WIFI_RETRY_DELAY  500          // ms di attesa tra un tentativo e l'altro

// ============================================================
// WIFI - Modalita' Access Point fallback
// ============================================================
#define AP_SSID           "esp32-luci"
#define AP_PASSWORD       "12345678"
#define AP_CHANNEL        1
#define AP_MAX_CONN       4
#define AP_IP             192,168,4,1

// ============================================================
// LIMITI PARAMETRI EFFETTI (coerenti con data/app.js -> PARAM_CONFIG)
// ============================================================

// --- Luminosita' globale matrice (0-255) ---
#define BRIGHTNESS_MIN        5
#define BRIGHTNESS_MAX        255
#define BRIGHTNESS_DEFAULT    110

// --- Velocita' degli effetti (1-100, 50 = nominale) ---
#define SPEED_MIN             1
#define SPEED_MAX             100
#define SPEED_DEFAULT         50

// --- Sensibilita' audio (0-100): amplifica la reazione al suono ---
#define SENSITIVITY_MIN       0
#define SENSITIVITY_MAX       100
#define SENSITIVITY_DEFAULT   50

// --- Guadagno microfono in dB (0-36) ---
#define GAIN_DB_MIN           0
#define GAIN_DB_MAX           36
#define GAIN_DB_DEFAULT       12

// --- Paletta colori di default (indice) ---
#define PALETTE_DEFAULT       0

// --- Auto-random: cambia effetto casuale ogni N millisecondi ---
#define RANDOM_PERIOD_MS      30000

// --- Numero di bande di frequenza calcolate (Goertzel) ---
#define NUM_BANDS             16

// ============================================================
// TASK FREERTOS AUDIO
// ------------------------------------------------------------
// ESP32-S2 e' SINGLE-CORE: la priorita' del task audio DEVE essere
// uguale a quella del loopTask (1), altrimenti affama il loop e
// il web server asincrono. Stack 8 KB (analisi + margine sicurezza).
// ============================================================
#define AUDIO_TASK_STACK      8192
#define AUDIO_TASK_PRIORITY   1

// ============================================================
// CONTROLLO SERIALE
// ============================================================
#define SERIAL_BAUD           115200

#endif // CONFIG_H
