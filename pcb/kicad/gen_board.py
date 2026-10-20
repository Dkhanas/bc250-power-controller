#!/usr/bin/env python3
"""Generate bc250_power.kicad_pcb from the v16 perfboard map.

Grid: col 1..24 (left -> right, col 24 = ATX side), row 1..30 (back wall -> open front).
All copper traces on B.Cu, following the plan's bottom solder paths.
"""
import uuid, json

BOARD_W, BOARD_H = 57.5, 72.0
X0, Y0, P = 6.5 - 5 * 2.54, -1.0, 2.54   # col 6 (Pico left row) at x = 6.5; cols 1-5 cut off

def g(c, r):
    # "col 24" is the routing lane just outside the ATX wall-side row
    x = XR if c == 24 else X0 + (c - 1) * P
    return (round(x, 3), round(Y0 + (r - 1) * P, 3))

def u():
    return str(uuid.uuid4())

NETS = ["", "GND", "5VSB", "VSYS", "+12V", "PS_ON", "GP15", "Q1_BASE",
        "LED+", "BTN", "GP2", "TPM9", "FAN_PWM", "FAN_TACH"]
NID = {n: i for i, n in enumerate(NETS)}
POWER = {"GND", "5VSB", "VSYS", "+12V"}

# ---------------- ATX 24-pin (Mini-Fit Jr 5566-24, 4.2 mm) -----------------
ATX_X_PICO = X0 + 19.5 * P            # Pico-side row (pins 1-12), col ~20.5
ATX_X_WALL = ATX_X_PICO + 5.5         # wall-side row (pins 13-24): Mini-Fit Jr rows are 5.5 mm apart
XR = 51.8                             # fan-GND lane beside the wall-side row
ATX_CH = round((ATX_X_PICO + ATX_X_WALL) / 2, 3)   # channel between the two pin rows
ATX_PEG_X = round(ATX_X_PICO + 5.04, 3)            # 5566-24A2 snap-in pegs (3.0 mm NPTH)
ATX_Y1 = g(1, 5)[1]                   # back end of the header (pins 12 / 24) at row 5
# Latch faces the wall -> pins 13-24 on the wall side, pin 1 at the FRONT end (KiCad 5566-24A2 top view)
ATX_NET = {9: "5VSB", 10: "+12V", 15: "GND", 16: "PS_ON"}

def atx_pegs():
    return [(ATX_PEG_X, round(ATX_Y1 - 4.7, 3)), (ATX_PEG_X, round(ATX_Y1 + 50.9, 3))]

def atx_pos(pin):
    if pin <= 12:
        return (round(ATX_X_PICO, 3), round(ATX_Y1 + (12 - pin) * 4.2, 3))
    return (round(ATX_X_WALL, 3), round(ATX_Y1 + (24 - pin) * 4.2, 3))

# ---------------- Pico 2 WH on female headers ------------------------------
def pico_pin_pos(pin):
    if pin <= 20:                      # col 13, rows 5->24 = pins 20->1
        return g(13, 25 - pin)
    return g(6, pin - 16)              # col 6, rows 5->24 = pins 21->40

PICO_NET = {20: "GP15", 18: "GND", 4: "GP2", 21: "LED+", 22: "BTN", 23: "GND", 39: "VSYS"}
PICO_NAMES = {1: "GP0", 3: "GND", 4: "GP2", 8: "GND", 13: "GND", 18: "GND", 20: "GP15",
              21: "GP16", 22: "GP17", 23: "GND", 28: "GND", 33: "GND", 38: "GND",
              39: "VSYS", 40: "VBUS NC"}

