#include "log.h"

#include <stdio.h>
#include <string.h>

#include "bt_bridge.h"
#include "config.h"
#include "hardware/sync.h"
#include "pico/stdio/driver.h"
#include "pico/stdlib.h"
#include "tusb.h"

#define LOG_MAGIC 0xB0C25005u

typedef struct {
    uint32_t magic;
    uint32_t total;          // bytes ever written (ring position = total % LOG_SIZE)
    uint32_t boots;          // starts since the ring was created
    bool line_start;
    char buf[LOG_SIZE];
} log_ring_t;

// Not cleared at reset, so the log survives a watchdog / self restart (cleared at power-on)
static log_ring_t __uninitialized_ram(ring);

static void (*status_fn)(void);

static void put(char c) {
    ring.buf[ring.total % LOG_SIZE] = c;
    ring.total++;
}

static void log_out_chars(const char* s, int len) {
    char stamp[24];
    uint32_t save = save_and_disable_interrupts();
    for (int i = 0; i < len; i++) {
        if (ring.line_start && s[i] != '\n') {
            uint64_t us = time_us_64();
            int n = snprintf(stamp, sizeof(stamp), "[%6lu.%03lu] ", (unsigned long)(us / 1000000),
                             (unsigned long)((us / 1000) % 1000));
            for (int k = 0; k < n; k++) put(stamp[k]);
            ring.line_start = false;
        }
        put(s[i]);
        if (s[i] == '\n') ring.line_start = true;
    }
    restore_interrupts(save);
}

static stdio_driver_t log_driver = {
    .out_chars = log_out_chars,
    .crlf_enabled = false,
};

void log_init(bool keep_old) {
    if (!keep_old || ring.magic != LOG_MAGIC || ring.total > 0x7FFFFFFFu) {
        memset(&ring, 0, sizeof(ring));
        ring.magic = LOG_MAGIC;
        ring.line_start = true;
    } else if (!ring.line_start) {
        log_out_chars("\n", 1);
    }
    ring.boots++;
    stdio_set_driver_enabled(&log_driver, true);
}

void log_set_status_fn(void (*fn)(void)) { status_fn = fn; }

// ------------------------------------------------------------------ USB serial port
static bool was_connected;
static uint32_t pos;   // next byte of the ring to send

static void help(void) {
    printf("bc250: log commands: s = status, r = restart Bluetooth now (test), c = clear log, h = help\n");
}

void log_task(void) {
    bool conn = tud_cdc_connected();
    if (conn && !was_connected) {
        pos = ring.total > LOG_SIZE ? ring.total - LOG_SIZE : 0;   // send the whole ring first
        printf("bc250: log port opened (firmware v6, %lu starts since power-on)\n", (unsigned long)ring.boots);
        help();
    }
    was_connected = conn;
    if (!conn) return;

    // commands
    while (tud_cdc_available()) {
        int c = tud_cdc_read_char();
        switch (c) {
            case 's': case 'S':
                if (status_fn) status_fn();
                break;
            case 'r': case 'R':
                printf("bc250: Bluetooth restart requested\n");
                bt_restart_now();
                break;
            case 'c': case 'C': {
                uint32_t save = save_and_disable_interrupts();
                ring.total = 0;
                ring.line_start = true;
                pos = 0;
                restore_interrupts(save);
                printf("bc250: log cleared\n");
                break;
            }
            case 'h': case 'H': case '?':
                help();
                break;
            default:
                break;
        }
    }

    // stream new log bytes
    uint32_t total = ring.total;
    if (total - pos > LOG_SIZE) pos = total - LOG_SIZE;   // fell behind: skip what was overwritten
    while (pos != total) {
        uint32_t room = tud_cdc_write_available();
        if (!room) break;
        uint32_t idx = pos % LOG_SIZE;
        uint32_t n = total - pos;
        if (n > LOG_SIZE - idx) n = LOG_SIZE - idx;   // up to the end of the ring buffer
        if (n > room) n = room;
        n = tud_cdc_write(&ring.buf[idx], n);
        if (!n) break;
        pos += n;
    }
    tud_cdc_write_flush();
}
