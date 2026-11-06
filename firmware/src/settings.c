#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_tlv.h"
#include "net.h"

#define SETTINGS_TAG   0x4243534Eu   // 'B' 'C' 'S' 'N'
#define SETTINGS_MAGIC 0xBC250008u

settings_t settings;
volatile uint32_t settings_version;

static bool tlv(const btstack_tlv_t** impl, void** ctx) {
    *impl = NULL;
    *ctx = NULL;
    btstack_tlv_get_instance(impl, ctx);
    return *impl != NULL;
}

void settings_load(void) {
    const btstack_tlv_t* impl;
    void* ctx;
    memset(&settings, 0, sizeof(settings));
    if (tlv(&impl, &ctx)) {
        settings_t s;
        if (impl->get_tag(ctx, SETTINGS_TAG, (uint8_t*)&s, sizeof(s)) == (int)sizeof(s) && s.magic == SETTINGS_MAGIC) {
            settings = s;
            // make sure every string is terminated, whatever is in flash
            settings.ssid[sizeof(settings.ssid) - 1] = 0;
            settings.pass[sizeof(settings.pass) - 1] = 0;
            settings.mqtt_host[sizeof(settings.mqtt_host) - 1] = 0;
            settings.mqtt_user[sizeof(settings.mqtt_user) - 1] = 0;
            settings.mqtt_pass[sizeof(settings.mqtt_pass) - 1] = 0;
            settings.pin[sizeof(settings.pin) - 1] = 0;
        }
    }
    if (settings.magic != SETTINGS_MAGIC) settings.wifi_mode = WIFI_MODE_ON_REQUEST;   // nothing saved yet
    if (settings.wifi_mode > WIFI_MODE_NEVER) settings.wifi_mode = WIFI_MODE_ON_REQUEST;
    settings.magic = SETTINGS_MAGIC;
    if (!settings.mqtt_port) settings.mqtt_port = 1883;
    printf("bc250: settings: wifi %s (%s), mqtt %s, web pin %s\n", settings.ssid[0] ? "set" : "not set",
           net_mode_name(settings.wifi_mode),
           settings.mqtt_host[0] ? "set" : "off", settings.pin[0] ? "set" : "off");
}

void settings_save(void) {
    const btstack_tlv_t* impl;
    void* ctx;
    settings_version++;
    if (!tlv(&impl, &ctx)) {
        printf("bc250: settings: flash store not ready - not saved\n");
        return;
    }
    impl->store_tag(ctx, SETTINGS_TAG, (const uint8_t*)&settings, sizeof(settings));
}

// Copy src (up to stop char or end) into dst of size n; returns pointer after the stop char (or NULL)
static const char* take(char* dst, size_t n, const char* src, char stop) {
    size_t i = 0;
    while (*src && *src != stop) {
        if (i + 1 < n) dst[i++] = *src;
        src++;
    }
    dst[i] = 0;
    return *src == stop ? src + 1 : NULL;
}

static bool starts(const char* line, const char* word) {
    size_t n = strlen(word);
    return strncmp(line, word, n) == 0 && (line[n] == 0 || line[n] == ' ');
}

bool settings_command(const char* line) {
    if (starts(line, "wifi")) {
        const char* a = line + 4;
        while (*a == ' ') a++;
        if (!*a) {
            printf("bc250: usage: wifi NETWORK|PASSWORD | wifi on | wifi always | wifi whileoff | wifi request | wifi off | wifi setup\n");
        } else if (strcmp(a, "on") == 0) {
            net_wifi_request();
        } else if (strcmp(a, "always") == 0 || strcmp(a, "whileoff") == 0 || strcmp(a, "request") == 0 ||
                   strcmp(a, "off") == 0) {
            settings.wifi_mode = a[0] == 'a' ? WIFI_MODE_ALWAYS : a[0] == 'w' ? WIFI_MODE_WHILE_OFF
                                 : a[0] == 'r' ? WIFI_MODE_ON_REQUEST : WIFI_MODE_NEVER;
            settings_save();
            printf("bc250: wifi: mode '%s'\n", net_mode_name(settings.wifi_mode));
        } else if (strcmp(a, "setup") == 0) {
            settings.ssid[0] = settings.pass[0] = 0;
            settings_save();
            net_wifi_request();
            printf("bc250: wifi: network forgotten - setup network BC250-setup starting\n");
        } else {
            const char* rest = take(settings.ssid, sizeof(settings.ssid), a, '|');
            if (rest) take(settings.pass, sizeof(settings.pass), rest, 0);
            else settings.pass[0] = 0;   // open network
            settings_save();
            net_wifi_request();          // connect now, whatever the mode
            printf("bc250: wifi: saved network '%s'%s - connecting\n", settings.ssid, settings.pass[0] ? " (with password)" : " (open)");
        }
        return true;
    }
#if !BC250_HA
    if (starts(line, "mqtt")) {
        printf("bc250: this firmware has no Home Assistant support - flash bc250_power_ha.uf2 for it\n");
        return true;
    }
#endif
    if (starts(line, "mqtt")) {
        const char* a = line + 4;
        while (*a == ' ') a++;
        if (!*a) {
            printf("bc250: usage: mqtt HOST[:PORT] [USER|PASSWORD]   or   mqtt off\n");
        } else if (strcmp(a, "off") == 0) {
            settings.mqtt_host[0] = 0;
            settings_save();
            printf("bc250: mqtt: turned off\n");
        } else {
            char hostport[80];
            const char* rest = take(hostport, sizeof(hostport), a, ' ');
            char* colon = strrchr(hostport, ':');
            settings.mqtt_port = 1883;
            if (colon) {
                int p = atoi(colon + 1);
                if (p > 0 && p < 65536) settings.mqtt_port = (uint16_t)p;
                *colon = 0;
            }
            snprintf(settings.mqtt_host, sizeof(settings.mqtt_host), "%s", hostport);
            settings.mqtt_user[0] = settings.mqtt_pass[0] = 0;
            if (rest) {
                while (*rest == ' ') rest++;
                const char* pw = take(settings.mqtt_user, sizeof(settings.mqtt_user), rest, '|');
                if (pw) take(settings.mqtt_pass, sizeof(settings.mqtt_pass), pw, 0);
            }
            settings_save();
            printf("bc250: mqtt: saved broker %s:%u%s\n", settings.mqtt_host, settings.mqtt_port,
                   settings.mqtt_user[0] ? " (with login)" : "");
        }
        return true;
    }
    if (starts(line, "pin")) {
        const char* a = line + 3;
        while (*a == ' ') a++;
        if (!*a) {
            printf("bc250: usage: pin 1234   or   pin off\n");
        } else if (strcmp(a, "off") == 0) {
            settings.pin[0] = 0;
            settings_save();
            printf("bc250: web pin: off (anyone on your network can use the buttons)\n");
        } else {
            take(settings.pin, sizeof(settings.pin), a, ' ');
            settings_save();
            printf("bc250: web pin: set (the web page asks for it once)\n");
        }
        return true;
    }
    if (strcmp(line, "net") == 0) {
        net_print_status();
        return true;
    }
    return false;
}