# ---------------- generic parts: (ref, value, [(padnum, (c,r), net, drill, size)]) --
P1 = (1.0, 1.7)    # header pin
PR = (0.8, 1.6)    # resistor / transistor lead
PD = (1.1, 2.0)    # 1N5819 lead
PARTS = [
    ("J1", "BUTTON", [("1", (14, 27), "GND", *P1), ("2", (13, 27), "BTN", *P1), ("3", (12, 27), "LED+", *P1)],
     ["GND", "C", "LED+"]),
    ("Q1", "2N2222", [("E", (17, 3), "GND", *PR), ("B", (18, 3), "Q1_BASE", *PR), ("C", (19, 3), "PS_ON", *PR)],
     ["E", "B", "C"]),
    ("R1", "1k", [("1", (14, 5), "GP15", *PR), ("2", (17, 5), "Q1_BASE", *PR)], None),
    ("R2", "10k", [("1", (18, 5), "Q1_BASE", *PR), ("2", (18, 8), "GND", *PR)], None),
    ("R3", "220R", [("1", (15, 21), "GP2", *PR), ("2", (15, 24), "TPM9", *PR)], None),
    ("J2", "TPM9", [("1", (15, 27), "TPM9", *P1)], None),
    ("D1", "1N5819", [("A", (18, 25), "5VSB", *PD), ("K", (14, 25), "VSYS", *PD)], None),
    ("J4", "J4003", [("1", (16, 27), "FAN_TACH", *P1), ("2", (17, 27), "FAN_PWM", *P1)], ["T", "P"]),
    ("J3", "FAN2", [("1", (18, 27), "FAN_PWM", *P1), ("2", (19, 27), "FAN_TACH", *P1),
                    ("3", (20, 27), "+12V", *P1), ("4", (21, 27), "GND", *P1)], ["P", "T", "12", "G"]),
]

# ---------------- traces (grid paths, or ("atx", pin) start points) --------
def path(net, *pts):
    return (net, [atx_pos(p[1]) if p[0] == "atx" else g(*p) for p in pts])

TRACES = [
    path("GND", (6, 7), (9, 7), (13, 7)),             # joins both Pico GND pins
    path("GND", (18, 8), (18, 7), (13, 7)),
    path("GND", (16, 7), (16, 3), (17, 3)),
    path("GP15", (13, 5), (14, 5)),
    path("Q1_BASE", (17, 5), (18, 5)),
    path("Q1_BASE", (18, 5), (18, 3)),
    ("PS_ON", [atx_pos(16), (ATX_CH, atx_pos(16)[1] - 2.1), (ATX_CH, g(19, 3)[1]), g(19, 3)]),
    ("5VSB", [atx_pos(9), (g(18, 1)[0], atx_pos(9)[1]), g(18, 25)]),
    path("VSYS", (14, 25), (7, 25), (7, 23), (6, 23)),
    ("GND", [atx_pos(15), (XR, atx_pos(15)[1] + 2.14), (XR, g(21, 27)[1]), g(21, 27)]),
    path("GND", (21, 27), (21, 29), (14, 29), (14, 27)),     # fan GND -> J1 GND (ties board GND to ATX pin 15)
    path("FAN_PWM", (18, 27), (17, 27)),
    path("FAN_TACH", (19, 27), (19, 28), (16, 28), (16, 27)),
    path("GP2", (13, 21), (15, 21)),
]
TOP_TRACES = [
    # +12V on top: ATX pin 10 -> J3 pin 3, crossing over the 5VSB trace
    ("+12V", 1.0, [atx_pos(10), (40.8, atx_pos(10)[1] + 2.54), (40.8, 63.0), g(20, 27)]),
    # Button J1 at the front edge -> Pico pins 21/22/23, on top, under the Pico
    ("LED+", 0.4, [g(12, 27), g(12, 5), g(6, 5)]),
    ("BTN", 0.4, [g(13, 27), g(13, 28), g(11, 28), g(11, 6), g(6, 6)]),
    ("GND", 0.6, [g(14, 27), g(14, 29), g(10, 29), g(10, 7), g(6, 7)]),
]
TRACES.append(path("TPM9", (15, 24), (15, 27)))
MOUNT = [(3.5, 3.5), (BOARD_W - 3.5, 3.5), (3.5, BOARD_H - 3.5), (BOARD_W - 3.5, BOARD_H - 3.5)]

# ======================= writer ============================================
def fnt(sz=1.0, th=0.15, just=None):
    j = f" (justify {just})" if just else ""
    return f"(effects (font (size {sz} {sz}) (thickness {th})){j})"

def netref(name):
    return f'(net {NID[name]} "{name}")' if name else ""

def pad(num, shape, rel, size, drill, net, layers='"*.Cu" "*.Mask"', kind="thru_hole"):
    return (f'    (pad "{num}" {kind} {shape} (at {rel[0]:.3f} {rel[1]:.3f}) (size {size} {size}) '
            f'(drill {drill}) (layers {layers}) {netref(net)} (tstamp {u()}))\n')

