# Post review (T-040, T-041)

**Status: NOT REVIEWED.** None of the shipped posts' goldens has been reviewed by the owner. PLAN.md T-040 makes a
line-by-line review by the owner the acceptance of these posts; until that review is done and recorded here by the
owner, the goldens only pin the current output so that changes are seen. Nothing in the code or the tests marks them
reviewed.

One document per shipped post:

| Post | File | Goldens | Review notes |
|---|---|---|---|
| Generic ISO / RS-274 | `resources/library/posts/generic-iso.post.js` | `tests/golden/gcode/generic-iso/` | [generic-iso.md](generic-iso.md) |
| FANUC 0i-MF / 30i-MB class | `resources/library/posts/fanuc.post.js` | `tests/golden/gcode/fanuc/` | [fanuc.md](fanuc.md) |
| Haas NGC | `resources/library/posts/haas-ngc.post.js` | `tests/golden/gcode/haas-ngc/` | [haas-ngc.md](haas-ngc.md) |
| Haas classic | `resources/library/posts/haas-classic.post.js` | `tests/golden/gcode/haas-classic/` | [haas-classic.md](haas-classic.md) |
| Mazak MAZATROL Smooth, EIA/ISO | `resources/library/posts/mazak-smooth.post.js` | `tests/golden/gcode/mazak-smooth/` | [mazak-smooth.md](mazak-smooth.md) |
| Mazak MAZATROL Matrix, EIA/ISO | `resources/library/posts/mazak-matrix.post.js` | `tests/golden/gcode/mazak-matrix/` | [mazak-matrix.md](mazak-matrix.md) |

The two Mazak posts (T-042) were written without a Mazak manual (not openly published): every Mazak-specific rule is
[unverified], and the T-043 interpreter has no Mazak dialect yet, so their goldens are not read back by the round trip.

## How the goldens are made

`tests/unit/post/shipped_posts_test.cpp` builds the programs: the drilling operation's golden toolpaths
(`tests/golden/toolpaths/drill_*.rctp`, readable as the `.txt` next to them) for the cycles, and hand-built toolpaths
for the milling features no operation generates yet. Each program is posted in millimetres (`*_mm.nc`) and inches
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

For each golden in a post's document: read the program top to bottom against the "what to check" list and the cited
manual section, and against the toolpath it came from. Things to look at on every file:

1. The start: `%`, the O line, the units block, the safe start block.
2. Each tool change: retract, `T.. M6`, pre-select, the restated modes, spindle before the `G43` approach, coolant.
3. Every number: units, decimals, signs, the rounding of the inch conversion.
4. Each canned cycle: G98/G99, R, Z, Q, P (units!), F, and the G80.
5. The end: Z retract first, spindle and coolant off, the XY return, `M30`, `%`.

Write findings in the post's document under "Review record", with the date and the golden files covered. A finding
changes the post, the goldens are made again, and the changed files are reviewed again.
