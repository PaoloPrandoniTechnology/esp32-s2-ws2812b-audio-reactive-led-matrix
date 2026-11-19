// ============================================================
//  ESP32-S2 mini + INMP441 + MATRICE WS2812B 8x32 -> "GIOCHI DI LUCE"
// ------------------------------------------------------------
//  Firmware SEMPLICE per giochi di luce sulla matrice 8x32 (256 LED).
//    - Effetti AUTONOMI (non dipendono dal suono)
//    - Effetti AUDIO-REATTIVI (guidati dal microfono INMP441)
//    - Pagina web minimale: scelta effetto, random, guadagno mic,
//      sensibilita', luminosita', velocita', palette + OTA.
//
//  Collegamenti (punto unico di verita': include/config.h)
//    INMP441 : SCK->GPIO7  WS->GPIO9  SD->GPIO11  L/R->GND  VDD->3V3
//    MATRICE : DIN->GPIO5  VCC->+5V esterno   GND->GND comune con ESP32
//
//  Convenzioni: commenti in italiano, costanti MAIUSCOLE,
//  funzioni e variabili snake_case.
// ============================================================

// ============================================================
// GUARDIE COMPILE-TIME USB (NON rimuovere! Safety net bug PIO)
// ============================================================
// PlatformIO Core 6.1.18 Windows ignora i build_flags multi-riga:
// se i flag USB non arrivano al compilatore, la CDC non si inizializza.
// Queste guardie bloccano la build PRIMA di Arduino.h.
#if defined(ARDUINO_USB_CDC_ON_BOOT)
#  define _CAP_USB_CDC_ON_BOOT  ARDUINO_USB_CDC_ON_BOOT
#else
#  define _CAP_USB_CDC_ON_BOOT  (-1)
#endif
#if defined(ARDUINO_USB_MODE)
#  define _CAP_USB_MODE         ARDUINO_USB_MODE
#else
#  define _CAP_USB_MODE         (-1)
#endif
static const int FLAG_USB_CDC_ON_BOOT = _CAP_USB_CDC_ON_BOOT;
static const int FLAG_USB_MODE        = _CAP_USB_MODE;
#if _CAP_USB_CDC_ON_BOOT != 1
#error "CDC on boot NON attivo! Aggiungi -DARDUINO_USB_CDC_ON_BOOT=1 in build_flags (UNA sola riga)."
#endif
#if _CAP_USB_MODE != 0
#error "USB MODE NON e' 0 (TinyUSB nativo)! Aggiungi -DARDUINO_USB_MODE=0 in build_flags (UNA sola riga)."
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <Update.h>
#include <driver/i2s.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <esp_system.h>   // esp_reset_reason()
#include "config.h"

// ============================================================
// WRAPPER SERIALE SAFE (anti-deadlock HWCDC ESP32-S2)
// ------------------------------------------------------------
// BUG HW: se il PC NON ha il monitor seriale aperto (DTR/RTS non
// asserito) Serial.printf/println RAW bloccano la CPU per sempre.
// Tutte le stampe passano da SFP()/SFL() con controllo !Serial +
// availableForWrite + timeout 20 ms.
// ============================================================
static inline int __attribute__((format(printf, 1, 2))) ssp(const char *fmt, ...) {
    if (!Serial) return 0;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    uint32_t t0 = (uint32_t)millis();
    while (Serial.availableForWrite() < n) {
        if ((int32_t)((uint32_t)millis() - t0) > 20) return 0;
        taskYIELD();
    }
    return (int)Serial.write((const uint8_t *)buf, (size_t)n);
}
static inline void ssl(const char *s) {
    if (!Serial) return;
    size_t n = strlen(s);
    if (n > 190) n = 190;
    uint32_t t0 = (uint32_t)millis();
    while (Serial.availableForWrite() < (int)(n + 2)) {
        if ((int32_t)((uint32_t)millis() - t0) > 20) return;
        taskYIELD();
    }
    Serial.println(s);
}
#define SFP ssp
#define SFL ssl

// ============================================================
// LED DI STATO ONBOARD (GPIO15, attivo basso su LOLIN S2 mini)
// ============================================================
#ifndef LED_BUILTIN
#define LED_BUILTIN 15
#endif
static inline void led_setup(void)      { pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, LOW); }
static inline void led_on(void)         { digitalWrite(LED_BUILTIN, HIGH); }
static inline void led_off(void)        { digitalWrite(LED_BUILTIN, LOW); }
static inline void led_pulse(uint32_t ms) { led_on(); delay(ms); led_off(); }
static inline void led_wait_serial_with_blink(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < timeout_ms) {
        led_on();  delay(60);
        led_off(); delay(60);
    }
}

