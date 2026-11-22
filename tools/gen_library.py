#!/usr/bin/env python3
"""Writes resources/library, the built-in RayCAD library (task T-031): generic tools, holders, materials and three
machine definitions, one JSON file per item in the form ILibrary::save writes (sorted keys, two-space indentation,
canonical numbers, final newline).

Sources. Every file has a "comment" naming the public source of its values:
  - Thread sizes, pitches and tap drill sizes come from resources/standards/threads.json (tools/gen_thread_table.py:
    ISO 261/262 sizes, ISO 68-1 / ASME B1.1 basic profile), and the number and letter drill diameters from the gauge
    lists in tools/gen_thread_table.py (the public "Drill bit sizes" list). These are facts of the standards.
  - Drill lengths: DIN 338 jobber length ranges (metric) and the ASME B94.11M jobber lengths of the fractional
    anchor sizes 1/16 to 1/2 in, interpolated for other inch sizes.
  - Tap, end mill, reamer, spot drill, chamfer mill and thread mill lengths are approximate generic proportions of
    the classes named in each comment (ASME B94.9 / DIN 371 / DIN 376 taps, DIN 6527 end mills, ...). Measure the
    real tool before relying on a length.
  - Cutting data are deliberately conservative generic starting values chosen by this script from the per-material
    factors in MATERIALS below. They are not copied from any table. The Wikipedia article "Speeds and feeds"
    (table "Cutting speeds for various materials using a plain high-speed steel cutter") and Machinery's Handbook
    (Industrial Press), section "Speeds and Feeds", give the published ranges to compare them with.
  - Machines: the manufacturers' public specification pages named in each file; a value a page does not state is
    0 (unknown). Every machine is "verified": false until someone checks it against the real machine.

Ids. Every item has a stable UUID kept in tools/library_ids.json (key -> uuid). A new item gets a fresh random
UUID once, with --new-ids; after that the ids never change, so documents that embed a shipped item keep matching it.

Usage: gen_library.py [--out DIR] [--check] [--new-ids]
  --check    exit 1 if the files on disk differ from what this script writes (or if an id is missing)
  --new-ids  add ids for new items to tools/library_ids.json (otherwise a missing id is an error)
Standard library only; the output is the same on every run.
"""
import argparse
import json
import math
import os
import sys
import uuid
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "resources", "library")
IDS_FILE = os.path.join(ROOT, "tools", "library_ids.json")
THREADS_FILE = os.path.join(ROOT, "resources", "standards", "threads.json")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_thread_table as gtt  # noqa: E402  (the number and letter drill gauges)

INCH = 25.4

CUTTING_SOURCE = (
    "Cutting data: conservative generic starting values computed by tools/gen_library.py from its per-material "
    "factors (not copied from any table); compare with the published ranges in Machinery's Handbook (Industrial "
    "Press), 'Speeds and Feeds', and the Wikipedia article 'Speeds and feeds' (HSS cutting speed table). Start low, "
    "watch the cut, and adjust for the machine, holder and stick-out."
)

# ---------- materials ----------
# mc, the chip thickness exponent of kc = kc1.1 h^-mc: the typical value of the material's row in the material group
# table "according to DIN / ISO 513 and VDI 3323" (kc1 and mc per ISO group and condition) that tool makers publish,
# here as printed in ISCAR's catalogue page "Material Groups". Approximate, for power and torque estimates only; kc1.1
# is this file's own approximate value above, not that table's (which is lower for steels, higher for titanium).
MC_SOURCE = ("mc %s: approximate, the typical value of row %s, in the material group table 'according to DIN / ISO 513 "
             "and VDI 3323' (ISCAR catalogue page 'Material Groups').")

