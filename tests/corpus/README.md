# Test corpus

STEP, IGES and DXF files for the kernel robustness, import and DXF tests (PLAN.md section 11). `manifest.json`
lists every file with its source, licence, exporter, units, size and SHA-256.

- `files/` is committed: small files whose licence allows redistribution (about 15 MB).
- `cache/` is not committed (it is in `.gitignore`): files that `tools/corpus_fetch.py` downloads, either because
  they are large or because we only use them and do not redistribute them.

## Using it

```
python tools/corpus_check.py               # manifest and committed files: hashes, sizes, missing, unlisted (no network)
python tools/corpus_fetch.py               # download the fetch-only files into tests/corpus/cache/
python tools/corpus_check.py --cache       # also verify the downloaded files
python tools/corpus_check.py --summary     # counts by format, source and licence
```

`corpus_fetch.py` checks every file against the manifest's SHA-256, skips files already in the cache, resumes
interrupted downloads, and downloads each archive (the NIST zips) once into `cache/_archives/`, taking out only the
members it needs. It sends a generic User-Agent and nothing else. `--source`, `--id` and `--limit` select a subset,
`--dry-run` lists what it would fetch. Certificates are always verified; if a download fails with "certificate has
expired" although the site is fine in a browser, the machine's certificate store is at fault (seen on Windows with
www.nist.gov): pass `--ca-file` with a current CA bundle (Git for Windows ships one in `mingw64/etc/ssl/certs/`), or
set `SSL_CERT_FILE`.

`corpus_check.py` runs in `tools/run_lints.py` and as the CTest test `corpus_check`, without network.

## Manifest fields

| Field | Meaning |
|-|-|
| `id` | `<source>/<name>`, stable; tests refer to files by id |
| `path` | relative to `tests/corpus/`; `files/...` committed, `cache/...` fetched |
| `format` | `STEP`, `IGES` or `DXF` |
| `size`, `sha256` | of the exact bytes |
| `source` | key into `sources`, which gives the origin, licence and how it was checked |
| `url` | where the file is downloaded from (GitHub links are pinned to a commit), or the `tools/` script that generated it |
| `archive`, `member` | for files taken out of a zip listed in `archives` |
| `original_path` | the file's path in its source repository or archive |
| `licence` | SPDX identifier, or `LicenseRef-USGov-PublicDomain` for NIST |
| `exporter` | originating system and translator as written in the file: STEP `FILE_NAME`, IGES global section, DXF `999` comment or application ids; `null` where the file records none |
| `schema`, `units` | STEP schema / IGES version / DXF version, and the length unit, as recorded in the file |
| `redistributable` | the licence allows redistribution; required for everything under `files/` |
| `expect` | `pass`: a valid model, import should give geometry; `fail`: import must report an error without crashing or hanging |
| `defects` | known defects, or `null` |
| `generated` | `true` for the synthetic parts |

## Sources and licences

| Source | Files | Licence | Committed |
|-|-|-|-|
| NIST MBE PMI test cases (CTC, FTC, STC), AP203 and AP242 | 39 STEP: 33 from the nist.gov zip, 6 from the copy of that zip in NIST's STEP File Analyzer release (`nist-pmi/sfa/`) | US Government work, public domain; NIST: "can be used without any restrictions" | 14 files up to 1 MB; the rest from the two zips |
| NIST D2MI models (NX) | 5 STEP | US Government work, public domain (README in the zip) | 4 of 5 |
| NIST/MTC box assembly: box, cover, plate (SOLIDWORKS MBD 2018 via HOOPS Exchange, AP242 with PMI) | 3 STEP | US Government work, public domain; listed with the NIST PMI project's CAD models | none: fetch-only (4.3 to 4.8 MB each) |
| OCCT repository `data/` (Euclid exports) | 2 STEP, 2 IGES | LGPL-2.1 with the OCCT exception | `screw.step` |
| FreeCAD repository example and test data | 2 STEP, 4 DXF | LGPL-2.1-or-later | all |
| FreeCAD parts library | 156 STEP, 4 DXF | CC-BY-3.0 (`LICENSE-Assets` of that repository) | none: fetch-only |
| Mayo `tests/inputs` | 1 STEP, 1 IGES, 1 DXF | BSD-2-Clause | all |
| ezdxf examples and integration tests | 37 DXF | MIT | all |
| build123d tests and docs | 2 STEP, 31 DXF | Apache-2.0 | all |
| CadQuery `tests/testdata` | 1 STEP, 5 DXF | Apache-2.0 | all |
| STEPcode parser tests | 3 STEP (no geometry, `expect: fail`) | BSD-3-Clause | all |
| RayCAD synthetic parts | 76 STEP, 11 IGES, 11 DXF | LGPL-2.1-or-later (this project) | all |

The licence texts of the committed third-party sources are in `LICENSES/`. The exact per-file counts are in the
manifest (`corpus_check.py --summary`).

