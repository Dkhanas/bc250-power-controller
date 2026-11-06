# Printed board – PCB v12

| | |
|---|---|
| **Top** (component side, rendered from the board file) | **Bottom** (seen from below) |
| ![top](images/top.png) | ![bottom](images/bottom.png) |

**Wiring diagram** (colour-coded nets, dashed = top layer):

![PCB v12 wiring](images/wiring.png)

## Files
| File | What |
|---|---|
| [`gerbers/bc250_power_gerbers_v12.zip`](gerbers/bc250_power_gerbers_v12.zip) | Upload this to the board maker |
| [`kicad/`](kicad/) | KiCad 7+ project (`bc250_power.kicad_pro` / `.kicad_pcb`), `gen_board.py` (generates the board), `check_board.py` (clearance / connectivity check), `render_board.py` (makes the images) |
| [`images/`](images/) | The pictures on this page |

**Never use PCB v1–v8, v10 or v11.** v1–v8: ATX rows 4.2 mm apart (the real header is 5.5 mm), no peg holes.
v10 / v11: ATX pin order mirrored (pin 1 at the back) – 12 V on GND.

- Board 57.5 × 72 mm, 2 layers, 1.6 mm. 4 × M3 holes (3.2 mm); three at 3.5 mm from the corners, the front-right one
  3.0 mm from the right edge and 2.5 mm from the front edge (clears J3).
- Grid: hole (col 6, row 1) at x 6.5, y −1.0 mm; 2.54 mm pitch. Derived from perfboard map v16.

## Ordering
Upload `gerbers/bc250_power_gerbers_v12.zip` to JLCPCB / PCBWay / Aisler: 2 layers, 1.6 mm, defaults for everything else.

## ATX header (Molex Mini-Fit Jr 5566-24A2)
Latch faces the wall, pins 13–24 on the wall side, **pin 1 at the FRONT end, pins 12 / 24 at the back**.
Rows 5.5 mm apart, 3.3 × 2.7 mm oval pads, 1.4 mm drills, two 3.0 mm holes for the snap-in pegs.
Matches KiCad's official footprint pin-for-pin and was **confirmed with the real PSU cable on a built v8 board** (whose own ATX footprint was wrong;
everything else on it worked): 5 V standby is on the
4th pin from the back in the Pico-side row.

| ATX pin | Position (counted from the back / honeycomb wall end) | Goes to |
|---|---|---|
| 9 5VSB (purple) | Pico-side row, 4th | col 18 → D1 anode → Pico VSYS |
| 10 +12V (yellow) | Pico-side row, 3rd | top layer, x 40.8 mm → row 26 → J3 pin 3 |
| 15 GND (black) | wall-side row, 10th (3rd from the front) | lane beside the wall row → J3 pin 4 → row 29 → J1 GND → Pico GND |
| 16 PS_ON (green) | wall-side row, 9th (4th from the front) | channel between the rows → Q1 collector |

All-black modular cables: check with a multimeter (black probe on the PSU case): only the 5VSB pin reads ~5 V while the PSU is off.

## v12 (6 Nov 2026)
- v9's ATX pin order (confirmed with the real PSU cable) with v11's spaced-out front headers.
- Front row (row 27), left to right: J1 button (cols 12–14: LED+, C, GND), gap, J2 TPM9 (col 16), gap,
  J4 → J4003 (cols 18 T, 19 P), gap, J3 fan 2 (cols 21–24: PWM, TACH, 12V, GND).
- Checks: KiCad DRC 0 electrical errors, 0 unconnected pads; independent clearance/connectivity script 0 errors;
  ATX pads compared with the official footprint (0.0 mm error).

## Layout
- Q1 2N2222 flat on row 3 (E col 15, B col 16, C col 17). R1 1k row 5 (col 14 → 17). R2 10k col 16 (row 6 base → row 9 GND).
  v12 moved Q1 and R2 left: ≥ 5.5 mm between them and the ATX connector body (was 0.4 mm).
- R3 220 Ω col 15 (rows 21–24) → J2. D1 1N5819 row 25 (K col 14, A col 18), VSYS row 25 → col 7 → Pico pin 39.
- Pico on cols 6/13 rows 5–24; VBUS (pin 40) unconnected.
- Trace widths: 1.0 mm for GND / 5VSB / VSYS / 12V, 0.4–0.6 mm for signals.

## Before first power-up
Plug the PSU cable in (PSU off): pad 9 must read +5 V to GND, every other ATX pad 0 V.
Then Pico VSYS (2nd pin from the USB end, left row) ≈ 4.7 V and 3V3 (5th pin) = 3.3 V.

## History
- v2–v5 (18 Oct): narrower board, J1 to the front, headers in one row, J2 joins the row.
- v6–v8: layout tweaks. v8 was built: everything worked except the ATX header (4.2 mm rows, mirrored).
- v9 (19 Oct): ATX header corrected (5.5 mm rows, peg holes, pin 1 at the front). Pin order later confirmed with the real PSU cable.
- v10 / v11 (6 Nov): pin 1 at the back + gaps between front headers – **wrong pin order, do not use**.
- v12 (6 Nov): v9 pin order + v11 gaps.