// ============================================================
// UTILITA' MATEMATICHE LEGGERE (nessuna FPU sull'ESP32-S2)
// ------------------------------------------------------------
// Tabella seno precalcolata: gli effetti ne fanno un uso massiccio,
// quindi evitiamo sinf()/cosf() dentro il loop di rendering.
// ============================================================
#define SIN_LUT_SIZE 512
static int16_t g_sin_lut[SIN_LUT_SIZE];
static void sin_lut_init(void) {
    for (int i = 0; i < SIN_LUT_SIZE; ++i) {
        g_sin_lut[i] = (int16_t)(32767.0f * sinf(2.0f * (float)M_PI * (float)i / (float)SIN_LUT_SIZE));
    }
}
// seno di un angolo in radianti, periodo 2*PI (wrap automatico)
static inline float fsin(float ph) {
    int idx = (int)(ph * ((float)SIN_LUT_SIZE / (2.0f * (float)M_PI)));
    idx &= (SIN_LUT_SIZE - 1);
    return (float)g_sin_lut[idx] * (1.0f / 32767.0f);
}
static inline float fclamp(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ============================================================
// MATRICE WS2812B 8x32
// ============================================================
static CRGB leds[FASTLED_NUM_LEDS];

// Conversione (x,y) -> indice LED, con cablaggio a serpentina opzionale.
static inline uint16_t matrix_xy(uint8_t x, uint8_t y) {
    if (MATRIX_FLIP_X) x = (uint8_t)(FASTLED_MATRIX_W - 1 - x);
    if (MATRIX_FLIP_Y) y = (uint8_t)(FASTLED_MATRIX_H - 1 - y);
#if MATRIX_SERPENTINE
    if (y & 1) return (uint16_t)(y * FASTLED_MATRIX_W + (FASTLED_MATRIX_W - 1 - x));
#endif
    return (uint16_t)(y * FASTLED_MATRIX_W + x);
}
static inline void px(uint8_t x, uint8_t y, const CRGB &c) {
    if (x >= FASTLED_MATRIX_W || y >= FASTLED_MATRIX_H) return;
    leds[matrix_xy(x, y)] = c;
}
static inline void px_add(uint8_t x, uint8_t y, const CRGB &c) {
    if (x >= FASTLED_MATRIX_W || y >= FASTLED_MATRIX_H) return;
    leds[matrix_xy(x, y)] += c;
}
static void clear_all(void) { fill_solid(leds, FASTLED_NUM_LEDS, CRGB::Black); }

// ============================================================
// PALETTE COLORI (9 palette, definite in firmware)
// ============================================================
DEFINE_GRADIENT_PALETTE(gp_arcobaleno) {
    0, 255,   0,   0,
   42, 255, 255,   0,
   85,   0, 255,   0,
  128,   0, 255, 255,
  170,   0,   0, 255,
  213, 255,   0, 255,
  255, 255,   0,   0,
};
DEFINE_GRADIENT_PALETTE(gp_festa) {
    0, 255,   0, 128,
   64, 255, 128,   0,
  128,   0, 255,   0,
  192,   0, 128, 255,
  255, 255,   0, 128,
};
DEFINE_GRADIENT_PALETTE(gp_oceano) {
    0,   0,  10,  40,
   85,   0,  80, 160,
  170,   0, 180, 200,
  255, 160, 255, 255,
};
DEFINE_GRADIENT_PALETTE(gp_lava) {
    0,  10,   0,   0,
   64, 120,   0,   0,
  128, 255,  60,   0,
  192, 255, 180,   0,
  255, 255, 255, 120,
};
DEFINE_GRADIENT_PALETTE(gp_foresta) {
    0,   0,  20,   0,
   85,   0, 100,   0,
  170,  80, 200,  20,
  255, 220, 255, 120,
};
DEFINE_GRADIENT_PALETTE(gp_neon) {
    0, 255,   0, 255,
   85,   0, 255, 255,
  170, 255, 255,   0,
  255, 255,   0, 255,
};
DEFINE_GRADIENT_PALETTE(gp_tramonto) {
    0,  20,   0,  60,
   64, 120,   0, 100,
  128, 255,  60,  80,
  192, 255, 160,   0,
  255, 255, 240, 180,
};
DEFINE_GRADIENT_PALETTE(gp_ghiaccio) {
    0,   0,  20,  60,
   85,   0, 120, 200,
  170, 120, 220, 255,
  255, 255, 255, 255,
};
DEFINE_GRADIENT_PALETTE(gp_pastello) {
    0, 255, 180, 200,
   64, 255, 220, 160,
  128, 180, 255, 200,
  192, 180, 200, 255,
  255, 230, 180, 255,
};

static const TProgmemRGBGradientPalettePtr g_palette_list[] = {
    gp_arcobaleno, gp_festa, gp_oceano, gp_lava, gp_foresta,
    gp_neon, gp_tramonto, gp_ghiaccio, gp_pastello
};
#define NUM_PALETTES ((uint8_t)(sizeof(g_palette_list) / sizeof(g_palette_list[0])))
static const char *g_palette_names[NUM_PALETTES] = {
    "Arcobaleno", "Festa", "Oceano", "Lava", "Foresta",
    "Neon", "Tramonto", "Ghiaccio", "Pastello"
};
static CRGBPalette16 g_pal;

// ============================================================
// STATO AUDIO CONDIVISO (scritto dal task audio, letto dal render)
// ============================================================
struct AudioState {
    float    level;                 // livello RMS 0..1 (smussato)
    float    peak;                  // picco 0..1 (decadimento rapido)
    float    bass;                  // energia bassi 0..1
    float    mid;                   // energia medi 0..1
    float    treble;                // energia alti 0..1
    float    bands[NUM_BANDS];      // 16 bande 0..1
    float    beat;                  // inviluppo impulso ritmico 0..1
    uint32_t beat_count;            // contatore impulsi
    bool     signal;                // true se c'e' segnale recente
    // --- Diagnostica I2S (visibile su /debug) ---
    int32_t  raw_peak;              // picco |campione| 24-bit dell'ultimo blocco
    int32_t  raw0;                  // primo campione grezzo (segno) dell'ultimo blocco
    uint32_t rd_ok;                 // letture complete riuscite
    uint32_t rd_fail;               // letture fallite / parziali
};
static AudioState       g_audio;
static AudioState       g_snap;     // snapshot coerente per il rendering
static portMUX_TYPE     g_audio_mux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// PARAMETRI RUNTIME (modificabili da web, mai da reboot)
// ============================================================
struct Params {
    uint8_t effect;       // indice effetto corrente
    bool    random_on;    // cambio effetto casuale periodico
    uint8_t brightness;   // 5..255
    uint8_t speed;        // 1..100 (50 = nominale)
    uint8_t sensitivity;  // 0..100
    uint8_t gain_db;      // 0..36 (guadagno microfono)
    uint8_t palette;      // 0..8
};
static Params   g_params;
static volatile bool g_gain_dirty = true;   // ricalcolo fattore guadagno nel task audio

// ============================================================
// CLOCK DI ANIMAZIONE E CONTESTO FRAME
// ============================================================
static uint32_t g_now_ms  = 0;       // millis() dell'ultimo frame
static float    g_anim_t  = 0.0f;    // tempo di animazione (scalato da speed)
static uint32_t g_last_frame_ms = 0;

// ============================================================
// FIRMA DEGLI EFFETTI
// ============================================================
typedef void (*fx_fn)(float dt);
static void fx_off(float dt);
static void fx_rainbow(float dt);
static void fx_plasma(float dt);
static void fx_fire(float dt);
static void fx_matrix_rain(float dt);
static void fx_rgb_waves(float dt);
static void fx_sparkle(float dt);
static void fx_comets(float dt);
static void fx_confetti(float dt);
static void fx_static_noise(float dt);
static void fx_fireworks(float dt);
static void fx_metaball(float dt);
static void fx_bouncing(float dt);
static void fx_lava_lamp(float dt);
static void fx_ripples(float dt);
static void fx_pulse_gradient(float dt);
static void fx_snakes(float dt);
static void fx_spectrum_bars(float dt);
static void fx_center_bars(float dt);
static void fx_vu_meter(float dt);
static void fx_beat_flash(float dt);
static void fx_color_organ(float dt);
static void fx_reactive_fire(float dt);
static void fx_reactive_rainbow(float dt);
static void fx_reactive_rings(float dt);
static void fx_eq_blocks(float dt);
static void fx_audio_ball(float dt);

static const fx_fn g_fx[] = {
    fx_off, fx_rainbow, fx_plasma, fx_fire, fx_matrix_rain,
    fx_rgb_waves, fx_sparkle, fx_comets, fx_confetti, fx_static_noise,
    fx_fireworks, fx_metaball, fx_bouncing, fx_lava_lamp, fx_ripples,
    fx_pulse_gradient, fx_snakes,
    fx_spectrum_bars, fx_center_bars, fx_vu_meter, fx_beat_flash,
    fx_color_organ, fx_reactive_fire, fx_reactive_rainbow, fx_reactive_rings,
    fx_eq_blocks, fx_audio_ball
};
#define FX_COUNT ((uint8_t)(sizeof(g_fx) / sizeof(g_fx[0])))

static const char *g_fx_names[] = {
    "Spento", "Arcobaleno", "Plasma", "Fuoco", "Pioggia Digitale",
    "Onde RGB", "Scintille", "Comete", "Coriandoli", "Interferenza",
    "Fuochi d'Artificio", "Metaball", "Palle Rimbalzanti", "Lampada Lava",
    "Onde Concentriche", "Gradiente Pulsante", "Serpenti",
    "Barre di Spettro", "Barre Centrali", "VU Meter", "Lampo sul Beat",
    "Organo di Colori", "Fuoco Reattivo", "Arcobaleno Reattivo",
    "Anelli Reattivi", "Blocchi EQ", "Palla Audio"
};
// Effetti audio-reattivi (solo informativo per la UI): da fx_spectrum_bars in poi.
#define FIRST_AUDIO_FX 17

//STATO EFFETTO (reset al cambio)
static uint8_t g_fx_prev = 0xFF;
static void fx_reset_state(void);

//Web server / websocket (definiti nella parte finale)
static AsyncWebServer  g_server(80);
static AsyncWebSocket  g_ws("/ws");
static bool            g_ws_mode_ap = false;

// ============================================================
// STATO CONDIVISO DEGLI EFFETTI (resettato ad ogni cambio effetto)
// ============================================================
static CRGBPalette16 g_fire_pal;                 // palette dedicata fuoco (Lava)
static uint8_t  fx_heat[FASTLED_MATRIX_W][FASTLED_MATRIX_H];
static float    fx_px[6], fx_py[6], fx_vx[6], fx_vy[6];
static uint8_t  fx_hue[6];
struct FxParticle { float x, y, vx, vy; int16_t life; uint8_t hue; };
static FxParticle fx_parts[64];
static float    fx_ring_r[5], fx_ring_x[5], fx_ring_y[5];
static uint8_t  fx_ring_h[5];
static float    fx_head[FASTLED_MATRIX_W];
static uint8_t  fx_sx[6], fx_sy[6];
static float    fx_ball_x = 16.0f, fx_ball_y = 4.0f;
static float    fx_fireworks_next = 0.0f;

// Azzera lo stato di tutti gli effetti (chiamata al cambio effetto).
static void fx_reset_state(void) {
    memset(fx_heat, 0, sizeof(fx_heat));
    memset(fx_parts, 0, sizeof(fx_parts));
    for (uint8_t i = 0; i < 6; i++) {
        fx_px[i]  = (float)random8(2, FASTLED_MATRIX_W - 2);
        fx_py[i]  = (float)random8(1, FASTLED_MATRIX_H - 1);
        fx_vx[i]  = (float)(random8(0, 2) ? 1 : -1) * (18.0f + (float)random8(0, 22));
        fx_vy[i]  = (float)(random8(0, 2) ? 1 : -1) * (18.0f + (float)random8(0, 22));
        fx_hue[i] = (uint8_t)(i * 45);
        fx_sx[i]  = random8(FASTLED_MATRIX_W);
        fx_sy[i]  = random8(FASTLED_MATRIX_H);
    }
    for (uint8_t i = 0; i < FASTLED_MATRIX_W; i++) fx_head[i] = -(float)random8(0, FASTLED_MATRIX_H);
    for (uint8_t i = 0; i < 5; i++) {
        fx_ring_r[i] = (float)i * 5.0f;
        fx_ring_x[i] = 16.0f;
        fx_ring_y[i] = 4.0f;
        fx_ring_h[i] = (uint8_t)(i * 55);
    }
    fx_ball_x = FASTLED_MATRIX_W / 2.0f;
    fx_ball_y = FASTLED_MATRIX_H / 2.0f;
    fx_fireworks_next = 0.0f;
    clear_all();
}

// ============================================================
// ==== EFFETTI AUTONOMI ====
// ============================================================

// 0 - Spento
static void fx_off(float dt) { (void)dt; clear_all(); }

// 1 - Arcobaleno diagonale scorrevole
static void fx_rainbow(float dt) {
    static float off = 0.0f;
    off += dt * 45.0f;
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++)
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
            px(x, y, CHSV((uint8_t)(off + (float)x * 6.0f + (float)y * 15.0f), 255, 255));
}

// 2 - Plasma (interferenza di 4 sinusoidi)
static void fx_plasma(float dt) {
    static float off = 0.0f;
    off += dt * 1.3f;
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++) {
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
            float v = fsin((float)x * 0.35f + off * 2.0f)
                    + fsin((float)y * 0.55f - off * 1.7f)
                    + fsin((float)(x + y) * 0.22f + off * 1.3f)
                    + fsin((float)(x - y) * 0.30f - off * 0.9f);
            px(x, y, CHSV((uint8_t)((v + 4.0f) * 31.0f), 210, 255));
        }
    }
}

// 3 - Fuoco (automatico, diffusione per colonna)
static void fx_fire(float dt) {
    (void)dt;
    for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
        for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++)
            fx_heat[x][y] = qsub8(fx_heat[x][y], random8(0, 20));
    for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
        for (uint8_t y = 0; y < FASTLED_MATRIX_H - 1; y++)
            fx_heat[x][y] = (uint8_t)(((uint16_t)fx_heat[x][y + 1] * 3u + fx_heat[x][y]) / 4u);
    for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
        if (random8() < 130) fx_heat[x][0] = qadd8(fx_heat[x][0], random8(150, 255));
        else                 fx_heat[x][0] = random8(0, 50);
    }
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++)
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
            px(x, (uint8_t)(FASTLED_MATRIX_H - 1 - y),
               ColorFromPalette(g_fire_pal, fx_heat[x][y], 255, LINEARBLEND));
}

// 4 - Pioggia Digitale (gocce verdi con scia)
static void fx_matrix_rain(float dt) {
    for (uint16_t i = 0; i < FASTLED_NUM_LEDS; i++) {
        leds[i].r = scale8(leds[i].r, 205);
        leds[i].g = scale8(leds[i].g, 230);
        leds[i].b = scale8(leds[i].b, 205);
    }
    for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
        fx_head[x] += dt * (7.0f + (float)(x % 5));
        if (fx_head[x] > (float)FASTLED_MATRIX_H + 1.5f) fx_head[x] = -(float)random8(0, 8);
        int y = (int)fx_head[x];
        if (y >= 0 && y < FASTLED_MATRIX_H) {
            px(x, (uint8_t)y, CRGB(190, 255, 200));
            if (y + 1 < FASTLED_MATRIX_H) px(x, (uint8_t)(y + 1), CRGB(0, 230, 40));
            if (y + 2 < FASTLED_MATRIX_H) px(x, (uint8_t)(y + 2), CRGB(0, 130, 20));
        }
    }
}

