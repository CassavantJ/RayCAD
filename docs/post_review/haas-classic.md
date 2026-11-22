# Haas classic control post: review notes

**Status: NOT REVIEWED by the owner.** Post: `resources/library/posts/haas-classic.post.js` (dialect
`haas-classic`). Goldens: `tests/golden/gcode/haas-classic/`. See [README.md](README.md).

The classic post writes the same words as the NGC post: read [haas-ngc.md](haas-ngc.md) for the block-by-block
sources, the feature matrix and the G/M word list; they apply here unchanged. The source, the 2015 Haas *Mill Series
Programming Workbook* (H1), was written for this generation of control (it mentions mill software 12.02, p.45), so
for the classic control the H1 citations are direct.

## Differences from the NGC post

| | NGC post | Classic post |
|---|---|---|
| dialect, title | `haas-ngc` | `haas-classic` |
| G187 | P1/P2/P3 smoothness with optional E [H2, partly verified]; `G187` again before M30 | **E only.** The workbook lists G187 for this control with E only ("Accuracy Control for High Speed Machining (E)", H1 p.17; E range 0.0001-0.25 in, H1 p.25). Chosen after the T-043 round trip (finding F3): the post writes `G187 E..` when smoothingCorner is set and never a P level; a smoothing level other than off is not posted and gives a warning naming the cause. No `G187` before M30 (resetting with a bare G187 is documented for the NGC only) |
| everything else | | identical |

## Goldens

The same set as the NGC post (`01`-`20`; no `32-arcs-r`), plus:

| Golden | Shows | Check |
|---|---|---|
| 30-g187-smoothing | smoothing = finish and smoothingCorner = 0.05 mm: `G187 E0.05` (inch `E0.002`), no P; the warning in `.warnings.txt` | is G187 E right for the machine? |
| 31-peck-ijk | `G83 ... I J K` | H1 p.77-79 |

## Questions for the owner

1. Which classic software version? If its G187 takes P1/P2/P3, the NGC post's levels could be enabled here (the post
   writes E only until that is confirmed).
2. As for the NGC post: rigid tapping, through-spindle coolant, Settings 22 and 28.

## Review record

(empty: no review yet)
