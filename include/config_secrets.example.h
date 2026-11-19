/*
 * config_secrets.example.h - MODELLO delle credenziali WiFi
 *
 * COME SI USA
 *   1. Copia questo file in  include/config_secrets.h
 *   2. Sostituisci i due valori qui sotto con il nome e la password
 *      della TUA rete WiFi (2.4 GHz: l'ESP32-S2 non ha il 5 GHz).
 *   3. Compila e carica. Fine: config.h legge il file automaticamente.
 *
 * config_secrets.h e' elencato in .gitignore e NON viene pubblicato.
 * Se il file non esiste, il firmware compila comunque e parte in
 * modalita' Access Point (rete "esp32-luci", http://192.168.4.1).
 */

#ifndef CONFIG_SECRETS_H
#define CONFIG_SECRETS_H

#define WIFI_SSID     "NOME_DELLA_TUA_RETE"
#define WIFI_PASSWORD "PASSWORD_DELLA_TUA_RETE"

#endif // CONFIG_SECRETS_H