// 5 - Onde RGB (tre sinusoidi indipendenti R/G/B)
static void fx_rgb_waves(float dt) {
    static float off = 0.0f;
    off += dt * 2.4f;
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++) {
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
            uint8_t r = (uint8_t)(128.0f + 127.0f * fsin((float)x * 0.30f + off * 1.6f));
            uint8_t g = (uint8_t)(128.0f + 127.0f * fsin((float)x * 0.20f - (float)y * 0.55f + off * 1.2f));
            uint8_t b = (uint8_t)(128.0f + 127.0f * fsin((float)y * 0.75f + off * 2.0f));
            px(x, y, CRGB(r, g, b));
        }
    }
}

// 6 - Scintille (puntini casuali con breve scia)
static void fx_sparkle(float dt) {
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 30);
    int n = 2 + (int)(dt * 50.0f);
    if (n > 10) n = 10;
    for (int i = 0; i < n; i++)
        px(random8(FASTLED_MATRIX_W), random8(FASTLED_MATRIX_H), CHSV(random8(), 255, 255));
}

// 7 - Comete (palline luminose con scia che rimbalzano)
static void fx_comets(float dt) {
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 55);
    for (uint8_t i = 0; i < 5; i++) {
        fx_px[i] += fx_vx[i] * dt;
        fx_py[i] += fx_vy[i] * dt;
        if (fx_px[i] < 0.0f) { fx_px[i] = 0.0f; fx_vx[i] = -fx_vx[i]; }
        if (fx_px[i] > (float)(FASTLED_MATRIX_W - 1)) { fx_px[i] = (float)(FASTLED_MATRIX_W - 1); fx_vx[i] = -fx_vx[i]; }
        if (fx_py[i] < 0.0f) { fx_py[i] = 0.0f; fx_vy[i] = -fx_vy[i]; }
        if (fx_py[i] > (float)(FASTLED_MATRIX_H - 1)) { fx_py[i] = (float)(FASTLED_MATRIX_H - 1); fx_vy[i] = -fx_vy[i]; }
        px((uint8_t)fx_px[i], (uint8_t)fx_py[i], CRGB::White);
    }
}

// 8 - Coriandoli (puntini colorati persistenti con tinta che scorre)
static void fx_confetti(float dt) {
    static float hue = 0.0f;
    hue += dt * 25.0f;
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 6);
    int n = 4 + (int)(dt * 40.0f);
    if (n > 14) n = 14;
    for (int i = 0; i < n; i++)
        px(random8(FASTLED_MATRIX_W), random8(FASTLED_MATRIX_H),
           CHSV((uint8_t)(hue + (float)random8(0, 90)), 255, 255));
}

// 9 - Interferenza (rumore bianco tipo TV)
static void fx_static_noise(float dt) {
    (void)dt;
    for (uint16_t i = 0; i < FASTLED_NUM_LEDS; i++) {
        uint8_t v = random8();
        v = (v < 190) ? 0 : (uint8_t)((v - 190) * 4);
        leds[i] = CRGB(v, v, v);
    }
}

// 10 - Fuochi d'Artificio (particelle con gravita')
static void fx_fireworks(float dt) {
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 50);
    for (int i = 0; i < 64; i++) {
        FxParticle &p = fx_parts[i];
        if (p.life <= 0) continue;
        p.vy += 150.0f * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.life--;
        if (p.x < 0.0f || p.x > (float)(FASTLED_MATRIX_W - 1)) continue;
        if (p.y < 0.0f || p.y > (float)(FASTLED_MATRIX_H - 1)) continue;
        px((uint8_t)(p.x + 0.5f), (uint8_t)(p.y + 0.5f), CHSV(p.hue, 255, 255));
    }
    fx_fireworks_next -= dt;
    if (fx_fireworks_next <= 0.0f) {
        fx_fireworks_next = 0.7f + (float)random8(0, 90) / 100.0f;
        uint8_t hue = random8();
        float cx = 4.0f + (float)random8(0, FASTLED_MATRIX_W - 8);
        float cy = 1.0f + (float)random8(0, FASTLED_MATRIX_H - 4);
        int spawned = 0;
        for (int i = 0; i < 64 && spawned < 20; i++) {
            if (fx_parts[i].life > 0) continue;
            float ang = (float)spawned * (2.0f * (float)M_PI / 20.0f);
            float spd = 9.0f + (float)random8(0, 6);
            fx_parts[i].x = cx;
            fx_parts[i].y = cy;
            fx_parts[i].vx = fsin(ang) * spd;
            fx_parts[i].vy = fsin(ang + 1.5708f) * spd;
            fx_parts[i].hue = (uint8_t)(hue + random8(0, 30));
            fx_parts[i].life = 28;
            spawned++;
        }
    }
}

// 11 - Metaball (3 sfere morbide che si fondono)
static void fx_metaball(float dt) {
    for (uint8_t i = 0; i < 3; i++) {
        fx_px[i] += fx_vx[i] * dt * 0.6f;
        fx_py[i] += fx_vy[i] * dt * 0.6f;
        if (fx_px[i] < 1.0f || fx_px[i] > (float)(FASTLED_MATRIX_W - 2)) fx_vx[i] = -fx_vx[i];
        if (fx_py[i] < 1.0f || fx_py[i] > (float)(FASTLED_MATRIX_H - 2)) fx_vy[i] = -fx_vy[i];
    }
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++) {
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
            CRGB acc(0, 0, 0);
            for (uint8_t i = 0; i < 3; i++) {
                float dx = (float)x - fx_px[i];
                float dy = ((float)y - fx_py[i]) * 2.6f;
                float infl = 8.0f / (dx * dx + dy * dy + 0.6f);
                if (infl > 1.0f) infl = 1.0f;
                CRGB c = CHSV(fx_hue[i], 240, 255);
                c.nscale8((uint8_t)(infl * 255.0f));
                acc += c;
            }
            px(x, y, acc);
        }
    }
}

// 12 - Palle Rimbalzanti (gravita' + rimbalzo)
static void fx_bouncing(float dt) {
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 35);
    for (uint8_t i = 0; i < 4; i++) {
        fx_vy[i] += 110.0f * dt;
        fx_px[i] += fx_vx[i] * dt;
        fx_py[i] += fx_vy[i] * dt;
        if (fx_px[i] < 0.0f) { fx_px[i] = 0.0f; fx_vx[i] = -fx_vx[i]; }
        if (fx_px[i] > (float)(FASTLED_MATRIX_W - 1)) { fx_px[i] = (float)(FASTLED_MATRIX_W - 1); fx_vx[i] = -fx_vx[i]; }
        if (fx_py[i] > (float)(FASTLED_MATRIX_H - 1)) { fx_py[i] = (float)(FASTLED_MATRIX_H - 1); fx_vy[i] = -0.85f * fx_vy[i]; }
        if (fx_py[i] < 0.0f) { fx_py[i] = 0.0f; fx_vy[i] = -0.85f * fx_vy[i]; }
        px((uint8_t)fx_px[i], (uint8_t)fx_py[i], CHSV(fx_hue[i], 255, 255));
    }
}

// 13 - Lampada Lava (blob che salgono lentamente)
static void fx_lava_lamp(float dt) {
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 16);
    for (uint8_t i = 0; i < 5; i++) {
        fx_py[i] -= fx_vy[i] * dt * 0.5f;
        fx_px[i] += fsin(g_anim_t * 0.7f + (float)i * 1.7f) * dt * 3.0f;
        if (fx_py[i] < -2.0f) { fx_py[i] = (float)FASTLED_MATRIX_H + 2.0f; fx_px[i] = (float)random8(FASTLED_MATRIX_W); }
        while (fx_px[i] < 0.0f) fx_px[i] += (float)FASTLED_MATRIX_W;
        while (fx_px[i] > (float)(FASTLED_MATRIX_W - 1)) fx_px[i] -= (float)FASTLED_MATRIX_W;
        int bx = (int)fx_px[i];
        int by = (int)fx_py[i];
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++)
                px_add((uint8_t)(bx + ox), (uint8_t)(by + oy),
                       CHSV(fx_hue[i], 220, (ox == 0 && oy == 0) ? 255 : 80));
    }
}

// 14 - Onde Concentriche (anelli che si espandono da centri casuali)
static void fx_ripples(float dt) {
    clear_all();
    for (uint8_t i = 0; i < 4; i++) {
        fx_ring_r[i] += dt * 9.0f;
        if (fx_ring_r[i] > 24.0f) {
            fx_ring_r[i] = 0.0f;
            fx_ring_x[i] = (float)(3 + random8(FASTLED_MATRIX_W - 6));
            fx_ring_y[i] = (float)(1 + random8(FASTLED_MATRIX_H - 2));
            fx_ring_h[i] = random8();
        }
        float r = fx_ring_r[i];
        float r2 = r * r;
        float band = r * 2.4f + 0.8f;
        for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++) {
            for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
                float dx = (float)x - fx_ring_x[i];
                float dy = ((float)y - fx_ring_y[i]) * 2.2f;
                float diff = (dx * dx + dy * dy) - r2;
                if (diff > -band && diff < band) {
                    float w = 1.0f - ((diff < 0.0f ? -diff : diff) / band);
                    px_add(x, y, CHSV(fx_ring_h[i], 255, (uint8_t)(w * 255.0f)));
                }
            }
        }
    }
}

// 15 - Gradiente Pulsante (palette che respira)
static void fx_pulse_gradient(float dt) {
    static float off = 0.0f;
    off += dt * 16.0f;
    float breathe = 0.45f + 0.55f * (0.5f + 0.5f * fsin(g_anim_t * 1.4f));
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++)
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
            px(x, y, ColorFromPalette(g_pal, (uint8_t)(off + (float)x * 7.0f + (float)y * 3.0f),
                                      (uint8_t)(255.0f * breathe), LINEARBLEND));
}

// 16 - Serpenti (4 camminatori casuali con scia)
static void fx_snakes(float dt) {
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 28);
    static float acc = 0.0f;
    acc += dt * 22.0f;
    int steps = (int)acc;
    if (steps > 10) steps = 10;
    acc -= (float)steps;
    for (int s = 0; s < steps; s++) {
        for (uint8_t i = 0; i < 4; i++) {
            int dir = random8(0, 4);
            if (dir == 0)      fx_sx[i]++;
            else if (dir == 1) fx_sx[i]--;
            else if (dir == 2) fx_sy[i]++;
            else               fx_sy[i]--;
            fx_sx[i] &= (FASTLED_MATRIX_W - 1);
            fx_sy[i] &= (FASTLED_MATRIX_H - 1);
            px(fx_sx[i], fx_sy[i], CHSV(fx_hue[i], 255, 255));
        }
    }
}

