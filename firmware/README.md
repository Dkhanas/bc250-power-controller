# BC-250 power controller firmware (Pico 2 W / Pico 2 WH)

## Flash it
1. **Take the Pico out of its sockets** (or unplug the PSU from the wall for 30 s). The Pico runs on standby
   power and only checks BOOTSEL at power-up.
2. Hold BOOTSEL, plug the Pico into a PC → it shows up as a USB drive `RP2350`. Release BOOTSEL.
3. Copy **one** file from `release/` onto it. The drive disappears; the Pico runs the new firmware.
   - `bc250_power_v8.uf2` – normal firmware (phone page, no Home Assistant).
   - `bc250_power_v8_ha.uf2` – the same plus **Home Assistant** (MQTT). Only if you run Home Assistant.

   You can switch between the two at any time; settings and paired controllers stay.
4. Put it back the same way round (USB toward the front). Paired controllers stay paired.

## What it does
| Input | State | Result |
|---|---|---|
| Button short press | off | PSU on (GP15 high) → board boots (AUTO_PWRON) |
| Button short press | running | USB **Power** key sent → SteamOS shuts down |
| Button short press | stopping | Power key sent again |
| Button held 2 s, released | any | **Pairing window** for 60 s: a new controller can pair (LED double-blinks) |
| Button held 8 s, released (LED stays lit) | off | **Wi-Fi on** for 30 min after the last use of the page (v8) |
| Button held 5 s | any (PSU on) | PSU off immediately (LED goes dark after 4.5 s of holding) |
| TPM pin 9 low ≥ 5 s | running/stopping | PSU off, Bluetooth controllers disconnected |
| Paired BT controller connects or PS/Xbox/Home pressed | off | PSU on |
| Controller in pairing mode, pairing window closed | any | Ignored, so it can be paired with a phone/PC |
| BT controller button | BC-250 USB suspended | USB remote wake-up |
| PSU on but TPM never goes high | starting | PSU off after 180 s |
| SteamOS ignores the key for 90 s | stopping | back to running (press again or hold) |

Short press while the board is still booting is ignored on purpose (hold 5 s to abort).

**Button LED:** off while off · slow blink (1 Hz) booting · solid running · fast blink shutting down / TPM dropped ·
double blink = pairing window open. While holding the button: fast flicker from 2 s ("release = pairing"),
dark from 4.5 s ("keep holding = force off", PSU on only).

**Self-repair (v4-v6):**
- **Bluetooth health check** every 10 s. If the radio stops answering or stops accepting controllers, only
  Bluetooth is restarted (the board keeps running). If that fails 3 times in 30 min, the Pico restarts itself
  after the next power-off.
- **Pairing key guard:** Bluepad32 deletes a controller's pairing key after one failed reconnect; the Pico keeps a
  backup of every key in flash and puts it back.
- **Safety restart** every 6 h in standby with nothing connected; hardware watchdog while the PSU is off.

**Wi-Fi (v8, optional):** phone page at `http://bc250.local` (power, controllers, pairing) and Home Assistant,
set up from your phone through the `BC250-setup` Wi-Fi. Off by default: hold the case button 8 s (console off)
to turn it on, or choose another mode in Settings. See the v8 section below.
**Low battery (v8):** DualSense / DualShock at ≤20 % rumble briefly (every 10 min) and the LED blinks dark every 3 s.

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

## Pairing a controller
1. Hold the case button **2 s** and release when the LED flickers fast. The LED then double-blinks: the
   pairing window is open for 60 s.
2. Put the controller in pairing mode (DualSense: Create + PS until it flashes; DS4: Share + PS;
   Switch Pro: sync button; Xbox: hold the pair button).
3. The Pico pairs it, stores the key in flash and closes the window. If the console was off, it switches on.

Outside the window, controllers in pairing mode are ignored, so you can pair yours with a phone or PC without the
Pico taking it. Exception: Bluetooth LE controllers already bonded to the Pico (e.g. Xbox).
After that, pressing the PS/Xbox/Home button wakes the BC-250.

When the BC-250 shuts down, the Pico disconnects the controllers so they go to sleep. For 20 s after power-off, reconnecting controllers are dropped instead of waking the board (stops an instant re-power).

## SteamOS check
Your plan already sets `HandlePowerKey=poweroff`. To confirm the key arrives: `sudo evtest` → pick the "BC-250 Power Controller" or "Microsoft Controller" device → short-press the case button → you should see `KEY_POWER`. If it shows but nothing happens, check `journalctl -u systemd-logind` for the power key and any inhibitor (`systemd-inhibit --list`).

