# Standards tables

## threads.json

The thread table of the hole feature (task T-097): ISO metric coarse and fine, UNC and UNF, with the major
diameter, pitch (and threads per inch for unified sizes), basic minor diameter and a tap drill for each size.
The UI reads it through the `standards.threads.list` action; the hole feature stores the chosen thread in its
props (`thread`), where CAM thread pickup (T-097b) reads it.

The file is generated. Do not edit it by hand: change `tools/gen_thread_table.py` and run

    python tools/gen_thread_table.py            # rewrites resources/standards/threads.json
    python tools/gen_thread_table.py --check    # exit 1 if the file is out of date

The program compiles the file in (`src/doc/CMakeLists.txt`), so the build tree and an installed program use the
same table without looking for files on disk.

### Where the values come from

Only facts and formulas, never a copied table. The inputs are:

- ISO metric sizes and their coarse and fine pitches (ISO 261 / ISO 262 preferred sizes) and the basic profile
  of ISO 68-1, as summarised in <https://en.wikipedia.org/wiki/ISO_metric_screw_thread>.
- UNC and UNF diameter / threads-per-inch pairs (ASME B1.1) and the number-size rule
  D = 0.060 in + 0.013 in x N, as summarised in <https://en.wikipedia.org/wiki/Unified_Thread_Standard>.
- Number (#1 to #80) and letter (A to Z) gauge drill diameters, as listed in
  <https://en.wikipedia.org/wiki/Drill_bit_sizes>.
- The tap-drill rules of <https://en.wikipedia.org/wiki/Tap_and_die> (about 75 % thread).

Computed by the script (all in the file's `method` member too):

- Basic minor diameter: D1 = D - 2 x 5/8 H = D - 1.082532 P, with H = sqrt(3)/2 P.
- Metric tap drill: d = D - P, rounded to the nearest point of a grid of 0.05 mm below 3 mm, 0.1 mm from 3 mm to
  below 14 mm, 0.25 mm from 14 mm to 25 mm and 0.5 mm above 25 mm; a value half way between two points takes the
  larger. Example: M8x1.25 gives 6.75, rounded to 6.8 mm.
- Unified tap drill: d = D - 0.974279 P (P = 1/TPI), the "75 % thread" of the percentage-of-thread rule, which
  counts 2 x 3/4 H = 1.299038 P as full engagement; rounded to the nearest of the number and letter gauge drills
  and fractional drills in 1/64 in steps. Example: 1/4-20 gives 0.2013 in, rounded to #7 (0.201 in).

These rules reproduce the values the cited pages print (all 16 metric rows of the Tap and die chart; every UNC and
UNF preferred tap drill on the Unified Thread Standard page). A shop chart may still pick a drill one step away
(10.2 mm instead of 10.3 mm for M12x1.75 is common); check the material and the thread class before tapping.

### Not included

Clearance-hole, counterbore and countersink sizes for socket-head and flat-head screws (ISO 273, ISO 4762,
ISO 10642, ASME B18.3). No free public summary of those tables was found, and they may not be copied from the
paid standards or from handbooks. Counterbore and countersink diameters, depths and angles of a hole are entered
by the user.
