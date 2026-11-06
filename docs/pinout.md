# Pinouts – what plugs where

All drawings are **top views** with the pin labels as they sit on the board. `^` marks pin 1 (or the ▲ on the silkscreen).
`*` marks a pin this build uses. Unmarked pins stay empty.

## Cables at a glance
| From (BC-250 / PSU / case) | Wire | To (controller board) |
|---|---|---|
| BC-250 **TPMS1 pin 9** (3.3 V while running) | 1 Dupont, 2.0 mm pitch on the BC-250 side | **J2** |
| BC-250 **J4003 F2T** (fan 2 tach) | Dupont | **J4 T** |
| BC-250 **J4003 F2P** (fan 2 PWM) | Dupont | **J4 P** |
| Case button: LED+, C, GND (NO + LED− joined at the button) | 3 Dupont | **J1** |
| Second fan, 4-pin | fan plug | **J3** |
| PSU 24-pin ATX cable | ATX plug | **ATX header** |
| Pico Micro-USB | USB data cable | BC-250 USB-A port |

No GND wire is needed to the BC-250: it shares ground with the board through the PSU.

---

## BC-250 board

### J4003 – fan signals (2.54 mm, 2 × 8)
```
[ GND F1T F2T* F3T F4T F5T DET     ]
[ GND F1P F2P* F3P F4P F5P GND GND ]
   ^
```
- **F2T → J4 T**, **F2P → J4 P**: the 3rd pair from the `^` end (pair 1 GND/GND, pair 2 = CPU fan, pair 3 = yours).
- The plug goes **across** the two rows (one pin in each), not along one row.
- On the board the header stands upright: ▲ at the bottom, the empty position at the top.
- No 12 V here; the fan gets 12 V from J3. In Linux the second fan shows up as `fan3` / `pwm3`.

### TPMS1 – "is the board running?" (2.0 mm, 2 × 9, one pin missing)
```
PCICLK  [  1    2 ] GND
FRAME   [  3    4 ] SMB_CLK
PCIRST# [  5    6 ] SMB_DATA
LAD3    [  7    8 ] LAD2
3V      [  9*  10 ] LAD1        <- pin 9 → J2: 3.3 V running, 0 V after shutdown
LAD0    [ 11   12 ] GND
        [  ✕   14 ] S_PWRDWN#   <- position 13 has no pin: your landmark
3VSB    [ 15   16 ] SERIRQ#     <- 15 is ALWAYS 3.3 V: don't use it
GND     [ 17   18 ] GND
```
Finding pin 9: find the gap, stay in its column, go toward the long end: 1st pin = 11, **2nd pin = 9**.
Check with a multimeter first: ~3.3 V running, 0 V after a SteamOS shutdown.

### AUTO_PWRON1 – jumper
```
[ 1  2  3 ]
  ^
  └──┘  jumper on pins 1–2: the board boots as soon as the PSU switches on
```

---

## PSU – ATX 24-pin header on the controller board (Mini-Fit Jr 5566-24A2)

Top view, latch towards the wall, **PCB v9 / v12** (pin 1 at the FRONT end; measured on a built v8 board):
```
            BACK (honeycomb wall)
       Pico-side row     wall-side row (latch)
      [ 12  3.3V     ]  [ 24  GND      ]
      [ 11  12V      ]  [ 23  5V       ]
      [ 10  12V*     ]  [ 22  5V       ]  yellow on 10
      [  9  5VSB*    ]  [ 21  5V       ]  purple on 9
      [  8  PWR_OK   ]  [ 20  NC       ]
      [  7  GND      ]  [ 19  GND      ]
      [  6  5V       ]  [ 18  GND      ]
      [  5  GND      ]  [ 17  GND      ]
      [  4  5V       ]  [ 16  PS_ON*   ]  green
      [  3  GND      ]  [ 15  GND*     ]  black
      [  2  3.3V     ]  [ 14  -12V     ]
      [  1  3.3V     ]  [ 13  3.3V     ]
            FRONT (open side)
```
`*` = used by the board. 5 V standby is the **4th pin from the back in the Pico-side row**; with the PSU on mains
(and off) only that pin reads +5 V. PCB v10 / v11 had this picture upside down (12 V on GND): never use them.

---

## Controller board (PCB v12) – front header row
Top view, front edge (open side) towards you, left to right (one empty hole between connectors):
```
   J1 button        J2      J4 → J4003      J3 second fan
[ LED+  C  GND ] _ [ TPM ] _ [ T  P ] _ [ PWM TACH 12V GND ]
                                                         ^ fan plug's GND wire (black) on this end
```
| Header | Pin | Goes to |
|---|---|---|
| J1 | LED+ | button LED+ (Pico GP16, PWM) |
| J1 | C | button C/COM (Pico GP17, pressed = GND) |
| J1 | GND | button NO **and** LED− (join them at the button) |
| J2 | TPM | BC-250 TPMS1 pin 9 (→ R3 220 Ω → Pico GP2) |
| J4 | T | BC-250 J4003 **F2T** (wired to J3 TACH) |
| J4 | P | BC-250 J4003 **F2P** (wired to J3 PWM) |
| J3 | PWM · TACH · 12V · GND | 4-pin fan wires: blue · green · yellow · black (usual colours) |

A 4-pin fan plug reads GND · 12V · TACH · PWM from its keyed side, so on J3 the **black wire goes to the right
end** (GND) and the blue PWM wire to the left end.

PCB v9: same order without the gaps (`[LED+ C GND][TPM][T P][PWM TACH 12V GND]`).

## Test board (perfboard map v16)
Top view, row 1 = back, col 1 = left:
```
J1  row 2,  cols 2–4    [ GND  C  LED+ ]        (col 2 GND, col 3 C, col 4 LED+)
J4  row 9,  cols 1–2    [ T  P ]                (col 1 tach → F2T, col 2 PWM → F2P)
J2  row 25, col 15      [ TPM ]
J3  row 29, cols 17–20  [ PWM TACH 12V GND ]
```
Note J1 is the other way round from the PCB. The perfboard's ATX holes are mirrored: wire the ATX by colour.

---

## Pico 2 W – pins used
Top view, USB at the top (on the board the USB end faces the front):
```
              ┌──USB──┐
   GP0*   1 ──┤       ├── 40  VBUS   ✕ leave empty (back-feeds the BC-250)
   GP1    2 ──┤       ├── 39  VSYS*  ← D1 ← ATX 5VSB
   GND*   3 ──┤       ├── 38  GND
   GP2*   4 ──┤       ├── 37  3V3_EN ✕
    ...       │       │   36  3V3    ✕
   GND*  18 ──┤       ├── 23  GND*
   GP14  19 ──┤       ├── 22  GP17*  ← button C
   GP15* 20 ──┤       ├── 21  GP16*  → button LED+
              └───────┘
```
| Pin | Name | Job |
|---|---|---|
| 1 | GP0 | optional debug UART TX (115200) |
| 4 | GP2 | TPM pin 9 via R3 220 Ω (high = running) |
| 20 | GP15 | R1 1 kΩ → Q1 → PS_ON (high = PSU on) |
| 21 | GP16 | button LED+ (PWM) |
| 22 | GP17 | button C (pull-up, pressed = low) |
| 39 | VSYS | power in from 5VSB through D1 1N5819 |
| 3 / 18 / 23 | GND | board ground |

Don't connect RUN, VREF, 3V3, 3V3_EN or **VBUS (40)**.
