# Mazak Matrix post: review notes

**Status: NOT REVIEWED by the owner.** Post: `resources/library/posts/mazak-matrix.post.js` (dialect `mazak-matrix`).
Goldens: `tests/golden/gcode/mazak-matrix/`. See [README.md](README.md).

For Mazak vertical machining centres with a MAZATROL Matrix control (Matrix, Matrix Nexus, Matrix 2), EIA/ISO G-code
text only, 3 axes. The post is generated from the Smooth post (`mazak-smooth.post.js`) and writes the same words:
read [mazak-smooth.md](mazak-smooth.md) for the sources, the block-by-block table, the feature matrix, every
[unverified] item and the G/M word list; all of it applies here.

**No Mazak manual of the Matrix generation was read either** (they are not openly published), so the two posts
cannot honestly differ in any rule yet: both are the same conservative ISO / FANUC-compatible subset. They are two
posts so that each control generation has its own machine, properties and goldens, and one can be changed without the
other once its manual is read.

## Differences from the Smooth post

| | Smooth post | Matrix post |
|---|---|---|
| dialect, title, control name in comments and messages | `mazak-smooth`, MAZATROL Smooth | `mazak-matrix`, MAZATROL Matrix |
| everything else | | identical |

## Goldens

The same matrix as the Smooth post (`01`-`16`, `20`, `30`, `31`), plus:

| Golden | Shows | Check |
|---|---|---|
| 32-high-speed-g05-p2 | highSpeedMode = G05P2 | `G05 P2` after the safe start, `G05 P0` before M30; the warning |
| 33-preselect-separate-t | toolPreselect = separateT | the next tool's `T` alone after `M6` |

## Starter machine

`Generic Mazak VMC, MAZATROL Matrix, EIA/ISO programs (VCN-530C class)`: Mazak no longer lists Matrix-generation
machines, so its values come from a used-machine dealer listing (Surplus Record 1058083, a 2014 Nexus 530C-II TT with
MAZATROL Matrix Nexus 2), not from Mazak; the machine file says so. Not verified.

## Questions for the owner

1. Which Matrix generation and software version? Its EIA/ISO programming manual settles every [unverified] item of
   mazak-smooth.md for this post.
2. Is there a Matrix machine in the shop at all, or should this post wait?

## Review record

(empty: no review yet)
