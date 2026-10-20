# BC-250 power controller

A small board that gives an AMD BC-250 (running SteamOS) a real power button and Bluetooth controller wake-up.
A Raspberry Pi Pico 2 W sits on the ATX power supply's 5 V standby rail and:

- **Case button** short press: PSU on (the BC-250 auto-boots) / clean SteamOS shutdown (USB power key)
- **Case button held 5 s:** force the PSU off
- **Bluetooth controller** (DualSense, DS4, Switch Pro, Xbox, 8BitDo…): pressing PS / Xbox / Home wakes the board,
  and the controller is passed to the BC-250 over USB (as itself, or as an Xbox 360 pad)
- **TPM pin 9** of the BC-250 goes low after shutdown -> PSU off after 5 s
- **Button LED:** slow blink booting, solid running, fast blink shutting down
- **Second fan** powered from the PSU's 12 V, speed controlled by the BC-250 (J4003)

| Printed board (PCB v9) | Test board (perfboard) |
|---|---|
| ![PCB v9](pcb/images/top.png) | ![Perfboard](perfboard/images/top-view.png) |

## Repository

| Folder | What |
|---|---|
| [`firmware/`](firmware/) | Pico 2 W firmware (C, Pico SDK 2.3.1 + Bluepad32). Ready-to-flash file in [`firmware/release/`](firmware/release/) |
| [`pcb/`](pcb/) | Printed board, v9: KiCad project, Gerbers for JLCPCB / PCBWay / Aisler, notes |
| [`perfboard/`](perfboard/) | Test board on a 70×90 mm perfboard: hole-by-hole map |
| [`docs/`](docs/) | Build plan: wiring to the BC-250, Pico pins, SteamOS settings, mounting |
| [`bom.csv`](bom.csv) | Parts list for both boards (opens in Excel; GitHub shows it as a table) |

## Quick start

1. **Flash:** hold BOOTSEL on the Pico, plug it into a PC, copy `firmware/release/bc250_power_v5.uf2` onto the `RP2350` drive.
2. **BC-250:** AUTO_PWRON jumper on pins 1–2; SteamOS `HandlePowerKey=poweroff`.
3. **Wire** per [`docs/build-plan.md`](docs/build-plan.md) (Pico GP15 -> PSU, GP17 <- button, GP16 -> LED, GP2 <- TPM pin 9).
4. **Pair a controller:** put it in pairing mode. Pairing turns the BC-250 on (expected).

## Downsides and known limitations

**Testing status**
- **Firmware v5 compiles cleanly but hasn't been tested on real hardware yet.** v4 ran on the real board.
  The Bluetooth repair is a workaround: the cause of the Bluetooth hang (after a controller disconnects, it
  sometimes can't reconnect) is still unknown. v5's log (`sudo cat /dev/ttyACM0`) is there to find it.
- **Bluetooth repair takes a moment:** when Bluetooth is stuck, the Pico notices within about 30 s and restarts
  only Bluetooth, which takes a few more seconds. Until then, PS doesn't reconnect.
- **The log port only exists while no controller is connected**, so controller identities stay exact.
- **The PCB v9 was checked with KiCad DRC and against the official connector footprint, not on a built board.**
  Check the first board with a multimeter before connecting the PSU (see [`pcb/README.md`](pcb/README.md)).
- **The perfboard map (v16) has the ATX header mirrored.** Wire it by wire colour, not by the hole numbers.

**Controllers**
- **Whenever a controller joins or leaves, every controller drops for about 0.5 s.** The Pico re-plugs itself on USB
  with the new layout, and Steam may reassign player numbers.
- **Several real DualSense / DS4 / Switch Pro controllers inside one USB device is unusual.** Linux supports it,
  but Steam may show only one of them. Mixed groups fall back to Xbox 360 pads, which lose the touchpad, gyro,
  lightbar and adaptive triggers.
- **No audio:** the DualSense / DS4 headphone jack, speaker and mic aren't passed through.
- **Up to 4 controllers. Only Bluetooth ones work**: no wired controllers through the Pico, and no non-Bluetooth Xbox One pads.
- **Pairing a controller turns the BC-250 on.** For 20 s after a shutdown, controllers that reconnect are ignored,
  so a quick press of PS right after power-off does nothing.

**Power and shutdown**
- **The shutdown key is a USB "Power" key.** SteamOS has to be set to `HandlePowerKey=poweroff`. Gaming mode or an
  inhibitor can swallow it; then press again, or hold 5 s.
- **Holding the button for 5 s cuts power hard,** with no clean shutdown, like pulling the plug.
- **A short press while the board is booting is ignored.**
- **"Off" is detected from TPM header pin 9.** If the board doesn't boot within 180 s, the PSU is switched off again.
- **When a controller identity is active, the power key needs a ~2 s USB re-plug first.** The controllers vanish from
  SteamOS at that moment, which doesn't matter because it's shutting down.

**Hardware**
- **The Pico is always powered from the PSU's 5 V standby rail,** with its Bluetooth radio on, so the PC draws a little more
  power while "off".
- **With a normal USB cable, a little standby power can leak into the switched-off BC-250** through the Pico's
  internal diode (its back-panel LEDs may glow faintly). The clean fix is a cable with the red 5 V wire cut, plus
  `USE_VBUS_SENSE 0` in `firmware/src/config.h`.
- **Specific to the BC-250 and an ATX PSU** (needs AUTO_PWRON, the TPM header and J4003). Other boards need new pin mapping and logic.
- **No Wi-Fi features** (no web page or remote power-on over the network).

## ⚠️ Safety notes

- **ATX header orientation:** with the latch facing the wall, **pin 1 is at the FRONT end**, and the two pin rows are **5.5 mm apart**.
  PCB versions v1–v8 and perfboard map v16 had this mirrored (12 V on GND). Only use **PCB v9 or later**.
- **Pico VBUS (pin 40) stays unconnected** on the board. The USB cable's 5 V (red wire) can leak standby power into a
  switched-off BC-250; firmware v4 and later handle a normal cable (`USE_VBUS_SENSE 1`).

## Versions

| | Version | Date |
|---|---|---|
| Firmware | v5 – Bluetooth repair while running (restarts only Bluetooth, board stays on) + event log on the BC-250 over USB serial | 2026-10-20 |
| PCB | v9 – corrected ATX header (5.5 mm rows, pin 1 at the front, peg holes) | 2026-10-19 |
| Perfboard map | v16 (ATX header superseded by PCB v9) | 2026-10-18 |

Full change history: [`firmware/README.md`](firmware/README.md), [`pcb/README.md`](pcb/README.md).
