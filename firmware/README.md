# BC-250 power controller firmware (Pico 2 W / Pico 2 WH)

## Flash it
1. Hold BOOTSEL on the Pico, plug it into a PC → it shows up as a USB drive `RP2350`.
2. Copy `release/bc250_power_v4.uf2` onto it. The Pico reboots and runs the firmware.

## What it does
| Input | State | Result |
|---|---|---|
| Button short press | off | PSU on (GP15 high) → board boots (AUTO_PWRON) |
| Button short press | running | USB **Power** key sent → SteamOS shuts down |
| Button short press | stopping | Power key sent again |
| Button held 5 s | any (PSU on) | PSU off immediately (LED goes dark after 1 s of holding) |
| TPM pin 9 low ≥ 5 s | running/stopping | PSU off, Bluetooth controllers disconnected |
| BT controller connects or PS/Xbox/Home pressed | off | PSU on |
| BT controller button | BC-250 USB suspended | USB remote wake-up |
| PSU on but TPM never goes high | starting | PSU off after 180 s |
| SteamOS ignores the key for 90 s | stopping | back to running (press again or hold) |

Short press while the board is still booting is ignored on purpose (hold 5 s to abort).

**Button LED:** off while off · slow blink (1 Hz) booting · solid running · fast blink shutting down / TPM dropped.

**Controllers - up to 4 at once, the BC-250 sees the real controllers:**
| You connect (Bluetooth) | BC-250 sees (USB) | What works |
|---|---|---|
| 1-4 DualSense / Edge | 1-4 DualSense (054C:0CE6 / 0DF2) | PS glyphs, touchpad, gyro, lightbar, player LEDs, adaptive triggers, rumble |
| 1-4 DualShock 4 | 1-4 DualShock 4 (054C:05C4 / 09CC) | PS glyphs, touchpad, gyro, lightbar, rumble |
| 1-4 Switch Pro | 1-4 Pro Controllers (057E:2009) | Nintendo layout, gyro, HD rumble, player LEDs |
| Mixed (e.g. DualSense + Switch Pro), or Xbox / 8BitDo / others | one Xbox 360 pad per controller | buttons, sticks, triggers, rumble |

How: DualSense, DualShock 4 and Switch Pro use the same report format over Bluetooth and USB, so the Pico re-plugs itself on USB as those exact controllers (one USB interface per controller) and passes the raw reports both ways. It also fetches each controller's own calibration and MAC so Linux/Steam treat them as genuine.
A USB device has one identity, so when the group becomes mixed everyone switches to Xbox 360 pads; when it's back to one kind they switch back. Each time the group changes (a controller joins or leaves) the BC-250 sees a short USB unplug/replug (~0.5 s) - all controllers drop for that moment.

Limits:
- Several real DualSense/DS4/Pro controllers inside one USB device is less common than one-per-cable; Linux handles it, but if Steam shows only one of them, set `MAX_PADS 1` or tell me and I'll add a switch that forces Xbox mode for groups.
- Not passed through: DualSense/DS4 headphone jack, speaker and mic (audio).
- With no controller connected the Pico shows up as a small "BC-250 Power Controller" device (power key only).
- Short-pressing the case button while PlayStation/Switch identities are active: the Pico re-plugs as the power-key device (~2 s), then sends the key. The controllers disappear from SteamOS at that moment - it's shutting down anyway. If the shutdown doesn't happen within 90 s, they come back.

## First-time pairing
Put the controller in pairing mode (DualSense: Create + PS until it flashes; Xbox: hold the pair button). The Pico pairs it automatically and stores the key in flash. **Pairing a controller counts as "controller connected" and turns the BC-250 on** — that's expected.
After that, pressing the PS/Xbox/Home button wakes the BC-250.

When the BC-250 shuts down, the Pico disconnects the controllers so they go to sleep. For 20 s after power-off, reconnecting controllers are dropped instead of waking the board (stops an instant re-power).

## SteamOS check
Your plan already sets `HandlePowerKey=poweroff`. To confirm the key arrives: `sudo evtest` → pick the "BC-250 Power Controller" or "Microsoft Controller" device → short-press the case button → you should see `KEY_POWER`. If it shows but nothing happens, check `journalctl -u systemd-logind` for the power key and any inhibitor (`systemd-inhibit --list`).

## Settings
All timings, pins, LED brightness and the controller limit are in `src/config.h`. Defaults:
`MAX_PADS 4` (also `CONFIG_BLUEPAD32_MAX_DEVICES` in `sdkconfig.h`), `LED_BRIGHTNESS_STANDBY 0` (set e.g. 8 for a faint glow when off), `BOOT_TIMEOUT_MS 180000` (0 = disable).

