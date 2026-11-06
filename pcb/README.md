# Printed board – PCB v11

| | |
|---|---|
| **Top** (component side, rendered from the board file) | **Bottom** (seen from below) |
| ![top](images/top.png) | ![bottom](images/bottom.png) |

**Wiring diagram** (colour-coded nets, dashed = top layer):

![PCB v11 wiring](images/wiring.png)

## Files
| File | What |
|---|---|
| [`gerbers/bc250_power_gerbers_v11.zip`](gerbers/bc250_power_gerbers_v11.zip) | v11: ATX pin 1 at the BACK (see the check below), spaced-out front headers |
| [`gerbers/bc250_power_gerbers_v9.zip`](gerbers/bc250_power_gerbers_v9.zip) | v9: ATX pin 1 at the FRONT (matches KiCad's official footprint) |
| [`kicad/`](kicad/) | KiCad 7+ project for v11 (`bc250_power.kicad_pro` / `.kicad_pcb`), `gen_board.py` (generates the board), `check_board.py` (clearance / connectivity check), `render_board.py` (makes the images). v9's and v10's files are in the git history. |
| [`images/`](images/) | The pictures on this page (v11) |

> ⚠️ **v9 and v11 differ in the ATX pin order, and only one of them matches your connector.**
> v11 (like v10) puts pin 1 at the back (honeycomb wall) end; v9 puts it at the front. KiCad's official
> Molex 5566-24A2 footprint gives the v9 order, so v11 is only right if your real cable says so.
> The wrong one puts 12 V on GND. **Check before ordering:** hold a bare header the way it sits on the board
> (latch facing the wall) and plug the PSU cable in. **Purple in the Pico-side row, 4th from the FRONT → order v11.
> Purple 4th from the BACK → order v9.**

Never use Gerbers from v1–v8: their ATX rows are 4.2 mm apart (the real header is 5.5 mm) and they have no peg holes.

- Board 57.5 × 72 mm, 2 layers, 1.6 mm. 4 × M3 holes (3.2 mm) at 3.5 mm from each corner.
- Grid: hole (col 6, row 1) at x 6.5, y −1.0 mm; 2.54 mm pitch. Derived from perfboard map v16.

## Ordering
After the purple-wire check, upload `gerbers/bc250_power_gerbers_v11.zip` (or `_v9.zip`) to JLCPCB / PCBWay / Aisler:
2 layers, 1.6 mm, defaults for everything else.

## v11 (6 Nov 2026) – gaps between the front headers
- One empty hole between each front connector, so the plugs don't touch. Front row (row 27), left to right:
  J1 button (cols 12–14: LED+, C, GND), gap, J2 TPM9 (col 16), gap, J4 → J4003 (cols 18 T, 19 P), gap,
  J3 fan 2 (cols 21–24: PWM, TACH, 12V, GND).
- J3 pin 4 (GND) is fed straight down the lane beside the ATX wall row; the bottom GND link along row 29 is gone
  (board GND reaches ATX pin 3 through R2 and row 7). +12V now runs along row 26 to J3 pin 3.
- Bottom-right screw hole moved 0.5 mm right and 1 mm down (centre 3.0 mm from the right edge, 2.5 mm from the front edge)
  so an M3 screw head clears J3.
- ATX pin order and everything else as v10. Checks: KiCad DRC 0 electrical errors, 0 unconnected pads; clearance script 0 errors.

## v10 (6 Nov 2026) – ATX pin 1 at the back
Same board as v9 (57.5 × 72 mm, 5.5 mm row spacing, 3.0 mm peg holes, latch towards the wall), with the ATX pin
order running back → front: pins 1 / 13 at the back, 12 / 24 at the front. This is the mirror of KiCad's official
5566-24A2 top view (v9).
- Used ATX pins: 3 GND (Pico-side row, 3rd from the back), 9 5VSB and 10 +12V (Pico-side row, 4th and 3rd from
  the front), 16 PS_ON (wall-side row, 4th from the back), 24 GND (wall-side row, front end).
- Routing: 3 GND → R2 (col 18, row 8). 5VSB: pin 9 → col 18 → D1 anode. +12V: pin 10 → col 19 → J3 pin 3.
  PS_ON: pin 16 → channel between the two pin rows → Q1's collector. 24 GND → lane beside the wall row (x 51.8) → J3 pin 4.
  Top-layer GND link between pins 24 and 3 in the channel; bottom GND link J3 pin 4 → row 29 → J1 GND.
- Checks: KiCad DRC 0 electrical errors, 0 unconnected pads; independent clearance/connectivity script 0 errors.

## Before first power-up
Plug the PSU cable into the soldered header (PSU off) and check each wire sits on its pad.
v10: purple on 9 and yellow on 10 (Pico-side row, 4th and 3rd from the front), green on 16 (wall-side row,
4th from the back), black on 3 and 24. v9: purple on 9, yellow on 10, green on 16, black on 15.
With the PSU on mains but off, pad 9 must read +5 V to GND.

## v9 (19 Oct 2026) – ATX header fixed
Checked against KiCad's official Molex_Mini-Fit_Jr_5566-24A2 footprint: all 24 pins and both pegs match exactly.
- Rows of the Mini-Fit Jr header are **5.5 mm apart** (v1–v8 wrongly used 4.2 mm). Pads 3.3 × 2.7 mm oval, 1.4 mm drill.
- Added the two **3.0 mm holes for the snap-in pegs**, on the wall-side row line, 4.7 mm beyond each end pin.
- **Pin order flipped:** with the latch facing the wall, pins 13–24 are on the wall side and **pin 1 is at the FRONT end, pin 12 / 24 at the back**.
- Used ATX pins: 9 5VSB, 10 +12V (Pico-side row, back half), 16 PS_ON and 15 GND (wall-side row, front half).
- Routing: PS_ON runs from pin 16 up the channel between the two pin rows to Q1's collector. 5VSB goes from pin 9 along the bottom to col 18 and down to D1. +12V runs on the top layer from pin 10 along x 40.8 mm to J3 pin 3. GND: pin 15 → lane beside the wall row (x 51.8) → J3 pin 4 → bottom row 29 → J1 GND → top trace → Pico GND (pins 23/18) → row 7 → R2 and Q1 E.
- R2 10k moved to col 18 (row 5 base → row 8 GND) so it doesn't touch the connector body.
- Board widened 2 mm (55.5 → 57.5) so the GND lane and the right-hand screws clear the connector.
- Placement: latch tip is 5.4 mm inside the board's right edge.

## Layout (unchanged since v5, both v9 and v10)
- Q1 2N2222 flat on row 3 (E col 17, B col 18, C col 19). R1 1k row 5 (col 14 → 17). R3, D1 (row 25, K col 14, A col 18), VSYS row 25 → col 7 → Pico pin 39.
- Front header row (row 27), v9/v10: J1 button (cols 12–14: LED+, C, GND), J2 TPM9 (col 15), J4 → BC-250 J4003 (cols 16 T, 17 P), J3 fan 2 (cols 18–21: PWM, TACH, 12V, GND). v11: see above.
- Pico on cols 6/13 rows 5–24; VBUS (pin 40) unconnected.
- Trace widths: 1.0 mm for GND / 5VSB / VSYS / 12V, 0.4–0.6 mm for signals.

## Checks
KiCad DRC: 0 electrical errors, 0 unconnected pads. Independent clearance/connectivity script: 0 errors.
Footprint compared pin-by-pin with the official KiCad footprint.

## History
- v2 (18 Oct): cut the free left strip; PS_ON / fan-GND lane moved closer to the header.
- v3: J1 moved from the back edge to the front, away from the button body.
- v4: all cable headers in one row at the front edge.
- v5: J2 (TPM) joins the front header row.
- v6–v8: layout tweaks (superseded – mirrored ATX header).
- v9 (19 Oct): ATX header corrected (5.5 mm rows, peg holes, pin 1 at the front).
- v10 (6 Nov): ATX pin 1 at the back; otherwise the same as v9.
- v11 (6 Nov): gaps between the front headers; bottom-right screw hole moved.
