// Event log (v5): everything printed (printf / Bluepad32 logi) goes to a 16 KB RAM ring that survives
// a Pico self-restart, and to UART0 as before. While no controller is connected the Pico also shows
// up on the BC-250 as a USB serial port ("BC-250 Power Controller log", /dev/ttyACM0): opening it
// prints the whole ring, then streams new lines. Single-letter commands can be typed into it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void log_init(bool keep_old);              // keep_old: after a self-restart, keep what was logged before
void log_set_status_fn(void (*fn)(void));  // called for the "s" command (prints a status line)
void log_task(void);                       // main loop, async_context lock held: feeds the USB serial port
