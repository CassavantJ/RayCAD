#!/usr/bin/env python3
"""Writes holes_labels.txt, the hand labels of the hole recognition test (task T-034, holes_test.cpp).

Run from anywhere: python tests/unit/cam/holes_labels_gen.py   (writes the file next to this script)

How each label was obtained, independently of the recogniser (src/cam/holes.cpp):
  raycad-synthetic/*  From the dimensions that made the part: the formulas below repeat those of
                      tools/corpus_gen/corpus_gen.cpp (the family functions, with their arrays and loops) and the
                      hole tools it cuts: drill() a cylinder, with a 118 degree point on blind holes; counterbore()
                      two cylinders; countersink() a 90 degree cone from the hole diameter up to the countersink
                      diameter at the top. The parts were checked by eye in the corpus (README: "Synthetic parts").
                      A hole is what a machinist would drill from that side: a counterbore or countersink made from
                      the other side is not reachable and is not part of the hole (the bore breaks out into it).
  nist-*              Measured through the kernel (holes_test.cpp's hidden "[.dump]" case: kernel::surfaceInfo of
                      every cylinder and cone face, with its box and the angle it spans), then read by eye: two half
                      cylinders on one axis are one hole wall (the exporter split it), partial cylinders are slot ends,
                      corner radii, a keyhole and cut-outs, and convex cylinders are bosses, standoffs and pins; the
                      depths are the faces' heights.
Kinds: "plain", or a comma list of counterbored / countersunk / chamfered (holes.cpp states the chamfer rule: an entry
cone at most max(0.5 mm, 0.2 x its lower diameter) wide radially is a chamfer).

Format (mm, WCS of the named frame; holes_test.cpp reads it):
  part <corpus id> <frame>      frame: z+ (model axes), z- (seen from below: x, -y, -z), y+ (x, -z, y), y- (x, z, -y),
                                tilt:a / tilt:b (the part turned by a fixed rotation, the frame turned back: the labels
                                are those of z+)
  hole <x> <y> <zTop> <bottom> <kind> <segment>...
       bottom: through | flat | conical
       segment: cyl:<diameter>:<zTop>:<zBottom> | cone:<dTop>:<dBottom>:<zTop>:<zBottom>:<included angle>
A part with no hole lines must give no holes.
"""
import math
import os

INCH = 25.4
TIP = 1.0 / math.tan(math.radians(59.0))  # drill point length per unit radius (118 degrees)

out = []


def part(pid, frame, holes):
    out.append("part raycad-synthetic/%s %s" % (pid, frame) if "/" not in pid else "part %s %s" % (pid, frame))
    for h in holes:
        out.append(h)


def fmt(v):
    s = "%.9f" % v
    s = s.rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def hole(x, y, ztop, bottom, kind, segs):
    return "hole %s %s %s %s %s %s" % (fmt(x), fmt(y), fmt(ztop), bottom, kind, " ".join(segs))


def cyl(d, z0, z1):
    return "cyl:%s:%s:%s" % (fmt(d), fmt(z0), fmt(z1))


def cone(d0, d1, z0, z1, angle):
    return "cone:%s:%s:%s:%s:%s" % (fmt(d0), fmt(d1), fmt(z0), fmt(z1), fmt(angle))


def through(x, y, ztop, d, zbot, kind="plain"):
    return hole(x, y, ztop, "through", kind, [cyl(d, ztop, zbot)])


def blind_point(x, y, ztop, d, depth):
    zb = ztop - depth
    return hole(x, y, ztop, "conical", "plain", [cyl(d, ztop, zb), cone(d, 0, zb, zb - d / 2 * TIP, 118)])


def below(x, y):
    """A point seen from below (frame z-): x, -y."""
    return x, -y


# ---- plate_thru_holes (corpus_gen plateThruHoles) ----
def plate_thru_holes(v):
    L = [120, 150, 80, 6 * INCH, 200, 4 * INCH][v]
    W = [80, 100, 60, 4 * INCH, 60, 3 * INCH][v]
    T = [10, 12, 8, 0.5 * INCH, 6, 0.375 * INCH][v]
    NX = [3, 5, 2, 4, 8, 3][v]
    NY = [2, 3, 2, 3, 2, 2][v]
    D = [6.6, 9, 5.5, 0.266 * INCH, 4.5, 0.201 * INCH][v]
    mx, my = L / (NX + 1), W / (NY + 1)
    pts = [(i * mx, j * my) for i in range(1, NX + 1) for j in range(1, NY + 1)]
    top = [through(x, y, T, D, 0) for x, y in pts]
    bot = [through(*below(x, y), 0, D, -T) for x, y in pts]
    return top, bot