## Settings
All timings, pins, LED brightness and the controller limit are in `src/config.h`. Defaults:
`MAX_PADS 4` (also `CONFIG_BLUEPAD32_MAX_DEVICES` in `sdkconfig.h`), `LED_BRIGHTNESS_STANDBY 0` (set e.g. 8 for a faint glow when off), `BOOT_TIMEOUT_MS 180000` (0 = disable).

## Log (troubleshooting)
Everything the firmware does goes into a 16 KB RAM log that survives a Pico self-restart (cleared when the PSU is
unplugged). **Don't unplug the PSU before reading it** when something went wrong.
- **On the BC-250** (while no controller is connected), in Konsole: `sudo cat /dev/ttyACM0` prints the log, then
  new lines live (Ctrl+C to stop). Commands, from a second Konsole: `echo s | sudo tee /dev/ttyACM0` (status line),
  `p` (open pairing window), `r` (restart Bluetooth, for testing), `c` (clear log), `h` (help).
- **UART:** GP0 (pin 1) TX, 115200 baud, with a 3.3 V USB-serial adapter (optional).
- A status line is logged every 5 min: state, PSU, USB, Bluetooth health, restarts, restored keys, connected pads.

## Rebuild
The library versions are pinned in `deps.env` (Pico SDK, Bluepad32, BTstack), so a build always gives the same
firmware. On Linux / WSL:
```
sudo apt install git cmake ninja-build gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
bash firmware/build.sh        # -> firmware/build/bc250_power.uf2 and bc250_power_ha.uf2 (downloads the libraries once into firmware/.deps/)
```
**GitHub builds it too:** every push builds the firmware (Actions tab → the run → *Artifacts*). Pushing a version tag
publishes a **Release** with both `.uf2` files (`bc250_power_v8.uf2`, `bc250_power_v8_ha.uf2`): on GitHub **Actions → Firmware → Run workflow**, type `v8`, Run (or `git tag v8 && git push origin v8`).
The Release text is that version's section in [`../CHANGELOG.md`](../CHANGELOG.md) (`## v8 — <date>`); write it before
tagging, otherwise the release step stops with an error.

## Files
- `src/main.c` – power state machine, button, TPM, LED, USB power key
- `src/bt_bridge.c` – Bluepad32 platform: pairing, wake events, Xbox 360 conversion, raw passthrough for DualSense/DS4/Switch Pro
- `src/usb_device.c` – USB identities (idle / Xbox 360 / DS4 / DualSense / Switch Pro), descriptors built per group, feature reports, re-plugging
- `src/pads.c` – per-controller slots shared between the Bluetooth and USB sides
- `src/usb_xinput.c` – TinyUSB class driver for the Xbox 360 interfaces
- `src/log.c` – RAM event log + USB serial log port (v5), settings commands (v8)
- `src/net.c` – Wi-Fi, web page, Home Assistant / MQTT (v8)
- `src/settings.c` – Wi-Fi / MQTT / PIN settings in flash (v8)
- `src/dhcpd.c`, `src/dnsd.c` – addresses + captive portal for the setup Wi-Fi (v8)
- `web/` – the web pages and icon (embedded by `web/embed.cmake`)
- `build.sh`, `deps.env` – build with the pinned library versions
- `src/lwipopts.h` – network stack settings (v8)
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

## v5 changes (20 Oct 2026) - Bluetooth repair while running + log on the BC-250
Seen on the real build: with the board **running**, a controller that disconnected after idling could not
reconnect (it kept searching). v4 only repaired Bluetooth after a shutdown, because restarting the whole Pico
drops GP15 and would cut the PSU.
- **Bluetooth-only restart:** the Bluetooth stack is stopped, the CYW43 radio chip is reset (its firmware is
  reloaded) and Bluepad32 is set up again. GP15 is never touched, so the board keeps running. Takes a few seconds;
  after that, pressing PS reconnects.
