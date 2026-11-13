// Firmware update from the web page (v10), A/B slots with automatic rollback.
//
// Flash layout (firmware/partition_table.json, installed once over USB): two slots "A" and "B" for the firmware,
// the settings / pairing keys stay where they always were (last 8 KB of flash, outside both slots).
// An update is written into the slot that isn't running, then the Pico restarts into it as a "trial" image
// (RP2350 bootrom: try before you buy). The new firmware keeps itself ("buys") only once it runs properly;
// if it hangs or can't start Bluetooth, the next restart goes back to the old firmware by itself.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void ota_boot_init(void);                      // early in main(), after stdio: what booted, from which slot
void ota_setup_slots(void);                    // right after: no slots yet (copied over USB)? set them up, restart
void ota_task(uint32_t now_ms, bool bt_ready); // main loop: keep (buy) a trial firmware once it runs properly
bool ota_just_updated(void);                   // this boot follows an update attempt (new firmware or rolled back)

// Upload (one at a time). Content is a .uf2 file as built (bc250_power_vX.uf2).
int ota_begin(uint32_t file_len, char* err, int en);              // 0 = ready
int ota_write(const uint8_t* d, uint32_t n, char* err, int en);   // 0 = ok, <0 = error (upload cancelled)
int ota_end(char* msg, int mn);                                   // 0 = complete + checked; then ota_reboot()
void ota_cancel(void);
bool ota_busy(void);
void ota_reboot(void);   // restart into the new firmware (does not return)

int ota_status_json(char* b, int n);   // "fw":{...} for /api/status