for v in (0, 1, 3, 4):
    top, bot = plate_thru_holes(v)
    name = "plate_thru_holes_%d" % (v + 1)
    part(name + ".stp", "z+", top)
    if v == 1:
        part(name + ".stp", "z-", bot)
        part(name + "_brep.igs", "z+", top)  # the IGES BRep copy of the same part


# ---- plate_counterbore (plateCounterbores) ----
def plate_counterbore(v):
    HD = [6.6, 9.0, 11.0, 0.266 * INCH, 5.5, 0.397 * INCH][v]
    CD = [11.0, 15.0, 18.0, 0.44 * INCH, 10.0, 0.625 * INCH][v]
    CH = [6.5, 8.6, 10.8, 0.26 * INCH, 5.5, 0.385 * INCH][v]
    T = [15, 20, 25, 0.75 * INCH, 12, 0.75 * INCH][v]
    N = [4, 4, 6, 4, 8, 2][v]
    W = CD * 5
    cols = N // 2
    pts = [(15 + CD * 1.25 + i * CD * 2.5, W * (0.3 if j == 0 else 0.7)) for i in range(cols) for j in range(2)]
    top = [hole(x, y, T, "through", "counterbored", [cyl(CD, T, T - CH), cyl(HD, T - CH, 0)]) for x, y in pts]
    bot = [through(*below(x, y), 0, HD, -(T - CH)) for x, y in pts]
    return top, bot


for v in (0, 2, 3):
    top, bot = plate_counterbore(v)
    name = "plate_counterbore_%d.stp" % (v + 1)
    part(name, "z+", top)
    if v == 0:
        part(name, "z-", bot)


# ---- plate_countersink (plateCountersinks) ----
def plate_countersink(v):
    HD = [4.5, 6.6, 9.0, 0.266 * INCH][v]
    CD = [9.0, 13.0, 18.0, 0.52 * INCH][v]
    T = 0.375 * INCH if v == 3 else 4 + 2 * v
    L, W = 100 + 20 * v, 50
    h = (CD - HD) / 2
    pts = [(15 + i * (L - 30) / (2 + v), W / 2) for i in range(3 + v)]
    top = [hole(x, y, T, "through", "countersunk", [cone(CD, HD, T, T - h, 90), cyl(HD, T - h, 0)]) for x, y in pts]
    bot = [through(*below(x, y), 0, HD, -(T - h)) for x, y in pts]
    return top, bot


for v in (0, 3):
    top, bot = plate_countersink(v)
    name = "plate_countersink_%d.stp" % (v + 1)
    part(name, "z+", top)
    if v == 0:
        part(name, "z-", bot)


# ---- block_blind_holes (blockBlindHoles) ----
def block_blind_holes(v):
    D = [5.0, 6.8, 8.5, 0.201 * INCH][v]
    DEPTH = [15, 20, 25, 0.6 * INCH][v]
    H = 40
    holes = [blind_point(15 + i * 50.0 / (1 + v), 20, H, D, DEPTH) for i in range(2 + v)]
    holes.append(blind_point(40, 45, H, D * 1.6, DEPTH * 0.6))
    return holes


for v in (0, 3):
    name = "block_blind_holes_%d.stp" % (v + 1)
    part(name, "z+", block_blind_holes(v))
    if v == 0:
        part(name, "z-", [])  # blind holes open upwards: nothing from below
        part(name, "tilt:a", block_blind_holes(v))

# ---- chamfer_block (chamferBlock): v 2 a through hole with a 1 mm edge break; v 3 a plain through hole ----
L, W, H = 70.0, 45.0, 30.0
part("chamfer_block_3.stp", "z+",
     [hole(L / 2, W / 2, H, "through", "chamfered", [cone(14, 12, H, H - 1, 90), cyl(12, H - 1, 0)])])
part("chamfer_block_3.stp", "z-", [through(*below(L / 2, W / 2), 0, 12, -(H - 1))])
part("chamfer_block_4.stp", "z+", [through(L / 2, W / 2, H, 12, 0)])  # the same block (only chamfer_block_2 is inch)

# ---- thin_wall_box_3 (thinWallBox v 2): a 10 mm hole through the 1 mm floor of an open box ----
part("thin_wall_box_3.stp", "z+", [through((80 + 20) / 2, (50 + 10) / 2, 1.0, 10, 0)])