Left out on purpose, with the reason (checked 2026-11-18): the ABC dataset (its page says the copyright of the models
stays with their creators and refers to the Onshape Terms of Use, not a licence that clearly allows our use), the
Fusion 360 Gallery dataset (licensed for non-commercial research only), CAx-IF test files (cax-if.org refused automated
access, so their terms could not be checked; the CAx-IF models that also sit in the FreeCAD and STEPcode repositories
were left out too), pythonocc-demos models (no licence file), files that look like third-party user or vendor data (an
issue attachment, a commercial CAM output, a drawing of unknown origin, vendor-named parts), and anything else whose
licence was unclear. The FreeCAD library's licence is clear, but its parts are fetch-only, so this repository
redistributes none of them.

NIST public test models (checked 2026-11-25). NIST publishes the CTC, FTC and STC parts natively for each CAD system
(CATIA V5, Creo, NX, Inventor, SolidWorks), but as STEP only in its own STEP-file zip, with the originating system
blanked in every header; there is no NIST download of the same part exported to STEP by several CAD systems. Taken:
everything in that zip, the six files in which the copy in NIST's STEP File Analyzer release on GitHub
(usnistgov/SFA) differs from it (four of them only in their header or PMI entities; the STC 09 file is a different
export), and the box, cover and plate of the NIST/MTC box assembly (usnistgov/AR-PMI, the only SOLIDWORKS exports in
the set). Left out: the STEP files inside the CAD-model and test-definition zips (the AP203 geometry-only files again,
differing in the FILE_DESCRIPTION line only), the STC, HTC and MTC CAD-model zips (native CAD files, no STEP), the
box assembly's assembly file (it includes vendor fastener parts), the box assembly dataset on data.nist.gov (249 MB
for one STEP file), and the NIST Engineering Design Model Repository (usnistgov/engineering-design-models: 1990s
parts from industrial collaborators, not NIST's own test models).

## Synthetic parts

`files/raycad-synthetic/` holds 82 STEP and IGES parts built with this project's OpenCASCADE by
`tools/corpus_gen/corpus_gen.cpp`, and 10 DXF profiles plus 6 deliberately broken files written by
`tools/corpus_synth.py`. The parts are prismatic job-shop parts: through and blind holes with drill points,
counterbores, countersinks, closed and open pockets with corner radii, islands, slots, a keyway, a T-slot, fillets,
chamfers, thin shelled walls, ribs, a flange with a bolt circle, an L bracket, a manifold block and fixture plates
in the style of benchmark part 1. They are written in mm and inch, as AP203, AP214 and AP242, and some as IGES
(BRep and trimmed surfaces). The broken files are a truncated STEP, a STEP with a wrong header, an empty STEP, an
HTML error page saved as STEP, a truncated IGES and a DXF without EOF.

Both tools write the same bytes on every run, with a fixed date and no user name, machine name or path in any
header. To regenerate, build the generator (a standalone CMake project, not part of the RayCAD build, because only
`src/kernel` may link OpenCASCADE there) against the same OpenCASCADE as the product, then run both tools:

```
cmake -S tools/corpus_gen -B build/corpus_gen -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<vcpkg_installed>/<triplet>
cmake --build build/corpus_gen
build/corpus_gen/corpus_gen tests/corpus/files/raycad-synthetic     # on Windows, put <vcpkg_installed>/<triplet>/bin on PATH
python tools/corpus_synth.py
python tools/corpus_check.py                                        # any changed byte shows up as a hash mismatch
```

On Windows run `call tools\win-env.cmd` first. `<vcpkg_installed>` is the `vcpkg_installed` folder of a configured
RayCAD build (for example `build/vcpkg_installed`, triplet `x64-windows`). With OpenCASCADE 8.0.1 the output is
identical to the committed files; another version may write different bytes, and then the manifest rows need
updating (`corpus_check.py --describe`).

## Adding a file

1. Check the licence. Only add files whose licence clearly allows our use: public domain, CC0, CC-BY, or a permissive
   open-source licence (MIT, BSD, Apache-2.0), or LGPL for files from LGPL projects. For a file we do not commit,
   the terms must at least allow automated download and use for testing. No proprietary or vendor sample files,
   even if they can be downloaded freely. If the licence is unclear, do not add the file.
2. Describe it: `python tools/corpus_check.py --describe path/to/file` prints format, size, SHA-256, exporter, schema
   and units read from the file.
3. Add a row to `files` in `manifest.json` (and a `sources` entry for a new source). Put small redistributable files
   under `files/<source>/` and commit them; give everything else a `cache/<source>/` path and an https `url`
   (pinned to a commit or a versioned release, never a moving branch).
4. Set `expect`, and `defects` if the file is known to be broken.
5. Run `python tools/corpus_check.py`, and `python tools/corpus_fetch.py --id <id>` for a fetch-only file.

Keep the committed part small (about 25 MB at most) and favour many small, varied parts over a few large ones.
