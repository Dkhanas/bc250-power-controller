// Wi-Fi (v8): web page at http://bc250.local and Home Assistant over MQTT (auto-discovery).
// Settings come from settings.h (set once over the log port). Everything here runs with the
// async_context lock held (main loop) or inside lwIP callbacks (which hold it too).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum { NET_REQ_NONE, NET_REQ_ON, NET_REQ_SHUTDOWN, NET_REQ_FORCE_OFF, NET_REQ_PAIR } net_req_t;

typedef struct {
    const char* state;      // "OFF" / "STARTING" / "RUNNING" / "STOPPING"
    bool psu;
    uint32_t uptime_s;
    bool pairing;
    int pads;               // connected controllers
    int battery_min;        // lowest battery in %, -1 = unknown / none
    int battery[MAX_PADS];  // per slot, -1 = no controller or unknown
} app_status_t;

// Provided by main.c
void app_get_status(app_status_t* s);
void app_request(net_req_t r);

void net_init(void);              // once, after settings_load()
void net_task(uint32_t now_ms);   // main loop, lock held
void net_notify_change(void);     // something changed: publish the state soon
void net_radio_was_reset(void);   // the CYW43 chip was reset (Bluetooth repair): rejoin Wi-Fi
void net_print_status(void);      // "net" command on the log port
void net_wifi_request(void);      // turn Wi-Fi on now (case button 8 s / log port), for WIFI_REQUEST_MS after last use
void net_console_on(bool on);     // console state, for WIFI_MODE_WHILE_OFF
const char* net_mode_name(int mode);
