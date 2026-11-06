// Bluetooth controllers (Bluepad32) -> pad slots (pads.h).
// Call these with the cyw43 async_context lock held (Bluepad32 callbacks already run inside it).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "pads.h"

// Event counters set by Bluetooth callbacks, compared by main.c
extern volatile uint32_t bt_ev_connected;      // a controller became ready
extern volatile uint32_t bt_ev_system_button;  // PS / Xbox / Home button pressed
extern volatile uint32_t bt_ev_activity;       // any button pressed

void bt_bridge_start(void);            // once, after cyw43_arch_init()
void bt_disconnect_all(void);          // drop every controller (they go to sleep)
void bt_rumble(uint8_t slot, uint8_t strong, uint8_t weak);   // Xbox 360 mode rumble
void bt_native_task(uint32_t now_ms);  // feature-report fetching + USB -> controller output reports

// Bluetooth health check: pings the radio with a harmless HCI command every BT_HEALTH_PERIOD_MS.
// v5: on trouble it first restarts only Bluetooth (radio chip reset + stack re-init), which keeps
// the board running. Returns false only if that keeps failing.
bool bt_health_task(uint32_t now_ms);
void bt_health_status(uint32_t now_ms, char* buf, int len);   // one-line summary for the log
bool bt_restarting(void);      // Bluetooth restart in progress (don't touch the CYW43 chip)

// v7: pairing window. Outside it, controllers in pairing mode are ignored (bonded controllers
// still reconnect). Closes after PAIRING_WINDOW_MS or as soon as a new controller is paired.
void bt_pairing_start(void);
bool bt_pairing_active(void);

// v8: for the web page
typedef struct {
    char addr[18];     // "AA:BB:CC:DD:EE:FF"
    char name[24];
    bool connected;
    int battery;       // %, -1 = unknown / not connected
} bt_ctrl_info_t;
int bt_controllers(bt_ctrl_info_t* out, int max);   // controllers paired with the Pico
bool bt_forget(const char* addr);                   // unpair one ("AA:BB:..."); false if unknown

typedef struct {
    bool active;
    int left_s;
    uint32_t seq;      // changes when a pairing window ends
    char result[8];    // "paired" / "timeout" / ""
    char name[24];     // what got paired
} bt_pair_info_t;
void bt_pairing_info(bt_pair_info_t* pi);
void bt_restart_now(void);     // manual restart (log port command), for testing
