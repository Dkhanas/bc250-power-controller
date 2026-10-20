#!/usr/bin/env python3
"""Independent copper check: clearances between nets, net connectivity, edge clearance."""
import json, itertools
from shapely.geometry import Point, LineString, box
from shapely.ops import unary_union

geo = json.load(open("geometry.json"))
MIN_CLR, MIN_EDGE = 0.25, 0.5
items = []   # (label, net, shape, is_pad, layer)
for name, x, y, size, drill, net in geo["pads"]:
    items.append((name, net, Point(x, y).buffer(size / 2, 32), True, "*"))
for i, (net, w, pts, lay) in enumerate(geo["traces"]):
    for j, (a, b) in enumerate(zip(pts, pts[1:])):
        items.append((f"trace{i}.{j}({net})", net, LineString([a, b]).buffer(w / 2, 16), False, lay))
for k, (x, y) in enumerate(geo["mount"]):
    items.append((f"H{k+1}", "#HOLE", Point(x, y).buffer(1.6, 32), True, "*"))

errors = 0
for (n1, net1, s1, _, l1), (n2, net2, s2, _, l2) in itertools.combinations(items, 2):
    if net1 == net2 and net1 not in ("",):
        continue
    if net1 == "" and net2 == "":
        continue
    if "*" not in (l1, l2) and l1 != l2:
        continue
    d = s1.distance(s2)
    if d < MIN_CLR:
        errors += 1
        print(f"CLEARANCE {d:.3f} mm: {n1} [{net1}] <-> {n2} [{net2}]")

W, H = geo["board"]
inner = box(MIN_EDGE, MIN_EDGE, W - MIN_EDGE, H - MIN_EDGE)
for n, net, s, _, _l in items:
    if net != "#HOLE" and not inner.contains(s):
        errors += 1
        print(f"EDGE: {n}")

nets = sorted({it[1] for it in items if it[1] not in ("", "#HOLE")})
for net in nets:
    shapes = [s for _, nn, s, _, _l in items if nn == net]
    u = unary_union(shapes)
    parts = len(getattr(u, "geoms", [u]))
    npads = sum(1 for it in items if it[1] == net and it[3])
    status = "OK" if parts == 1 else "SPLIT"
    if parts != 1:
        errors += 1
    print(f"net {net:9s} pads={npads:2d} islands={parts} {status}")
print("ERRORS:", errors)
