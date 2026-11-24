# Third-party software used by RayCAD

RayCAD is licensed under LGPL-2.1-or-later (see `LICENSE`). This file is the dependency licence ledger
required by `PLAN.md` section 2.4. `tools/lint_licences.py` checks it in CI:

- every dependency in `vcpkg.json`, and every port vcpkg really installs for a build (transitive and host
  ports included), must match a row of the first table;
- no row may have a GPL-only licence. The one exception is a row whose "How it is used" cell starts with
  "Build tool only": a program that runs during the build and is neither linked into RayCAD nor shipped.

Write vcpkg port names in backticks; `boost-*` style patterns are allowed; `-` means the component does not
come from vcpkg. Versions of vcpkg ports are those of the baseline pinned in `vcpkg-configuration.json`.

## In use now

| Component | vcpkg port | Version | Licence (SPDX) | Source | How it is used |
|-|-|-|-|-|-|
| Open CASCADE Technology (OCCT) | `opencascade` | 8.0.1 | `LGPL-2.1-only WITH OCCT-exception-1.0` | https://github.com/Open-Cascade-SAS/OCCT | Dynamically linked, from `src/kernel` only. Ship the licence text, offer the source, allow relinking. Modified: built from the overlay port `ports/opencascade` (the baseline port, port-version 5) with four RayCAD patches: three to the fillet/chamfer builder, `0006-chfi3d-null-neighbour-curve.patch` (fails a stripe instead of dereferencing a null curve), `0007-chfi3d-moresurfdata-own-stripe.patch` (a corner reads the SurfData of its own stripe, not of the first one) and `0008-chfi3d-initialise-prol.patch` (a corner no longer reads an uninitialised "surface extended" flag), and one to the mesher, `0009-brepmesh-heal-faces-serially.patch` (BRepMesh_ModelHealer heals faces one at a time; in parallel it raced on pcurve points); each patch header says why. The offered source must include those patches; it is under OCCT's own licence. |
| Qt (Core, Gui, Qml, Quick and the Qt libraries they load) | - | 6.12.0 | `LGPL-3.0-only` | https://download.qt.io/official_releases/qt/6.12/ | Dynamically linked, from `src/ui` and `src/render` only. Prebuilt binaries, not built by vcpkg. Ship the licence text and notice, offer the source, allow replacing the Qt libraries. |
| Qt build tools (moc, rcc, qmlcachegen, qmltyperegistrar) | - | 6.12.0 | `GPL-3.0-only WITH Qt-GPL-exception-1.0` | https://download.qt.io/official_releases/qt/6.12/ | Build tool only: run at build time; their output is not covered by the GPL. Not shipped. |
| tl::expected | `tl-expected` | 1.3.1 | `CC0-1.0` | https://github.com/TartanLlama/expected | Header-only; `ray::Result` in `include/ray/core.h`. |
| BLAKE3 | `blake3` | 1.8.7 | `CC0-1.0 OR Apache-2.0` | https://github.com/BLAKE3-team/BLAKE3 | Linked into `src/core`; content hashing (`ray::Hasher`). Used under CC0-1.0. |
| oneTBB | `tbb` | 2023.2.0 | `Apache-2.0` | https://github.com/uxlfoundation/oneTBB | Dynamically linked into `src/core`; the job system. Ship the licence text and notice. |
| musl libm (subset, vendored as `third_party/dmath`) | - | 1.2.6 | `MIT` | https://musl.libc.org/ | Compiled into `src/core` as `ray::dmath` (sin, cos, tan, asin, acos, atan2, hypot and their helpers). Upstream files, unchanged except for one include line in three of them; a build header renames every function. See `third_party/dmath/README.md`. Ship the copyright notice (`third_party/dmath/COPYRIGHT`). |
| SQLite | `sqlite3` | 3.54.1 | `blessing` | https://sqlite.org/ | Dynamically linked into `src/store` only; the `.raycad` container (PLAN.md 4.1). Public domain; no notice required. Built without default features (no JSON functions). |
| Zstandard | `zstd` | 1.5.7 | `BSD-3-Clause` | https://github.com/facebook/zstd | Dynamically linked into `src/store` only; compression of stored objects. vcpkg records `BSD-3-Clause OR GPL-2.0-only`; used under BSD-3-Clause. Ship the licence text. |
| QuickJS-NG | `quickjs-ng` | 0.18.0 | `MIT AND Unicode-3.0` | https://github.com/quickjs-ng/quickjs | Statically linked into `src/post` only: the post-processor script host (`src/post/engine*`). Built as the plain `qjs` library; `quickjs-libc` (the std, os and bjson modules, console, print) and the qjs and qjsc programs are not built. Modified: built from the overlay port `ports/quickjs-ng` (0.18.0, port-version 1; the baseline has 0.16.2) with two RayCAD patches: `0001-interrupt-counter-100.patch` (`JS_INTERRUPT_COUNTER_INIT` 100 instead of 10000 in `quickjs.c`, so the CPU limit and cancel act on time; spike S8) and `0002-library-only-no-fp-contract.patch` (a `QJS_BUILD_PROGRAMS` option the port turns off, and `-ffp-contract=off` on GCC and Clang for identical output on every OS); each patch header says why. `Unicode-3.0` covers the Unicode tables in `libunicode-table.h`. Ship the MIT notice and the Unicode licence text. |
| Clipper2 | `clipper2` | 2.0.1 (port-version 1) | `BSL-1.0` | https://github.com/PolyClipping/Clipper2 | Statically linked into `src/cam` only, as `Clipper2::Clipper2Z` (built with `USINGZ`, one int64 Z per vertex): the offsets and booleans of the 2D geometry core (`src/cam/geom2d*`, task T-050), which tags every vertex with the exact curve it came from. Modified: built from the overlay port `ports/clipper2` (the baseline port, 2.0.1, port-version 1) with one RayCAD patch, `0001-stable-intersection-sort.patch` (the sweep's intersections sorted with `std::stable_sort`: with `std::sort` equal intersections came out in a different order with MSVC and GCC and the same Union gave different polygons; task T-060); the patch header says why. The port downloads the release tag from the repository's former name `AngusJohnson/Clipper2`, which GitHub forwards to the new home. Ship the licence text. |
| Catch2 | `catch2` | 3.16.1 | `BSL-1.0 AND BSD-3-Clause` | https://github.com/catchorg/Catch2 | Tests only. Not shipped. |
| Boost (Graph, Math, and the Boost libraries those two depend on) | `boost-*` | 1.92.0 | `BSL-1.0` | https://www.boost.org/ | Headers only (`Boost::headers`, no library linked), for the vendored PlaneGCS in `src/sketch`: Graph's `adjacency_list` and `connected_components`, Math's constants. |
| Eigen | `eigen3` | 5.0.1 | `MPL-2.0` | https://gitlab.com/libeigen/eigen | Header-only, in `src/sketch` and the vendored PlaneGCS only. FreeCAD 1.1.4 pins 3.4.0; 5.0.1 is the pinned vcpkg baseline's version (an older version needs a vcpkg version override, which the project's shallow vcpkg clones cannot resolve). Every Eigen 5 module used, `SparseCholesky` included, is MPL-2.0; MPL-2.0 is file-level copyleft: ship the licence text and keep Eigen's files as they are. |
| PlaneGCS (FreeCAD's sketch solver, vendored as `third_party/planegcs`) | - | FreeCAD 1.1.4 (`4fd3bf32`) | `LGPL-2.1-or-later` | https://github.com/FreeCAD/FreeCAD/tree/1.1.4/src/Mod/Sketcher/App/planegcs | Compiled into `src/sketch` as the static library `ray_planegcs`. The 11 upstream files are unmodified (`third_party/planegcs/sync_upstream.py` checks them); a four-header shim (spike S7) stands in for FreeCAD's `Base/Console.h`, `FCConfig.h`, `SketcherGlobal.h` and `boost_graph_adjacency_list.hpp`. Licence text and file headers kept. See `third_party/planegcs/README.md`. |
| Code adapted from PlaneGCS (`src/sketch/solver_iterate.cpp`, `.h`; `src/sketch/solver_diagnose.cpp`, `.h`) | - | FreeCAD 1.1.4 | `LGPL-2.1-or-later` | https://github.com/FreeCAD/FreeCAD/blob/1.1.4/src/Mod/Sketcher/App/planegcs/GCS.cpp | Part of `src/sketch`: the partition of `System::initSolution`, the DogLeg loop of `System::solve_DL` (plus a cancellation check, and a sparse Jacobian and Gauss step for large components), the drag iteration of `System::solve(SubSystem*, SubSystem*)` and `qp_eq.cpp` with sparse linear algebra (spike S11), and `System::diagnose` with its QR decompositions and redundancy heuristic run per decoupled component. Upstream copyright (Konstantinos Poulios, 2011) and the LGPL notice are in the file headers. |
| Code adapted from FreeCAD's adaptive clearing (`src/cam/ops/adaptive2d/engine_adaptive.cpp`, `.h`) | - | FreeCAD main, commit `56062b61c7085af5af12ead4c02ce5765d0107c4` | `LGPL-2.1-or-later` | https://github.com/FreeCAD/FreeCAD/blob/56062b61c7085af5af12ead4c02ce5765d0107c4/src/Mod/CAM/libarea/Adaptive.cpp | Part of `src/cam`: the constant-engagement engine of the adaptive operation (task T-060), behind `src/cam/ops/adaptive2d/engine.h`. A modified fork of `Adaptive.cpp` and `Adaptive.hpp` (Copyright (c) 2018 Kresimir Tusek; FreeCAD's Clipper2 port of it), measured by spike S5. The upstream files are the first version of those two files in the repository's history, unmodified; the next commit's changes are listed at the top of `engine_adaptive.cpp` (no wall-clock decisions, 64-bit integers and rounding, no shared state, initialised outputs, `ray::dmath` trigonometry, warnings instead of console output, cancel polling, integer output paths with depth estimates). The LGPL notice and the attribution are kept in both file headers; the modified files are under LGPL-2.1-or-later, as RayCAD is. Built against Clipper2 2.0.1 with `USINGZ` (the Clipper2 row). |
| Code adapted from Clipper2's offsetting (`src/cam/ops/adaptive2d/engine_offset.cpp`, `.h`) | - | 2.0.1 (tag `Clipper2_2.0.1`, commit `21ebba05`) | `BSL-1.0` | https://github.com/PolyClipping/Clipper2/blob/Clipper2_2.0.1/CPP/Clipper2Lib/src/clipper.offset.cpp | Part of `src/cam`, used by the adaptive engine only: Clipper2's `ClipperOffset` and `InflatePaths`, copied with the copyright notice kept, in a namespace of their own, with the trigonometry of round joins and ends through `ray::dmath` so offsets are identical on every platform (the library calls the platform's libm). The file headers say what changed. Ship the licence text (as for the Clipper2 row). |
| FreeType | `freetype` | 2.15.0 | `FTL OR GPL-2.0-or-later` | https://freetype.org/ | Dependency of OCCT (default `freetype` feature), dynamically linked by OCCT. Used under the FreeType License (FTL). |
| zlib | `zlib` | 1.4 | `Zlib` | https://www.zlib.net/ | Dependency of FreeType and libpng. |
| libpng | `libpng` | 1.6.59 | `libpng-2.0` | https://github.com/pnggroup/libpng | Dependency of FreeType. |
| bzip2 | `bzip2` | 1.0.8 | `bzip2-1.0.6` | https://sourceware.org/bzip2/ | Dependency of FreeType. |
| Brotli | `brotli` | 1.2.1 | `MIT` | https://github.com/google/brotli | Dependency of FreeType. |
| Fontconfig | `fontconfig` | 2.17.1 | `MIT` | https://www.freedesktop.org/wiki/Software/fontconfig | Linux only. Dependency of OCCT's `freetype` feature. |
| Expat | `expat` | 2.10.0 | `MIT AND CC0-1.0` | https://github.com/libexpat/libexpat | Linux only. Dependency of Fontconfig. |
| libuuid | `libuuid` | 1.0.3 | `BSD-3-Clause` | https://sourceforge.net/projects/libuuid/ | Linux only. Dependency of Fontconfig. |
| gperf | `gperf` | 3.3 | `GPL-3.0-or-later` | https://www.gnu.org/software/gperf/ | Build tool only: vcpkg runs it on the build machine while building Fontconfig (Linux). Not linked, not shipped. |
| Khronos OpenGL registry headers | `opengl-registry` | 2026-08-03 | `Apache-2.0 AND MIT` | https://github.com/KhronosGroup/OpenGL-Registry | Headers OCCT is built against. RayCAD does not use OCCT's visualisation. |
| Khronos EGL registry headers | `egl-registry` | 2025-05-27 | `Apache-2.0` | https://github.com/KhronosGroup/EGL-Registry | Headers OCCT is built against. |
| System OpenGL (vcpkg meta-port) | `opengl` | system | `LicenseRef-system-library` | https://github.com/microsoft/vcpkg/tree/master/ports/opengl | Points OCCT's build at the operating system's OpenGL library. Nothing is built or shipped. |
| System C library stand-ins (vcpkg stub ports) | `pthread`, `pthreads`, `dirent`, `libiconv` | system | `LicenseRef-system-library` | https://github.com/microsoft/vcpkg | Linux only. Empty ports that resolve to the system C library. Nothing is built or shipped. |
| vcpkg build helper ports | `vcpkg-*` | baseline | `MIT` | https://github.com/microsoft/vcpkg | Build tool only: CMake, Make and Meson helper scripts vcpkg uses while building ports. Not shipped. |

## Planned, not yet in `vcpkg.json` or the tree

These come in with the task that first needs them (see `PLAN.md` section 2.1). They are listed here for the
record only; add a row to the table above in the same change that adds the dependency.

| Component | Planned version | Licence (SPDX) | Needed by |
|-|-|-|-|
| Clipper2 | 2.0.1 | `BSL-1.0` | `cam` |
| nlohmann/json | current vcpkg | `MIT` | plain-text formats |
| Inter, JetBrains Mono (fonts) | to be pinned | `OFL-1.1` | `ui` |

## Original assets of the project

`resources/fonts/raycad-single.rsf` ("RayCAD Single Line", the engraving font of the engrave operation, task T-056) is
original work drawn for RayCAD and is under the project's own licence, LGPL-2.1-or-later. It contains no glyph data
from any other font, CAD or CAM product, or the Hershey sets; `resources/fonts/README.md` describes it.

`resources/library/fixtures/*.step` (a generic 6 inch class milling vise and a pair of parallels, the fixture models of
task T-077) are original shapes built in code with RayCAD's own kernel from round numbers of the size class
(`tests/unit/cam/fixture_models.h`, written by the test `ray_cam_fixture_tests`), under the project's own licence,
LGPL-2.1-or-later. No vendor's model, drawing, dimension table, name or trade dress was used;
`resources/library/fixtures/README.md` says how they were made.

## Reference data

`resources/standards/threads.json` (the thread table of the hole feature, task T-097) is generated by
`tools/gen_thread_table.py` from published size and pitch lists and stated formulas; it copies no table from a
handbook, a paid standard or a vendor. The size lists were taken from these public summaries (facts, cited, not
reproduced text): Wikipedia, "ISO metric screw thread", "Unified Thread Standard", "Drill bit sizes" and "Tap and
die". See `resources/standards/README.md` for the formulas and the rounding rule.