# Per-material factors for the cutting data (all chosen here, conservatively):
#   em      carbide end mill surface speed, m/min (rough)
#   drill   HSS-Co drill surface speed, m/min
#   fzD     carbide end mill chip load per flute as a fraction of the diameter
#   fpr     drill feed per revolution as a fraction of the diameter
#   tap     HSS-E tap surface speed, m/min
#   ream    carbide reamer surface speed, m/min
#   face    carbide insert face mill surface speed, m/min, and fzFace its chip load per insert, mm
#   ramp    ramp angle, degrees; slot depth of a full-width slot and rough radial step, fractions of the diameter;
#   axial   largest depth of cut for side milling, as a fraction of the diameter (capped by the flute length)
MATERIALS = [
    {
        "key": "material.6061-t6", "name": "6061-T6", "group": "Aluminium", "hardnessHb": 95,
        "specificCuttingForce": 700, "density": 2.7, "mc": 0.25, "mcRow": "22, aluminium wrought alloy, hardenable",
        "comment": "6061 aluminium alloy, T6 temper. Density 2.70 g/cm3: Wikipedia '6061 aluminium alloy' (infobox). "
        "Hardness 95 HB: typical value of public 6061-T6 datasheets (ASM International / MatWeb), not re-checked here. "
        "kc1.1 700 N/mm2: approximate value for force estimates, of the order published for wrought aluminium "
        "(ISO 513 group N) in tooling handbooks; not from a specific table.",
        "em": 250, "drill": 60, "fzD": 0.008, "fpr": 0.02, "tap": 15, "ream": 60, "face": 400, "fzFace": 0.1,
        "ramp": 3, "slot": 0.5, "rough": 0.15, "axial": 2.0,
    },
    {
        "key": "material.7075-t6", "name": "7075-T6", "group": "Aluminium", "hardnessHb": 150,
        "specificCuttingForce": 800, "density": 2.81, "mc": 0.25, "mcRow": "22, aluminium wrought alloy, hardenable",
        "comment": "7075 aluminium alloy, T6 temper. Density 2.81 g/cm3: Wikipedia '7075 aluminium alloy' (infobox). "
        "Hardness 150 HB: typical value of public 7075-T6 datasheets (ASM International / MatWeb; Wikipedia gives "
        "87 HRB), not re-checked here. kc1.1 800 N/mm2: approximate, ISO 513 group N order of magnitude.",
        "em": 220, "drill": 50, "fzD": 0.007, "fpr": 0.018, "tap": 12, "ream": 50, "face": 350, "fzFace": 0.1,
        "ramp": 3, "slot": 0.5, "rough": 0.15, "axial": 2.0,
    },
    {
        "key": "material.1018", "name": "1018 (cold drawn)", "group": "Steel", "hardnessHb": 126,
        "specificCuttingForce": 1700, "density": 7.85, "mc": 0.21, "mcRow": "1, non-alloy steel below 0.25 % C",
        "comment": "AISI/SAE 1018 low-carbon steel, cold drawn. Density 7.85 g/cm3: Wikipedia 'Carbon steel' (mild "
        "steel, approximately 7.85 g/cm3). Hardness 126 HB: typical value of public cold-drawn 1018 datasheets "
        "(MatWeb), not re-checked here. kc1.1 1700 N/mm2: approximate, ISO 513 group P (low-carbon steel) order of "
        "magnitude.",
        "em": 110, "drill": 24, "fzD": 0.005, "fpr": 0.015, "tap": 9, "ream": 25, "face": 180, "fzFace": 0.1,
        "ramp": 2, "slot": 0.35, "rough": 0.1, "axial": 1.5,
    },
    {
        "key": "material.4140-annealed", "name": "4140 (annealed)", "group": "Steel", "hardnessHb": 197,
        "specificCuttingForce": 2000, "density": 7.85, "mc": 0.24, "mcRow": "6, low alloy steel, annealed",
        "comment": "AISI 4140 chromium-molybdenum steel, annealed (Wikipedia '41xx steel'). Density 7.85 g/cm3 as for "
        "carbon steel (Wikipedia 'Carbon steel'); hardness 197 HB: typical value of public annealed 4140 datasheets "
        "(MatWeb), not re-checked here. kc1.1 2000 N/mm2: approximate, ISO 513 group P (alloy steel).",
        "em": 90, "drill": 18, "fzD": 0.0045, "fpr": 0.012, "tap": 7, "ream": 20, "face": 150, "fzFace": 0.1,
        "ramp": 2, "slot": 0.3, "rough": 0.1, "axial": 1.5,
    },
    {
        "key": "material.4140ph", "name": "4140PH (pre-hardened, 28-32 HRC)", "group": "Steel", "hardnessHb": 286,
        "specificCuttingForce": 2400, "density": 7.85, "mc": 0.24, "mcRow": "8, low alloy steel, quenched and tempered",
        "comment": "AISI 4140 bought quenched and tempered ('pre-hard'), commonly sold at 28-32 HRC. Hardness 286 HB: "
        "30 HRC converted with the ASTM E140 hardness conversion tables. Density as 4140. kc1.1 2400 N/mm2: "
        "approximate, ISO 513 group P (hardened alloy steel).",
        "em": 70, "drill": 12, "fzD": 0.004, "fpr": 0.01, "tap": 5, "ream": 15, "face": 120, "fzFace": 0.08,
        "ramp": 1.5, "slot": 0.25, "rough": 0.08, "axial": 1.0,
    },
    {
        "key": "material.304-annealed", "name": "304 (annealed)", "group": "Stainless steel", "hardnessHb": 201,
        "specificCuttingForce": 2300, "density": 7.9, "mc": 0.2, "mcRow": "14, stainless steel, austenitic",
        "comment": "AISI 304 austenitic stainless steel, annealed. Density 7.90 g/cm3: Wikipedia 'SAE 304 stainless "
        "steel' (7,900 kg/m3). Hardness 201 HB: the maximum ASTM A240 allows for annealed 304 plate, a public "
        "specification limit (real stock is often softer). kc1.1 2300 N/mm2: approximate, ISO 513 group M.",
        "em": 65, "drill": 10, "fzD": 0.004, "fpr": 0.01, "tap": 4, "ream": 12, "face": 120, "fzFace": 0.08,
        "ramp": 1.5, "slot": 0.25, "rough": 0.08, "axial": 1.0,
    },
    {
        "key": "material.17-4ph-h900", "name": "17-4 PH (H900)", "group": "Stainless steel", "hardnessHb": 409,
        "specificCuttingForce": 2700, "density": 7.8, "mc": 0.21, "mcRow": "13, stainless steel, martensitic",
        "comment": "17-4 PH precipitation-hardening stainless steel (SAE type 630), aged to about 44 HRC: Wikipedia "
        "'17-4 stainless steel' (density 7,800 to 7,900 kg/m3; 'approximately 44 Rc' after heat treatment). "
        "Hardness 409 HB: 44 HRC converted with the ASTM E140 tables. For condition A or H1150 stock the data here "
        "are on the safe side. kc1.1 2700 N/mm2: approximate, ISO 513 group M (precipitation hardened).",
        "em": 45, "drill": 7, "fzD": 0.003, "fpr": 0.008, "tap": 3, "ream": 8, "face": 80, "fzFace": 0.06,
        "ramp": 1, "slot": 0.2, "rough": 0.06, "axial": 1.0,
    },
    {
        "key": "material.ti-6al-4v", "name": "Ti-6Al-4V (annealed)", "group": "Titanium", "hardnessHb": 336,
        "specificCuttingForce": 1500, "density": 4.43, "mc": 0.22, "mcRow": "37, titanium alloys, alpha+beta",
        "comment": "Ti-6Al-4V titanium alloy (grade 5). Density 4.43 g/cm3: Wikipedia 'Ti-6Al-4V' (4.429 to 4.512 "
        "g/cm3). Hardness 336 HB: the typical 36 HRC that page gives, converted with the ASTM E140 tables. kc1.1 "
        "1500 N/mm2: approximate, ISO 513 group S (titanium alloys).",
        "em": 40, "drill": 8, "fzD": 0.003, "fpr": 0.008, "tap": 3, "ream": 8, "face": 50, "fzFace": 0.06,
        "ramp": 1, "slot": 0.2, "rough": 0.06, "axial": 1.0,
    },
]