def footprint(ref, value, at, pads, silk=(), ref_at=(0, -2), hide_val=True, extra=""):
    s = (f'  (footprint "{ref}_{value}" (layer "F.Cu") (tstamp {u()}) (at {at[0]:.3f} {at[1]:.3f})\n'
         f'    (property "Reference" "{ref}") (property "Value" "{value}")\n'
         f'    (fp_text reference "{ref}" (at {ref_at[0]} {ref_at[1]}) (layer "F.SilkS") (tstamp {u()}) {fnt(0.9, 0.15)})\n'
         f'    (fp_text value "{value}" (at 0 0) (layer "F.Fab"){" hide" if hide_val else ""} (tstamp {u()}) {fnt(0.8, 0.12)})\n')
    for (a, b) in silk:
        s += (f'    (fp_line (start {a[0]:.3f} {a[1]:.3f}) (end {b[0]:.3f} {b[1]:.3f}) '
              f'(stroke (width 0.15) (type solid)) (layer "F.SilkS") (tstamp {u()}))\n')
    s += extra
    s += "".join(pads) + "  )\n"
    return s

def rect_lines(x1, y1, x2, y2):
    return [((x1, y1), (x2, y1)), ((x2, y1), (x2, y2)), ((x2, y2), (x1, y2)), ((x1, y2), (x1, y1))]

