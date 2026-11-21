# planegcs: FreeCAD's 2D constraint solver, vendored

The solver behind `ray::sketch::makePlaneGcsSolver` (`src/sketch`, task T-090). Spike S7 (`spikes/s7-planegcs/REPORT.md`)
showed it builds unmodified outside FreeCAD with a small shim; spike S11 (`spikes/s11-sketch-drag/`) made the drag step
fast without changing it.

## Origin

- Project: FreeCAD, https://github.com/FreeCAD/FreeCAD, directory `src/Mod/Sketcher/App/planegcs`.
- Version: tag `1.1.4`, commit `4fd3bf320d9566a27e60069fc8387448aaa3a094` (2026-09-28), the same as spike S7.
- Licence: the file headers say GNU Library General Public License, version 2 or (at your option) any later version
  (`LGPL-2.1-or-later`). `LICENSE` is FreeCAD's licence file, unchanged (LGPL 2.1 text, CRLF line endings as upstream).
- Fetched with `git clone --filter=blob:none --sparse --depth 1 --branch 1.1.4` and a sparse checkout of that directory
  and `LICENSE`.

## Files

`src/` holds the 11 upstream files, **unmodified** (byte for byte; `.gitattributes` keeps git from converting line
endings): `Constraints.cpp`, `Constraints.h`, `GCS.cpp`, `GCS.h`, `Geo.cpp`, `Geo.h`, `qp_eq.cpp`, `qp_eq.h`,
`SubSystem.cpp`, `SubSystem.h`, `Util.h`. `sync_upstream.py` checks them against the upstream git blob ids (no
network), or fetches the tag again and compares or copies.

`shim/include/` is spike S7's shim, unchanged: four small headers standing in for the FreeCAD headers PlaneGCS includes.

| FreeCAD header | Shim |
|-|-|
| `<Base/Console.h>` | `Base::Console().log/warning(fmt, ...)`; messages are dropped unless a sink function is set |
| `<FCConfig.h>` | empty; nothing from it is used |
| `"../../SketcherGlobal.h"` | defines `SketcherExport` as empty (static library). Found without editing the sources because `shim/include/_up/_up` is on the include path, so the relative path resolves to `shim/include/SketcherGlobal.h`; that folder holds only a placeholder file |
| `<boost_graph_adjacency_list.hpp>` | includes `<boost/graph/adjacency_list.hpp>` |

## Dependencies

Both come from vcpkg (`vcpkg.json`), header-only; nothing of either is committed here.

- **Eigen 5.0.1** (port `eigen3`, the version of the pinned vcpkg baseline), `MPL-2.0`. FreeCAD 1.1.4 pins Eigen 3.4.0 and
  spikes S7 and S11 used 3.4.0. Matching that pin needs a vcpkg version override, and vcpkg resolves an override from
  the git history of its registry, which the shallow vcpkg clones of this project's CI and build scripts do not have.
  PlaneGCS 1.1.4 builds and passes every sketch test with 5.0.1 unchanged (its version macros still read 3.5.0, so it
  takes its Eigen >= 3.2.2 code paths). In Eigen 5.0.1 every module, `SparseCholesky` included, is under MPL-2.0.
- **Boost 1.92.0** headers (ports `boost-graph` and `boost-math`, in the manifest since T-001), `BSL-1.0`: Graph's
  `adjacency_list` and `connected_components`, Math's constants. The same version S7 used. CMake target
  `Boost::headers`; `BOOST_ALL_NO_LIB` is set, so nothing is linked.

## Build

`CMakeLists.txt` defines the static library `ray_planegcs`, added from `src/sketch/CMakeLists.txt`; only `ray_sketch`
links it. Upstream code is compiled without the project's warning flags (`/W3` on MSVC, `-w` elsewhere) and with the
same floating-point flags as the deterministic modules (`/fp:strict`; `-ffp-contract=off -fno-fast-math`), so solver
and adapter form one fixed set of flags (adapter rule 3, PLAN.md section 8). `EIGEN_NO_DEBUG` is defined, as FreeCAD
does. On Linux it links `Threads::Threads` (`GCS::System::diagnose` uses `std::async`).

## Code adapted from PlaneGCS outside this folder

`src/sketch/solver_iterate.cpp` and `.h` (LGPL-2.1-or-later, upstream attribution in the file header) contain code
adapted from `GCS.cpp` and `qp_eq.cpp`: the partition of `System::initSolution`, the DogLeg loop of `System::solve_DL`
(with a cancellation check, and a sparse Jacobian and Gauss step for large components) and the drag iteration of
`System::solve(SubSystem*, SubSystem*)` with sparse linear algebra (spike S11). `src/sketch/solver_diagnose.cpp` and `.h`
(same licence and attribution) adapt `System::diagnose`, its QR decompositions and its redundancy heuristic, run per
decoupled component. Listed in `THIRD_PARTY.md`.