# ---------- helpers ----------


def mm(inches):
    return round(float(inches) * INCH, 4)


def r(x, digits=4):
    v = round(float(x), digits)
    return int(v) if v == int(v) else v


def frac_text(f):
    f = Fraction(f)
    whole = f.numerator // f.denominator
    rest = f - whole
    if rest == 0:
        return "%d" % whole
    if whole == 0:
        return "%d/%d" % (rest.numerator, rest.denominator)
    return "%d-%d/%d" % (whole, rest.numerator, rest.denominator)


def normalise(value):
    """Integral floats become ints, so the text matches the canonical writer (3, not 3.0)."""
    if isinstance(value, float):
        if math.isfinite(value) and value == int(value) and abs(value) < 1e15:
            return int(value)
        if value != 0 and abs(value) < 1e-4:
            raise SystemExit("value %r would be written in exponent form" % value)
        return value
    if isinstance(value, dict):
        return {k: normalise(v) for k, v in value.items()}
    if isinstance(value, list):
        return [normalise(v) for v in value]
    return value


def render(item):
    return json.dumps(normalise(item), indent=2, sort_keys=True, ensure_ascii=False) + "\n"


# ---------- cutting data ----------


def preset(material_id, op_class, speed, chip, radial, axial, ramp):
    return {
        "material": material_id,
        "opClass": op_class,
        "surfaceSpeed": r(speed, 0),
        "chipLoad": r(chip, 4),
        "maxRadialFraction": r(radial, 3),
        "maxAxialFraction": r(axial, 3),
        "rampAngle": r(ramp, 2),
    }


def presets_for(tool, ids):
    out = []
    d = tool["diameter"]
    flute_ratio = tool["fluteLength"] / d
    for m in MATERIALS:
        mid = ids[m["key"]]
        kind = tool["type"]
        if kind in ("flatEndMill", "ballEndMill", "bullEndMill"):
            fz = m["fzD"] * d
            axial = min(flute_ratio, m["axial"])
            out.append(preset(mid, "rough", m["em"], fz, m["rough"], axial, m["ramp"]))
            out.append(preset(mid, "finish", m["em"] * 1.2, fz * 0.8, 0.05, axial, m["ramp"]))
            out.append(preset(mid, "slot", m["em"] * 0.8, fz * 0.8, 1.0, min(flute_ratio, m["slot"]), m["ramp"]))
        elif kind == "drill":
            # chipLoad is per flute: half the feed per revolution of a two-flute drill. maxAxialFraction is the
            # suggested peck step.
            peck = 3.0 if m["group"] == "Aluminium" else (2.0 if m["group"] == "Steel" else 1.0)
            out.append(preset(mid, "drill", m["drill"], m["fpr"] * d / 2, 1.0, peck, 0))
        elif kind == "spotDrill":
            out.append(preset(mid, "spot", m["em"] * 0.4, m["fpr"] * d / 4, 1.0, 0.5, 0))
        elif kind == "tap":
            # A tap's feed is its pitch per revolution, so chipLoad is 0. maxAxialFraction: deepest suggested thread.
            depth = 2.0 if m["group"] == "Aluminium" else 1.5
            out.append(preset(mid, "tap", m["tap"], 0, 1.0, depth, 0))
        elif kind == "chamferMill":
            out.append(preset(mid, "chamfer", m["em"] * 0.8, m["fzD"] * d * 0.5, 0.5, 0.5, 0))
        elif kind == "faceMill":
            depth = 0.06 if m["group"] == "Aluminium" else (0.04 if m["group"] == "Steel" else 0.03)
            out.append(preset(mid, "face", m["face"], m["fzFace"], 0.75, depth, 0))
        elif kind == "reamer":
            out.append(preset(mid, "ream", m["ream"], m["fpr"] * 1.5 * d / tool["flutes"], 1.0, 3.0, 0))
        elif kind == "threadMill":
            out.append(preset(mid, "thread", m["em"] * 0.5, m["fzD"] * d * 0.3, 0.1, flute_ratio, 0))
        else:
            raise SystemExit("no cutting data rule for " + kind)
    return out


# ---------- tools ----------

def threads():
    with open(THREADS_FILE, "r", encoding="utf-8") as f:
        table = json.load(f)
    out = {}
    for series in table["series"]:
        for size in series["sizes"]:
            out[size["designation"]] = size
    return out


# ASME B94.11M jobber lengths of the fractional anchor sizes: diameter, flute length, overall length (inches).
JOBBER_INCH = [
    (Fraction(1, 16), Fraction(7, 8), Fraction(15, 8)),
    (Fraction(1, 8), Fraction(13, 8), Fraction(11, 4)),
    (Fraction(3, 16), Fraction(37, 16), Fraction(7, 2)),
    (Fraction(1, 4), Fraction(11, 4), Fraction(4)),
    (Fraction(5, 16), Fraction(51, 16), Fraction(9, 2)),
    (Fraction(3, 8), Fraction(29, 8), Fraction(5)),
    (Fraction(7, 16), Fraction(65, 16), Fraction(11, 2)),
    (Fraction(1, 2), Fraction(9, 2), Fraction(6)),
]