// ============================================================
// ==== EFFETTI AUDIO-REATTIVI ====
// ============================================================

// 17 - Barre di Spettro (16 bande dal basso)
static void fx_spectrum_bars(float dt) {
    (void)dt;
    clear_all();
    const uint8_t w_per = FASTLED_MATRIX_W / NUM_BANDS;
    for (uint8_t b = 0; b < NUM_BANDS; b++) {
        int h = (int)(g_snap.bands[b] * (float)FASTLED_MATRIX_H + 0.5f);
        if (h > FASTLED_MATRIX_H) h = FASTLED_MATRIX_H;
        for (int y = 0; y < h; y++) {
            CRGB c = ColorFromPalette(g_pal, (uint8_t)(b * 16 + y * 12), 255, LINEARBLEND);
            for (uint8_t k = 0; k < w_per; k++)
                px((uint8_t)(b * w_per + k), (uint8_t)(FASTLED_MATRIX_H - 1 - y), c);
        }
    }
}

// 18 - Barre Centrali (16 bande dal centro verso l'alto e il basso)
static void fx_center_bars(float dt) {
    (void)dt;
    clear_all();
    const uint8_t w_per = FASTLED_MATRIX_W / NUM_BANDS;
    const uint8_t mid = FASTLED_MATRIX_H / 2;
    for (uint8_t b = 0; b < NUM_BANDS; b++) {
        int h = (int)(g_snap.bands[b] * (float)mid + 0.5f);
        if (h > mid) h = mid;
        for (int y = 0; y < h; y++) {
            CRGB c = ColorFromPalette(g_pal, (uint8_t)(b * 16 + y * 22), 255, LINEARBLEND);
            for (uint8_t k = 0; k < w_per; k++) {
                uint8_t xx = (uint8_t)(b * w_per + k);
                px(xx, (uint8_t)(mid - 1 - y), c);
                px(xx, (uint8_t)(mid + y), c);
            }
        }
    }
}

// 19 - VU Meter (riempimento progressivo verde/giallo/rosso)
static void fx_vu_meter(float dt) {
    (void)dt;
    clear_all();
    int fill = (int)(g_snap.level * (float)FASTLED_NUM_LEDS);
    if (fill > FASTLED_NUM_LEDS) fill = FASTLED_NUM_LEDS;
    for (int i = 0; i < fill; i++) {
        CRGB c;
        if (i > (int)(FASTLED_NUM_LEDS * 92 / 100))      c = CRGB(255, 0, 0);
        else if (i > (int)(FASTLED_NUM_LEDS * 75 / 100)) c = CRGB(255, 180, 0);
        else                                             c = CRGB(0, 220, 60);
        leds[i] = c;
    }
}

// 20 - Lampo sul Beat (tutto il pannello lampeggia a ritmo)
static void fx_beat_flash(float dt) {
    static float flash = 0.0f;
    static float hue = 0.0f;
    hue += dt * 30.0f;
    flash -= flash * fclamp(dt * 7.0f, 0.0f, 1.0f);
    if (g_snap.beat > flash) flash = g_snap.beat;
    fill_solid(leds, FASTLED_NUM_LEDS, CHSV((uint8_t)hue, 200, (uint8_t)(fclamp(flash, 0.0f, 1.0f) * 255.0f)));
}

// 21 - Organo di Colori (bassi=rosso, medi=verde, alti=blu)
static void fx_color_organ(float dt) {
    (void)dt;
    uint8_t r = (uint8_t)(fclamp(g_snap.bass, 0.0f, 1.0f) * 255.0f);
    uint8_t g = (uint8_t)(fclamp(g_snap.mid, 0.0f, 1.0f) * 255.0f);
    uint8_t b = (uint8_t)(fclamp(g_snap.treble, 0.0f, 1.0f) * 255.0f);
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++) {
        uint8_t f = (uint8_t)(210 - (int)y * 14);
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
            CRGB c(r, g, b);
            c.nscale8(f);
            px(x, y, c);
        }
    }
}

// 22 - Fuoco Reattivo (fiamme spinte dal livello audio)
static void fx_reactive_fire(float dt) {
    (void)dt;
    uint8_t boost = (uint8_t)(fclamp(g_snap.level * 2.0f, 0.0f, 1.0f) * 120.0f);
    for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
        for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++)
            fx_heat[x][y] = qsub8(fx_heat[x][y], random8(0, 15));
    for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
        for (uint8_t y = 0; y < FASTLED_MATRIX_H - 1; y++)
            fx_heat[x][y] = (uint8_t)(((uint16_t)fx_heat[x][y + 1] * 3u + fx_heat[x][y]) / 4u);
    for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
        if (random8() < 150) fx_heat[x][0] = qadd8(fx_heat[x][0], qadd8(random8(120, 255), boost));
        else                 fx_heat[x][0] = qadd8(random8(0, 50), boost);
    }
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++)
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
            px(x, (uint8_t)(FASTLED_MATRIX_H - 1 - y),
               ColorFromPalette(g_fire_pal, fx_heat[x][y], 255, LINEARBLEND));
}

// 23 - Arcobaleno Reattivo (scorre e si accende col suono)
static void fx_reactive_rainbow(float dt) {
    static float off = 0.0f;
    float lvl = fclamp(g_snap.level, 0.0f, 1.0f);
    off += dt * (20.0f + lvl * 140.0f);
    uint8_t val = (uint8_t)(70.0f + lvl * 185.0f);
    uint8_t sat = (uint8_t)(255.0f - lvl * 90.0f);
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++)
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++)
            px(x, y, CHSV((uint8_t)(off + (float)x * 6.0f + (float)y * 14.0f), sat, val));
}

// 24 - Anelli Reattivi (un anello parte ad ogni colpo di basso)
static void fx_reactive_rings(float dt) {
    static float r = 0.0f;
    static uint8_t hue = 0;
    static uint32_t last_beat = 0;
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 40);
    if (g_snap.beat_count != last_beat) {
        last_beat = g_snap.beat_count;
        r = 0.0f;
        hue += 40;
    }
    r += dt * (10.0f + g_snap.level * 30.0f);
    if (r > 30.0f) r = 30.0f;
    float r2 = r * r;
    float band = r * 2.0f + 1.0f;
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++) {
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
            float dx = (float)x - 15.5f;
            float dy = ((float)y - 3.5f) * 2.2f;
            float diff = (dx * dx + dy * dy) - r2;
            if (diff > -band && diff < band) {
                float w = 1.0f - ((diff < 0.0f ? -diff : diff) / band);
                px_add(x, y, CHSV(hue, 255, (uint8_t)(w * 255.0f)));
            }
        }
    }
}

// 25 - Blocchi EQ (16 colonne a segmenti, stile equalizzatore)
static void fx_eq_blocks(float dt) {
    (void)dt;
    clear_all();
    const uint8_t w_per = FASTLED_MATRIX_W / NUM_BANDS;
    const uint8_t segs = FASTLED_MATRIX_H;
    for (uint8_t b = 0; b < NUM_BANDS; b++) {
        int lit = (int)(g_snap.bands[b] * (float)segs + 0.5f);
        for (int y = 0; y < segs; y++) {
            CRGB c = (y < lit) ? ColorFromPalette(g_pal, (uint8_t)(b * 16 + y * 8), 255, LINEARBLEND)
                               : CRGB(0, 0, 0);
            for (uint8_t k = 0; k < w_per; k++)
                px((uint8_t)(b * w_per + k), (uint8_t)(FASTLED_MATRIX_H - 1 - y), c);
        }
    }
}

// 26 - Palla Audio (sfera guidata da bassi/alti, dimensione dal livello)
static void fx_audio_ball(float dt) {
    fadeToBlackBy(leds, FASTLED_NUM_LEDS, 55);
    float tx = 3.0f + fclamp(g_snap.bass, 0.0f, 1.0f) * (float)(FASTLED_MATRIX_W - 6);
    float ty = 1.0f + fclamp(g_snap.treble, 0.0f, 1.0f) * (float)(FASTLED_MATRIX_H - 2);
    fx_ball_x += (tx - fx_ball_x) * fclamp(dt * 8.0f, 0.0f, 1.0f);
    fx_ball_y += (ty - fx_ball_y) * fclamp(dt * 8.0f, 0.0f, 1.0f);
    float size = 3.0f + g_snap.level * 6.0f;
    for (uint8_t y = 0; y < FASTLED_MATRIX_H; y++) {
        for (uint8_t x = 0; x < FASTLED_MATRIX_W; x++) {
            float dx = (float)x - fx_ball_x;
            float dy = ((float)y - fx_ball_y) * 2.4f;
            float infl = size / (dx * dx + dy * dy + 0.6f);
            if (infl > 0.03f) {
                uint8_t w = (uint8_t)(fclamp(infl, 0.0f, 1.0f) * 255.0f);
                px_add(x, y, CHSV((uint8_t)(g_anim_t * 20.0f), 230, w));
            }
        }
    }
}

// ============================================================
// INIZIALIZZAZIONE DRIVER I2S (RX) - MICROFONO INMP441
// ------------------------------------------------------------
// Ordine NON negoziabile (collaudato su hardware reale):
//   1. i2s_driver_install()
//   2. i2s_set_pin()
//   3. i2s_zero_dma_buffer()   (MAI prima di set_pin!)
//   4. delay(15)
//   5. i2s_start()
// Nessuna gpio_set_direction / gpio_set_pull_mode dopo i2s_set_pin:
// quelle chiamate rimuovono i pin dalla periferica I2S e il microfono
// non riceve piu' il clock -> campioni a zero per sempre.
// ============================================================

