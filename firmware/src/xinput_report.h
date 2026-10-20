// Xbox 360 input report layout (no USB stack dependency, shared with the Bluetooth side)
#pragma once
#include <stdint.h>

// 20-byte input report, exactly as an Xbox 360 wired pad sends it
typedef struct __attribute__((packed)) {
    uint8_t  report_id;   // 0x00
    uint8_t  length;      // 0x14
    uint16_t buttons;     // see XI_* below
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;   // Y axes: up = positive
    uint8_t  reserved[6];
} xinput_report_t;

enum {
    XI_DPAD_UP = 1u << 0, XI_DPAD_DOWN = 1u << 1, XI_DPAD_LEFT = 1u << 2, XI_DPAD_RIGHT = 1u << 3,
    XI_START = 1u << 4,   XI_BACK = 1u << 5,      XI_LS = 1u << 6,        XI_RS = 1u << 7,
    XI_LB = 1u << 8,      XI_RB = 1u << 9,        XI_GUIDE = 1u << 10,
    XI_A = 1u << 12,      XI_B = 1u << 13,        XI_X = 1u << 14,        XI_Y = 1u << 15,
};