# DIN 338 jobber drills: diameter range (mm) -> flute length, overall length.
JOBBER_METRIC = [
    (2.36, 2.65, 30, 57), (2.66, 3.00, 33, 61), (3.01, 3.35, 36, 65), (3.36, 3.75, 39, 70), (3.76, 4.25, 43, 75),
    (4.26, 4.75, 47, 80), (4.76, 5.30, 52, 86), (5.31, 6.00, 57, 93), (6.01, 6.70, 63, 101), (6.71, 7.50, 69, 109),
    (7.51, 8.50, 75, 117), (8.51, 9.50, 81, 125), (9.51, 10.60, 87, 133), (10.61, 11.80, 94, 142),
    (11.81, 13.20, 101, 151),
]


def jobber_inch(d_in):
    d = Fraction(d_in).limit_denominator(100000)
    for (d0, f0, l0), (d1, f1, l1) in zip(JOBBER_INCH, JOBBER_INCH[1:]):
        if d0 <= d <= d1:
            t = (d - d0) / (d1 - d0)
            sixteenth = Fraction(1, 16)
            flute = round((f0 + (f1 - f0) * t) / sixteenth) * sixteenth
            length = round((l0 + (l1 - l0) * t) / sixteenth) * sixteenth
            exact = any(d == a for a, _, _ in JOBBER_INCH)
            return flute, length, exact
    raise SystemExit("no jobber length for %s in" % d_in)


def jobber_metric(d):
    for lo, hi, flute, length in JOBBER_METRIC:
        if lo - 1e-9 <= d <= hi + 1e-9:
            return flute, length
    raise SystemExit("no DIN 338 length for %s mm" % d)


def base_tool(key, name, kind, flutes, d, shank, flute, overall, body, comment, **extra):
    t = {
        "schema": 1, "id": key, "name": name, "vendor": "", "productId": "", "type": kind, "flutes": flutes,
        "diameter": r(d), "shankDiameter": r(shank), "fluteLength": r(flute), "shoulderLength": r(flute),
        "overallLength": r(overall), "cornerRadius": 0, "tipAngle": 0, "tipDiameter": 0, "threadPitch": 0,
        "threadSpec": "", "bodyMaterial": body, "defaultToolNumber": 0, "comment": comment,
    }
    # Lengths to 4 decimals (0.1 um; inch values such as number and letter drills are 25.4 x a 4-decimal inch size,
    # at most 5 decimals in mm), but a thread pitch to 6 (1 nm, what the toolpath stores as threadPitchNm): 25.4 / TPI
    # rounded to 4 decimals made #8-32 0.7937 instead of 0.79375 mm.
    for k, v in extra.items():
        t[k] = r(v, 6 if k == "threadPitch" else 4) if isinstance(v, float) else v
    return t


def end_mills():
    out = []
    em_comment = ("Generic {what}. Lengths: approximate {cls} proportions (measure the real tool). " + CUTTING_SOURCE)
    inch = [
        # key, diameter (in), flutes, length of cut (in), overall (in)
        ("flat.1-8.2fl", Fraction(1, 8), 2, Fraction(1, 2), Fraction(3, 2)),
        ("flat.1-4.3fl", Fraction(1, 4), 3, Fraction(3, 4), Fraction(5, 2)),
        ("flat.3-8.3fl", Fraction(3, 8), 3, Fraction(1), Fraction(5, 2)),
        ("flat.1-2.3fl", Fraction(1, 2), 3, Fraction(1), Fraction(3)),
        ("flat.1-2.4fl", Fraction(1, 2), 4, Fraction(1), Fraction(3)),
        ("flat.3-4.3fl", Fraction(3, 4), 3, Fraction(3, 2), Fraction(4)),
    ]
    for key, d, fl, loc, oal in inch:
        name = "%s in %dFL carbide flat end mill" % (frac_text(d), fl)
        out.append(base_tool("tool.em." + key, name, "flatEndMill", fl, mm(d), mm(d), mm(loc), mm(oal), "carbide",
                             em_comment.format(what="square end mill, %s in diameter" % frac_text(d),
                                               cls="regular-length inch catalogue")))
    metric = [("flat.6mm.3fl", 6, 3, 13, 57), ("flat.10mm.4fl", 10, 4, 22, 72), ("flat.12mm.4fl", 12, 4, 26, 83)]
    for key, d, fl, loc, oal in metric:
        name = "%d mm %dFL carbide flat end mill" % (d, fl)
        out.append(base_tool("tool.em." + key, name, "flatEndMill", fl, d, d, loc, oal, "carbide",
                             em_comment.format(what="square end mill, %d mm diameter" % d,
                                               cls="DIN 6527 short-series")))
    out.append(base_tool("tool.em.ball.1-4.2fl", "1/4 in 2FL carbide ball end mill", "ballEndMill", 2, mm("0.25"),
                         mm("0.25"), mm("0.75"), mm("2.5"), "carbide",
                         em_comment.format(what="ball end mill, 1/4 in diameter", cls="regular-length inch catalogue"),
                         cornerRadius=mm("0.125")))
    out.append(base_tool("tool.em.ball.6mm.2fl", "6 mm 2FL carbide ball end mill", "ballEndMill", 2, 6, 6, 13, 57,
                         "carbide", em_comment.format(what="ball end mill, 6 mm diameter", cls="DIN 6527 short-series"),
                         cornerRadius=3))
    out.append(base_tool("tool.em.bull.1-2.r030.4fl", "1/2 in 4FL carbide corner radius end mill R0.030 in",
                         "bullEndMill", 4, mm("0.5"), mm("0.5"), mm("1"), mm("3"), "carbide",
                         em_comment.format(what="corner radius end mill, 1/2 in diameter, 0.030 in corner radius",
                                           cls="regular-length inch catalogue"),
                         cornerRadius=mm("0.03")))
    return out