// Installazione + avvio del driver con il canale indicato.
static bool i2s_install_start(i2s_channel_fmt_t channel) {
    i2s_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = AUDIO_SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = channel;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;   // Philips Standard
    cfg.intr_alloc_flags     = 0;
    cfg.dma_buf_count        = I2S_DMA_BUF_COUNT;
    cfg.dma_buf_len          = I2S_DMA_BUF_LEN;
    cfg.use_apll             = false;                       // nessun APLL
    cfg.tx_desc_auto_clear   = false;
    cfg.fixed_mclk           = 0;

    esp_err_t err = i2s_driver_install(I2S_PORT_NUM, &cfg, 0, NULL);
    if (err != ESP_OK) {
        SFP("[I2S] ERRORE install driver: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_pin_config_t pin_cfg;
    memset(&pin_cfg, 0, sizeof(pin_cfg));
    pin_cfg.mck_io_num   = I2S_PIN_NO_CHANGE;
    pin_cfg.bck_io_num   = I2S_BCLK_PIN;
    pin_cfg.ws_io_num    = I2S_WS_PIN;
    pin_cfg.data_out_num = I2S_PIN_NO_CHANGE;
    pin_cfg.data_in_num  = I2S_DATA_IN_PIN;
    err = i2s_set_pin(I2S_PORT_NUM, &pin_cfg);
    if (err != ESP_OK) {
        SFP("[I2S] ERRORE set_pin: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_zero_dma_buffer(I2S_PORT_NUM);   // DOPO set_pin, MAI prima
    delay(15);

    err = i2s_start(I2S_PORT_NUM);
    if (err != ESP_OK) {
        SFP("[I2S] ERRORE start: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

// Scarta `blocks` blocchi (sincronizza BCK/WS) e misura il picco |campione|
// 24-bit sui `blocks` successivi. Serve per capire se il mic produce dati.
static int32_t i2s_measure_peak(int blocks, int discards) {
    static int32_t junk[AUDIO_BLOCK_SAMPLES];
    size_t br = 0;
    for (int i = 0; i < discards; ++i)
        i2s_read(I2S_PORT_NUM, junk, sizeof(junk), &br, pdMS_TO_TICKS(200));

    int32_t peak = 0;
    for (int k = 0; k < blocks; ++k) {
        if (i2s_read(I2S_PORT_NUM, junk, sizeof(junk), &br,
                     pdMS_TO_TICKS(200)) != ESP_OK) continue;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            int32_t s = junk[i] >> 8;        // 24 bit utili
            if (s < 0) s = -s;
            if (s > peak) peak = s;
        }
    }
    return peak;
}

// Riavvia il driver sul canale indicato (stop + uninstall + reinstall).
static bool i2s_reinstall_channel(i2s_channel_fmt_t channel) {
    i2s_stop(I2S_PORT_NUM);
    i2s_driver_uninstall(I2S_PORT_NUM);
    delay(20);
    return i2s_install_start(channel);
}

static bool i2s_init_driver(void) {
    // --- Primo tentativo: canale LEFT (INMP441 con L/R a GND) ---
    if (!i2s_install_start(I2S_CHANNEL_FMT_ONLY_LEFT)) return false;
    int32_t peak_left = i2s_measure_peak(6, 8);

#if (I2S_FORCE_CHANNEL == 2)
    // --- AUTO: se LEFT e' silenzioso prova RIGHT (L/R a 3V3 o flottante) ---
    if (peak_left < 100) {
        if (!i2s_reinstall_channel(I2S_CHANNEL_FMT_ONLY_RIGHT)) return false;
        int32_t peak_right = i2s_measure_peak(6, 8);
        if (peak_right >= peak_left) {
            SFP("[I2S] Auto-canale: RIGHT (L=%ld R=%ld) -> MICROFONO OK\n",
                (long)peak_left, (long)peak_right);
        } else {
            if (!i2s_reinstall_channel(I2S_CHANNEL_FMT_ONLY_LEFT)) return false;
            (void)i2s_measure_peak(2, 8);
            SFP("[I2S] Auto-canale: LEFT (L=%ld R=%ld) -> nessun segnale\n",
                (long)peak_left, (long)peak_right);
        }
    } else {
        SFP("[I2S] Auto-canale: LEFT OK (peak=%ld) -> MICROFONO OK\n",
            (long)peak_left);
    }
#elif (I2S_FORCE_CHANNEL == 1)
    if (!i2s_reinstall_channel(I2S_CHANNEL_FMT_ONLY_RIGHT)) return false;
    int32_t p = i2s_measure_peak(6, 8);
    SFP("[I2S] Canale FORZATO RIGHT. Picco 24-bit = %ld %s\n",
        (long)p, p > 100 ? "-> MICROFONO OK" : "-> silenzio / mic spento");
#else
    SFP("[I2S] Canale FORZATO LEFT. Picco 24-bit = %ld %s\n",
        (long)peak_left, peak_left > 100 ? "-> MICROFONO OK" : "-> silenzio / mic spento");
#endif

    SFP("[I2S] Driver RX @ %u Hz (32-bit/slot, APLL OFF)\n",
        (unsigned)AUDIO_SAMPLE_RATE);
    delay(10);
    return true;
}

// ============================================================
// ANALISI SPETTRALE LEGGERA: FILTRO DI GOERTZEL A 16 BANDE
// ------------------------------------------------------------
// L'ESP32-S2 non ha accelerazione FPU vettoriale: invece di una FFT
// completa calcoliamo 16 bande logaritmiche con il filtro di Goertzel
// (un solo coefficiente per banda, precalcolato una volta sola).
// ============================================================
static float g_band_coeff[NUM_BANDS];

static void goertzel_init(void) {
    const float fs   = (float)AUDIO_SAMPLE_RATE;
    const float f_lo = 80.0f;
    const float f_hi = 7000.0f;
    for (int b = 0; b < NUM_BANDS; ++b) {
        float t = (float)b / (float)(NUM_BANDS - 1);
        float f = f_lo * powf(f_hi / f_lo, t);            // spaziatura logaritmica
        int   k = (int)(0.5f + f * (float)AUDIO_BLOCK_SAMPLES / fs);
        if (k < 1) k = 1;
        if (k > AUDIO_BLOCK_SAMPLES / 2 - 1) k = AUDIO_BLOCK_SAMPLES / 2 - 1;
        float omega = 2.0f * (float)M_PI * (float)k / (float)AUDIO_BLOCK_SAMPLES;
        g_band_coeff[b] = 2.0f * cosf(omega);
    }
}

// ============================================================
// TASK FREERTOS: ACQUISIZIONE + ANALISI AUDIO
// ------------------------------------------------------------
// Scrive in g_audio (protetto dal mutex g_audio_mux); il loop ne fa
// una copia coerente in g_snap per il rendering degli effetti.
// ============================================================
static void audio_task(void *arg) {
    (void)arg;
    static int32_t raw[AUDIO_BLOCK_SAMPLES];   // campioni grezzi 32-bit
    static float   buf[AUDIO_BLOCK_SAMPLES];   // campioni float [-1,1] con guadagno

    float  gain_factor = 1.0f;
    float  level_s = 0.0f, peak_s = 0.0f;
    float  bands_s[NUM_BANDS];
    memset(bands_s, 0, sizeof(bands_s));
    float  bass_s = 0.0f, mid_s = 0.0f, treb_s = 0.0f;
    float  bass_avg = 0.0f;
    float  beat_env = 0.0f;
    uint32_t beat_count = 0;
    uint32_t last_beat_ms = 0;
    uint32_t last_signal_ms = 0;
    uint32_t rd_ok = 0, rd_fail = 0;        // diagnostica letture I2S
    uint32_t last_rawdump_ms = 0;           // dump diagnostico periodico su seriale

    SFL("[AUDIO] Task acquisizione avviato (ONLY_LEFT, Goertzel 16 bande)");

    for (;;) {
        // --- Guadagno microfono: ricalcolo solo al cambio (niente expf nel loop) ---
        if (g_gain_dirty) {
            gain_factor = powf(10.0f, (float)g_params.gain_db / 20.0f);
            g_gain_dirty = false;
            SFP("[AUDIO] Guadagno microfono = %u dB (x%.2f)\n",
                (unsigned)g_params.gain_db, gain_factor);
        }

        // --- Lettura di un blocco MONO ONLY_LEFT ---
        // Timeout FINITO (NON portMAX_DELAY): se la catena DMA si incastra,
        // il task riprova invece di restare bloccato per sempre -> stato audio
        // congelato a zero. Il firmware LED standalone FUNZIONANTE usava 500 ms.
        size_t    bytes_read = 0;
        esp_err_t err = i2s_read(I2S_PORT_NUM, raw, sizeof(raw), &bytes_read,
                                 pdMS_TO_TICKS(AUDIO_READ_TIMEOUT_MS));
        if (err != ESP_OK || bytes_read < sizeof(raw)) {
            rd_fail++;
            if (rd_fail <= 3 || (rd_fail % 200) == 0) {
                SFP("[AUDIO] i2s_read FALLITA #%lu: err=%s bytes=%u/%u\n",
                    (unsigned long)rd_fail, esp_err_to_name(err),
                    (unsigned)bytes_read, (unsigned)sizeof(raw));
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        rd_ok++;

        // --- Diagnostica: picco grezzo 24-bit del blocco appena letto ---
        int32_t raw_peak = 0;
        int32_t raw0     = raw[0] >> 8;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            int32_t s = raw[i] >> 8;
            if (s < 0) s = -s;
            if (s > raw_peak) raw_peak = s;
        }

        // --- Conversione 24-bit -> float con guadagno, RMS e picco in un passo ---
        // INMP441: 24 bit utili left-aligned nello slot 32-bit -> >> 8.
        float sum_sq = 0.0f;
        float pk     = 0.0f;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            float v = (float)(raw[i] >> 8) * (1.0f / 8388608.0f) * gain_factor;
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            buf[i] = v;
            sum_sq += v * v;
            float a = (v < 0.0f) ? -v : v;
            if (a > pk) pk = a;
        }
        float rms = sqrtf(sum_sq / (float)AUDIO_BLOCK_SAMPLES);

        // --- Smussamento livello (attacco rapido, rilascio morbido) e picco ---
        level_s += (rms - level_s) * ((rms > level_s) ? 0.55f : 0.12f);
        peak_s   = (pk > peak_s) ? pk : peak_s * 0.92f;

        // --- Goertzel: energia delle 16 bande ---
        // Scala legata alla sensibilita' utente (2..100 -> fattore 6..36).
        const float band_gain = (2.0f + (float)g_params.sensitivity * 0.10f) * 3.0f;
        for (int b = 0; b < NUM_BANDS; ++b) {
            float coeff = g_band_coeff[b];
            float q1 = 0.0f, q2 = 0.0f;
            for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
                float q0 = coeff * q1 - q2 + buf[i];
                q2 = q1;
                q1 = q0;
            }
            float mag2 = q1 * q1 + q2 * q2 - q1 * q2 * coeff;
            if (mag2 < 0.0f) mag2 = 0.0f;
            float mag = sqrtf(mag2) * (1.0f / (float)AUDIO_BLOCK_SAMPLES);
            float val = mag * band_gain;
            if (val > 1.0f) val = 1.0f;
            // salita immediata, discesa morbida (barre "vive")
            if (val > bands_s[b]) bands_s[b] = val;
            else                  bands_s[b] += (val - bands_s[b]) * 0.25f;
        }

        // --- Raggruppamento in bassi / medi / alti ---
        bass_s = (bands_s[0] + bands_s[1] + bands_s[2] + bands_s[3] + bands_s[4]) * 0.2f;
        mid_s  = (bands_s[5] + bands_s[6] + bands_s[7] + bands_s[8] + bands_s[9] + bands_s[10]) / 6.0f;
        treb_s = (bands_s[11] + bands_s[12] + bands_s[13] + bands_s[14] + bands_s[15]) * 0.2f;

        // --- Rilevazione battito (bassi sopra la media mobile) ---
        uint32_t now_ms = millis();
        bass_avg += (bass_s - bass_avg) * 0.05f;
        if (bass_s > 0.12f && bass_s > bass_avg * 1.55f && (now_ms - last_beat_ms) > 140) {
            last_beat_ms = now_ms;
            beat_env = 1.0f;
            beat_count++;
        } else {
            beat_env *= 0.80f;
        }

        // --- Presenza di segnale (per la UI) ---
        if (rms > 0.006f) last_signal_ms = now_ms;
        bool signal = (now_ms - last_signal_ms) < 1200;

        // --- Dump diagnostico periodico (5 s) solo su monitor seriale aperto ---
        if ((uint32_t)(now_ms - last_rawdump_ms) > 5000UL) {
            last_rawdump_ms = now_ms;
            SFP("[AUDIO] rd_ok=%lu rd_fail=%lu raw_peak=%ld raw0=%ld rms=%.4f lvl=%d\n",
                (unsigned long)rd_ok, (unsigned long)rd_fail,
                (long)raw_peak, (long)raw0, (double)rms, (int)(level_s * 100.0f));
        }

        // --- Pubblicazione coerente dello stato audio ---
        portENTER_CRITICAL(&g_audio_mux);
        g_audio.level      = level_s;
        g_audio.peak       = peak_s;
        g_audio.bass       = bass_s;
        g_audio.mid        = mid_s;
        g_audio.treble     = treb_s;
        for (int i = 0; i < NUM_BANDS; ++i) g_audio.bands[i] = bands_s[i];
        g_audio.beat       = beat_env;
        g_audio.beat_count = beat_count;
        g_audio.signal     = signal;
        g_audio.raw_peak   = raw_peak;
        g_audio.raw0       = raw0;
        g_audio.rd_ok      = rd_ok;
        g_audio.rd_fail    = rd_fail;
        portEXIT_CRITICAL(&g_audio_mux);

        // Cede il controllo (single-core: senza yield si affama il loop).
        taskYIELD();
    }
}

// ============================================================
// APPLICAZIONE PARAMETRI (clamp di sicurezza + effetti collaterali)
// ------------------------------------------------------------
// Chiamata dopo ogni modifica dei parametri (setup o comando web).
// NON riavvia nulla: tutto si applica a runtime.
// ============================================================
static void apply_params(void) {
    // --- Clamp di sicurezza (il browser gia' limita, ma non ci fidiamo) ---
    // Nota: i parametri sono uint8_t (range 0..255). I limiti che coincidono
    // con gli estremi del tipo (brightness max 255, sensitivity/gain min 0)
    // renderebbero il confronto un no-op che GCC segnala con -Wtype-limits:
    // per questo quei singoli confronti sono omessi. Restano attivi tutti i
    // clamp significativi (limiti intermedi o inferiori/superiori reali).
    if (g_params.effect >= FX_COUNT)                    g_params.effect = 0;
    if (g_params.brightness < BRIGHTNESS_MIN)           g_params.brightness = BRIGHTNESS_MIN;
    if (g_params.speed       < SPEED_MIN)               g_params.speed = SPEED_MIN;
    if (g_params.speed       > SPEED_MAX)               g_params.speed = SPEED_MAX;
    if (g_params.sensitivity > SENSITIVITY_MAX)         g_params.sensitivity = SENSITIVITY_MAX;
    if (g_params.gain_db     > GAIN_DB_MAX)             g_params.gain_db = GAIN_DB_MAX;
    if (g_params.palette >= NUM_PALETTES)               g_params.palette = PALETTE_DEFAULT;

    // --- Palette corrente (usata dagli effetti che leggono g_pal) ---
    g_pal = g_palette_list[g_params.palette];

    // --- Luminosita' globale applicata da FastLED.show() ---
    FastLED.setBrightness(g_params.brightness);

    // --- Guadagno microfono: il task audio ricalcola il fattore al prossimo tick ---
    g_gain_dirty = true;

    // --- Reset dello stato interno effetti al cambio effetto ---
    if (g_params.effect != g_fx_prev) {
        g_fx_prev = g_params.effect;
        fx_reset_state();
    }
}

// ============================================================
// WEBSOCKET: invio stato al browser
// ============================================================
static void ws_send_status(AsyncWebSocketClient *client) {
    if (ESP.getFreeHeap() < 12288) return;   // guardia heap (no PSRAM)
    JsonDocument doc;
    doc["type"]        = "status";
    doc["effect"]      = g_params.effect;
    doc["random"]      = g_params.random_on;
    doc["brightness"]  = g_params.brightness;
    doc["speed"]       = g_params.speed;
    doc["sensitivity"] = g_params.sensitivity;
    doc["gainDb"]      = g_params.gain_db;
    doc["palette"]     = g_params.palette;
    doc["fxCount"]     = FX_COUNT;
    doc["heap"]        = (uint32_t)ESP.getFreeHeap();
    char buf[256];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n == 0) return;
    if (client) client->text(buf, n);
    else        g_ws.textAll(buf, n);
}

// Telemetria audio leggera per il VU-meter / indicatore di presenza del browser.
static void ws_send_audio(void) {
    if (ESP.getFreeHeap() < 12288) return;
    JsonDocument doc;
    doc["type"]   = "audio";
    doc["level"]  = (int)(fclamp(g_snap.level,  0.0f, 1.0f) * 100.0f);
    doc["bass"]   = (int)(fclamp(g_snap.bass,   0.0f, 1.0f) * 100.0f);
    doc["mid"]    = (int)(fclamp(g_snap.mid,    0.0f, 1.0f) * 100.0f);
    doc["treble"] = (int)(fclamp(g_snap.treble, 0.0f, 1.0f) * 100.0f);
    doc["beats"]  = g_snap.beat_count;
    doc["signal"] = g_snap.signal;
    char buf[192];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n) g_ws.textAll(buf, n);
}

// ============================================================
// WEBSOCKET: comando "set" (aggiorna uno o piu' parametri)
// ============================================================
static void ws_handle_set(const JsonObjectConst &p) {
    bool changed = false;
    if (!p["effect"].isNull())      { g_params.effect      = (uint8_t)p["effect"].as<int>();      changed = true; }
    if (!p["random"].isNull())      { g_params.random_on   = p["random"].as<bool>();              changed = true; }
    if (!p["brightness"].isNull())  { g_params.brightness  = (uint8_t)p["brightness"].as<int>();  changed = true; }
    if (!p["speed"].isNull())       { g_params.speed       = (uint8_t)p["speed"].as<int>();       changed = true; }
    if (!p["sensitivity"].isNull()) { g_params.sensitivity = (uint8_t)p["sensitivity"].as<int>(); changed = true; }
    if (!p["gainDb"].isNull())      { g_params.gain_db     = (uint8_t)p["gainDb"].as<int>();      changed = true; }
    if (!p["palette"].isNull())     { g_params.palette     = (uint8_t)p["palette"].as<int>();     changed = true; }

    if (changed) apply_params();
    ws_send_status(NULL);
}

// ============================================================
// WEBSOCKET: dispatcher eventi
// ============================================================
static void ws_on_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            SFP("[WS] Client #%u connesso (%s)\n",
                (unsigned)client->id(), client->remoteIP().toString().c_str());
            ws_send_status(client);
            break;

        case WS_EVT_DISCONNECT:
            SFP("[WS] Client #%u disconnesso\n", (unsigned)client->id());
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo *info = (AwsFrameInfo *)arg;
            // I comandi JSON sono piccoli e arrivano in un unico frame di testo.
            if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
                static char json_buf[256];
                size_t n = (len < sizeof(json_buf) - 1) ? len : (sizeof(json_buf) - 1);
                memcpy(json_buf, data, n);
                json_buf[n] = '\0';
                JsonDocument doc;
                if (deserializeJson(doc, json_buf) != DeserializationError::Ok) return;
                const char *cmd = doc["cmd"] | "";
                if (strcmp(cmd, "set") == 0)      ws_handle_set(doc.as<JsonObjectConst>());
                else if (strcmp(cmd, "get") == 0) ws_send_status(client);
            }
            (void)server;
            break;
        }
        default:
            break;
    }
}

