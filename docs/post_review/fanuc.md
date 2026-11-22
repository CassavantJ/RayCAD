# FANUC post: review notes

**Status: NOT REVIEWED by the owner.** Post: `resources/library/posts/fanuc.post.js` (dialect `fanuc`). Goldens:
`tests/golden/gcode/fanuc/`. See [README.md](README.md) for how the goldens are made and how to review.

**Read this first.** FANUC does not publish its operator's manuals openly, and the author of the post could not read
them. Every FANUC-specific rule in the post is therefore marked *[F1, unverified]*: it is general knowledge of FANUC
controls, written in its most common form, and must be checked against the manual of the actual control and the
machine builder's documentation (tool change macro, M codes) before the post is trusted. Where RS274/NGC (NIST, read)
gives the same meaning, that is cited as [N] and is checked.

## Sources

| Key | Document | Read? |
|---|---|---|
| F1 | FANUC Series 0i-MODEL F Operator's Manual (Common to Lathe System / Machining Center System), B-64604EN; FANUC Series 30i/31i/32i-MODEL B Operator's Manual (Machining Center System), B-64484EN-2 | **No** (not openly published) |
| N | T. Kramer, F. Proctor, E. Messina, *The NIST RS274NGC Interpreter - Version 3*, NISTIR 6556, NIST, 2000 | Yes |
| ISO | ISO 6983-1:2009, Program format and definitions of address words, Part 1 | No (sold by ISO); title only |

## Block by block

| Block | Output (mm) | Source |
|---|---|---|
| Leader | `%` | F1 "Program configuration", unverified |
| Program number and name | `O1101 (MATRIX: ...)`, O0001-O9999 (8 digits by property) | F1, unverified; 8000-9999 warned as often protected (parameter 3202, unverified) |
| Header comments | `(...)`, upper case, characters A-Z 0-9 space . , - + / * = : # _ | F1 comments "control out/in", unverified; N 3.3.4 |
| Units | `G21` / `G20` alone | F1 "G20/G21 in an independent block at the start", unverified; N 3.5.7 |
| Safe start | `G17 G40 G49 G80 G90 G94` (+ property safeStartExtra) | F1 G code list, unverified; N 3.5.6, 3.5.10, 3.5.11, 3.5.15, 3.5.17, 3.5.19 |
| First retract | `G91 G28 Z0.` then `G90` (or `G0 G53 Z0.`, or none) | F1 "Reference position return", unverified; N 3.5.8, 3.5.12 |
| Tool change | `T1 M6`, then `T2` (pre-select) | F1 + builder's M6 macro, unverified; N 3.6.3, 3.7.3 |
| Modes after tool change | `G21`, then `G0 G90 G54 G17 X.. Y..` | F1, unverified |
| Spindle | `S6000 M3` (M4 counter-clockwise) | N 3.6.2, 3.7.2 |
| Length offset | `G43 Z25. H1` on the first Z approach | F1 "Tool length offset", unverified; N 3.5.11 |
| Coolant | `M8` flood, `M7` mist, `M9` off; through and air have no default code | N 3.6.4; through/air are builder-specific |
| Linear, rapid | `G1 X.. Y.. Z.. F..`, `G0 ...` | N 3.5.1, 3.5.2 |
| Arcs | `G17 G2/G3 X.. Y.. I.. J..`, `G18 ... I.. K..`, `G19 ... J.. K..`; both end-point words of the plane always | F1 "Circular interpolation", unverified; N 3.5.3.2 |
| Full circle | one block, end = start, with I/J | F1, unverified; N 3.5.3.2 ("OK if the end point is the same as the current point") |
| R arcs (property) | pieces of at most 90 degrees, positive R | N 3.5.3.1 |
| Helical | XY only; XZ/YZ helices are cut as lines (host) | F1 "Helical interpolation", unverified (conservative) |
| Cutter compensation | `G41/G42 G1 X.. Y.. D..` on a linear X/Y move, `G40 G1 X.. Y..` | F1 "Cutter compensation C", unverified; N 3.5.10, Appendix B |
| Drill | `G81` / `G82 P..` (P milliseconds, integer) | F1 "Canned cycles", unverified (P units); N 3.5.16.2-3 |
| Peck | `G83 ... Q..`; no P (a dwell is warned and dropped) | F1, unverified; N 3.5.16.4 |
| Chip break | `G73 ... Q..`; back-off from parameter 5114 | F1, unverified |
| Tap | `M29 S..` then `G84`/`G74` (rigid); or plain `G84`/`G74` with M3/M4 first | F1 "Rigid tapping", unverified |
| Bore, ream | `G85` / `G89 P..` | F1, unverified; N 3.5.16.6, 3.5.16.10 |
| Return level | `G98` / `G99` per hole | F1, unverified; N 3.5.20 |
| Cycle cancel | `G80` | N 3.5.15 |
| Dwell | `G4 P500` (ms, integer) or `G4 X0.5` (property) | F1 "Dwell", unverified |
| Tool change / end retract | `G91 G28 Z0.`, `G90`, `M5`, `M9` | F1, unverified |
| Optional XY return | `G91 G28 Y0.` / `G91 G28 X0. Y0.`, `G90` | F1, unverified |
| End | `M30`, `%` | F1, unverified; N 3.6.1 |