def spot_drills():
    comment = ("Generic 90 degree carbide spot drill (spots and chamfers a hole in one). Lengths: approximate "
               "catalogue proportions (measure the real tool). " + CUTTING_SOURCE)
    return [
        base_tool("tool.spot.1-4.90", "1/4 in 90 deg carbide spot drill", "spotDrill", 2, mm("0.25"), mm("0.25"),
                  mm("0.3125"), mm("2"), "carbide", comment, tipAngle=90),
        base_tool("tool.spot.1-2.90", "1/2 in 90 deg carbide spot drill", "spotDrill", 2, mm("0.5"), mm("0.5"),
                  mm("0.625"), mm("3"), "carbide", comment, tipAngle=90),
        base_tool("tool.spot.10mm.90", "10 mm 90 deg carbide spot drill", "spotDrill", 2, 10, 10, 15, 75, "carbide",
                  comment, tipAngle=90),
    ]


# Taps: designation (threads.json), flutes, (overall, thread length, shank) in inches or mm, length standard.
TAPS_INCH = [
    ("#6-32 UNC", 2, ("2", "0.6875", "0.141")),
    ("#8-32 UNC", 2, ("2.125", "0.75", "0.168")),
    ("#10-24 UNC", 3, ("2.375", "0.875", "0.194")),
    ("1/4-20 UNC", 3, ("2.5", "1", "0.255")),
    ("5/16-18 UNC", 3, ("2.71875", "1.125", "0.318")),
    ("3/8-16 UNC", 3, ("2.9375", "1.25", "0.381")),
    ("1/2-13 UNC", 3, ("3.375", "1.65625", "0.367")),
    ("#10-32 UNF", 3, ("2.375", "0.875", "0.194")),
    ("1/4-28 UNF", 3, ("2.5", "1", "0.255")),
    ("3/8-24 UNF", 3, ("2.9375", "1.25", "0.381")),
]
TAPS_METRIC = [
    ("M3x0.5", 2, (56, 9, 3.5), "DIN 371"),
    ("M4x0.7", 3, (63, 13, 4.5), "DIN 371"),
    ("M5x0.8", 3, (70, 15, 6), "DIN 371"),
    ("M6x1", 3, (80, 17, 6), "DIN 371"),
    ("M8x1.25", 3, (90, 20, 8), "DIN 371"),
    ("M10x1.5", 3, (100, 22, 10), "DIN 371"),
    ("M12x1.75", 3, (110, 24, 9), "DIN 376"),
]


def key_of(text):
    return (text.lower().replace("#", "no").replace("/", "-").replace(" ", "").replace(".", "_"))


def taps(table):
    out = []
    for spec, fl, (oal, thread, shank) in TAPS_INCH:
        size = table[spec]
        comment = ("Generic HSS-E spiral point plug tap, %s. Thread designation, pitch (25.4 / %s = %s mm) from "
                   "resources/standards/threads.json (ASME B1.1). Lengths and shank: approximate ASME B94.9 ground "
                   "thread hand tap proportions (measure the real tool). " % (spec, size["tpi"], r(size["pitch"], 6))
                   + CUTTING_SOURCE)
        out.append(base_tool("tool.tap." + key_of(spec), spec + " HSS-E spiral point tap", "tap", fl,
                             size["majorDiameter"], mm(shank), mm(thread), mm(oal), "HSS-E", comment,
                             threadPitch=r(size["pitch"], 6), threadSpec=spec))
    for spec, fl, (oal, thread, shank), std in TAPS_METRIC:
        size = table[spec]
        comment = ("Generic HSS-E spiral point plug tap, %s. Thread designation and pitch from "
                   "resources/standards/threads.json (ISO 261 / ISO 68-1). Lengths and shank: approximate %s "
                   "proportions (measure the real tool). " % (spec, std) + CUTTING_SOURCE)
        out.append(base_tool("tool.tap." + key_of(spec), spec + " HSS-E spiral point tap", "tap", fl,
                             size["majorDiameter"], shank, thread, oal, "HSS-E", comment,
                             threadPitch=size["pitch"], threadSpec=spec))
    return out


def drill_tool(size_name, d_mm, system, purpose):
    drill_comment = "Generic HSS-Co jobber drill, 135 degree split point. "
    if system == "inch":
        d_in = d_mm / INCH
        flute, length, exact = jobber_inch(d_in)
        how = ("ASME B94.11M jobber length" if exact else
               "jobber length interpolated between the ASME B94.11M fractional anchor sizes")
        if size_name.startswith("#") or (len(size_name) == 1 and size_name.isalpha()):
            label = "%s (%s in)" % (size_name, ("%.4f" % d_in).rstrip("0"))
            what = ("Diameter: the %s gauge drill, %s in, from the drill gauge list of tools/gen_thread_table.py "
                    "(Wikipedia 'Drill bit sizes')" % ("number" if size_name.startswith("#") else "letter",
                                                       ("%.4f" % d_in).rstrip("0")))
        else:
            label = size_name.replace(" in", "") + " in"
            what = "Diameter: fractional size %s" % label
        name = label + " HSS-Co jobber drill"
        comment = drill_comment + what + ". Lengths: " + how + ". " + purpose + CUTTING_SOURCE
        key = "tool.drill." + key_of(size_name.replace(" in", ""))
        return base_tool(key, name, "drill", 2, r(d_mm), r(d_mm), mm(flute), mm(length), "HSS-Co", comment,
                         tipAngle=135)
    flute, length = jobber_metric(d_mm)
    label = ("%g" % d_mm) + " mm"
    comment = (drill_comment + "Diameter: metric size %s. Lengths: DIN 338 jobber length for that diameter range. "
               % label + purpose + CUTTING_SOURCE)
    return base_tool("tool.drill." + key_of("%gmm" % d_mm), label + " HSS-Co jobber drill", "drill", 2, d_mm, d_mm,
                     flute, length, "HSS-Co", comment, tipAngle=135)


