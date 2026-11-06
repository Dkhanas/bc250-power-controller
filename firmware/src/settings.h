// Network settings (v8), kept in the Pico's flash (BTstack TLV store, own tag), set from the log port.
// Nothing secret lives in the source code or the repo.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t magic;
    char ssid[33];
    char pass[64];
    char mqtt_host[64];   // empty = MQTT off
    uint16_t mqtt_port;
    char mqtt_user[32];
    char mqtt_pass[64];
    char pin[16];         // empty = web actions need no PIN
    uint8_t wifi_mode;    // WIFI_MODE_* : when Wi-Fi may run
} settings_t;

enum {
    WIFI_MODE_ALWAYS = 0,      // always on
    WIFI_MODE_WHILE_OFF = 1,   // only while the console is off (never during play)
    WIFI_MODE_ON_REQUEST = 2,  // only after a request (case button 8 s / log port), then off after WIFI_REQUEST_MS idle
    WIFI_MODE_NEVER = 3,       // off (a request still turns it on temporarily)
};

extern settings_t settings;
extern volatile uint32_t settings_version;   // bumped on every change (net.c reconnects)

void settings_load(void);   // after cyw43_arch_init() (the TLV store is ready then)
void settings_save(void);   // store + bump settings_version (net.c picks the change up)
// Handle one log-port line ("wifi ...", "mqtt ...", "pin ...", "net"). Returns false if it isn't one.
// Lock held. Never prints passwords.
bool settings_command(const char* line);
