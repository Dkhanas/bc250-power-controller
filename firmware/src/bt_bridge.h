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
// bt_health_task() returns false once Bluetooth has stopped answering (main.c then restarts the
// Pico, but only while the PSU is off).
bool bt_health_task(uint32_t now_ms);
void bt_health_status(uint32_t now_ms, char* buf, int len);   // one-line summary for the log
