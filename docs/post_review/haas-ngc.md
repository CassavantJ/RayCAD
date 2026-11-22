# Haas NGC post: review notes

**Status: NOT REVIEWED by the owner.** Post: `resources/library/posts/haas-ngc.post.js` (dialect `haas-ngc`).
Goldens: `tests/golden/gcode/haas-ngc/`. See [README.md](README.md). The classic-control post
(`haas-classic.post.js`, [haas-classic.md](haas-classic.md)) writes the same words.

## Sources

| Key | Document | Read? |
|---|---|---|
| H1 | Haas Automation, *Mill Series Programming Workbook*, 2015, haascnc.com (cited by printed page, e.g. H1 p.42) | Yes |
| H2 | Haas Mill Operator's Manual, G-codes chapter, haascnc.com "codes-settings" pages | **No**: the pages refused automated reading. G187 rules come from search-result quotes of those pages and the Haas Automation UK tips "Smooth Operator" and "Varying Accuracy" (haas.co.uk, read) |
| N | NIST RS274NGC Interpreter Version 3, NISTIR 6556 | Yes |

H1 was written for the controls of 2015, before the NGC. The post assumes the NGC keeps the same meaning for every
word it writes (Haas says NGC runs earlier programs): **unverified on an NGC**.

## Block by block

| Block | Output (mm) | Source |
|---|---|---|
| Leader | `%` | H1 examples (e.g. p.82) |
| Program number and name | `O01101 (Matrix: ...)`, O00001-O99999, name in parentheses on the O line | H1 p.26 ("O ... 0 and 99999", "program name between parenthesis in the first three lines") |
| Comments | `(...)`, lower case kept | H1 p.12 |
| Units | `G21` / `G20` alone (verifies Setting 9; a mismatch alarms) | H1 p.44 |
| Safe start | `G17 G40 G49 G80 G90 G94` (the power-up defaults) | H1 p.18 |
| G187 (property) | `G187 P2 E0.05` once after the safe start; `G187` before M30 | H2, partly verified; E range 0.0001-0.25 in H1 p.25 |
| First retract | `G00 G53 G49 Z0.` (or `G28 G91 Z0.` + `G90`, or none) | H1 p.43 |
| Tool change | `T1 M06`, pre-select `T2` | H1 p.12, p.27 (T in any block before its M06); M06 retracts Z itself, H1 p.19 |
| Modes after tool change | `G21`, `G00 G90 G54 G17 X.. Y..` | H1 p.12, p.42 |
| Spindle | `S6000 M03` | H1 p.12, p.42 |
| Length offset | `G43 H01 Z25.` (H01 written as H1) | H1 p.42, p.47 |
| Coolant | `M8`/`M9` flood, `M88`/`M89` through-spindle (option), `M83`/`M84` air jet (option); no mist code | H1 p.20-21, p.124-129 |
| One M code per block | always | H1 p.11, p.19 |
| Arcs | `G17 G02/G03 X.. Y.. I.. J..`, G18 I K, G19 J K; full circle with I/J | H1 p.33, p.70 |
| R arcs (property) | at most 90 degrees; R cannot do 360 | H1 p.33 |
| Helical | XY only (G02/G03 with Z) | H1 p.70 |
| Cutter compensation | on/off on a G01 X/Y move, never with Z alone or in G02/G03, D; G40 with X and Y | H1 p.67 |
| Drill | `G81`, `G82 P0.300` | H1 p.75, p.76, p.26 (P) |
| Peck | `G83 Q..` (or `I J K` by property), `P` allowed | H1 p.77, p.26 |
| Chip break | `G73 Q..`, `P` allowed; back-off Setting 22 | H1 p.95, p.15 |
| Tap | `G84` / `G74` rigid, F = pitch x rpm to 4 decimals (inch) | H1 p.82-83 |
| Bore, ream | `G85`, `G89 P..` | H1 p.84, p.88, p.26 |
| Return level | `G98` / `G99` per hole | H1 p.72, p.79 |
| Cycle cancel | `G80` | H1 p.73 |
| Dwell | `G04 P0.500` (always with a fraction: without one P is milliseconds) | H1 p.48 |
| Tool change / end | `G00 G53 G49 Z0.`, `M05`, `M09`, `G00 G53 Y0.`, `M30`, `%` | H1 p.43, p.78 |