// ============================================================
// SETUP WIFI: 10 TENTATIVI STA, POI FALLBACK AP
// ============================================================
static bool wifi_connect_sta(void) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setHostname("esp32-luci");
    WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
    WiFi.setAutoReconnect(false);
    // Antenna PCB piccola: potenza > 8 dBm satura il PHY trasmettendo
    // pacchetti lunghi -> ACK TCP persi. 8 dBm e' sicuro e collaudato.
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    SFP("[WIFI] Scan passivo per '%s' ... ", WIFI_SSID);
    int nets = WiFi.scanNetworks(false, true, false, 300);
    bool ssid_found = false;
    int  target_channel = 0;
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
    SFP("[WIFI] Connessione a '%s' (max %d tentativi)\n", WIFI_SSID, WIFI_MAX_RETRIES);

    for (int attempt = 1; attempt <= WIFI_MAX_RETRIES; ++attempt) {
        SFP("[WIFI] Tentativo %d/%d ... ", attempt, WIFI_MAX_RETRIES);
        if (target_channel > 0) WiFi.begin(WIFI_SSID, WIFI_PASSWORD, target_channel, NULL, true);
        else                    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < 8000UL) {
            delay(40);
            led_pulse(15);
            // Single-core: cedi SEMPRE a LwIP (priorita' 4) ad ogni tick.
            vTaskDelay(pdMS_TO_TICKS(1));
            taskYIELD();
        }

        if (WiFi.status() == WL_CONNECTED) {
            SFL(" OK");
            SFP("[WIFI] IP: %s (tentativo %d, RSSI %d dBm)\n",
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
        delay(120 + (uint32_t)attempt * 40);
        vTaskDelay(pdMS_TO_TICKS(1));
        taskYIELD();
    }

    SFP("[WIFI] Impossibile connettersi dopo %d tentativi, avvio AP fallback\n", WIFI_MAX_RETRIES);
    WiFi.disconnect(true);
    return false;
}

