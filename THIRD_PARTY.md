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
| Open CASCADE Technology (OCCT) | `opencascade` | 8.0.1 | `LGPL-2.1-only WITH OCCT-exception-1.0` | https://github.com/Open-Cascade-SAS/OCCT | Dynamically linked, from `src/kernel` only. Ship the licence text, offer the source, allow relinking. |
| Qt (Core, Gui, Qml, Quick and the Qt libraries they load) | - | 6.12.0 | `LGPL-3.0-only` | https://download.qt.io/official_releases/qt/6.12/ | Dynamically linked, from `src/ui` and `src/render` only. Prebuilt binaries, not built by vcpkg. Ship the licence text and notice, offer the source, allow replacing the Qt libraries. |
| Qt build tools (moc, rcc, qmlcachegen, qmltyperegistrar) | - | 6.12.0 | `GPL-3.0-only WITH Qt-GPL-exception-1.0` | https://download.qt.io/official_releases/qt/6.12/ | Build tool only: run at build time; their output is not covered by the GPL. Not shipped. |
| tl::expected | `tl-expected` | 1.3.1 | `CC0-1.0` | https://github.com/TartanLlama/expected | Header-only; `ray::Result` in `include/ray/core.h`. |
| Catch2 | `catch2` | 3.16.1 | `BSL-1.0 AND BSD-3-Clause` | https://github.com/catchorg/Catch2 | Tests only. Not shipped. |
| Boost (Graph, Math, and the Boost libraries those two depend on) | `boost-*` | 1.92.0 | `BSL-1.0` | https://www.boost.org/ | Headers, for the PlaneGCS sketch solver. In the manifest since T-001; nothing includes or links it yet. |
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
| oneTBB | current vcpkg | `Apache-2.0` | `core` job system (T-003) |
| BLAKE3 | current vcpkg | `CC0-1.0 OR Apache-2.0` | `core` hashing (T-002) |
| musl libm (vendored as `third_party/dmath`) | to be pinned | `MIT` | `core` dmath (T-002) |
| Eigen | 3.4.0 | `MPL-2.0` | `sketch` (PlaneGCS) |
| PlaneGCS (vendored from FreeCAD as `third_party/planegcs`) | FreeCAD 1.1.4 | `LGPL-2.1-or-later` | `sketch` |
| FreeCAD `Adaptive.cpp` (adapted in `src/cam/ops/adaptive2d`) | FreeCAD main, 56062b61 or later | `LGPL-2.1-or-later` | `cam` |
| Clipper2 | 2.0.1 | `BSL-1.0` | `cam` |
| SQLite | current vcpkg | `blessing` | `store` |
| Zstandard | current vcpkg | `BSD-3-Clause` | `store` |
| nlohmann/json | current vcpkg | `MIT` | plain-text formats |
| QuickJS-NG | 0.18 | `MIT` | `post` |
| Inter, JetBrains Mono (fonts) | to be pinned | `OFL-1.1` | `ui` |
