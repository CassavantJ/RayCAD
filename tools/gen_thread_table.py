#!/usr/bin/env python3
"""Writes resources/standards/threads.json, the thread table of the hole feature (task T-097).

Every value in the table is either a size and pitch pair of a published thread series or computed here from a
formula; nothing is copied from a handbook, a paid standard or a vendor table. The inputs below are:

  - ISO metric: the nominal diameters with their coarse and fine pitches of the ISO 261 / ISO 262 preferred sizes,
    as listed in the public summary https://en.wikipedia.org/wiki/ISO_metric_screw_thread.
  - Unified (ASME B1.1): the UNC and UNF diameter and threads-per-inch pairs, as listed in
    https://en.wikipedia.org/wiki/Unified_Thread_Standard. Number sizes have the major diameter
    0.060 in + 0.013 in x N (same page).
  - Drill gauges: the diameters of the number (#1 to #80) and letter (A to Z) gauge drills, as listed in
    https://en.wikipedia.org/wiki/Drill_bit_sizes.

Computed:
  - basic minor diameter  D1 = D - 2 x 5/8 H = D - 1.082532 P, with H = sqrt(3)/2 P (ISO 68-1 / ASME B1.1 basic
    profile);
  - tap drill, metric     d = D - P (the rule the ISO metric screw thread and Tap and die pages state, about 75 %
    thread), rounded to the nearest point of a 0.05 mm grid below 3 mm, 0.1 mm from 3 mm to below 14 mm, 0.25 mm
    from 14 mm to 25 mm and 0.5 mm above 25 mm; a value half way between two grid points takes the larger;
  - tap drill, unified    d = D - 0.974279 P = D - 0.75 x 2 x 3/4 H: the "75 % thread" of the percentage-of-thread
    rule, which counts 3/4 H = 0.649519 P per side as full depth, then the nearest of the number and letter gauge
    drills and the fractional drills in 1/64 in steps. A tie would take the larger drill (none occurs; checked).

Usage: gen_thread_table.py [--out resources/standards/threads.json] [--check]
  --check  exit 1 if the file on disk differs from what this script writes (for CI and reviews).
Standard library only; the output is the same on every run.
"""
import argparse
import json
import math
import os
import sys
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "resources", "standards", "threads.json")

INCH = 25.4
SQRT3_2 = math.sqrt(3.0) / 2.0
MINOR_FACTOR = 2.0 * 5.0 / 8.0 * SQRT3_2  # 1.082532
UNIFIED_TAP_FACTOR = 0.75 * 2.0 * 0.75 * SQRT3_2  # 0.974279

# ISO 262 preferred sizes: nominal diameter (mm), coarse pitch, fine pitches.
METRIC = [
    ("1", 0.25, [0.2]),
    ("1.2", 0.25, [0.2]),
    ("1.4", 0.3, [0.2]),
    ("1.6", 0.35, [0.2]),
    ("1.8", 0.35, [0.2]),
    ("2", 0.4, [0.25]),
    ("2.5", 0.45, [0.35]),
    ("3", 0.5, [0.35]),
    ("3.5", 0.6, [0.35]),
    ("4", 0.7, [0.5]),
    ("5", 0.8, [0.5]),
    ("6", 1.0, [0.75]),
    ("7", 1.0, [0.75]),
    ("8", 1.25, [1.0, 0.75]),
    ("10", 1.5, [1.25, 1.0]),
    ("12", 1.75, [1.5, 1.25]),
    ("14", 2.0, [1.5]),
    ("16", 2.0, [1.5]),
    ("18", 2.5, [2.0, 1.5]),
    ("20", 2.5, [2.0, 1.5]),
    ("22", 2.5, [2.0, 1.5]),
    ("24", 3.0, [2.0]),
    ("27", 3.0, [2.0]),
    ("30", 3.5, [2.0]),
    ("33", 3.5, [2.0]),
    ("36", 4.0, [3.0]),
    ("39", 4.0, [3.0]),
    ("42", 4.5, [3.0]),
    ("45", 4.5, [3.0]),
    ("48", 5.0, [3.0]),
    ("52", 5.0, [4.0]),
    ("56", 5.5, [4.0]),
    ("60", 5.5, [4.0]),
    ("64", 6.0, [4.0]),
]

# Unified sizes: nominal, UNC threads per inch (None: none), UNF threads per inch (None: none). Numbers are "#N";
# fractional sizes are written "1-1/8" for 1 1/8 in.
UNIFIED = [
    ("#0", None, 80),
    ("#1", 64, 72),
    ("#2", 56, 64),
    ("#3", 48, 56),
    ("#4", 40, 48),
    ("#5", 40, 44),
    ("#6", 32, 40),
    ("#8", 32, 36),
    ("#10", 24, 32),
    ("#12", 24, 28),
    ("1/4", 20, 28),
    ("5/16", 18, 24),
    ("3/8", 16, 24),
    ("7/16", 14, 20),
    ("1/2", 13, 20),
    ("9/16", 12, 18),
    ("5/8", 11, 18),
    ("3/4", 10, 16),
    ("7/8", 9, 14),
    ("1", 8, 12),
    ("1-1/8", 7, 12),
    ("1-1/4", 7, 12),
    ("1-3/8", 6, 12),
    ("1-1/2", 6, 12),
    ("1-3/4", 5, None),
    ("2", Fraction(9, 2), None),
]

