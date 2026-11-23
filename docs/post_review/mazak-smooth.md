# Mazak Smooth post: review notes

**Status: not yet run on a control.** Every golden is read back by the project's round-trip interpreter (T-043), in
both of the readings it knows for a Mazak control, and the words are checked against RS274/NGC; no Mazak manual could
be read (below), so every Mazak-specific rule is [unverified]. No program from this post has been run on a real
MAZATROL Smooth control, and nobody who runs one has reviewed it yet. Review is wanted from people who run a Mazak
mill with a Smooth control: see [Reporting a result](README.md#reporting-a-result).

Post: `resources/library/posts/mazak-smooth.post.js` (dialect `mazak-smooth`).
Goldens: `tests/golden/gcode/mazak-smooth/`. See [README.md](README.md) for how the goldens are made and reviewed.
The Matrix post ([mazak-matrix.md](mazak-matrix.md)) is generated from this one and writes the same words.

For Mazak vertical machining centres with a MAZATROL Smooth control (SmoothX, SmoothG, SmoothAi; the SmoothEz of the
starter machine is assumed to read the same EIA/ISO programs), EIA/ISO G-code text only: no MAZATROL conversational
programs and no Mazak program file formats. 3 axes.

**Read this first.** Mazak's EIA/ISO programming manuals are supplied with the machine and through Mazak's service
centres; they are not openly published, and no Mazak manual was read for this post (copies on document-sharing sites
are not Mazak's publications and were not used). **Every Mazak-specific rule is [unverified].** The post writes the
common ISO / FANUC-compatible subset, with the meaning of each word taken from the public NIST RS274/NGC description,
and takes the safe side wherever controls are known to differ. It must be checked against the machine's own EIA/ISO
programming manual before it runs a program.

## Sources

| Key | Document | Read? |
|---|---|---|
| M | Mazak, EIA/ISO programming manual for the MAZATROL Smooth series (with the machine) | **No** (not openly published) |
| N | T. Kramer, F. Proctor, E. Messina, *The NIST RS274NGC Interpreter - Version 3*, NISTIR 6556, NIST, 2000 | Yes |
| ISO | ISO 6983-1:2009, Program format and definitions of address words, Part 1 | No (sold by ISO); title only |

## Block by block (every row [unverified] for this control; the N section gives the RS-274 meaning)

| Block | Output (mm) | Source |
|---|---|---|
| Leader, program number | `%`, `O1101 (NAME)` (4 digits, 8 by property) | [unverified] |
| Comments | `(...)`, upper case | N 3.3.4 |
| Units | `G21` / `G20` alone | N 3.5.7 |
| Safe start | `G17 G40 G49 G80 G90 G94` | N 3.5.6, 3.5.10, 3.5.11, 3.5.15, 3.5.17, 3.5.19 |
| High-speed mode (property, off) | `G61.1` ... `G64`, or `G05 P2` ... `G05 P0`; a warning when used | [unverified], from general knowledge only |
| First retract, tool-change and end retract | `G91 G28 Z0.` + `G90` (or `G0 G53 Z0.`; none only with m6LeavesZRetracted) | N 3.5.8, 3.5.12 |
| Tool change | `T1 M6`; pre-select off (or `T2` in a block of its own after the M6 block, by property). The one-block form `T1 T2 M6` is not offered (note 6) | N 3.6.3, 3.7.3 |
| After a tool change | `G21`, `G0 G90 G54 G17 X Y`, `S M3`, `G43 Z H`, coolant | N 3.5.11 |
| Coolant | `M8` / `M9`; mist, through, air: no default code | N 3.6.4 |
| Arcs | IJK, at most 180 degrees per block (full circles as two halves); R by property (90 degrees) | N 3.5.3 |
| Helical | XY only (XZ / YZ as lines) | N 3.5.3 (conservative) |
| Cutter compensation | `G41/G42 G1 X Y D`, `G40 G1 X Y`, XY plane | N 3.5.10, Appendix B |
| Work offsets | G54-G59, `G54.1 P1`-`P48` | [unverified] |
| Drill, dwell drill | `G81`, `G82 ... P300` (P in milliseconds by default, seconds by property; warned) | N 3.5.16.2-3 |
| Peck, chip break | `G83 ... Q`, `G73 ... Q`; Q on every block; no P (warned) | N 3.5.16.4 |
| Tap | `M29 S` + `G84`/`G74`; `G84.2`/`G84.3`; or plain `G84`/`G74` after M3/M4 (property) | [unverified] |
| Bore, ream | `G85`, `G89 ... P` | N 3.5.16.6, 3.5.16.10 |
| Return level | `G98`/`G99`; see note 3 | N 3.5.20 |
| End | `M5`, `M9`, optional `G91 G28 Y0.`, `M30`, `%` | N 3.6.1 |

## Goldens and what to check

Each file exists as `_mm.nc` and `_inch.nc`, the same matrix as the other posts (`01`-`16`, `20`; see
[fanuc.md](fanuc.md) for what each program contains), plus:

| Golden | Shows | Check |
|---|---|---|
| 30-tap-g84-2 | tapping = G84.2 | `S500` without M3, `G84.2` / `G84.3`; is this the machine's synchronous tapping form? |
| 31-tap-plain-g84 | tapping = plainG84 | `M3` / `M4` before `G84` / `G74`, no M29 |
| 32-high-speed-g61-1 | highSpeedMode = G61.1 | `G61.1` after the safe start, `G64` before M30; the warning |
| 34-dwell-seconds | dwellUnits = seconds | `G82 ... P0.3` |

Things to look at on every file: the full circles split in two (01, 05, 20), the G98 holes after G99 holes ended and
restarted from the clearance (09, 10, 20), Q and P on every cycle block, the bore, ream and tap holes that end with
G99, G80 and `G0 Z<clearance>` instead of G98, the `.warnings.txt` (dwell unit, tapping form).

## Feature matrix

| Item | Golden |
|---|---|
| each cycle | 09 (G81), 10 (G82), 11 (G83), 12 (G73), 13 (G89), 14 (G85), 15 (G84 M29, G74), 30 (G84.2/G84.3), 31 (plain G84) |
| arcs in four quadrants and full circles | 01 |
| helical | 03 |
| tool change with pre-select | 04 (off); the Matrix golden 33 shows separateT |
| G43 | every file; 04 (H25) |
| G41/G42 | 05 |
| coolant modes | 06 (codes set by the golden: examples, not Mazak codes) |
| inch and metric | `_inch` / `_mm` |
| sequence numbers on/off | 08 / others |
| program number and comment sanitising | 07 |

## Unsure: every Mazak-specific behaviour that could not be confirmed

1. EIA/ISO mode itself: whether the control needs a parameter or mode to run EIA/ISO programs, and which G-code system
   (FANUC-compatible or other) it is set to. The post assumes the common ISO/FANUC-compatible meanings.
2. Program numbering: `O` and 4 digits (8 by property), work-number range and name length; whether the `O` line and the
   program name in a comment are read as such.
3. Canned cycles: G98 after G99 in one run is not relied on (the run is ended, Z goes to the clearance and the cycle
   restarts); G84/G85/G89 never use G98 (G99, then G80 and a rapid up), as some controls feed out to the initial
   level; Q and P on every block (not assumed modal); whether G83 and G73 take a P dwell (not written, warned); the
   peck return amount (a parameter, not written, warned).
4. Dwell P unit in G4 and in cycles (milliseconds by default, a warning on every program with a dwell).
5. Tapping: M29, G84.2/G84.3, or synchronous G84 by parameter; whether the spindle must be stopped before the
   synchronous form (the post leaves it stopped).
6. Tool change: `T.. M6`; whether M6 retracts Z itself (the post retracts with G28 first, and refuses none without
   m6LeavesZRetracted); the next-tool forms; how T words are numbered (pocket or tool number). The one-block form
   `T<this> T<next> M6` is not offered (T-043 finding M1): if the control mounted the last T word of the block, every
   operation would run with the wrong tool and length offset (RS274/NGC: the most recent T word takes effect, N 3.7.3).
   It is not offered until the control's manual confirms how it reads two T words.
7. Tool length: G43 H, or the length taken from the control's tool data (and the parameter that selects it). If the
   control applies the tool data length itself, G43 H must be checked so that the length is not applied twice.
8. Work offsets G54.1 P (count), G28 / G53 behaviour, machine Z zero at the top of travel. Whether G28 (or G53) with Z
   alone moves only Z is unknown (RS274/NGC's G28 goes to a home position that may include X and Y, N 3.5.8), so
   before the retract the tool first rapids straight up to the operation's clearance height when it is below it
   (T-043 finding M2; golden 06-coolant shows `G0 Z25.` before `G91 G28 Z0.`).
9. High-speed / smoothing codes (G61.1, G05 P2) and their cancels.
10. Coolant: M8/M9 only; mist, through-spindle and air codes unknown.
11. Full circles in one IJK block, helical interpolation in G18/G19, R arcs over 180 degrees: avoided.
12. Decimal point and least increment: a decimal point is always written; how numbers without one are read is unknown.
13. G20/G21: whether they switch units or only check a parameter; the post writes the unit alone at the start and
    after each tool change.
14. M30 versus M02, and `%` at both ends.
15. File extension: `.nc` is written; Mazak's own name rules for EIA/ISO files on the control are unknown.

## G and M words this post can write (for T-043)

G0 G1 G2 G3 G4 G17 G18 G19 G20 G21 G28 G40 G41 G42 G43 G49 G53 G54 G55 G56 G57 G58 G59 G54.1 G73 G74 G80 G81 G82 G83
G84 G84.2 G84.3 G85 G89 G90 G91 G94 G98 G99, and with highSpeedMode G61.1 G64 G05; M1 M3 M4 M5 M6 M29 M30 and the codes
of coolantCodes (default M8 M9). Other words: N O T S F H D X Y Z I J K R Q P, comments in parentheses, `%` lines.
Forms: `G91 G28 Z0.` / `Y0.` / `X0. Y0.` (home through the current point), `G0 G53 Z0.`, `G4 P<ms>` (or `P<s.sss>` by
property), cycle P in the same unit, `M29 S<rpm>` alone before G84/G74, the next tool's `T<m>` alone after `T<n> M6`
(property), `G05 P2` / `G05 P0`.

## Questions for people who run this control (they need the machine or its manuals)

1. Which Mazak control exactly (SmoothX, SmoothG, SmoothAi, SmoothEz) and which software version? With its EIA/ISO
   programming manual every [unverified] item above can be checked.
2. Is the control set up for EIA/ISO programs, and with which G-code system?
3. Tool change: does M6 retract Z? Which next-tool form does your shop use? Are T numbers pockets or tool numbers?
4. Tool length: G43 H, or tool data? Which parameter?
5. Dwell P unit; synchronous tapping form; peck return parameter.
6. Coolant codes for mist, through-spindle and air.
7. Which high-speed / smoothing mode, if any, should programs turn on?
8. Program number range and the file name the control expects.

## Review record

(empty: no program from this post has been run on a control yet)