def build():
    out = ['(kicad_pcb (version 20221018) (generator pcbnew)\n',
           '  (general (thickness 1.6))\n  (paper "A4")\n',
           '  (title_block (title "BC-250 power controller") (rev "9") (comment 1 "From perfboard map v16"))\n',
           '''  (layers
    (0 "F.Cu" signal) (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive") (33 "F.Adhes" user "F.Adhesive")
    (34 "B.Paste" user) (35 "F.Paste" user)
    (36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen")
    (38 "B.Mask" user) (39 "F.Mask" user)
    (40 "Dwgs.User" user "User.Drawings") (41 "Cmts.User" user "User.Comments")
    (44 "Edge.Cuts" user) (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard") (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user) (49 "F.Fab" user)
  )
  (setup (pad_to_mask_clearance 0)
    (pcbplotparams (layerselection 0x00010fc_ffffffff) (plot_on_all_layers_selection 0x0000000_00000000)
      (useauxorigin false) (usegerberextensions false) (usegerberattributes true)
      (usegerberadvancedattributes true) (creategerberjobfile true) (excludeedgelayer true)
      (plotframeref false) (viasonmask false) (mode 1) (useauxorigin false)
      (hpglpennumber 1) (hpglpenspeed 20) (hpglpendiameter 15.000000)
      (dxfpolygonmode true) (dxfimperialunits true) (dxfusepcbnewfont true)
      (psnegative false) (psa4output false) (plotreference true) (plotvalue false)
      (plotinvisibletext false) (sketchpadsonfab false) (subtractmaskfromsilk true)
      (outputformat 1) (mirror false) (drillshape 0) (scaleselection 1) (outputdirectory "gerbers/"))
  )
''']
    for i, n in enumerate(NETS):
        out.append(f'  (net {i} "{n}")\n')

    # Pico headers
    cx, cy = (g(6, 5)[0] + g(13, 5)[0]) / 2, (g(6, 5)[1] + g(6, 24)[1]) / 2
    pads = []
    for pin in range(1, 41):
        x, y = pico_pin_pos(pin)
        shape = "rect" if pin == 1 else "circle"
        pads.append(pad(pin, shape, (x - cx, y - cy), 1.7, 1.0, PICO_NET.get(pin, "")))
    hw, hh = 10.5, 25.3
    silk = rect_lines(-hw, -hh, hw, hh)
    extra = ""
    for pin, name in PICO_NAMES.items():
        x, y = pico_pin_pos(pin)
        lx = (x - cx) + (-2.0 if pin > 20 else 2.0)
        just = "left" if pin > 20 else "right"
        lx = (x - cx) + (2.0 if pin > 20 else -2.0)
        extra += (f'    (fp_text user "{name}" (at {lx:.3f} {y - cy:.3f}) (layer "F.SilkS") (tstamp {u()}) '
                  f'{fnt(0.8, 0.13, just)})\n')
    extra += (f'    (fp_text user "USB" (at 0 {hh - 1.5:.2f}) (layer "F.SilkS") (tstamp {u()}) {fnt(1, 0.15)})\n'
              f'    (fp_text user "Pico 2 WH" (at 0 -2) (layer "F.SilkS") (tstamp {u()}) {fnt(1.2, 0.18)})\n')
    out.append(footprint("U1", "Pico2WH", (cx, cy), pads, silk, ref_at=(0, -hh + 1.5), extra=extra))

    # ATX header
    acx = (ATX_X_PICO + ATX_X_WALL) / 2
    acy = ATX_Y1 + 5.5 * 4.2
    pads = []
    for pin in range(1, 25):
        x, y = atx_pos(pin)
        shape = "rect" if pin == 1 else "oval"
        net = ATX_NET.get(pin, "")
        pads.append(f'    (pad "{pin}" thru_hole {shape} (at {x - acx:.3f} {y - acy:.3f}) (size 3.3 2.7) '
                    f'(drill 1.4) (layers "*.Cu" "*.Mask") {netref(net)} (tstamp {u()}))\n')
    for px, py in atx_pegs():
        pads.append(pad("", "circle", (px - acx, py - acy), 3.0, 3.0, "", kind="np_thru_hole"))
    bl, br, bt = ATX_X_PICO - 2.6 - acx, ATX_X_PICO + 7.65 - acx, 25.7
    silk = rect_lines(bl, -bt, br, bt)
    silk += rect_lines(br, -1.7, ATX_X_PICO + 8.75 - acx, 1.7)        # latch (wall side)
    bw, bh = br, 29.0
    extra = (f'    (fp_text user "LATCH" (at {ATX_X_PICO + 9.7 - acx:.2f} 0 90) (layer "F.SilkS") (tstamp {u()}) {fnt(0.8, 0.12)})\n'
             f'    (fp_text user "1" (at {atx_pos(1)[0] - acx:.2f} {bt + 1.0:.2f}) (layer "F.SilkS") (tstamp {u()}) {fnt(0.9, 0.15)})\n')
    for pin, name in ATX_NET.items():
        x, y = atx_pos(pin)
        extra += (f'    (fp_text user "{pin}" (at {x - acx:.2f} {y - acy + 2.0:.2f}) (layer "F.Fab") (tstamp {u()}) {fnt(0.8, 0.12)})\n')
    out.append(footprint("J5", "ATX24", (acx, acy), pads, silk, ref_at=(-1.0, bh + 1.6), extra=extra))

    # Small parts
    for ref, val, pl, labels in PARTS:
        pts = [g(*p[1]) for p in pl]
        fx = sum(p[0] for p in pts) / len(pts)
        fy = sum(p[1] for p in pts) / len(pts)
        pads = [pad(num, "rect" if i == 0 and ref.startswith("J") else "circle",
                    (x - fx, y - fy), size, drill, net)
                for i, ((num, _, net, drill, size), (x, y)) in enumerate(zip(pl, pts))]
        xs, ys = [p[0] - fx for p in pts], [p[1] - fy for p in pts]
        m = 1.3
        silk, extra = [], ""
        vertical = max(ys) - min(ys) > max(xs) - min(xs)
        if ref[0] == "R" or ref == "D1":
            if vertical:
                silk = rect_lines(-0.9, min(ys) + 1.4, 0.9, max(ys) - 1.4)
            else:
                silk = rect_lines(min(xs) + 1.6, -1.0, max(xs) - 1.6, 1.0)
            if ref == "D1":   # cathode band at the K pad end (left)
                kx = min(xs) + 2.2
                silk.append(((kx, -1.0), (kx, 1.0)))
                silk.append(((kx + 0.3, -1.0), (kx + 0.3, 1.0)))
        elif len(pts) > 1 and ref != "J4":
            silk = rect_lines(min(xs) - m, min(ys) - m, max(xs) + m, max(ys) + m)
        if labels:
            for (x, y), lab in zip(zip(xs, ys), labels):
                off = (0, -2.1) if ref == "J1" else ((0, 1.9) if not vertical else (-1.7, 0))
                if not lab:
                    continue
                extra += (f'    (fp_text user "{lab}" (at {x + off[0]:.2f} {y + off[1]:.2f}) (layer "F.SilkS") '
                          f'(tstamp {u()}) {fnt(0.8, 0.13)})\n')
        if vertical:
            ra = (-2.4 if ref not in ("Q1",) else 1.6, min(ys) - 1.4) if ref != "R2" else (1.9, 0)
        else:
            ra = (0, -2.0)
        if ref == "R2":
            ra = (0, -5.4)
        if ref == "R3":
            ra = (2.0, 0)
        if ref == "J2":
            ra = (0, 2.1)
        if ref == "D1":
            ra = (0, -2.0)
        if ref == "J1":
            ra = (0, 2.1)
        if ref == "Q1":
            ra = (-5.6, 0)
        extra += f'    (fp_text user "{val}" (at {ra[0]:.2f} {ra[1] + (1.0 if ref[0] in "RD" or ref=="Q1" else -1.0):.2f}) (layer "F.Fab") (tstamp {u()}) {fnt(0.8, 0.12)})\n'
        out.append(footprint(ref, val, (fx, fy), pads, silk, ref_at=ra, extra=extra))

    # Mounting holes (NPTH, M3)
    for i, (x, y) in enumerate(MOUNT, 1):
        out.append(footprint(f"H{i}", "M3", (x, y),
                             [pad("", "circle", (0, 0), 3.2, 3.2, "", kind="np_thru_hole")],
                             ref_at=(0, 0)).replace('(fp_text reference', '(fp_text reference', 1)
                   .replace(f'"H{i}" (at 0 0) (layer "F.SilkS")', f'"H{i}" (at 0 0) (layer "F.Fab")'))

    # Traces
    for net, pts in TRACES:
        w = 1.0 if net in POWER else 0.4
        for a, b in zip(pts, pts[1:]):
            out.append(f'  (segment (start {a[0]:.3f} {a[1]:.3f}) (end {b[0]:.3f} {b[1]:.3f}) (width {w}) '
                       f'(layer "B.Cu") (net {NID[net]}) (tstamp {u()}))\n')

    for net, w, pts in TOP_TRACES:
        for a, b in zip(pts, pts[1:]):
            out.append(f'  (segment (start {a[0]:.3f} {a[1]:.3f}) (end {b[0]:.3f} {b[1]:.3f}) (width {w}) '
                       f'(layer "F.Cu") (net {NID[net]}) (tstamp {u()}))\n')

    # Outline
    out.append(f'  (gr_rect (start 0 0) (end {BOARD_W} {BOARD_H}) (stroke (width 0.1) (type solid)) '
               f'(fill none) (layer "Edge.Cuts") (tstamp {u()}))\n')
    # Board text
    texts = [
        ("BACK / HONEYCOMB WALL", (BOARD_W / 2 - 6, 1.6), 0.8, "F.SilkS"),
        ("FRONT / OPEN SIDE", (40.0, BOARD_H - 1.2), 0.8, "F.SilkS"),
        ("BC-250 PWR v9", (g(9.5, 15)[0], 40), 1.2, "B.SilkS"),
    ]
    for t, (x, y), sz, layer in texts:
        mir = " (justify mirror)" if layer.startswith("B") else ""
        out.append(f'  (gr_text "{t}" (at {x:.2f} {y:.2f}) (layer "{layer}") (tstamp {u()}) '
                   f'(effects (font (size {sz} {sz}) (thickness 0.15)){mir}))\n')
    out.append(")\n")
    return "".join(out)

if __name__ == "__main__":
    import sys
    open(sys.argv[1] if len(sys.argv) > 1 else "bc250_power.kicad_pcb", "w").write(build())
    # also dump geometry for the checker
    geo = {"pads": [], "traces": [], "board": [BOARD_W, BOARD_H], "mount": MOUNT + atx_pegs()}
    for pin in range(1, 41):
        geo["pads"].append(["U1." + str(pin), *pico_pin_pos(pin), 1.7, 1.0, PICO_NET.get(pin, "")])
    for pin in range(1, 25):
        geo["pads"].append(["J5." + str(pin), *atx_pos(pin), 3.3, 1.4, ATX_NET.get(pin, "")])
    for ref, val, pl, _ in PARTS:
        for num, cr, net, drill, size in pl:
            geo["pads"].append([f"{ref}.{num}", *g(*cr), size, drill, net])
    for net, pts in TRACES:
        geo["traces"].append([net, 1.0 if net in POWER else 0.4, pts, "B"])
    for net, w, pts in TOP_TRACES:
        geo["traces"].append([net, w, pts, "F"])
    json.dump(geo, open("geometry.json", "w"))