# ---- rib_plate_1 (ribPlate v 0): blind 3.3 holes with points in two bosses ----
T, RH = 4, 15
ztop = T + RH - 0.01
part("rib_plate_1.stp", "z+", [blind_point(12, 12, ztop, 3.3, RH), blind_point(120 - 12, 80 - 12, ztop, 3.3, RH)])

# ---- flange_bolt_circle_2 (flangeBoltCircle v 1): the bore through the hub, six bolt holes ----
D, T, n = 120.0, 14.0, 6
bc, hd = D * 0.38, D / 12
bolts = [(bc * math.cos(2 * math.pi * i / n), bc * math.sin(2 * math.pi * i / n)) for i in range(n)]
part("flange_bolt_circle_2.stp", "z+", [through(0, 0, 2 * T - 0.01, D / 6, 0)] + [through(x, y, T, hd, 0) for x, y in bolts])
part("flange_bolt_circle_2.stp", "z-",
     [through(0, 0, 0, D / 6, -(2 * T - 0.01))] + [through(*below(x, y), 0, hd, -T) for x, y in bolts])

# ---- l_bracket_4 (lBracket v 3): two holes in the base; the centre slot and the upright's slots are not holes ----
L, W, T = 80.0, 50.0, 8.0
part("l_bracket_4.stp", "z+", [through(L * 0.65, W * 0.3, T, 6.6, 0), through(L * 0.65, W * 0.7, T, 6.6, 0)])

# ---- manifold_block_4 (manifoldBlock v 3): the vertical passage (its point cut by the cross passages), four
# mounting holes, the through passage; the side ports are along X and Y ----
S = 60 + 10 * 3
H, d = S * 0.8, S / 8
mf = [blind_point(S / 2, S / 2, H, d, H * 0.6)]
mf += [blind_point(S * (0.15 if i % 2 == 0 else 0.85), S * (0.15 if i < 2 else 0.85), H, 5.0, 12) for i in range(4)]
mf += [through(S * 0.25, S * 0.75, H, d, 0)]
part("manifold_block_4.stp", "z+", mf)
part("manifold_block_4.stp", "z-", [through(*below(S * 0.25, S * 0.75), 0, d, -H)])

# ---- fixture_plate_2 (fixturePlate v 1, inch): four counterbored holes, four blind tap drills; pocket corners ----
L, W, T = 6 * INCH, 4 * INCH, 0.75 * INCH
cx, cy = [0.08, 0.92, 0.08, 0.92], [0.12, 0.12, 0.88, 0.88]
HD, CD, CH = 0.266 * INCH, 0.44 * INCH, 0.26 * INCH
cb = [(L * cx[i], W * cy[i]) for i in range(4)]
taps = [(L * (0.5 + (-0.04 if i % 2 == 0 else 0.04)), W * (0.1 if i < 2 else 0.9)) for i in range(4)]
fp = [hole(x, y, T, "through", "counterbored", [cyl(CD, T, T - CH), cyl(HD, T - CH, 0)]) for x, y in cb]
fp += [blind_point(x, y, T, 0.201 * INCH, 0.5 * INCH) for x, y in taps]
part("fixture_plate_2.stp", "z+", fp)
part("fixture_plate_2.stp", "z-", [through(*below(x, y), 0, HD, -(T - CH)) for x, y in cb])
part("fixture_plate_2.stp", "tilt:b", fp)

# ---- circular_pocket_1 (circularPocketBoss v 0): the 50 mm pocket (its floor runs into the boss) is not a hole; the
# blind hole in the boss is ----
part("circular_pocket_1.stp", "z+", [blind_point(40, 40, 40 - 4.5, 6, 10)])

# ---- parts without holes along Z: corner radii, a round island, slots, a keyway, a filleted pocket floor ----
for pid in ("closed_pocket_5.stp", "slot_through_3.stp", "keyway_tslot_1.stp", "circular_pocket_2.stp",
            "fillet_block_4.stp", "open_pocket_step_4.stp", "plate_counterbore_3_surf.igs"):
    part(pid, "z+", [])

# ---- NIST D2MI 827-9999-905 (NX, ST-Developer): a bushing. From the top a 3.5687 bore breaks out into the 4.0894
# bore drilled from the bottom; from the bottom the 4.0894 bore, its 118 degree drill point step, then the 3.5687
# bore through the top. ----
zj = -1.748566  # the step: the 3.5687 bore meets the drill point cone (faces 5 and 6)
part("nist-d2mi/827-9999-905.stp", "z+", [through(0, 0, 2.159, 3.5687, zj)])
part("nist-d2mi/827-9999-905.stp", "z-",
     [hole(0, 0, 23.114, "through", "counterbored",
           [cyl(4.0894, 23.114, 1.905), cone(4.0894, 3.5687, 1.905, -zj, 118), cyl(3.5687, -zj, -2.159)])])

