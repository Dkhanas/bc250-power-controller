# Release notes

Firmware versions are the `v…` tags; each GitHub Release shows the section of the same name below.
How to flash: [`firmware/README.md`](firmware/README.md#flash-it). Paired controllers and settings survive an update.

## v9 — 2026-11-13
**Firmware update from the phone page, and Wi-Fi setup checks the connection before closing `BC250-setup`.**
Not tested on real hardware yet.

Install (once, over USB): BOOTSEL, copy `bc250_power_v9.uf2` (or `_ha`), leave it plugged in for 10 s - on its
first start it sets up the two firmware slots by itself and restarts once. Settings and paired controllers stay.
From then on, updates work from the page.

New
- **Settings → Firmware:** choose a `.uf2`, tap **Update firmware** (console off). It is written into the second
  firmware slot while the Pico keeps running, then the Pico restarts into it. If the new firmware doesn't run
  properly (hangs, or Bluetooth doesn't start within 60 s), the Pico **goes back to the previous one by itself**.
- Only BC-250 firmware with web update is accepted; damaged or wrong files are refused before anything changes.
- New flash layout with two firmware slots, set up by the firmware itself on its first start (one file to copy).
- **Pairing window by double-clicking the case button** (was: hold 2 s). Single clicks act 0.4 s after release,
  while the Pico waits for a possible second click.
- **Wi-Fi turns on by itself for 30 min** when the Pico gets power (5 V standby / PSU plugged in) and after a new
  firmware is installed: a freshly flashed Pico shows `BC250-setup` without the 8 s button hold. Not at power-up in
  the "Never" mode; not after the Pico's own self-restarts.

Changed
- **Connect** on the setup page now tries the network first, with `BC250-setup` still on. Only when the Pico got an
  address is the network saved; the page shows **Connected ✓** with the address, then the setup Wi-Fi closes.
- On failure nothing is saved and the setup Wi-Fi stays: the page says why (wrong password, network not found,
  no address from the router, no answer) and you can try again right away instead of waiting 5 minutes.
  **Save anyway** keeps the old behaviour.
- `BC250-setup` starts on the home network's likely channel, so the phone stays connected during the check.
- Firmware images now carry a version number (9.0) and a SHA-256 hash checked by the bootrom. Every release must
  raise `BC250_FW_MAJOR` / `BC250_FW_MINOR` in `firmware/CMakeLists.txt`; the release workflow checks it.

## PCB v12 — 2026-11-06
**Back to v9's ATX pin order, now confirmed with the real PSU cable, plus spaced-out front headers.**

Fixed
- PCB v10 and v11 had the ATX header mirrored (pin 1 at the back). On a built v8 board (everything except the ATX connector worked), 5 V standby measured on the 4th pin
  from the back in the Pico-side row, exactly as v9 (and KiCad's official footprint) expect. v10 / v11 would put 12 V
  on GND: never order them.

Changed
- Front row: J1 button, gap, J2 TPM, gap, J4 → J4003, gap, J3 fan 2 (from v11).
- Bottom-right screw hole moved 1 mm towards the corner so an M3 screw head clears J3.

## v8 — 2026-11-06
**Wi-Fi: control the console from your phone, set up in a minute.** Not tested on real hardware yet.

New
- **Phone page / home-screen icon** (`http://bc250.local`): power on, shut down, force off; status and battery.
- **Your controllers** on the page: every controller paired with the Pico, connected or not, with battery.
  **+ Pair a new controller** shows a countdown and the result ("✓ DualSense paired"). **Forget** unpairs one.
- **Setup from the phone:** hold the case button 8 s (console off), join the open Wi-Fi `BC250-setup`, the setup page opens by itself, pick your network
  from the list, type its password. If the network later can't be reached for 5 min, `BC250-setup` returns for 10 min.
- **Home Assistant** (MQTT) in its own firmware file, `bc250_power_v8_ha.uf2`: the BC-250 appears by itself with a
  power switch, Force off / Pair buttons and State / Controllers / Battery sensors. Without Home Assistant, flash
  `bc250_power_v8.uf2` (no MQTT code, no Home Assistant section in Settings). Switching between the two keeps all settings.
- **Low-battery warning:** DualSense / DualShock at 20 % or less rumble briefly (every 10 min) and the case-button
  LED blinks dark every 3 s while the console is on.
- Optional **PIN** for the page's buttons and settings.
- **Wi-Fi only when you want it** (Settings → "Wi-Fi is on"): only when turned on (default: hold the case button 8 s
  with the console off; off again 30 min after the page was last used), only while the console is off, always, or never.

Changed
- Firmware build uses the Wi-Fi variant of the radio driver (lwIP). Web pages live in `firmware/web/`.
- Library versions are pinned (`firmware/deps.env`); `firmware/build.sh` builds the same firmware every time;
  GitHub builds every push and publishes Releases for tags.

Known issues / watch
- Wi-Fi and Bluetooth share one radio chip: if controllers lag with Wi-Fi "Always", use "Only while the console is off"
  and report it.
- Home Assistant only sees the BC-250 while Wi-Fi is on (not with the default "only when turned on").
- `bc250.local` doesn't open on some Android phones: use the Pico's address from your router.
- `BC250-setup` is open while the Pico isn't set up yet; someone nearby could set it up first.
- 2.4 GHz Wi-Fi only. Battery level only for DualSense / DualShock.

## v7 — 2026-11-06
**Pairing window: your controller is no longer taken by the Pico when you pair it with something else.**

New
- Controllers in pairing mode are **ignored** unless the pairing window is open.
- **Hold the case button 2 s** and release: the Pico accepts a new controller for 60 s (LED double-blinks); the
  window closes as soon as one is paired. Log-port command `p` does the same.

Changed
- Holding the button with the console on: the LED flickers from 2 s ("release = pairing") and goes dark from
  4.5 s ("keep holding = force off"). Force off is still at 5 s.

Known issues
- Bluetooth LE controllers already bonded to the Pico (e.g. Xbox) are still taken when put in pairing mode.

## v6 — 2026-10-20
**Fix: a controller that disconnected could only come back after re-pairing.**

Fixed
- Cause: Bluepad32 deletes a controller's pairing key after a single failed reconnect. The Pico now keeps a backup
  of every controller's key in flash and puts it back within half a second (**pairing key guard**).

Note
- Controllers paired before v6 are protected after their first successful connection with v6. If one already lost
  its key, re-pair it once.

## v5 — 2026-10-20
**Bluetooth repairs itself while the console is running; log readable on the BC-250.**

New
- **Bluetooth-only restart** (radio chip reset + Bluetooth stack restart) when the radio stops answering, stops
  accepting controllers, or a controller keeps failing to connect. The console keeps running. At most 3 per 30 min,
  then the Pico restarts itself after the next shutdown (as in v4).
- **Event log** (16 KB, survives a Pico restart): with no controller connected, `sudo cat /dev/ttyACM0` on the
  BC-250. Commands: `s` status, `r` restart Bluetooth, `c` clear, `h` help.

## v4 — 2026-10-20
**Fix: controllers stopped waking the console after a day or so in standby.**

New
- Bluetooth health check every 10 s. If Bluetooth is dead, the Pico restarts itself, **only while the PSU is off**
  (it never cuts a running board). Back-off after 3 restarts.
- Safety restart every 6 h in standby with nothing connected.

Changed
- USB power sensing back on (`USE_VBUS_SENSE 1`), for cables with the red 5 V wire still connected.

## v3 — 2026-10-17
Changed
- Option `USE_VBUS_SENSE 0`: USB is enabled whenever the PSU is on, for a cable with the red 5 V wire cut
  (stops standby power leaking into the switched-off BC-250).

## v2 — 2026-10-17
**Fix: after a SteamOS shutdown the button did nothing until the PSU was unplugged; BC-250 LEDs glowed while off.**

Fixed
- The Pico's USB is now switched off while the PSU is off, so it no longer waits for a computer that is gone and no
  longer feeds a little power into the unpowered BC-250.

New
- Hardware watchdog while the PSU is off: if the firmware ever hangs, the Pico restarts within 3 s.

## v1 — 2026-10-16
**First release.**
- Case button: short press = power on / clean SteamOS shutdown (USB power key); hold 5 s = force off.
- PSU switched by the Pico (GP15 → PS_ON); "off" detected from the BC-250's TPM pin 9; boot / shutdown timeouts.
- Bluetooth controllers (DualSense, DualShock 4, Switch Pro, Xbox, 8BitDo, …) wake the console and are passed to the
  BC-250 over USB: up to 4, as themselves (touchpad, gyro, lightbar, adaptive triggers) or as Xbox 360 pads when mixed.
- Case-button LED: off / slow blink booting / on / fast blink shutting down.

---

## PCB

### PCB v9 — 2026-10-19
**Fix: ATX 24-pin header.** Only order this version.
- Rows **5.5 mm apart** (v1–v8: 4.2 mm, the connector didn't fit).
- **Pin 1 at the front** with the latch facing the wall (v1–v8 were mirrored: 12 V on the GND traces).
- Holes for the connector's two snap-in pegs; board 2 mm wider (57.5 × 72 mm); R2 moved clear of the connector.

### PCB v1–v8 — 2026-10-18
Board drawn from perfboard map v16 and refined (narrower board, all cable headers in one row at the front edge).
**Don't use: mirrored ATX header.**

### Perfboard map v16 — 2026-10-18
Hole-by-hole test-board layout. Its ATX header is mirrored like PCB v1–v8: wire the ATX pins by wire colour.
