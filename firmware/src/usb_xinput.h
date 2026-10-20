// Minimal Xbox 360 (XInput) gamepad class driver for TinyUSB.
// Linux binds its "xpad" driver to these interfaces, so SteamOS sees a normal Xbox 360 pad.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "tusb.h"
#include "xinput_report.h"

// One XInput interface = interface descriptor + vendor 0x21 descriptor + 2 endpoints
#define XINPUT_DESC_LEN (9 + 17 + 7 + 7)
#define XINPUT_DESCRIPTOR(_itf, _ep_in, _ep_out) \
    9, TUSB_DESC_INTERFACE, _itf, 0, 2, 0xFF, 0x5D, 0x01, 0, \
    17, 0x21, 0x00, 0x01, 0x01, 0x25, _ep_in, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, _ep_out, 0x08, 0x00, 0x00, \
    7, TUSB_DESC_ENDPOINT, _ep_in, TUSB_XFER_INTERRUPT, 32, 0, 1, \
    7, TUSB_DESC_ENDPOINT, _ep_out, TUSB_XFER_INTERRUPT, 32, 0, 8

// Is pad `idx` enumerated and its IN endpoint free?
bool xinput_ready(uint8_t idx);
// Queue a report. Returns false if not ready (try again next loop).
bool xinput_send(uint8_t idx, const xinput_report_t* report);

// Called (from tud_task context) when the host sets rumble on pad `idx`. 0..255 each.
void xinput_rumble_cb(uint8_t idx, uint8_t strong, uint8_t weak);