def drills(table, tap_specs):
    wanted = {}
    for spec in tap_specs:
        tap_drill = table[spec]["tapDrill"]
        system = "metric" if spec.startswith("M") else "inch"
        size = tap_drill["size"]
        wanted.setdefault((system, size), (tap_drill["diameter"], []))[1].append(spec)
    for name, d in (("1/8 in", 3.175), ("1/4 in", 6.35), ("1/2 in", 12.7)):
        wanted.setdefault(("inch", name), (d, []))
    for d in (6.0, 10.0):
        wanted.setdefault(("metric", "%g mm" % d), (d, []))
    out = []
    for (system, size), (d, specs) in sorted(wanted.items(), key=lambda kv: (kv[0][0], kv[1][0])):
        purpose = ("Tap drill for %s (resources/standards/threads.json). " % ", ".join(specs)) if specs else ""
        name = size.replace(" mm", "")
        out.append(drill_tool(size if system == "inch" else name, float(d), system, purpose))
    return out


def chamfer_mills():
    comment = ("Generic 90 degree (45 degree per side) carbide chamfer mill. Lengths: approximate catalogue "
               "proportions (measure the real tool). " + CUTTING_SOURCE)
    return [
        base_tool("tool.chamfer.1-4.90", "1/4 in 90 deg carbide chamfer mill", "chamferMill", 4, mm("0.25"),
                  mm("0.25"), mm("0.125"), mm("2.5"), "carbide", comment, tipAngle=90),
        base_tool("tool.chamfer.1-2.90", "1/2 in 90 deg carbide chamfer mill", "chamferMill", 4, mm("0.5"),
                  mm("0.5"), mm("0.25"), mm("3"), "carbide", comment, tipAngle=90),
    ]


def face_mills():
    comment = ("Generic 2 in, 45 degree lead, 4-insert indexable face mill on a 3/4 in arbour (shankDiameter is the "
               "body diameter, fluteLength the largest depth of cut). Dimensions: approximate generic proportions "
               "(measure the real cutter). " + CUTTING_SOURCE)
    return [base_tool("tool.face.2in.45.4z", "2 in 45 deg 4-insert face mill", "faceMill", 4, mm("2"), 40, 6, 40,
                      "carbide inserts", comment)]


def reamers():
    comment = ("Generic carbide straight-flute machine reamer, {what}. Lengths: approximate catalogue proportions "
               "(measure the real tool). " + CUTTING_SOURCE)
    return [
        base_tool("tool.ream.1-4", "1/4 in carbide reamer", "reamer", 6, mm("0.25"), mm("0.25"), mm("1"), mm("3"),
                  "carbide", comment.format(what="1/4 in nominal")),
        base_tool("tool.ream.6mm.h7", "6 mm H7 carbide reamer", "reamer", 6, 6, 6, 20, 80, "carbide",
                  comment.format(what="6 mm, H7 tolerance (ISO 286)")),
        base_tool("tool.ream.10mm.h7", "10 mm H7 carbide reamer", "reamer", 6, 10, 10, 26, 100, "carbide",
                  comment.format(what="10 mm, H7 tolerance (ISO 286)")),
    ]


def thread_mills(table):
    out = []
    single = ("Generic carbide single-profile 60 degree thread mill: cuts any 60 degree pitch it fits (threadPitch 0 "
              "means any). The 60 degree profile is the ISO 68-1 / ASME B1.1 basic profile. Dimensions: approximate "
              "catalogue proportions (measure the real tool). " + CUTTING_SOURCE)
    out.append(base_tool("tool.threadmill.single60.6mm", "6 mm single-profile 60 deg carbide thread mill",
                         "threadMill", 3, 6, 6, 1.5, 57, "carbide", single, tipAngle=60))
    m6 = table["M6x1"]
    unc = table["1/4-20 UNC"]
    out.append(base_tool(
        "tool.threadmill.p1_0", "Pitch 1.0 mm carbide thread mill (M6x1 and up)", "threadMill", 3, 4.0, 6, 12, 57,
        "carbide",
        "Generic carbide full-form thread mill for ISO metric pitch 1.0 mm; diameter 4.0 mm stays inside the M6x1 "
        "basic minor diameter %s mm (resources/standards/threads.json, ISO 68-1). Dimensions: approximate catalogue "
        "proportions (measure the real tool). " % r(m6["basicMinorDiameter"], 3) + CUTTING_SOURCE,
        threadPitch=1.0))
    out.append(base_tool(
        "tool.threadmill.20tpi", "20 TPI carbide thread mill (1/4-20 UNC and up)", "threadMill", 3, 4.5, 6, 13, 57,
        "carbide",
        "Generic carbide full-form thread mill for 20 threads per inch (pitch 1.27 mm); diameter 4.5 mm stays inside "
        "the 1/4-20 UNC basic minor diameter %s mm (resources/standards/threads.json, ASME B1.1). Dimensions: "
        "approximate catalogue proportions (measure the real tool). " % r(unc["basicMinorDiameter"], 3)
        + CUTTING_SOURCE,
        threadPitch=r(unc["pitch"], 6)))
    return out


# ---------- holders ----------

