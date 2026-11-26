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

## The wider NIST set

`set_nist.tsv` (same columns) lists 26 more real parts, all NIST public test models from the corpus, measured the same
way by the ctest `ray_kernel_directedit_nist` (slow tier, 8 processors, the same per-case time limit):

| Parts | Corpus ids |
|-|-|
| 10 other exports of the CTC/FTC parts in the old set: AP242 CTC 01-05 and FTC 06-10 | `nist-pmi/nist_*_ap242-*` |
| 5 AP203 files with PMI, CTC 01-05 | `nist-pmi/nist_ctc_0*_asme1_ap203` |
| 6 STC 06-10 (AP242), STC 09 in two different exports | `nist-pmi/nist_stc_*`, `nist-pmi/sfa/nist_stc_09_asme1_ap242-e4` |
| 3 D2MI parts (NX 8.5, and one CT CoreTechnologie translation) | `nist-d2mi/827-9999-904_rev_c`, `-905`, `-906` |
| 2 NIST/MTC box assembly parts, cover and plate (SOLIDWORKS MBD 2018) | `nist-mtc/nist_mtc_crada_cover_rev-b`, `nist-mtc/nist_mtc_crada_plate_rev-a` |

The CTC/FTC parts reappear on purpose: the same design written by other exporters and AP versions, which is where
import and direct-edit quirks show. Files that differ from another listed file only in their header or PMI entities
(the D2MI 907 and 908 files, four of the `sfa/` files) are left out so no geometry counts twice. Two NIST files are
not in the set because they do not import as a valid solid: the FTC 08 `-tg` files (tessellated geometry, no B-rep
solid) and the MTC box (one solid that fails the validity check with the default healing; the STEP reader's own fix makes it valid, see docs/reports/import-mtc-box-heal.md).

20 of the 26 are fetch-only: `python tools/corpus_fetch.py --source nist-pmi nist-d2mi nist-mtc`. CI fetches nothing,
so there the test runs the 6 committed parts and checks every case (a valid solid or a KernelFailure with a message,
the time limit kept) but not the rates, which it judges only when all 26 parts are present; with no part present it
is skipped.
