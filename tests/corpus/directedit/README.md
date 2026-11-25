# Direct-edit set (spike S9)

The 50 solids spike S9 measured the direct edits on (`spikes/s9-directedit/REPORT.md` section 3.2), kept so that
`tests/unit/kernel/directedit_corpus_test.cpp` (ctest `ray_kernel_directedit_corpus`) measures `deleteFaces`,
`resizeHole` and `offsetFaces` by the spike's method and the rates can be set beside the spike's (PLAN.md T-080).
`set.tsv` lists every part with its path relative to `tests/corpus/`, its size and SHA-256.

| Parts | Where | Source and licence |
|-|-|-|
| 11 real: NIST CTC 01-05 and FTC 06-11 | `../files/nist-pmi/` (9, committed) and `../cache/nist-pmi/` (CTC 02 and FTC 11, fetch-only: `python tools/corpus_fetch.py --source nist-pmi`) | The corpus manifest rows named in `set.tsv`: US Government work, public domain (`../README.md`) |
| 39 synthetic: 13 families in 3 variants | `synthetic/` (committed, 3.2 MB) | Written by the spike's own generator (`spikes/s9-directedit/gen.cpp`, `s9 gen <dir>`) with this project's OpenCASCADE; LGPL-2.1-or-later like RayCAD |

The real parts are the AP203 geometry-only files the spike used, except FTC 11, whose geometry-only file holds a
6-face solid and not the part, so the AP242 file is used. The synthetic parts are the spike's files byte for byte;
their STEP header carries the date they were written and no user or machine name. Variant 1 of each family (the
files ending `_1`) arrives with its cylinders split into two half faces, as many exporters write them.

These are test models and generated parts, not job-shop customer parts. The plan's step "add 20 or more real job-shop
STEP files before M6" is still open: rates measured here are on this set only.

If a fetch-only part is missing the test leaves it out, names it in its output, and measures the rest.