def holders():
    flange = ("Flange per the {taper} standard ({std}); the body and nose are a conservative generic envelope for "
              "collision checks{nose}. Measure your own holders and correct the segments.")
    cat = {"taper": "CAT40 (V-flange)", "std": "ASME B5.50, flange 2.50 in = 63.5 mm"}
    bt40 = {"taper": "BT40", "std": "JIS B 6339 / ISO 7388-1, flange 63 mm"}
    bt30 = {"taper": "BT30", "std": "JIS B 6339 / ISO 7388-1, flange 46 mm"}

    def holder(key, name, gauge, segments, taper, nose=""):
        return {"schema": 1, "id": key, "name": name, "vendor": "", "gaugeLength": r(gauge),
                "segments": [{"height": r(h), "lowerDiameter": r(lo), "upperDiameter": r(up)}
                             for h, lo, up in segments],
                "comment": flange.format(nose=nose, **taper)}

    er32 = "; nose: ER32 collet nut about 50 mm across (ISO 15488 ER32 collets)"
    er16 = "; nose: ER16 collet nut about 28 mm across (ISO 15488 ER16 collets)"
    er20 = "; nose: ER20 collet nut about 34 mm across (ISO 15488 ER20 collets)"
    return [
        holder("holder.cat40.er32.70", "CAT40 ER32 collet chuck, 70 mm gauge", 70,
               [(15.9, 63.5, 63.5), (20.1, 44.5, 44.5), (34, 50, 50)], cat, er32),
        holder("holder.cat40.er16.70", "CAT40 ER16 collet chuck, 70 mm gauge", 70,
               [(15.9, 63.5, 63.5), (24.1, 44.5, 44.5), (30, 28, 32)], cat, er16),
        holder("holder.cat40.em1-2.63", "CAT40 end mill holder 1/2 in, 63.5 mm gauge", 63.5,
               [(15.9, 63.5, 63.5), (47.6, 35, 44.5)], cat, "; nose about 35 mm across for a 1/2 in bore"),
        holder("holder.cat40.em3-4.63", "CAT40 end mill holder 3/4 in, 63.5 mm gauge", 63.5,
               [(15.9, 63.5, 63.5), (47.6, 44.5, 44.5)], cat, "; nose about 44.5 mm across for a 3/4 in bore"),
        holder("holder.cat40.shell3-4.50", "CAT40 shell mill arbour 3/4 in pilot, 50.8 mm gauge", 50.8,
               [(15.9, 63.5, 63.5), (34.9, 44.5, 44.5)], cat, "; nose about 44.5 mm across, for 3/4 in bore face mills"),
        holder("holder.cat40.drillchuck1-2.100", "CAT40 keyless drill chuck 0-1/2 in, 100 mm gauge", 100,
               [(15.9, 63.5, 63.5), (34.1, 44.5, 44.5), (50, 40, 52)], cat,
               "; chuck body about 52 mm across, tapering to 40 mm at the jaws"),
        holder("holder.bt40.er32.70", "BT40 ER32 collet chuck, 70 mm gauge", 70,
               [(16, 63, 63), (20, 44.5, 44.5), (34, 50, 50)], bt40, er32),
        holder("holder.bt30.er20.60", "BT30 ER20 collet chuck, 60 mm gauge", 60,
               [(12, 46, 46), (20, 31.75, 31.75), (28, 34, 34)], bt30, er20),
    ]


# ---------- machines ----------

def machines():
    def axes(x, y, z, rapid, feed, negative):
        out = []
        for letter, travel in (("X", x), ("Y", y), ("Z", z)):
            lo, hi = (-travel, 0) if negative else (0, travel)
            out.append({"letter": letter, "min": r(lo), "max": r(hi), "rapid": r(rapid), "maxFeed": r(feed),
                        "accel": 0})
        return out

    return [
        {
            "schema": 1, "id": "machine.vmc.haas-ngc", "name": "Generic VMC, Haas NGC style (VF-2 class)",
            "vendor": "", "model": "", "kind": "mill3", "verified": False,
            "axes": axes(762, 406, 508, 25400, 16510, True),
            "spindle": {"maxRpm": 8100, "minRpm": 0, "powerKw": 22.4, "torqueNm": 122},
            "toolChanger": {"capacity": 20, "changeSeconds": 4.2},
            "control": {"dialect": "haas-ngc", "defaultPost": "haas-ngc", "rigidTapping": True, "maxBlockNumber": 0,
                        "maxProgramNameLength": 0},
            "options": {"throughSpindleCoolant": False, "probe": False, "toolSetter": False},
            "postProperties": {"useG187": True},
            "comment": "Published catalogue values of the Haas VF-2 (Haas Automation, VF-2 specifications page, "
            "haascnc.com). X travel 762 mm, 8100 rpm, 22.4 kW, 122 N m, rapids 25.4 m/min, cutting feed 16.5 m/min "
            "(650 in/min), 20 tools, 4.2 s tool change and rigid tapping are the values recorded from that page in "
            "PLAN.md 3.6; Y 406 mm and Z 508 mm are the published VF-2 travels of 16 and 20 in, from memory of that page. "
            "The page refused automated reading when this file was made, so none was re-checked. Axis signs follow "
            "the usual Haas convention (home at 0, travel negative): check. Acceleration and the block and program "
            "name limits are not published: 0 = unknown. Not verified on a real machine.",
        },
        {
            "schema": 1, "id": "machine.vmc.fanuc", "name": "Generic Fanuc-controlled VMC (ROBODRILL class)",
            "vendor": "", "model": "", "kind": "mill3", "verified": False,
            "axes": axes(500, 400, 330, 54000, 30000, False),
            "spindle": {"maxRpm": 24000, "minRpm": 0, "powerKw": 0, "torqueNm": 0},
            "toolChanger": {"capacity": 21, "changeSeconds": 0.9},
            "control": {"dialect": "fanuc", "defaultPost": "fanuc", "rigidTapping": False, "maxBlockNumber": 0,
                        "maxProgramNameLength": 0},
            "options": {"throughSpindleCoolant": False, "probe": False, "toolSetter": False},
            "postProperties": {},
            "comment": "Published catalogue values of the FANUC ROBODRILL alpha-D21MiB5 Plus (FANUC America product "
            "page 'α D21MiB5 Plus', fanucamerica.com/products/robodrill/a-d21mib5-plus): travels 500 x 400 x 330 mm, "
            "24000 min-1 (listed as the high-speed spindle; check which one the machine has), rapids 54 m/min, "
            "programmable cutting feed 30000 mm/min, 21 tools, 0.9 s tool change, BBT30 holders, ROBODRILL "
            "31i-MB5 Plus control. Spindle power and torque, acceleration and rigid tapping are not stated there: "
            "0 / false = unknown. Axis directions are not stated; travels are given from 0. Not verified on a real "
            "machine.",
        },
        {
            "schema": 1, "id": "machine.vmc.mazak-eia", "name": "Generic Mazak VMC, EIA/ISO programs (VC-Ez 20 class)",
            "vendor": "", "model": "", "kind": "mill3", "verified": False,
            "axes": axes(1050, 510, 635, 0, 0, False),
            "spindle": {"maxRpm": 12000, "minRpm": 0, "powerKw": 18, "torqueNm": 0},
            "toolChanger": {"capacity": 30, "changeSeconds": 0},
            "control": {"dialect": "mazak-eia", "defaultPost": "mazak-eia", "rigidTapping": False, "maxBlockNumber": 0,
                        "maxProgramNameLength": 0},
            "options": {"throughSpindleCoolant": False, "probe": False, "toolSetter": False},
            "postProperties": {},
            "comment": "Published catalogue values of the Mazak VC-Ez 20 (Mazak Corporation product page 'VC-Ez 20', "
            "mazak.com/us-en/detail/usa/vertical/us-vcez20zk/): travels 41.34 x 20.08 x 25 in (1050 x 510 x 635 "
            "mm), 12000 rpm, 18 kW (25 hp, 5 min rating), 30 tools, CAT No. 40; the series page ('Vertical Machining "
            "Centers VC-Ez') gives MAZATROL SmoothEz as standard and SmoothG as an option. Whether the control runs "
            "EIA/ISO programs is not stated there: check the control's options. Rapids, cutting feed, torque, tool change time, "
            "acceleration and rigid tapping are not stated there: 0 / false = unknown. Axis directions are not "
            "stated; travels are given from 0. Not verified on a real machine.",
        },
    ]