## Goldens and what to check

Each file exists as `_mm.nc` and `_inch.nc`.

| Golden | Shows | Check |
|---|---|---|
| 01-arcs-xy | four quadrant arcs each way, full circles each way, 135 and 270 degree arcs | G2/G3 direction against the toolpath; I/J signs (centre minus start); full circle as one block with X Y = start |
| 02-arcs-planes | G18 and G19 arcs | plane word before the arc; I K for G18, J K for G19; G2/G3 seen from +Y (G18) and +X (G19) |
| 03-helical | two full helical turns, a quarter turn, a flat circle, a dwell, and an XZ helix | the XY helices as single G3 blocks with Z; the XZ helix as many short G1 lines (helical in XZ is not posted); `G4 P500` |
| 04-tool-change | three tools, tool 5 twice (no change between), H25 / D25 for tool 5 | retract before each M6; `T.. M6` then the next T; G43 with H25 for tool 5; the second slot reuses the tool: Z up first, then XY, no M6 |
| 05-cutter-comp | G41 lead-in, arc with comp, G40 lead-out; then G42 | G41/G42 only on G1 blocks with X and Y; D14; G40 with both X and Y |
| 06-coolant | flood, mist, through, air, off on one tool, then a new tool | the codes come from coolantCodes set to `through=M88/M89 air=M83/M84` for this golden: those two are examples, not FANUC codes |
| 07-names | program 42 with characters no control accepts | `O0042`; parentheses, `;`, `%`, `[ ]`, non-ASCII turned into spaces; upper case |
| 08-sequence-numbers | N numbers on every block | N10, N20...; `%`, O line and comments without N |
| 09-spot-drill | G81 over eight holes, R changes on a step | G99 between holes, G98 before the step and at the end; on the R change: G80, back up to the initial level, a full new G81 block |
| 10-drill-dwell | G82 with 0.3 s | `P300` (milliseconds) |
| 11-peck | G83, Q3 | Q in program units; G98 on the last hole |
| 12-chip-break | G73, Q1.5; the IR's 0.3 mm back-off | a warning (parameter 5114), no word for it |
| 13-bore | G89 with 0.2 s | `P200` |
| 14-ream | G85 | feed out (G85), no dwell |
| 15-tap | M6x1 rigid G84, then a left-hand G74 with its own tool | `M29 S500` before each cycle; spindle not started with M3 before M29; F500. = 1 mm x 500 rpm; inch F19.6850 |
| 20-realistic | face, contour with comp and arcs, spot, drill with dwell, chip break (same tool), tap, chamfer | the whole program |
| 30-tap-plain-g84 | property tapping = plainG84 | M3 before G84, M4 before G74, no M29 |
| 31-arcs-r | property arcFormat = R | each quadrant arc one block with R10.; full circles as four R blocks |
| 32-g53-dwell-x-style-one | G53 retracts, end Z then X Y, dwell X, decimal style one | `G0 G53 Z0.`, `G0 G53 X0. Y0.`; `G4 X0.5`; numbers as `X10.0` |

