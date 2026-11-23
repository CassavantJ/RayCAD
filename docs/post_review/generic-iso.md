# Generic ISO / RS-274 post: review notes

**Status: not yet run on a control.** Every golden is read back by the project's round-trip interpreter (T-043) and
the words are checked against the documentation cited below (RS274/NGC). No program from this post has been run on a
real control, and nobody who runs one has reviewed it yet. Review is wanted from people who run an RS-274 / ISO
control: see [Reporting a result](README.md#reporting-a-result).

Post: `resources/library/posts/generic-iso.post.js` (dialect `generic-iso`). Goldens: `tests/golden/gcode/generic-iso/`.
See [README.md](README.md).

The post is for a control that follows the common RS-274 / ISO 6983 word meanings and has no post of its own. It
writes only words whose meaning the NIST RS274/NGC description fixes, in their most portable form. What controls
treat differently is a property or is left out: chip-break drilling (G73 is not an RS274/NGC code) is expanded by the
host into plain moves, left-hand tapping (no G74 in RS274/NGC) is refused, and arcs are split into pieces of at most
180 degrees.

## Sources

| Key | Document | Read? |
|---|---|---|
| N | T. Kramer, F. Proctor, E. Messina, *The NIST RS274NGC Interpreter - Version 3*, NISTIR 6556, NIST, 2000 (public domain) | Yes |
| ISO | ISO 6983-1:2009, Automation systems and integration, Numerical control of machines, Program format and definitions of address words, Part 1 | No (sold by ISO); cited by title only, nothing depends on it alone |

## Block by block

| Block | Output (mm) | Source |
|---|---|---|
| Leader | `%` | common tape format; not in N (unverified for a given control) |
| Program number | `O1101 (NAME)` (property programNumberLine; RS274/NGC has no O word) | ISO title only, unverified; N 3.3.2 table 3 lists no O |
| Comments | `(...)`, upper case, no nested parentheses | N 3.3.4 |
| Units | `G21` / `G20` alone, before any motion | N 3.5.7 |
| Safe start | `G17 G40 G49 G80 G90 G94` | N 3.5.6, 3.5.10, 3.5.11, 3.5.15, 3.5.17, 3.5.19 |
| Retract (tool change and end) | `G0 G53 Z0.` (machine coordinates, Z only) by default; `G91 G28 Z0.` + `G90` by property | N 3.5.12, 3.5.8 |
| Tool change | `T1 M6`, pre-select `T2` after | N 3.6.3 (M6 makes no retract of its own), 3.7.3 |
| Spindle | `S6000 M3` / `M4` | N 3.6.2, 3.7.2 |
| Length offset | `G43 Z25. H1` | N 3.5.11 |
| Coolant | `M7` mist, `M8` flood, `M9` off; through and air: no default | N 3.6.4 |
| Arcs | centre format, both end-point words of the plane, both offsets; at most 180 degrees per block | N 3.5.3.2 |
| R arcs (property) | at most 90 degrees, positive R | N 3.5.3.1 (165 degrees and more is ill-conditioned) |
| Helical | in all three planes | N 3.5.3 |
| Cutter compensation | XY plane only; on/off on G1 with X and Y; `D` | N 3.5.10, Appendix B |
| Cycles | `G81`, `G82 P..`, `G83 Q..`, `G84`, `G85`, `G89 P..`; `G98`/`G99` | N 3.5.16.2-3.5.16.10, 3.5.20 |
| Dwell | `G4 P0.5` seconds (or integer milliseconds by property) | N 3.5.4 |
| Cycle cancel | `G80` | N 3.5.15 |
| End | `M30`, `%` | N 3.6.1 |

## Goldens and what to check

Each file exists as `_mm.nc` and `_inch.nc`.

| Golden | Shows | Check |
|---|---|---|
| 01-arcs-xy | quadrant arcs, full circles, 135 and 270 degrees | full circles and the 270-degree arc split into halves (180 degrees at most); I/J signs |
| 02-arcs-planes | G18 and G19 arcs | I K for G18, J K for G19 |
| 03-helical | XY helices, an XZ helix, a dwell | full turns as two half-turn blocks; the XZ helix as G18 G2 with Y moving; `G4 P0.5` |
| 04-tool-change | three tools, a repeated tool, H25 | `G0 G53 Z0.` before each M6; pre-select; no M6 between the two slots |
| 05-cutter-comp | G41 / G42 with linear leads | as for FANUC |
| 06-coolant | the coolant modes | through/air codes set by the golden's properties (examples only) |
| 07-names | sanitising | `O0042`, characters replaced by spaces, upper case |
| 08-sequence-numbers | N on every block | |
| 09-spot-drill .. 14-ream | G81, G82, G83, G73 expanded, G89, G85 | 12: no G73, plain G0/G1 moves with 0.3 mm back-offs (the IR's value); a G98 hole after G99 holes: G80, `G0 Z35.`, the cycle again with G98; Q and P on every block; G89/G85 holes that return to the clearance: G99, then G80 and `G0 Z35.` |
| 15-tap | right-hand G84 only | M3 before G84; F = pitch x rpm; the last hole G99 then G80 and `G0 Z35.` |
| 16-cycle-change | G81, G82, G82 deeper with another R, G81 in one run | the code changes restart from the clearance |
| 20-realistic | the whole program | chip break expanded |
| 30-arcs-r | R format | |
| 31-dwell-ms-g28 | dwell in milliseconds, G28 retracts | `G89 ... P200`; `G91 G28 Z0.` then `G90` |
| 32-helical-dwell-ms | `G4 P500` | |

## Feature matrix

| Item | Golden |
|---|---|
| each cycle | 09-14, 15 (G84); chip break expanded (12); no left-hand tap |
| arcs, quadrants, full circles | 01, 30 |
| helical | 03, 32 |
| tool change with pre-select | 04 |
| G43 | every file, 04 |
| G41/G42 | 05 |
| coolant modes | 06 |
| inch and metric | `_inch` / `_mm` |
| sequence numbers on/off | 08 / others |
| program number and comment sanitising | 07 |

## Unsure, unverified, or a choice for someone who runs the control to confirm

1. `%` and the `O` line are not RS274/NGC; most ISO controls want them. Property programNumberLine turns O off.
2. `G0 G53 Z0.` assumes machine Z zero is the top of travel, as on most vertical machining centres. G28 on RS274/NGC
   goes to a home position that may include X and Y; the post's G28 form (`G91 G28 Z0.`) moves only Z on FANUC-style
   controls, but read the control's manual before choosing it.
3. RS274/NGC's G83 retracts to the clear level (R or initial) between pecks, FANUC's to R: either is safe, the cycle
   time differs.
4. G84 and G83 have no dwell word in RS274/NGC: a dwell there is warned about and not posted.
5. Coolant codes for through-spindle and air are not standard.
6. Canned cycles inside one run (changed after the T-043 round trip, findings F1 and F2):
   - Later holes write X/Y, Z and R only when they change (the only "sticky" numbers, N 3.5.16), and Q and P on every
     block (required items of G83, G82 and G89, N 3.5).
   - G98 is written only where its meaning cannot be in doubt. N 3.5.20 returns to the Z "just before the canned cycle
     started"; read per block (as the example of N 3.5.16.2 does), a G98 hole after G99 holes would return only to the
     previous R plane. So a hole that needs the initial level after a lower one ends the run: G80, `G0 Z<clearance>`,
     and the cycle starts again there.
   - G84, G85 and G89 feed out all the way to the clear level (N 3.5.16.5, .6, .10), which with G98 would mean feeding up
     to the clearance. Those holes are posted with G99 and followed by G80 and `G0 Z<clearance>`: the toolpath's motion
     exactly. No G98 is written for them.
   - A change of the cycle G code inside a run is written as G80, a rapid back up to the clearance and the new cycle.
7. Tool change without a retract (toolChangeRetract = none) is refused unless the property m6LeavesZRetracted states
   that the machine's M6 leaves Z at the tool-change height: RS274/NGC's M6 leaves the axes where they are (N 3.6.3), so
   the first X/Y move after it would otherwise run at an unknown height (T-043 finding F5). With the assertion the
   program is posted; the round trip still reports the X/Y move at an unknown Z, as it cannot know what M6 does.
8. Z first before a G28 retract (T-043 finding M2, raised for the Mazak posts): RS274/NGC's G28 goes to the home
   position by way of the programmed point (N 3.5.8), which may include X and Y, so with toolChangeRetract or
   endRetract G28 the tool first rapids up to the operation's clearance height when it is below it. The default
   `G0 G53 Z0.` needs no such move: G53 moves only the axes programmed (N 3.5.12). No golden changed (in the matrix
   the G28 programs already end at their clearance).

## G and M words this post can write (for T-043)

G0 G1 G2 G3 G4 G17 G18 G19 G20 G21 G28 G40 G41 G42 G43 G49 G53 G54 G55 G56 G57 G58 G59 G59.1 G59.2 G59.3 G80 G81 G82
G83 G84 G85 G89 G90 G91 G94 G98 G99; M1 M3 M4 M5 M6 M30 and the codes of coolantCodes (default M7 M8 M9). Other
words: N O T S F H D X Y Z I J K R Q P, comments, `%`. Forms: `G0 G53 Z0.` (machine coordinates), `G91 G28 Z0.`
(home through the current point), `G4 P<seconds>` or `P<integer ms>`, G82/G89 P in the same unit.

## Round-trip findings (T-043)

The independent interpreter (tests/unit/post/roundtrip) reported, and the post now answers: F1 G98 after G99 in one
run, and G98 on feed-out cycles (note 6); F2 Q and P left off repeated blocks (note 6); F5 retract none (note 7, the
pairwise cases with the assertion stay listed in the round trip's known findings). F4, the diagonal rapid out of the
helix bore, was the hand-built test toolpath's own move; `helical()` in tests/unit/post/shipped_matrix.h now retracts
straight up.

## Questions for people who run this control

1. Is there a control this generic post is meant for? If so, which, and does it want `%` and an O line?
2. Is machine Z zero the top of travel on it?

## Review record

(empty: no program from this post has been run on a control yet)
