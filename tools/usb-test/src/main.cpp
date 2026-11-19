// ============================================================
// tools/usb-test/src/main.cpp  - MINIMO TEST USB CDC
// ------------------------------------------------------------
// Solo:
//   1. Serial.begin(115200)
//   2. Lampeggio LED_BUILTIN (blink non bloccante via millis)
//   3. println("vivo - timestamp_ms=...") ogni 500 ms
// Nessun WiFi, LittleFS, I2S, Task FreeRTOS.
// ============================================================
#include <Arduino.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN  15
#endif
#ifdef LED_BUILTIN_ACTIVELOW
  #define LED_ON()    digitalWrite(LED_BUILTIN, LOW)
  #define LED_OFF()   digitalWrite(LED_BUILTIN, HIGH)
#else
  #define LED_ON()    digitalWrite(LED_BUILTIN, HIGH)
  #define LED_OFF()   digitalWrite(LED_BUILTIN, LOW)
#endif

// --- Valori catturati dei flag PRIMA di Arduino.h (safety) ---
#if defined(ARDUINO_USB_MODE)
  static const int CAPTURED_USB_MODE = ARDUINO_USB_MODE;
#else
  static const int CAPTURED_USB_MODE = -1;
#endif
#if defined(ARDUINO_USB_CDC_ON_BOOT)
  static const int CAPTURED_USB_CDC = ARDUINO_USB_CDC_ON_BOOT;
#else
  static const int CAPTURED_USB_CDC = -1;
#endif

static uint32_t last_blink_ms = 0;
static uint32_t last_print_ms = 0;
static bool     led_state     = false;

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    LED_OFF();
    Serial.begin(115200);

    // Attesa massima enumerazione USB: 4s con lampeggio lento per feedback
    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 4000) {
        LED_ON();  delay(80);
        LED_OFF(); delay(80);
    }

    if (Serial) {
        Serial.println("\n==== USB TEST MINIMO ====");
        Serial.printf(" Build: %s %s\n", __DATE__, __TIME__);
        Serial.printf(" ARDUINO_USB_MODE=%d  ARDUINO_USB_CDC_ON_BOOT=%d\n",
                      CAPTURED_USB_MODE, CAPTURED_USB_CDC);
        Serial.printf(" Heap iniziale=%u B\n", (unsigned)ESP.getFreeHeap());
        Serial.println("=========================");
    }
}

void loop() {
    uint32_t now = millis();

    // --- Blink LED ogni 500ms ---
    if ((now - last_blink_ms) >= 500) {
        last_blink_ms = now;
        led_state = !led_state;
        if (led_state) LED_ON(); else LED_OFF();
    }

    // --- Print seriale "vivo" ogni 500ms (solo se la USB e' connessa) ---
    if ((now - last_print_ms) >= 500) {
        last_print_ms = now;
        if (Serial) {
            Serial.printf("vivo - uptime=%lu ms  heap=%u B\n",
                          (unsigned long)now,
                          (unsigned)ESP.getFreeHeap());
        }
    }
}
