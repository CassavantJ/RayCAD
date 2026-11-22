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
| G187 | P1/P2/P3 smoothness with optional E [H2, partly verified] | the workbook lists G187 with E only ("Accuracy Control for High Speed Machining (E)", H1 p.17); P levels need a later classic software version: **unverified**. Off by default |
| everything else | | identical |

## Goldens

The same set as the NGC post (`01`-`20`), plus:

| Golden | Shows | Check |
|---|---|---|
| 30-g187-smoothing | `G187 P3` (finish, no E) | does the machine's software accept P on G187? |
| 31-peck-ijk | `G83 ... I J K` | H1 p.77-79 |

## Questions for the owner

1. Which classic software version? Does its G187 take P1/P2/P3, or E only?
2. As for the NGC post: rigid tapping, through-spindle coolant, Settings 22 and 28.

## Review record

(empty: no review yet)
