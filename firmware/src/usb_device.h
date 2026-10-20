// USB side: which device the Pico pretends to be ("personality"), built from the connected pads.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    USB_P_IDLE,     // no controller: only the HID power key
    USB_P_XBOX,     // one Xbox 360 pad per controller + HID power key (mixed / other controllers)
    USB_P_DS4,      // one real DualShock 4 per controller (all DS4)
    USB_P_DS5,      // one real DualSense per controller (all DualSense)
    USB_P_SWITCH,   // one real Switch Pro Controller per controller (all Switch Pro)
} usb_personality_t;

extern usb_personality_t usb_active;   // what the BC-250 currently sees
extern uint8_t usb_n;                  // pad interfaces currently enumerated
extern int8_t usb_map[MAX_PADS];       // pad interface index -> pad slot (pads.h)

// Ask for a personality showing the pads in slot_mask (bit i = pads[i]); re-plugs if different
void usb_set_target(usb_personality_t p, uint8_t slot_mask);
usb_personality_t usb_target_personality(void);
uint8_t usb_active_mask(void);
bool usb_switching(void);
bool usb_has_power_key(void);   // HID power key available right now (instance 0)
void usb_device_init(void);      // once at boot, before tusb_init()
// Main loop, async_context lock held: re-plugging + forwarding native input reports
void usb_task(uint32_t now_ms);
// Switch the USB connection on/off (D+ pull-up). Off while the BC-250 has no power.
void usb_allow(bool on);
bool usb_allowed(void);