static void wifi_start_ap(void) {
    WiFi.mode(WIFI_AP);
    IPAddress ip(AP_IP);
    IPAddress netmask(255, 255, 255, 0);
    WiFi.softAPConfig(ip, ip, netmask);
    if (strlen(AP_PASSWORD) >= 8) WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN);
    else                          WiFi.softAP(AP_SSID, NULL,       AP_CHANNEL, 0, AP_MAX_CONN);
    SFP("[WIFI] AP avviato SSID='%s' -> IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

// ============================================================
// REGISTRAZIONE HANDLER WEB (OTA + endpoint diagnostici)
// ============================================================
static void web_register_handlers(void) {
    const char *OTA_PASS = AP_PASSWORD;

    // --- Pagina /update (form HTML semplice per firmware / LittleFS) ---
    g_server.on("/update", HTTP_GET, [OTA_PASS](AsyncWebServerRequest *req) {
        bool auth = false;
        AsyncWebParameter *pwd_get  = req->getParam("pwd", true);
        AsyncWebParameter *pwd_post = req->getParam("pwd", false);
        if ((pwd_get  && strcmp(pwd_get->value().c_str(),  OTA_PASS) == 0) ||
            (pwd_post && strcmp(pwd_post->value().c_str(), OTA_PASS) == 0)) auth = true;
        if (!auth && req->hasHeader("Authorization")) {
            String hdr = req->header("Authorization");
            if (hdr.startsWith("Basic ")) {
                String b64 = hdr.substring(6);
                String ref_plain = "admin:"; ref_plain += OTA_PASS;
                String ref_b64 = "";
                static const char B64[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                const uint8_t *s = (const uint8_t *)ref_plain.c_str();
                int n = ref_plain.length();
                ref_b64.reserve(((n + 2) / 3) * 4);
                for (int i = 0; i < n; i += 3) {
                    uint32_t v = (uint32_t)s[i] << 16;
                    if (i + 1 < n) v |= (uint32_t)s[i + 1] << 8;
                    if (i + 2 < n) v |= (uint32_t)s[i + 2];
                    ref_b64 += B64[(v >> 18) & 0x3F];
                    ref_b64 += B64[(v >> 12) & 0x3F];
                    ref_b64 += (i + 1 < n) ? B64[(v >> 6) & 0x3F] : '=';
                    ref_b64 += (i + 2 < n) ? B64[(v) & 0x3F] : '=';
                }
                if (b64 == ref_b64) auth = true;
            }
        }
        if (!auth) {
            req->send(401, "text/html; charset=utf-8",
                "<!doctype html><html><head><meta charset=\"utf-8\"><title>401 OTA</title></head>"
                "<body style=\"background:#1E1E1E;color:#e6ecf6;font-family:Segoe UI,system-ui\">"
                "<h2>Accesso OTA non autorizzato</h2>"
                "<p>Apri: <code>http://" + WiFi.localIP().toString() +
                "/update?pwd=" + String(OTA_PASS) + "</code></p></body></html>");
            return;
        }
        String html =
            "<!doctype html><html lang=\"it\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Aggiornamento OTA</title><style>"
            "body{background:#1E1E1E;color:#e6ecf6;font-family:Segoe UI,system-ui,Arial;margin:2rem auto;max-width:720px;padding:1rem}"
            "h1{margin-top:0;font-size:1.2rem}a{color:#60a5fa}"
            ".box{border:1px solid #333;background:#242424;padding:1rem 1.2rem;margin-bottom:1rem;border-radius:4px}"
            "input[type=file]{margin:.6rem 0;padding:.5rem;background:#1a1a1a;border:1px solid #3a3a3a;border-radius:4px;color:#e5e7eb}"
            "select{padding:.4rem;background:#1a1a1a;color:#e5e7eb;border:1px solid #3a3a3a;border-radius:4px}"
            "button{background:#2563eb;color:#fff;border:0;padding:.7rem 1.1rem;border-radius:4px;font-weight:600;cursor:pointer}"
            "button:hover{background:#1d4ed8}.ok{color:#4ade80}"
            ".bar{height:12px;background:#1a1a1a;border-radius:4px;margin:.6rem 0;overflow:hidden}"
            ".bar>span{display:block;height:100%;background:#2563eb;width:0%}</style></head><body>"
            "<h1>Aggiornamento OTA - ESP32 Luci</h1>"
            "<div class=\"box\"><b>Firmware</b>: <code>.pio/build/lolin_s2_mini/firmware.bin</code><br>"
            "<b>Filesystem</b>: <code>.pio/build/lolin_s2_mini/littlefs.bin</code></div>"
            "<form class=\"box\" id=\"f\" method=\"POST\" enctype=\"multipart/form-data\" action=\"/update?pwd=" + String(OTA_PASS) + "\">"
            "<label><b>Tipo di file:</b><br><select name=\"type\">"
            "<option value=\"firmware\" selected>Firmware (.bin)</option>"
            "<option value=\"fs\">Filesystem LittleFS (.bin)</option></select></label><br><br>"
            "<label><b>File:</b><br><input type=\"file\" name=\"update\" id=\"file\" accept=\".bin\"></label><br><br>"
            "<button type=\"submit\">Carica e Installa</button>&nbsp;&nbsp;<a href=\"/\">&larr; Torna ai giochi di luce</a>"
            "<div class=\"bar\"><span id=\"p\"></span></div><div id=\"st\">Attesa file...</div></form>"
            "<script>"
            "const f=document.getElementById('f'),st=document.getElementById('st'),p=document.getElementById('p');"
            "f.addEventListener('submit',e=>{e.preventDefault();const fd=new FormData(f);"
            "const fl=document.getElementById('file').files[0];"
            "if(!fl){st.textContent='Seleziona un file .bin';return}"
            "p.style.width='0%';st.textContent='Upload '+fl.name+' ('+fl.size+' byte)...';"
            "const xhr=new XMLHttpRequest();xhr.open('POST',f.action);"
            "xhr.upload.onprogress=e=>{if(e.lengthComputable){const w=(100*e.loaded/e.size)|0;p.style.width=w+'%';st.textContent='Upload '+w+'%...'}};"
            "xhr.onload=()=>{p.style.width='100%';st.innerHTML='<span class=ok>Completato</span>: '+xhr.responseText;"
            "setTimeout(()=>location.reload(),4000)};"
            "xhr.onerror=()=>st.textContent='Errore di rete';xhr.send(fd);});"
            "</script></body></html>";
        req->send(200, "text/html; charset=utf-8", html);
    });

    // --- Handler upload /update (multipart/form-data) ---
    struct OtaCtx {
        bool    auth_ok;
        uint8_t partition;
        bool    update_started;
        size_t  total_written;
        size_t  expected_size;
    };
    static OtaCtx ota_ctx;

    g_server.on("/update", HTTP_POST,
        [OTA_PASS](AsyncWebServerRequest *req) {
            AsyncWebParameter *pwd_q = req->getParam("pwd", true);
            AsyncWebParameter *pwd_f = req->getParam("pwd", false);
            ota_ctx.auth_ok =
                (pwd_q && strcmp(pwd_q->value().c_str(), OTA_PASS) == 0) ||
                (pwd_f && strcmp(pwd_f->value().c_str(), OTA_PASS) == 0);
            if (!ota_ctx.auth_ok) {
                req->send(401, "text/plain", "401 password errata");
                return;
            }
            if (!ota_ctx.update_started) {
                req->send(400, "text/plain", "Nessun dato ricevuto o Update.begin() fallito.");
                return;
            }
            bool end_ok = Update.end(true);
            SFP("[OTA] Update.end() -> %s, scritti %u/%u byte\n",
                end_ok ? "OK" : "FALLITO",
                (unsigned)ota_ctx.total_written, (unsigned)ota_ctx.expected_size);
            if (end_ok) {
                req->send(200, "text/plain; charset=utf-8",
                          String("Aggiornamento completato. Riavvio in 3s. Scritti ") +
                          String(ota_ctx.total_written) + " byte.");
                delay(3000);
                ESP.restart();
            } else {
                req->send(500, "text/plain",
                          String("Errore Update.end(): ") + Update.errorString());
                Update.abort();
            }
        },
        [OTA_PASS](AsyncWebServerRequest *req, const String &filename,
                   size_t index, uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                AsyncWebParameter *pwdQ = req->getParam("pwd", true);
                AsyncWebParameter *pwdF = req->getParam("pwd", false);
                ota_ctx.auth_ok =
                    (pwdQ && strcmp(pwdQ->value().c_str(), OTA_PASS) == 0) ||
                    (pwdF && strcmp(pwdF->value().c_str(), OTA_PASS) == 0);
                if (!ota_ctx.auth_ok) { req->send(401, "text/plain", "401 password errata"); return; }

                ota_ctx.partition = U_FLASH;
                AsyncWebParameter *t = req->getParam("type", false);
                if (t && (t->value() == "fs" || filename.endsWith("littlefs.bin") ||
                          filename.endsWith("spiffs.bin"))) {
                    ota_ctx.partition = U_SPIFFS;   // Update.h usa U_SPIFFS anche per LittleFS
                }
                size_t up_size = req->contentLength();
                ota_ctx.expected_size = up_size;
                // Update.begin(0) auto-size NON affidabile su core 2.0.9: passiamo la dimensione reale.
                if (up_size == 0) up_size = 1400 * 1024;
                ota_ctx.update_started = Update.begin(up_size, ota_ctx.partition);
                ota_ctx.total_written = 0;
                SFP("[OTA] Inizio '%s' tipo=%s begin=%d err=%s\n",
                    filename.c_str(),
                    (ota_ctx.partition == U_FLASH) ? "FIRMWARE" : "FILESYSTEM",
                    ota_ctx.update_started,
                    ota_ctx.update_started ? "OK" : Update.errorString());
                if (!ota_ctx.update_started) return;
            }
            if (!ota_ctx.update_started || !ota_ctx.auth_ok) return;
            if (len > 0) {
                size_t w = Update.write(data, len);
                if (w == len) ota_ctx.total_written += w;
                else SFP("[OTA] Write fallito: %u/%u -> %s\n",
                         (unsigned)len, (unsigned)w, Update.errorString());
            }
            (void)final;
        }
    );

    // --- Endpoint /effects: elenco effetti + palette + metadati UI ---
    g_server.on("/effects", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (ESP.getFreeHeap() < 12288) {
            req->send(503, "application/json", "{\"error\":\"heap\"}");
            return;
        }
        JsonDocument doc;
        JsonArray fx = doc["effects"].to<JsonArray>();
        for (uint8_t i = 0; i < FX_COUNT; i++) fx.add(g_fx_names[i]);
        JsonArray pal = doc["palettes"].to<JsonArray>();
        for (uint8_t i = 0; i < NUM_PALETTES; i++) pal.add(g_palette_names[i]);
        doc["firstAudioFx"] = FIRST_AUDIO_FX;
        doc["fxCount"]      = FX_COUNT;
        doc["paletteCount"] = NUM_PALETTES;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // --- Endpoint /debug: stato interno in JSON ---
    g_server.on("/debug", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["heap"]         = (uint32_t)ESP.getFreeHeap();
        doc["heap_min"]     = (uint32_t)ESP.getMinFreeHeap();
        doc["heap_largest"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        doc["psram"]        = (uint32_t)ESP.getFreePsram();
        doc["ws_clients"]   = (uint32_t)g_ws.count();
        doc["effect"]       = g_params.effect;
        doc["random"]       = g_params.random_on;
        doc["brightness"]   = g_params.brightness;
        doc["speed"]        = g_params.speed;
        doc["sensitivity"]  = g_params.sensitivity;
        doc["gain_db"]      = g_params.gain_db;
        doc["palette"]      = g_params.palette;
        doc["level"]        = (int)(g_snap.level * 100.0f);
        doc["beats"]        = g_snap.beat_count;
        // --- Diagnostica I2S: distingue causa software (letture fallite)
        //     da causa hardware (letture OK ma campioni tutti a zero) ---
        doc["raw_peak"]     = g_snap.raw_peak;
        doc["raw0"]         = g_snap.raw0;
        doc["rd_ok"]        = g_snap.rd_ok;
        doc["rd_fail"]      = g_snap.rd_fail;
        doc["uptime_ms"]    = (uint32_t)millis();
        doc["rssi"]         = (int8_t)WiFi.RSSI();
        doc["ip"]           = WiFi.localIP().toString();
        char buf[512];
        size_t n = serializeJson(doc, buf, sizeof(buf));
        (void)n;
        req->send(200, "application/json", String(buf));
    });

    g_server.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "404: risorsa non trovata (carica il filesystem LittleFS).");
    });
}