# Number gauge drills #1 to #80 and letter gauge drills A to Z, diameters in inches.
NUMBER_DRILLS = [
    0.228, 0.221, 0.213, 0.209, 0.2055, 0.204, 0.201, 0.199, 0.196, 0.1935,
    0.191, 0.189, 0.185, 0.182, 0.180, 0.177, 0.173, 0.1695, 0.166, 0.161,
    0.159, 0.157, 0.154, 0.152, 0.1495, 0.147, 0.144, 0.1405, 0.136, 0.1285,
    0.120, 0.116, 0.113, 0.111, 0.110, 0.1065, 0.104, 0.1015, 0.0995, 0.098,
    0.096, 0.0935, 0.089, 0.086, 0.082, 0.081, 0.0785, 0.076, 0.073, 0.070,
    0.067, 0.0635, 0.0595, 0.055, 0.052, 0.0465, 0.043, 0.042, 0.041, 0.040,
    0.039, 0.038, 0.037, 0.036, 0.035, 0.033, 0.032, 0.031, 0.0292, 0.028,
    0.026, 0.025, 0.024, 0.0225, 0.021, 0.020, 0.018, 0.016, 0.0145, 0.0135,
]
LETTER_DRILLS = [
    0.234, 0.238, 0.242, 0.246, 0.250, 0.257, 0.261, 0.266, 0.272, 0.277, 0.281, 0.290, 0.295,
    0.302, 0.316, 0.323, 0.332, 0.339, 0.348, 0.358, 0.368, 0.377, 0.386, 0.397, 0.404, 0.413,
]

SOURCES = [
    {
        "title": "ISO metric screw thread (Wikipedia): basic profile, ISO 261/262 preferred sizes and pitches",
        "url": "https://en.wikipedia.org/wiki/ISO_metric_screw_thread",
    },
    {
        "title": "Unified Thread Standard (Wikipedia): UNC and UNF sizes and threads per inch, number-size formula",
        "url": "https://en.wikipedia.org/wiki/Unified_Thread_Standard",
    },
    {
        "title": "Drill bit sizes (Wikipedia): number and letter gauge drill diameters, metric drill steps",
        "url": "https://en.wikipedia.org/wiki/Drill_bit_sizes",
    },
    {
        "title": "Tap and die (Wikipedia): tap drill about major diameter minus pitch for about 75 % thread",
        "url": "https://en.wikipedia.org/wiki/Tap_and_die",
    },
]

METHOD = {
    "profile": "ISO 68-1 / ASME B1.1 basic 60 degree profile: H = sqrt(3)/2 P; the basic internal thread depth is "
    "5/8 H on each side.",
    "basicMinorDiameter": "D1 = D - 2 x 5/8 H = D - 1.082532 P.",
    "tapDrillMetric": "d = D - P (about 75 % thread), rounded to the nearest point of a grid of 0.05 mm below 3 mm, "
    "0.1 mm from 3 mm to below 14 mm, 0.25 mm from 14 mm to 25 mm and 0.5 mm above 25 mm; a value half way between "
    "two points takes the larger.",
    "tapDrillUnified": "d = D - 0.974279 P = D - 0.75 x 2 x 3/4 H: the '75 % thread' of the percentage-of-thread "
    "rule (which counts 3/4 H = 0.649519 P per side as full depth), rounded to the nearest of the number gauge "
    "drills #1 to #80, the letter gauge drills A to Z and fractional drills in 1/64 in steps; a tie would take the "
    "larger drill (none occurs in this table).",
    "note": "Generated values. A shop chart may pick a drill one step away (for example 10.2 mm for M12x1.75, where "
    "this rule gives 10.3 mm); check the material and the thread class before tapping.",
}


def fmt_mm(v):
    """'5.0 mm', '6.8 mm', '2.05 mm', '26.5 mm'."""
    text = ("%.2f" % v).rstrip("0")
    if text.endswith("."):
        text += "0"
    return text + " mm"


def metric_drill(d):
    if d < 3.0:
        step = 0.05
    elif d < 14.0:
        step = 0.1
    elif d <= 25.0:
        step = 0.25
    else:
        step = 0.5
    k = d / step
    lo = math.floor(k + 1e-9)
    n = lo + 1 if k - lo > 0.5 - 1e-9 else lo
    v = round(n * step, 4)
    return v