## Feature matrix (PLAN.md T-040 acceptance)

| Item | Golden |
|---|---|
| each cycle | 09 (G81), 10 (G82), 11 (G83), 12 (G73), 13 (G89), 14 (G85), 15 (G84 M29, G74), 30 (plain G84/G74) |
| arcs in four quadrants and full circles | 01, 31 (R) |
| arcs in XZ / YZ | 02 |
| helical | 03 |
| tool change with pre-select | 04, 20 |
| G43 length offsets | every file; 04 (H25) |
| G41/G42 | 05, 20 |
| coolant modes | 06 |
| inch and metric | every file, `_inch` and `_mm` |
| sequence numbers on/off | 08 on; all others off |
| program number and comment sanitising | 07 |

## Unsure, unverified, or a choice the owner should confirm

1. Everything marked [F1, unverified] above: in particular the dwell units (`G4 P` milliseconds without a decimal
   point, cycle `P` milliseconds), G83/G73 having no P word, M29 with the spindle stopped, and `G54.1 P` for extra
   work offsets.
2. Whether `G20/G21` may be restated after each tool change (the post does it, in a block of its own, always the same
   unit as at the start).
3. Tool change: the builder's M6 macro decides what happens; many retract Z themselves. The post retracts with
   `G91 G28 Z0.` before every M6 by default. The length offset is not cancelled with G49 before M6: the next G43
   replaces it. Confirm the machine's G28 also cancels the offset as the builder set it up.
4. Arc and tapping feeds: F of a rigid tap is written with 3 decimals in mm and 4 in inch (the pitch x rpm match);
   check the control accepts that many.
5. Through-spindle coolant and air blast have no default code (builder-specific): a program that uses them fails
   until coolantCodes gives the codes.
6. Helical arcs in G18/G19 are cut as lines (safe, slower).
7. Comment length: no limit by default (property commentMaxLength); FANUC limits are not known to the author.
8. Maximum line length: none documented in the sources read; not enforced.

## G and M words this post can write (for T-043)

G0 G1 G2 G3 G4 G17 G18 G19 G20 G21 G28 G40 G41 G42 G43 G53 G54 G55 G56 G57 G58 G59 G54.1 G73 G74 G80 G81 G82 G83
G84 G85 G89 G90 G91 G94 G98 G99; M1 M3 M4 M5 M6 M29 M30 and the codes of coolantCodes (default M7 M8 M9).
Other words: N O T S F H D X Y Z I J K R Q P, comments in parentheses, `%` lines. Forms: `G91 G28 Z0.` and
`G91 G28 Y0.` / `X0. Y0.` (reference return through the current point; not a move to work zero), `G0 G53 Z0.`
(machine coordinates), `G4 P<ms>` or `G4 X<s>`, cycle P in integer milliseconds, `M29 S<rpm>` on its own line.

## Questions for the owner (need the real machine or its manuals)

1. Which FANUC control and builder exactly (0i-MF, 31i, ROBODRILL...)? With the manual, every [F1, unverified] item
   above can be checked.
2. Tool change: what the builder's M6 macro does (Z return, spindle orientation, coolant), and whether the post's
   `G91 G28 Z0.` before M6 is wanted.
3. Rigid tapping: M29 S.. form, or G84 rigid by parameter 5200#0? Spindle stopped before M29 or not?
4. Through-spindle coolant and air blast M codes.
5. Are program numbers 8000-9999 protected on the machine, and what O-number range does the shop use?
6. G54.1 P (additional work offsets) installed?

## Review record

(empty: no review yet)
