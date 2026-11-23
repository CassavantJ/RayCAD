# Post review (T-040, T-041, T-042)

## Status

**No program from any shipped post has been run on a real control yet.** What has been checked, for every post:

- **The round trip.** Every golden program is read back by the project's own G-code interpreter (T-043,
  `tests/unit/post/roundtrip`), which shares no code with the post host, and compared with the toolpaths it was posted
  from: end points, feeds, arcs, canned cycles, the rapids the post adds, and machine-level safety facts (no feed move
  with the spindle stopped, Z up before X/Y travel, cycles cancelled at a tool change, and so on). A Mazak program is
  read in both of the ways the interpreter knows for a Mazak control and must pass under each.
- **The documentation.** Each post was written from the control's published documentation where it could be read,
  and its document here cites the source of every block. Where a maker does not publish its manuals openly (FANUC,
  Mazak), the post writes the common ISO / RS-274 subset and every maker-specific rule is marked [unverified].
- **The goldens.** They pin the current output, so any change to a program is seen and must be reviewed.

What has not been done: none of these programs has run on a machine, and nobody who runs one of these controls has
reviewed the goldens yet. A post can be wrong in ways neither check finds (a tool-change macro, a parameter of the
control, an option the machine does not have). Until someone who runs the control has reviewed a post and run its
programs, treat its output as unproven: dry-run every program before cutting (the main [README](../../README.md),
"Status and safety"). The post dialog says the same for every shipped post with no recorded machine verification.

| Post | File | Goldens | Notes | Round trip | Documentation | Run on a control |
|---|---|---|---|---|---|---|
| Generic ISO / RS-274 | `resources/library/posts/generic-iso.post.js` | `tests/golden/gcode/generic-iso/` | [generic-iso.md](generic-iso.md) | passes | RS274/NGC (read) | not yet: open |
| FANUC 0i-MF / 30i-MB class | `resources/library/posts/fanuc.post.js` | `tests/golden/gcode/fanuc/` | [fanuc.md](fanuc.md) | passes | FANUC manuals not read; RS274/NGC | not yet: open |
| Haas NGC | `resources/library/posts/haas-ngc.post.js` | `tests/golden/gcode/haas-ngc/` | [haas-ngc.md](haas-ngc.md) | passes | Haas workbook 2015 (read) | not yet: open |
| Haas classic | `resources/library/posts/haas-classic.post.js` | `tests/golden/gcode/haas-classic/` | [haas-classic.md](haas-classic.md) | passes | Haas workbook 2015 (read) | not yet: open |
| Mazak MAZATROL Smooth, EIA/ISO | `resources/library/posts/mazak-smooth.post.js` | `tests/golden/gcode/mazak-smooth/` | [mazak-smooth.md](mazak-smooth.md) | passes, both readings | Mazak manuals not read; RS274/NGC | not yet: open |
| Mazak MAZATROL Matrix, EIA/ISO | `resources/library/posts/mazak-matrix.post.js` | `tests/golden/gcode/mazak-matrix/` | [mazak-matrix.md](mazak-matrix.md) | passes, both readings | Mazak manuals not read; RS274/NGC | not yet: open |

"Passes" means no issue beyond the known findings listed, with the documentation they rest on, in
`tests/unit/post/roundtrip/roundtrip_test.cpp`.

## Reporting a result

If you run one of these controls, a review or a dry run is the most useful contribution to this project. Open an issue
in the project's issue tracker with:

1. the machine and the control: maker, model, control model and its software version (the control's "about" or
   version screen shows it), and the options that matter (rigid tapping, through-spindle coolant, extra work offsets);
2. the post and the RayCAD version or commit you used, and any post properties you changed;
3. the program: attach the `.nc` file (or the shortest program that shows the problem) and, if you can, the
   `.raycad` document it came from;
4. what you did (read it against the manual, single block above the part, a cut in wax or metal) and what happened:
   each line that was wrong, what the control did with it, and what it should have been, with the manual section if
   you have it.

A finding changes the post, the goldens are made again, and the changed files are reviewed again. A result is recorded
under "Review record" in the post's document here, with the date, the issue and the goldens or features it covers.

## Marking a post as run on a control

A post's declaration lists the controls a program from it has run on: `verifiedOn` in its `definePost` block
(`include/ray/post_api.d.ts`), one string per control, naming the control model and its software version. It is empty
for every shipped post today. When someone who runs a control has reviewed a post and run its programs, a maintainer
adds that control to `verifiedOn` and records the result here under the post's "Review record" with the same text;
`tests/unit/post/shipped_posts_test.cpp` checks that every entry of `verifiedOn` is recorded in the post's document.
No code changes: the post dialog reads `verifiedOn` and stops calling a shipped post unproven once it names a control.
It does not change the program the post writes. Posts in a user's own library are not labelled either way.

## How the goldens are made

`tests/unit/post/shipped_posts_test.cpp` builds the programs: the drilling operation's golden toolpaths
(`tests/golden/toolpaths/drill_*.rctp`, readable as the `.txt` next to them) for the cycles, and hand-built toolpaths
for the milling features (arcs in each plane, helices, cutter compensation, tool changes, coolant), so that each
feature is shown on its own. Each program is posted in millimetres (`*_mm.nc`) and inches
(`*_inch.nc`); the inch files are the same metric toolpaths converted, so their numbers are not round. The machine is
a test VMC (12000 rpm, rigid tapping, travel X 0-762, Y 0-406, Z -508-0 mm). The program's header comment (the O line)
says what the file demonstrates. When the post warns, the warnings are in `<name>.warnings.txt` beside the program
(and the test fails if they change).

The test compares every golden byte for byte on every OS. To write the produced files after a reviewed change:

```
set RAYCAD_GOLDEN_PRODUCED=M:\some\folder
ray_post_tests "[shipped][golden]"
python tools/update_golden.py --produced M:\some\folder\fanuc --suite gcode/fanuc          (and so on per post)
python tools/update_golden.py --produced M:\some\folder\fanuc --suite gcode/fanuc --write  (after reading the diff)
```

The same test file also checks, for every golden, that the line map covers every line with an axis word, and, for a
pairwise set of property combinations, that the realistic program passes the structural checks (safe start first,
end block last, no feed move with the spindle stopped, no cycle or cutter compensation left on at a tool change or
the end, Z retracted before M30).

## How to review

This is for someone who knows the control. For each golden in a post's document: read the program top to bottom
against the "what to check" list and the control's manual, and against the toolpath it came from (the `.txt` dumps in
`tests/golden/toolpaths/`, or the program's own header comment for the hand-built ones). Things to look at on every
file:

1. The start: `%`, the O line, the units block, the safe start block.
2. Each tool change: retract, `T.. M6`, pre-select, the restated modes, spindle before the `G43` approach, coolant.
3. Every number: units, decimals, signs, the rounding of the inch conversion.
4. Each canned cycle: G98/G99, R, Z, Q, P (units!), F, and the G80.
5. The end: Z retract first, spindle and coolant off, the XY return, `M30`, `%`.

Report what you find as described under "Reporting a result" above; the answers to the "Questions" section of the
post's document help as much as the findings.
