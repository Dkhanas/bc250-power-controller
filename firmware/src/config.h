// BC-250 power controller - user settings
// Everything you may want to tweak lives in this file.
#pragma once

// ---------------------------------------------------------------- pins
#define PIN_PSU         15   // GP15 (pin 20) -> R1 -> Q1 base. High = PSU on (PS_ON pulled to GND)
#define PIN_LED         16   // GP16 (pin 21) -> button LED+
#define PIN_BUTTON      17   // GP17 (pin 22) <- button C, internal pull-up, pressed = low
#define PIN_TPM         2    // GP2  (pin 4)  <- R3 <- TPM pin 9, internal pull-down, high = BC-250 running

// ---------------------------------------------------------------- button
#define BUTTON_DEBOUNCE_MS      30
#define BUTTON_LONG_PRESS_MS    5000    // hold this long = force PSU off

// ---------------------------------------------------------------- power sequencing
#define TPM_DEBOUNCE_MS         50
#define TPM_OFF_DELAY_MS        5000    // TPM low this long while running -> PSU off
// PSU on but TPM never went high (board did not boot) -> PSU off after this.
// 0 = never (leave PSU on until the button is held).
#define BOOT_TIMEOUT_MS         180000
// After the USB power key was sent: if the board is still running after this,
// go back to "running" (LED solid) so a new short press can try again.
#define SHUTDOWN_WAIT_MS        90000
// Length of the USB power-key "press"
#define POWER_KEY_PRESS_MS      150

// ---------------------------------------------------------------- Bluetooth
// After the PSU turns off, controllers that (re)connect within this window do NOT
// wake the board - they are dropped instead. Stops a controller that auto-reconnects
// right after shutdown from switching the BC-250 straight back on.
#define BT_WAKE_COOLDOWN_MS     20000
// Up to this many controllers at once (also the Bluepad32 limit in sdkconfig.h).
// All the same kind (all DualSense, all DS4 or all Switch Pro) -> each appears as the real thing.
// Mixed kinds, or other controllers -> each appears as an Xbox 360 pad.
#define MAX_PADS                4
// Duration passed to the controller for each rumble command (games refresh/stop it).
#define RUMBLE_DURATION_MS      5000

// ---------------------------------------------------------------- button LED (0..255)
#define LED_BRIGHTNESS_ON       255
#define LED_BRIGHTNESS_STANDBY  0       // e.g. 8 for a faint glow while off
#define LED_BLINK_STARTING_MS   500     // half-period while booting
#define LED_BLINK_STOPPING_MS   125     // half-period while shutting down

// ---------------------------------------------------------------- safety
// While the PSU is off the Pico keeps its USB connection switched off (no D+ pull-up).
// Otherwise it leaks power into the unpowered BC-250 (its LEDs glow) and, because the
// RP2350 cannot tell "host switched off" from "host asleep", it keeps waiting for a host
// that is gone. USB is switched on when the PSU turns on and the BC-250's USB port has 5 V.
// 1 = only enable USB when the BC-250's USB 5 V is seen on the Pico's VBUS pin.
// 0 = ignore VBUS (use with a cable whose VBUS line is cut/taped, the recommended setup:
//     otherwise the Pico's VSYS->VBUS diode leaks 5VSB into the switched-off BC-250).
#define USE_VBUS_SENSE          1       // v4: back to 1 - the cable's red wire is still connected
#define VBUS_POLL_MS            250     // only polled while the PSU is on (no CYW43 traffic in standby)
// While the PSU is off a hardware watchdog restarts the Pico if the firmware ever stops
// running for this long. (Not armed while the PSU is on, so a restart can never cut power.)
#define WATCHDOG_OFF_MS         3000

// ---------------------------------------------------------------- Bluetooth self-healing (v4)
// Seen on the real build: after a day or so in standby the button still worked but controllers
// could no longer wake the board until the PSU was unplugged. These restart the Pico by itself.
// A restart only ever happens while the PSU is OFF (GP15 is low after reset, PSU stays off),
// and paired controllers stay saved in flash.
//
// Health check: ask the radio a harmless question every BT_HEALTH_PERIOD_MS. No answer
// BT_HEALTH_MAX_MISSES times in a row (or the stack not running for BT_HEALTH_NOT_READY_MS)
// -> Bluetooth is considered dead.
#define BT_HEALTH_PERIOD_MS     10000
#define BT_HEALTH_TIMEOUT_MS    3000
#define BT_HEALTH_MAX_MISSES    3
#define BT_HEALTH_NOT_READY_MS  60000
// v5: Bluetooth restart without touching the PSU (works while the board is running).
// Also triggered when page scan is off (controllers can't connect), when a controller links up
// but never becomes ready within BT_CONNECT_STUCK_MS, or fails to connect BT_CONNECT_FAILS_MAX
// times within BT_CONNECT_WINDOW_MS.
#define BT_CONNECT_STUCK_MS     20000
#define BT_CONNECT_FAILS_MAX    3
#define BT_CONNECT_WINDOW_MS    60000
#define BT_RESTART_STOP_MS      10000   // stack must stop within this
#define BT_RESTART_START_MS     20000   // and be running again within this
// At most this many Bluetooth restarts per window; after that the Pico restarts itself
// at the next power-off (as in v4).
#define BT_RESTART_MAX_IN_WINDOW 3
#define BT_RESTART_WINDOW_MS    (30u * 60u * 1000u)
// v6: pairing key guard. Bluepad32 deletes a controller's pairing key when one reconnect attempt fails;
// the Pico keeps a backup of every key that worked and restores it. Checked this often.
#define KEY_GUARD_CHECK_MS      500
// Safety net: with the PSU off and no controller connected, restart the Pico this often anyway.
// 0 = never.
#define IDLE_RESTART_MS         (6u * 60u * 60u * 1000u)   // 6 hours
// Don't restart within this long after the PSU went off, or while the button is pressed.
#define RESTART_QUIET_MS        60000
// Status line in the log (USB serial + UART GP0 115200) this often. 0 = off.
#define STATUS_LOG_MS           300000

// ---------------------------------------------------------------- log (v5)
// RAM log size. Readable on the BC-250 over USB serial while no controller is connected.
#define LOG_SIZE                16384
