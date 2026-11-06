"""Render top/bottom PNGs of the board from KiCad layer SVGs (component-side look)."""
import subprocess, sys, os, io
import cairosvg, pcbnew
from PIL import Image
import numpy as np

PCB = sys.argv[1]; OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)
PX_PER_MM = 1134 / 57.5
b = pcbnew.LoadBoard(PCB)
bb = b.GetBoardEdgesBoundingBox()
x0, y0 = pcbnew.ToMM(bb.GetX()), pcbnew.ToMM(bb.GetY())
W, H = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
w, h = int(round(W * PX_PER_MM)), int(round(H * PX_PER_MM))

def layer_mask(layer, mirror=False):
    svg = os.path.join(OUT, f"_{layer}.svg")
    cmd = ["kicad-cli", "pcb", "export", "svg", "-l", layer + ",Edge.Cuts", "--exclude-drawing-sheet",
           "--page-size-mode", "2", "--black-and-white", "--drill-shape-opt", "0", "-o", svg, PCB]
    if mirror: cmd.insert(4, "-m")
    subprocess.run(cmd, check=True, capture_output=True)
    png = cairosvg.svg2png(url=svg, output_width=w, output_height=h, background_color="white")
    os.remove(svg)
    return np.array(Image.open(io.BytesIO(png)).convert("L")) < 128

def render(side):
    m = side == "bottom"
    cu, mask, silk = (layer_mask(f"{'B' if m else 'F'}.{n}", m) for n in ("Cu", "Mask", "SilkS"))
    edge = layer_mask("Edge.Cuts", m)
    img = np.zeros((h, w, 3), np.uint8); img[:] = (30, 90, 55)
    img[cu] = (63, 160, 96)
    img[cu & mask] = (201, 164, 58)
    img[silk] = (245, 245, 245)
    img[edge] = (20, 60, 35)
    im = Image.fromarray(img)
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    for fp in b.GetFootprints():
        for p in fp.Pads():
            ds = p.GetDrillSize()
            if ds.x <= 0: continue
            x, y = pcbnew.ToMM(p.GetPosition().x) - x0, pcbnew.ToMM(p.GetPosition().y) - y0
            if m: x = W - x
            r = pcbnew.ToMM(ds.x) / 2 * PX_PER_MM
            cx, cy = x * PX_PER_MM, y * PX_PER_MM
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(10, 10, 10))
    im.save(os.path.join(OUT, f"{side}.png"))

render("top"); render("bottom")
