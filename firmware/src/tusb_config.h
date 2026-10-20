#pragma once
#include "config.h"

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_OS             OPT_OS_PICO

#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

// Standard class: one HID interface (System Control -> Power key)
#define CFG_TUD_HID             MAX_PADS   // power key, or one HID per native controller
#define CFG_TUD_HID_EP_BUFSIZE  64
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

// The XInput (Xbox 360) interfaces come from our own class driver (usb_xinput.c)