def inch_drills():
    out = []
    for i, d in enumerate(NUMBER_DRILLS):
        out.append(("#%d" % (i + 1), d))
    for i, d in enumerate(LETTER_DRILLS):
        out.append((chr(ord("A") + i), d))
    for n in range(1, 2 * 64 + 1):
        f = Fraction(n, 64)
        whole = f.numerator // f.denominator
        rest = f - whole
        if rest == 0:
            name = "%d in" % whole
        elif whole == 0:
            name = "%d/%d in" % (rest.numerator, rest.denominator)
        else:
            name = "%d-%d/%d in" % (whole, rest.numerator, rest.denominator)
        out.append((name, float(f)))
    return out


def inch_drill(d_in):
    best = None
    for name, d in inch_drills():
        err = abs(d - d_in)
        if best is None or err < best[2] - 1e-12:
            best = (name, d, err)
        elif abs(err - best[2]) <= 1e-12:
            if abs(d - best[1]) > 1e-12:
                raise SystemExit("tie rounding %.6f in between %s and %s" % (d_in, best[0], name))
    return best[0], best[1]


def nominal_inches(nominal):
    if nominal.startswith("#"):
        n = int(nominal[1:])
        return Fraction(60, 1000) + Fraction(13, 1000) * n
    whole, _, frac = nominal.partition("-")
    if "/" in whole:
        return Fraction(whole)
    value = Fraction(int(whole))
    if frac:
        value += Fraction(frac)
    return value


def tpi_text(tpi):
    if isinstance(tpi, Fraction) and tpi.denominator != 1:
        whole = tpi.numerator // tpi.denominator
        rest = tpi - whole
        return "%d-%d/%d" % (whole, rest.numerator, rest.denominator)
    return str(int(tpi))


def metric_series(fine):
    sizes = []
    for nominal, coarse, fines in METRIC:
        d = float(nominal)
        for p in fines if fine else [coarse]:
            tap = metric_drill(d - p)
            sizes.append(
                {
                    "designation": "M%sx%s" % (nominal, ("%g" % p)),
                    "nominal": "M" + nominal,
                    "majorDiameter": d,
                    "pitch": p,
                    "basicMinorDiameter": round(d - MINOR_FACTOR * p, 6),
                    "tapDrill": {"size": fmt_mm(tap), "diameter": tap},
                }
            )
    if fine:
        name, sid = "ISO metric fine", "iso_metric_fine"
    else:
        name, sid = "ISO metric coarse", "iso_metric_coarse"
    return {
        "id": sid,
        "name": name,
        "system": "metric",
        "standard": "ISO 261 / ISO 262 sizes, ISO 68-1 basic profile",
        "sizes": sizes,
    }


def unified_series(fine):
    sizes = []
    suffix = "UNF" if fine else "UNC"
    for nominal, unc, unf in UNIFIED:
        tpi = unf if fine else unc
        if tpi is None:
            continue
        d_in = nominal_inches(nominal)
        p_in = Fraction(1) / Fraction(tpi)
        tap_name, tap_in = inch_drill(float(d_in) - UNIFIED_TAP_FACTOR * float(p_in))
        d_mm = float(d_in) * INCH
        p_mm = INCH / float(tpi)
        entry = {
            "designation": "%s-%s %s" % (nominal, tpi_text(tpi), suffix),
            "nominal": nominal,
            "majorDiameter": round(d_mm, 6),
            "majorDiameterIn": float(d_in),
            "tpi": float(tpi) if isinstance(tpi, Fraction) and tpi.denominator != 1 else int(tpi),
            "pitch": p_mm,
            "basicMinorDiameter": round(d_mm - MINOR_FACTOR * p_mm, 6),
            "tapDrill": {"size": tap_name, "diameter": round(tap_in * INCH, 6), "diameterIn": tap_in},
        }
        sizes.append(entry)
    return {
        "id": "unf" if fine else "unc",
        "name": "Unified fine (UNF)" if fine else "Unified coarse (UNC)",
        "system": "inch",
        "standard": "ASME B1.1 unified inch screw threads, basic profile",
        "sizes": sizes,
    }


def table():
    series = [metric_series(False), metric_series(True), unified_series(False), unified_series(True)]
    for s in series:
        for z in s["sizes"]:
            drill = z["tapDrill"]["diameter"]
            if not (z["basicMinorDiameter"] - 1e-9 <= drill < z["majorDiameter"]):
                raise SystemExit("%s: tap drill %.4f outside [D1, D)" % (z["designation"], drill))
    return {
        "schema": 1,
        "title": "RayCAD thread standards",
        "generatedBy": "tools/gen_thread_table.py",
        "units": "Lengths in mm; unified sizes also give inches in the fields ending in In.",
        "method": METHOD,
        "sources": SOURCES,
        "series": series,
    }


def render():
    return json.dumps(table(), indent=1, ensure_ascii=True) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = render()
    if args.check:
        try:
            with open(args.out, "r", encoding="utf-8", newline="") as f:
                current = f.read()
        except OSError:
            current = None
        if current != text:
            print("%s is out of date; run tools/gen_thread_table.py" % args.out)
            return 1
        return 0
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    count = sum(len(s["sizes"]) for s in table()["series"])
    print("wrote %s: %d sizes" % (args.out, count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