// ============================================================
// SETUP GENERALE
// ============================================================
void setup() {
    delay(200);
    led_setup();
    led_on();
    Serial.begin(SERIAL_BAUD);
    // Attesa USB CDC: fino a 3s con lampeggio (necessario per usare Serial con DTR asserito).
    led_wait_serial_with_blink(3000);
    led_off();

    // --- Banner boot: SOLO wrapper SFP/SFL (mai Serial raw, bug HWCDC) ---
    SFL("\n==========================================");
    SFL(" ESP32-S2 mini - Giochi di Luce 8x32 + Mic ");
    SFP(" Build  : %s %s\n", __DATE__, __TIME__);
    SFP(" Flags  : ARDUINO_USB_MODE=%d  ARDUINO_USB_CDC_ON_BOOT=%d\n",
        FLAG_USB_MODE, FLAG_USB_CDC_ON_BOOT);
    SFP(" Heap   : free=%u B\n", (unsigned)ESP.getFreeHeap());
    SFP(" Reset  : motivo %d (esp_reset_reason)\n", (int)esp_reset_reason());
    SFL("==========================================");

    // --- Parametri di default ---
    g_params.effect      = 1;                     // Arcobaleno (mai "Spento" all'avvio)
    g_params.random_on   = false;
    g_params.brightness  = BRIGHTNESS_DEFAULT;
    g_params.speed       = SPEED_DEFAULT;
    g_params.sensitivity = SENSITIVITY_DEFAULT;
    g_params.gain_db     = GAIN_DB_DEFAULT;
    g_params.palette     = PALETTE_DEFAULT;

    // --- Inizializzazioni interne ---
    sin_lut_init();
    goertzel_init();
    g_fire_pal = CRGBPalette16(gp_lava);   // palette fuoco dedicata

    // --- Matrice LED ---
    FastLED.addLeds<WS2812B, FASTLED_DATA_PIN, GRB>(leds, FASTLED_NUM_LEDS);
    FastLED.setBrightness(g_params.brightness);
    clear_all();
    FastLED.show();

    apply_params();
    fx_reset_state();
    SFP("[FX] %u effetti caricati (%u audio-reattivi dal #%u)\n",
        (unsigned)FX_COUNT, (unsigned)(FX_COUNT - FIRST_AUDIO_FX), (unsigned)FIRST_AUDIO_FX);

    // --- Filesystem LittleFS (pagina web) ---
    if (!LittleFS.begin(true)) SFL("[FS] LittleFS mount FALLITO (carica il filesystem!)");
    else                       SFL("[FS] LittleFS montato correttamente");

    // --- Microfono I2S ---
    if (!i2s_init_driver()) SFL("[FATAL] Inizializzazione I2S fallita");

    // --- Task di acquisizione / analisi audio ---
    // Avviato SUBITO dopo l'init I2S (e NON dopo il WiFi): tra i2s_start() e
    // il primo i2s_read() non deve esserci un intervallo lungo. Se la coda RX
    // DMA resta piena per secondi (durante la connessione WiFi) la catena si
    // blocca e il microfono resta a zero per sempre.
    xTaskCreatePinnedToCore(audio_task, "audio_acq", AUDIO_TASK_STACK, NULL,
                            AUDIO_TASK_PRIORITY, NULL, 0);

    // --- WiFi: STA con fallback AP ---
    if (!wifi_connect_sta()) {
        wifi_start_ap();
        g_ws_mode_ap = true;
    }

    // --- WebSocket ---
    g_ws.onEvent(ws_on_event);
    g_server.addHandler(&g_ws);

    // --- Handler HTTP (OTA + diagnostica + statico) ---
    web_register_handlers();
    g_server.serveStatic("/", LittleFS, "/")
              .setDefaultFile("index.html")
              .setCacheControl("max-age=60");
    g_server.begin();
    SFL("[HTTP] Server web avviato sulla porta 80");

    SFL("[BOOT] Setup completato.");
}

// ============================================================
// LOOP: rendering effetti + telemetria WebSocket
// ============================================================
void loop() {
    // --- Cache WiFi.status() ogni 100 ms (anti-deadlock mutex LwIP) ---
    static uint32_t wifi_ms  = 0;
    static int      wifi_ok  = WL_DISCONNECTED;
    if (millis() - wifi_ms > 100UL) {
        wifi_ms = millis();
        wifi_ok = WiFi.status();
    }

    // --- Clock di animazione ---
    g_now_ms = millis();
    float dt_raw = (float)(g_now_ms - g_last_frame_ms) / 1000.0f;
    if (dt_raw > 0.1f) dt_raw = 0.1f;      // clamp dopo pause/impostazioni
    g_last_frame_ms = g_now_ms;

    // --- Copia coerente dello stato audio ---
    portENTER_CRITICAL(&g_audio_mux);
    g_snap = g_audio;
    portEXIT_CRITICAL(&g_audio_mux);

    // --- Tempo di animazione scalato dalla velocita' (50 = 1x) ---
    float dt = dt_raw * ((float)g_params.speed / 50.0f);
    g_anim_t += dt;

    // --- Cambio effetto casuale periodico ---
    static uint32_t last_random_ms = 0;
    if (g_params.random_on && (g_now_ms - last_random_ms) > RANDOM_PERIOD_MS) {
        last_random_ms = g_now_ms;
        uint8_t ne = g_params.effect;
        for (uint8_t i = 0; i < 6 && ne == g_params.effect; i++)
            ne = (uint8_t)random8(1, FX_COUNT);   // evita "Spento" (indice 0)
        g_params.effect = ne;
        apply_params();
        ws_send_status(NULL);
    } else if (!g_params.random_on) {
        last_random_ms = g_now_ms;                // riallinea quando si riattiva
    }

    // --- Rendering dell'effetto corrente ---
    clear_all();
    g_fx[g_params.effect](dt);
    FastLED.show();

    // --- Telemetria audio alla UI (10 Hz, solo se c'e' un client) ---
    static uint32_t last_tel = 0;
    if (wifi_ok == WL_CONNECTED && g_ws.count() > 0 && (g_now_ms - last_tel) > 100UL) {
        last_tel = g_now_ms;
        ws_send_audio();
    }

    // --- Heartbeat status (raro, solo se c'e' un client) ---
    static uint32_t last_hb = 0;
    if (wifi_ok == WL_CONNECTED && g_ws.count() > 0 && (g_now_ms - last_hb) > 5000UL) {
        last_hb = g_now_ms;
        ws_send_status(NULL);
    }

    // Single-core: cedi sempre il controllo (LwIP + idle task).
    vTaskDelay(pdMS_TO_TICKS(1));
}