# ---------- assembly ----------

def all_items():
    table = threads()
    tap_list = taps(table)
    tools = (end_mills() + spot_drills() + drills(table, [t["threadSpec"] for t in tap_list]) + tap_list
             + chamfer_mills() + face_mills() + reamers() + thread_mills(table))
    materials = [{"schema": 1, "id": m["key"], "name": m["name"], "group": m["group"], "hardnessHb": m["hardnessHb"],
                  "specificCuttingForce": m["specificCuttingForce"], "density": m["density"], "mc": m["mc"],
                  "comment": m["comment"] + " " + MC_SOURCE % (m["mc"], m["mcRow"])
                  + " Cutting data for this material are in the tools' presets."}
                 for m in MATERIALS]
    return {"tools": tools, "holders": holders(), "materials": materials, "machines": machines()}


SUFFIX = {"tools": ".tool.json", "holders": ".holder.json", "materials": ".material.json", "machines": ".machine.json"}


def build(ids, allow_new):
    items = all_items()
    missing = []

    def id_of(key):
        if key not in ids:
            if not allow_new:
                missing.append(key)
                return "00000000-0000-4000-8000-000000000000"
            ids[key] = str(uuid.uuid4())
        return ids[key]

    files = {}
    for folder, entries in items.items():
        keys = set()
        for item in entries:
            if item["id"] in keys:
                raise SystemExit("duplicate key " + item["id"])
            keys.add(item["id"])
            item["id"] = id_of(item["id"])
            if folder == "tools":
                item["presets"] = presets_for(item, {m["key"]: id_of(m["key"]) for m in MATERIALS})
            files[os.path.join(folder, item["id"] + SUFFIX[folder])] = render(item)
    files["library.json"] = render({
        "schema": 1, "name": "RayCAD built-in library",
        "comment": "Generated by tools/gen_library.py; do not edit by hand. Generic tools, holders, materials and "
        "unverified machine definitions; every file names the public source of its values.",
    })
    return files, missing


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--new-ids", action="store_true")
    args = parser.parse_args()
    ids = {}
    if os.path.exists(IDS_FILE):
        with open(IDS_FILE, "r", encoding="utf-8") as f:
            ids = json.load(f)
    files, missing = build(ids, args.new_ids and not args.check)
    if missing:
        print("gen_library: no id for %s; run with --new-ids" % ", ".join(sorted(missing)))
        return 1
    if args.check:
        problems = []
        for rel, text in sorted(files.items()):
            path = os.path.join(args.out, rel)
            try:
                with open(path, "r", encoding="utf-8", newline="") as f:
                    if f.read() != text:
                        problems.append(rel + " differs")
            except OSError:
                problems.append(rel + " is missing")
        for folder in SUFFIX:
            expected = {os.path.basename(rel) for rel in files if rel.startswith(folder + os.sep)}
            present = set(os.listdir(os.path.join(args.out, folder))) if os.path.isdir(os.path.join(args.out, folder)) else set()
            problems += ["%s/%s is not generated" % (folder, n) for n in sorted(present - expected)]
        for p in problems:
            print("gen_library: " + p)
        print("gen_library: %d files, %s" % (len(files), "%d problems" % len(problems) if problems else "up to date"))
        return 1 if problems else 0
    for folder in SUFFIX:
        path = os.path.join(args.out, folder)
        if os.path.isdir(path):
            for name in os.listdir(path):
                if name.endswith(SUFFIX[folder]):
                    os.remove(os.path.join(path, name))
    for rel, text in sorted(files.items()):
        path = os.path.join(args.out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    with open(IDS_FILE, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(dict(sorted(ids.items())), indent=2) + "\n")
    counts = {folder: sum(1 for rel in files if rel.startswith(folder + os.sep)) for folder in SUFFIX}
    print("gen_library: wrote %s" % ", ".join("%d %s" % (n, k) for k, n in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