# ---- NIST D2MI 827-9999-906: a plate (top z 2.54) with four through holes in standoffs below it, two bosses (to
# z 8.89) with counterbored blind holes (118 degree points), four pins and a corner radius. ----
standoffs = [(4.269994, 4.269994), (13.769994, 65.269994), (88.769994, 65.269994), (98.269994, 4.269994)]
d906 = [through(x, y, 2.54, 3.2512, -4.2672) for x, y in standoffs]
d906.append(hole(60.325, 6.35, 8.89, "conical", "counterbored",
                 [cyl(6.35, 8.89, 5.715), cyl(3.81, 5.715, 2.54), cone(3.81, 0, 2.54, 1.3953604177, 118)]))
d906.append(hole(63.5, 63.1952, 8.89, "conical", "counterbored",
                 [cyl(6.35, 8.89, 6.35), cyl(4.7625, 6.35, 2.54), cone(4.7625, 0, 2.54, 1.10920054713, 118)]))
d906.sort()
part("nist-d2mi/827-9999-906.stp", "z+", d906)
part("nist-d2mi/827-9999-906.stp", "z-", [through(*below(x, y), 4.2672, 3.2512, -2.54) for x, y in standoffs])

# ---- NIST FTC 09 (split cylinders: every hole wall is two half cylinders): a 3.03784 plate in the XZ plane, holes
# along Y. Seen along +Y (frame y+: WCS x, -z, y) and along -Y (y-: x, z, -y). Four holes have a 90 degree entry
# cone 7.4676 to 5.9436 (0.762 wide: a chamfer by the rule) on the +Y side. Not holes: the keyhole (face 71, 316
# degrees), slot ends, cut-out corners and halves, corner radii and the partial cylinders of the engraving. ----
TOP = 3.03784
ftc = []  # (model x, model z, diameter, chamfered)
ftc += [(x, z, 6.35, False) for x, z in [(-19.05, -129.54), (0, -129.54), (19.05, -129.54), (-57.15, -101.6),
                                         (-0.150114, -101.6), (-57.15, 38.39972), (-0.150114, 38.39972),
                                         (-85.09, 19.05), (-85.09, 0), (-85.09, -19.05)]]
ftc += [(x, z, 5.9436, True) for x, z in [(-82.55, -107.95), (82.55, -107.95), (-82.55, 107.95), (82.55, 107.95)]]
ftc += [(-50.8, 91.44, 5.6134, False), (-12.7, 91.44, 5.6134, False), (-31.75, 91.44, 19.05, False)]
ftc += [(x, 91.44, 7.1374, False) for x in (27.94, 46.99, 66.04)]
ftc += [(x, z, 3.96875, False) for x, z in [(50.8, -15.24), (70.5565634663, -7.05656346635), (78.74, 12.7),
                                            (-45.72, 110.49), (-55.88, 110.49), (-17.78, 110.49), (-7.62, 110.49)]]
ftc += [(57.15, 57.15, 8.001, False), (38.1, 57.15, 8.001, False)]
up, down = [], []
for x, z, dd, ch in ftc:
    if ch:
        up.append(hole(x, -z, TOP, "through", "chamfered", [cone(7.4676, dd, TOP, 2.27584, 90), cyl(dd, 2.27584, 0)]))
        down.append(through(x, z, 0, dd, -2.27584))
    else:
        up.append(through(x, -z, TOP, dd, 0))
        down.append(through(x, z, 0, dd, -TOP))
part("nist-pmi/nist_ftc_09_asme1_rd_geometry.stp", "y+", sorted(up))
part("nist-pmi/nist_ftc_09_asme1_rd_geometry.stp", "y-", sorted(down))

header = [
    "# Hand labels of the hole recognition test (task T-034). Written by holes_labels_gen.py, which says how each",
    "# label was obtained (from the corpus generator's dimensions, or measured and checked by eye); do not edit by",
    "# hand. Format: see holes_labels_gen.py.",
]
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "holes_labels.txt")
with open(path, "w", newline="\n") as f:
    f.write("\n".join(header + out) + "\n")
print("wrote %s: %d parts, %d holes" % (path, sum(1 for l in out if l.startswith("part")),
                                        sum(1 for l in out if l.startswith("hole"))))