Debug log: UART0 TX on GP0, 115200 baud (optional, nothing has to be connected).

## Rebuild (Linux)
```
sudo apt install cmake ninja-build gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
git clone -b 2.3.1 https://github.com/raspberrypi/pico-sdk.git
(cd pico-sdk && git submodule update --init lib/tinyusb lib/btstack lib/cyw43-driver lib/mbedtls)
git clone https://github.com/ricardoquesada/bluepad32.git
(cd bluepad32 && git submodule update --init external/btstack)
export PICO_SDK_PATH=$PWD/pico-sdk
cmake -S firmware -B firmware/build -G Ninja -DBLUEPAD32_ROOT=$PWD/bluepad32
ninja -C firmware/build   # -> firmware/build/bc250_power.uf2
```
Built with Pico SDK 2.3.1 and Bluepad32 master (Oct 2026).

## Files
- `src/main.c` – power state machine, button, TPM, LED, USB power key
- `src/bt_bridge.c` – Bluepad32 platform: pairing, wake events, Xbox 360 conversion, raw passthrough for DualSense/DS4/Switch Pro
- `src/usb_device.c` – USB identities (idle / Xbox 360 / DS4 / DualSense / Switch Pro), descriptors built per group, feature reports, re-plugging
- `src/pads.c` – per-controller slots shared between the Bluetooth and USB sides
- `src/usb_xinput.c` – TinyUSB class driver for the Xbox 360 interfaces
- `src/config.h` – everything you'd tweak

## v2 changes (17 Oct 2026)
Problem seen on the real build: after a SteamOS shutdown the PSU switched off correctly, but the Pico then ignored the button until 5VSB was unplugged; and with only 5VSB present, the BC-250's rear LEDs glowed. Both happened only with the Pico's USB plugged into the BC-250.
Cause: the RP2350 forces "VBUS present" in TinyUSB, so it can't tell "host switched off" from "host asleep"; its D+ pull-up also back-feeds the unpowered BC-250 through the USB lines.
Fix:
- USB (D+ pull-up) is **off while the PSU is off**; switched on when the PSU is on **and** the BC-250's USB port has 5 V (VBUS read via CYW43 WL_GPIO2, polled every `VBUS_POLL_MS`).
- The power key is only requested when USB is enabled.
- **Watchdog while off** (`WATCHDOG_OFF_MS` 3000): restarts the Pico if it ever hangs with the PSU off; never armed while the PSU is on.
- `hardware_watchdog` added to CMakeLists.

## v3 changes (17 Oct 2026)
- The Pico's own VBUS->VSYS diode leaks 5VSB back into the BC-250's USB port while the board is off
  (back-panel LEDs glow). Fix: use a USB cable with the VBUS (red, pin 1) line cut or taped; only D+, D- and GND go through.
- `USE_VBUS_SENSE 0` (config.h): USB is enabled whenever the PSU is on, without waiting for VBUS, so it works with that cable.

## v4 changes (20 Oct 2026) - Bluetooth self-healing
Seen on the real build: after a day or so in standby the case button still worked, but controllers
could no longer wake the BC-250 until the PSU was unplugged for ~10 s (the Bluetooth side of the
CYW43 radio / BTstack had stopped, while the main loop kept running, so the 3 s watchdog never fired).
- **Bluetooth health check:** every 10 s the Pico sends the radio a harmless HCI command
  (Read Local Version Information). 3 missed answers in a row (or the stack not running for 60 s)
  -> Bluetooth is considered dead.
- **Self-restart, PSU off only:** when Bluetooth is dead the Pico restarts itself (same as unplugging
  the PSU, ~2 s). If it happens while the board is running, the restart waits until it is switched off.
  After a restart GP15 is low, so the PSU stays off; paired controllers stay saved.
  Back-off: after 3 such restarts in a row, only every 30 min.
- **Idle restart:** with the PSU off for 6 h and no controller connected, the Pico restarts anyway
  (`IDLE_RESTART_MS`, 0 = never).
- **VBUS sensing back on** (`USE_VBUS_SENSE 1`) for cables with the red wire still connected, and
  VBUS is now only polled while the PSU is on, so the CYW43 chip is left to Bluetooth in standby.
- **Debug log** (GP0, 115200): a status line every minute (`bc250: alive ... bt ready=1 hci=2 pings=.. misses=..`),
  and the reason for each restart is printed on the next boot.
- Settings: `BT_HEALTH_*`, `IDLE_RESTART_MS`, `RESTART_QUIET_MS`, `STATUS_LOG_MS` in `src/config.h`.