- **Triggers:**
  - the radio doesn't answer the 10 s health check 3 times in a row;
  - **page scan is off** (the Pico isn't reachable for controllers). The health check now uses HCI
    "Read Scan Enable", so it sees this even when the radio itself is fine. It first tries switching
    page scan back on, then restarts;
  - a controller links up but isn't ready within 20 s, or fails to connect 3 times within 60 s.
- **Limits:** at most 3 Bluetooth restarts per 30 min. If they don't help, the Pico restarts itself after the next
  power-off, as in v4.
- **Event log:** everything the firmware prints goes into a 16 KB RAM log that survives a Pico self-restart
  (cleared when the PSU is unplugged). The status line is now every 5 min; controller connects, disconnects,
  failed attempts and every repair are logged.
- **Reading the log on the BC-250:** while **no controller is connected**, the Pico also appears as a USB serial
  port ("BC-250 Power Controller log"). In SteamOS desktop mode, open Konsole:
  ```
  sudo cat /dev/ttyACM0          # prints the whole log, then new lines live; Ctrl+C to stop
  ```
  Commands (in a second Konsole while `cat` runs): `echo s | sudo tee /dev/ttyACM0` = status line,
  `echo r | sudo tee /dev/ttyACM0` = restart Bluetooth now (to test the repair), `c` = clear log, `h` = help.
  `sudo` needs a password: set one once with `passwd`.
- Settings: `BT_CONNECT_*`, `BT_RESTART_*`, `LOG_SIZE`, `STATUS_LOG_MS` in `src/config.h`.

## v6 changes (20 Oct 2026) - pairing key guard
Seen on the real build with v5: a controller that had disconnected kept "searching" and only came back after
**re-pairing**. So the Pico's Bluetooth was alive, but the controller's pairing key was gone.
Cause: Bluepad32 (`uni_bt_bredr.c`, `on_l2cap_channel_opened`) **deletes a controller's stored pairing key**
whenever opening its L2CAP channel fails once ("Removing key for device"). One unlucky reconnect is enough.
After that the controller keeps reconnecting with its key, the Pico refuses, and only re-pairing helps.
- **Key guard:** when a controller connects successfully, the Pico saves a backup of its pairing key in flash,
  in the same BTstack storage, under its own tags. It writes only when the key changes, e.g. after a re-pair;
  up to 8 controllers. Twice a second it checks that every backed-up key is still there, and puts back any key
  Bluepad32 deleted.
- Logged as `key guard: pairing key for XX:XX:... was deleted - restored it`; the status line shows `keys_restored=`.
- A controller you **re-pair** gets its new key backed up automatically. Controllers paired before v6 get a
  backup the first time they connect with v6.
- Setting: `KEY_GUARD_CHECK_MS` in `src/config.h`.

## v7 changes (6 Nov 2026) - pairing window
Before v7 the Pico paired with **any** controller put in pairing mode nearby. So pairing your controller with a
phone or PC failed: the Pico took it first, and also switched the BC-250 on.
- **Controllers in pairing mode are now ignored**, unless the pairing window is open.
- **Open the pairing window:** hold the case button **2 s** and release. The LED double-blinks fast while you
  hold, which means "release now". Then the LED **double-blinks for up to 60 s** (`PAIRING_WINDOW_MS`).
  Put the new controller in pairing mode within that time. The window closes as soon as it's paired.
  It works with the console on or off. Pairing while off still switches the console on.
- **Hold 5 s** with the console on still forces it off. Past 4.5 s the LED goes dark ("keep holding = force off"),
  and releasing between 4.5 and 5 s does nothing.
- **Already-paired controllers reconnect as before** with PS / Xbox / Home. A reconnect doesn't use pairing mode.
  Bluetooth LE controllers (e.g. Xbox) reconnect by advertising, so they're let in when already bonded to the Pico.
- Log: `ignoring XX:XX:... in pairing mode (pairing window closed ...)`, `pairing window open ...`,
  `new controller ... paired`. The log port command `p` also opens the window.
- Settings: `PAIR_HOLD_MS`, `PAIR_HOLD_MAX_MS`, `PAIRING_WINDOW_MS`, `LED_BLINK_PAIRING_MS` in `src/config.h`.

## v8 changes (6 Nov 2026) - Wi-Fi: phone page + Home Assistant, controller list, low-battery warning
The Pico 2 W's Wi-Fi is now used. Nothing secret is in the code or the repo: settings are saved in the Pico's flash.

### Set up Wi-Fi (once, from your phone)
0. **Turn Wi-Fi on:** console off, **hold the case button 8 s** until the LED stays lit, release.
   (Wi-Fi is off by default, see "When Wi-Fi is on" below.)
1. On your phone, open the Wi-Fi settings and tap **`BC250-setup`** (no password). This network only exists while the
   Pico isn't set up yet.
2. **The setup page opens by itself** (captive portal). If it doesn't, open any web address, or `http://192.168.4.1`.
3. **Tap your network** in the list, type its password, tap **Connect**. 2.4 GHz networks only.
4. `BC250-setup` disappears and the Pico joins your network. Put your phone back on your normal Wi-Fi and open
   **http://bc250.local**, then use **Add to Home Screen** in the browser menu: you get a **BC-250 icon**.

If the saved network can't be reached for 5 minutes (wrong password, new router), `BC250-setup` comes back for
10 minutes so you can fix it, then the Pico tries your network again.

### When Wi-Fi is on (Settings → "Wi-Fi is on")
| Mode | Wi-Fi runs | Good for |
|---|---|---|
| **Only when I turn it on** (default) | after holding the case button 8 s (console off); off again 30 min after the page was last used | rarely using the page; nothing runs in the background |
| **Only while the console is off** | whenever the console is off; off 15 s after it switches on | power on from the phone / Home Assistant, never disturbing controllers during play |
| **Always** | always | page and Home Assistant always reachable |
| **Never** | never | no Wi-Fi at all |

Holding the case button 8 s (console off) turns Wi-Fi on for 30 min **in every mode**, so you can always get back to
the page. The main page shows when Wi-Fi will switch off.

### The page (http://bc250.local or the home-screen icon)
- **Power on / Shut down** (clean), status and the lowest controller battery.
- **Controllers:** every controller paired with the Pico, connected or not, with its battery.
  **+ Pair a new controller** opens the 60 s pairing window and shows a countdown and the result
  ("✓ DualSense paired"). **Forget** removes a controller, which then needs pairing again.
- Bottom: **Settings** (Wi-Fi network from a list, PIN, Home Assistant) and **Force off** (asks first).
- **PIN** (optional, in Settings): then the buttons and settings ask for it once per phone. Without it, anyone on
  your Wi-Fi can use the buttons.
- If `bc250.local` doesn't open (some Android phones), use the address shown in your router's device list ("bc250").

### Home Assistant (optional, `bc250_power_v8_ha.uf2` only)
Needs the Home Assistant firmware file; the normal one has no MQTT code and no Home Assistant section in Settings.
Home Assistant itself can run on a Raspberry Pi 5 (Home Assistant OS); add the **Mosquitto broker** add-on and the
**MQTT** integration. For Apple Home, add Home Assistant's **HomeKit Bridge** integration and include the BC-250's
power switch.

Settings → **Home Assistant (MQTT)**: broker address (+ user / password). The BC-250 then appears in Home Assistant
by itself as a device **"BC-250"**: **Power** switch (on = power on, off = clean shutdown), **Force off** and
**Pair a new controller** buttons, **State**, **Controllers** and **Controller battery** sensors.
Topics: `bc250/<id>/state` (JSON), `bc250/<id>/cmd/power|force_off|pair`, `bc250/<id>/availability`.

### Low-battery warning
For DualSense / DualShock (they report their battery): at **20 % or less** the controller **rumbles briefly**
(again every 10 min) and the **button LED blinks dark briefly every 3 s** while the console is on.
Xbox / Switch Pro / 8BitDo don't report battery to the Pico.

### Also possible from the BC-250 (log port, no controller connected)
`echo 'wifi NETWORK|PASSWORD' | sudo tee /dev/ttyACM0`, `wifi on` (turn on now, 30 min),
`wifi request` / `wifi whileoff` / `wifi always` / `wifi off` (the mode), `wifi setup` (forget the network, start `BC250-setup`), `mqtt HOST[:PORT] [USER|PASSWORD]`, `mqtt off`,
`pin 1234`, `pin off`, `net` (status). Answers appear in `sudo cat /dev/ttyACM0`.

### Notes
- **Wi-Fi and Bluetooth share one radio chip.** If controllers lag with "Always", use "Only while the console is off".
- In "Always" / "Only while the console is off", Wi-Fi adds a little standby power (that's what makes "Power on" work).
- **Home Assistant** only sees the BC-250 while Wi-Fi is on: use "Always" or "Only while the console is off" with it.
- A Bluetooth repair (v5) resets the radio chip; Wi-Fi drops for a few seconds and reconnects by itself.
- `BC250-setup` is open (no password) so setup needs no typing; it only exists until a network is saved, and only
  then (no network saved yet) does saving work without a PIN. To give it a password, set `SETUP_HOTSPOT_PASSWORD`
  (8+ characters) in `src/config.h`.
- Web pages are normal files in `firmware/web/` (HTML/CSS/JS + icon); the build embeds them (`web/embed.cmake`).
  Edit them there and rebuild.
- Code: `src/net.c` (Wi-Fi, web server, Home Assistant), `src/settings.c` (settings in flash), `src/dhcpd.c` +
  `src/dnsd.c` (addresses + captive portal on `BC250-setup`), `src/lwipopts.h` (network stack).
  `CMakeLists.txt` switches Bluepad32 to the Wi-Fi driver variant without modifying Bluepad32.
- The controller list remembers up to 8 controllers (key guard table, now with name and type).
- Settings: `LOW_BATTERY_*`, `SETUP_HOTSPOT_*`, `WIFI_FAIL_TO_SETUP_MS`, `WIFI_HOLD_MS`, `WIFI_REQUEST_MS`,
  `WIFI_WHILE_OFF_GRACE_MS` in `src/config.h`.