## Goldens and what to check

Each file exists as `_mm.nc` and `_inch.nc`.

| Golden | Shows | Check |
|---|---|---|
| 01-arcs-xy | quadrant arcs, full circles, 135 and 270 degrees | full circles as single I/J blocks |
| 02-arcs-planes | G18, G19 | |
| 03-helical | XY helices; the XZ helix as lines; G04 P0.500 | |
| 04-tool-change | three tools, a repeated tool, H25 | `G00 G53 G49 Z0.` before M06 |
| 05-cutter-comp | G41 / G42 | |
| 06-coolant | flood, mist, through, air, off | mist set to M7/M9 by the golden's properties (the code list has no mist code); M89 stops the spindle (H1 p.129), so the next operation starts it again |
| 07-names | sanitising | lower case kept; `O00042` |
| 08-sequence-numbers | N on every block | |
| 09-14 | G81, G82, G83, G73, G89, G85 | P in seconds with three decimals |
| 15-tap | G84, then G74 with its own tool | M03 / M04 first (not needed on a Haas, H1 p.82, but harmless); F to 3 decimals mm / 4 inch |
| 20-realistic | the whole program | |
| 30-g187-smoothing | `G187 P2 E0.05` | the E value in program units (0.0020 in inch) |
| 31-peck-ijk | `G83 I3. J0.75 K0.75`, no Q | J and K as fractions of the peck (properties) |
| 32-arcs-r | R format | |

## Feature matrix (T-041 acceptance)

| Item | Golden |
|---|---|
| each cycle | 09-15 |
| arcs in four quadrants and full circles | 01, 32 |
| helical | 03 |
| tool change with pre-select | 04 |
| G43 | every file |
| G41/G42 | 05 |
| coolant modes | 06 |
| inch and metric | `_inch` / `_mm` |
| sequence numbers on/off | 08 / others |
| program number and comment sanitising | 07 |
| G187 smoothing property | 30 (off in every other file) |
| G84 rigid tapping | 15 |
| G83 peck, Haas I/J/K options off by default | 11 (Q), 31 (I J K by property) |

## Unsure, unverified, or a choice the owner should confirm

1. Everything is from the 2015 workbook; the NGC is assumed to read the same words (unverified on an NGC).
2. G187: P1/P2/P3 and E from H2 quotes, not from a page read in full. The starter Haas machine sets smoothing to
   medium (also the control's factory default for Setting 191).
3. G89 P: the workbook's G code table lists G89 without P, its address table lists P for G89; the post writes P.
4. Helical in G18/G19 is linearised (only G17 helical is documented in H1).
5. The post starts the spindle with M03/M04 before G84/G74 although the cycle starts it itself.
6. Mist coolant has no code in the workbook's M code list (it may on a given machine).
7. Program end brings Y to machine zero (table forward) by default, as H1 p.43; property endPosition.
8. Max comment and line length: not documented in H1; no limit enforced by default.

## G and M words this post can write (for T-043)

G0 G1 G2 G3 G4 (written G00 G01 G02 G03 G04) G17 G18 G19 G20 G21 G28 G40 G41 G42 G43 G49 G53 G54-G59 G110-G129
G154 G73 G74 G80 G81 G82 G83 G84 G85 G89 G90 G91 G94 G98 G99 G187; M01 M03 M04 M05 M06 M30 and the coolant codes
(default M8 M9 M88 M89 M83 M84). Other words: N O T S F H D X Y Z I J K R Q P E, comments, `%`. Forms:
`G00 G53 G49 Z0.` (machine Z zero and length offset off), `G00 G53 Y0.`, `G28 G91 Z0.` (+ `G90`), `G04 P<s.sss>`,
cycle P seconds with three decimals, G83 with I J K instead of Q, G187 with P1-P3 and E.

## Questions for the owner

1. Which Haas control software version, and is it an NGC? Rigid tapping and through-spindle coolant installed?
2. G187: is medium the right default for the shop's VF-2? Should roughing and finishing operations get different
   levels (the post has one level per program)?
3. Is a mist or shower coolant code fitted?
4. Setting 28 (canned cycle act without X/Y) and Setting 22 (peck clearance) values.
5. End of program: table forward (Y home) as default?

## Review record

(empty: no review yet)
