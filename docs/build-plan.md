# BC-250 power controller – build plan (Pico 2 WH version)

## What it does
- Case button short press: PSU on (board auto-boots) / clean shutdown (USB power key)
- Case button hold 5 s: force PSU off
- Bluetooth controller wakes the board and is passed to the BC-250 over USB
- After SteamOS shutdown, TPM pin 9 drops -> PSU off after 5 s
- Button LED driven by the Pico (on while running, blinks while starting / stopping)
- Second fan powered from PSU 12V, speed controlled by BC-250 J4003

## BC-250 / SteamOS settings
- AUTO_PWRON1 jumper on pins 1–2 (board boots when the PSU turns on)
- SteamOS power key = shutdown (`HandlePowerKey=poweroff` in logind)
- Check the key arrives: `sudo evtest` -> "BC-250 Power Controller" / "Microsoft Controller" -> short-press the button -> `KEY_POWER`

## Space (case compartment)
Floor (inside): ~7.7 cm wall to wall × ~8.3 cm honeycomb wall to open side, ~17 cm tall.
The button goes in the round hole in the honeycomb wall, ~3–4.5 cm above the floor, centred left-right; it sticks ~3 cm in.
Case stays in its normal orientation. The board lies on the floor, button above the back half of the Pico.

## Pico pins (firmware)
| Pico | Pin | Connects to |
|---|---|---|
| GP15 | 20 | R1 1 kΩ -> Q1 base -> PSU PS_ON (high = PSU on) |
| GP17 | 22 | Button C (internal pull-up, pressed = low) |
| GP16 | 21 | Button LED+ (PWM) |
| GP2 | 4 | R3 220 Ω <- BC-250 TPM header pin 9 (internal pull-down, high = running) |
| VSYS | 39 | D1 1N5819 <- ATX 5VSB (pin 9) |
| GND | 3 / 18 / 23 | ATX GND |
| GP0 | 1 | Optional debug log, UART TX 115200 baud (3.3 V adapter) |

Do **not** connect RUN, VREF, 3V3, 3V3_EN, **VBUS (pin 40)**: 5VSB on VBUS back-feeds the BC-250 through the USB cable.

## ATX 24-pin (Molex Mini-Fit Jr 5566-24A2)
Used pins: **9** 5VSB (purple), **10** +12V (yellow), **15** GND (black), **16** PS_ON (green).
With the latch facing the wall, pin 1 is at the FRONT end and the two rows are 5.5 mm apart.
Before first power-up, plug the PSU cable in (PSU off) and check each wire colour sits on the right pad;
with the PSU on mains, pad 9 must read +5 V to GND.

## Cables
- Micro-USB -> USB-A data cable (~50 cm) from the Pico to the BC-250 (controllers + power key).
- Dupont wires: button (3), TPM pin 9 (1), J4003 tach/PWM (2), second fan (4-pin).

## Mounting
4 corner M3 screws, board flat on the plastic floor, washers if it rocks.

## Shopping
- kamami_pl: Pico 2 WH, 70×90 double-sided perfboard, Dupont wires
- elektronika-plus: female headers 1×40, male goldpin 1×40, 1 kΩ ×50, 10 kΩ ×50, 1N5819
- zielony7210: 24-pin Mini-Fit Jr. male PCB header (5566-24A2, with snap-in pegs)
- straight Micro-USB data cable

Full list with quantities: [`../bom.csv`](../bom.csv).
