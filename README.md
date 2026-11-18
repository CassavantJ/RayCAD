# RayCAD

RayCAD is an open-source desktop CAD/CAM application for machine shops: import a STEP file, repair it
with direct editing, program 2.5D milling, simulate, and post G-code. It is written in C++20 on Qt 6.12
(Qt Quick) and OpenCASCADE 8.0.1.

**Status: early development.** The repository currently holds the project skeleton only: the module
libraries are stubs, the desktop application opens an empty window, and `raycad-cli` prints its version.
`PLAN.md` is the technical plan; `BRIEF.md` is the original brief.

## Layout

| Path | What |
|-|-|
| `include/ray/` | Public module headers (the contracts between modules) |
| `src/<module>/` | One static library per module: `core`, `kernel`, `naming`, `sketch`, `lib`, `doc`, `store`, `cam`, `sim`, `post`, `api`, `render`, `ui` |
| `apps/raycad/` | Desktop executable |
| `apps/raycad-cli/` | Headless executable |
| `tests/` | Tests, run through CTest |
| `tools/` | Lint tools and build helpers (Python 3, standard library only) |
| `docs/interfaces/` | The interface drafts the headers in `include/ray/` were copied from |
| `spikes/` | Throwaway experiments; never linked into the product |

Module rules (`PLAN.md` sections 0.4 and 1.1), enforced by the lint tools: only `src/kernel` uses
OpenCASCADE; only `src/ui` and `src/render` use Qt; the geometry, CAM, simulation, post and storage modules
never link `render` or `ui`.

## Building

You need:

- CMake 3.28 or newer and Ninja
- a C++20 compiler: MSVC 2022 or newer, GCC 13 or newer, or Clang 18 or newer
- Python 3.9 or newer (lint tools and tests)
- [vcpkg](https://github.com/microsoft/vcpkg), as a git clone that contains the baseline commit named in
  `vcpkg-configuration.json`; set `VCPKG_ROOT` to it
- Qt 6.12.0 prebuilt binaries (Qt online installer or `aqtinstall`), with `QT_ROOT_DIR` set to the kit
  folder, for example `C:\Qt\6.12.0\msvc2022_64` or `~/Qt/6.12.0/gcc_64`

vcpkg runs in manifest mode: the first configure builds OpenCASCADE and the other dependencies from
`vcpkg.json`, which takes a while. Set `VCPKG_DEFAULT_BINARY_CACHE` to a folder to keep the built packages
between build directories.

### Windows

From a plain `cmd` prompt (`tools\win-env.cmd` finds Visual Studio and sets up the x64 compiler, CMake and
Ninja for that prompt):

```bat
call tools\win-env.cmd
set VCPKG_ROOT=C:\path\to\vcpkg
set QT_ROOT_DIR=C:\Qt\6.12.0\msvc2022_64
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

To run the application from the build folder, put Qt's `bin` folder on `PATH`:

```bat
set PATH=%QT_ROOT_DIR%\bin;%PATH%
build\windows-msvc\bin\Debug\raycad.exe
```

The OpenCASCADE DLLs are delay-loaded and are not copied next to the executables. Code that calls the
kernel also needs `build\vcpkg_installed\x64-windows\bin` (Release) or `...\x64-windows\debug\bin` (Debug)
on `PATH`; CTest sets this for the tests.

### Linux and macOS

```sh
export VCPKG_ROOT=/path/to/vcpkg
export QT_ROOT_DIR=$HOME/Qt/6.12.0/gcc_64        # macOS: $HOME/Qt/6.12.0/macos
cmake --preset linux-gcc                          # or linux-clang, macos-arm64
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

On Linux, vcpkg needs the usual build tools and the OpenGL and X11 development packages to build
OpenCASCADE; `.github/workflows/build-test.yml` lists the Ubuntu packages CI installs.

### Presets

Configure presets: `windows-msvc`, `linux-gcc`, `linux-clang`, `macos-arm64` (each with `-debug`,
`-relwithdebinfo` and `-release` build and test presets), plus `linux-clang-asan` (AddressSanitizer and
UndefinedBehaviorSanitizer) and `linux-clang-tsan` (ThreadSanitizer). Build output goes to
`build/<preset>/`.

`linux-clang-tsan` uses the overlay triplet `triplets/x64-linux-dynamic-tsan.cmake`, which builds oneTBB
with Clang and ThreadSanitizer so that TSan sees the synchronisation inside the job system's thread pool.
Its dependencies are installed separately, in `build/vcpkg_installed-tsan/`, and the first configure builds
all of them once for that triplet (release only; about 25 minutes on 32 hardware threads, then they come
from the vcpkg binary cache). Its tests include `ray_core_jobs_tsan_selftest`, which passes only if TSan
reports a deliberate race in a `parallelChunks` body.

## Tests and lints

`ctest` runs everything: a standalone-compile test for every public header, the module link-graph test,
the OpenCASCADE smoke test (it calls into every toolkit the kernel links, and on Windows also proves
they are delay-loaded), the two application smoke tests, and the lint tools with their self-tests.

The lints can also be run on their own, without building:

```sh
python tools/run_lints.py
```

| Tool | Rule |
|-|-|
| `tools/lint_includes.py` | Only `src/kernel` includes OCCT headers; only `src/ui` and `src/render` include Qt headers; a module includes only the headers of modules it may depend on |
| `tools/lint_cmake_deps.py` | The CMake link graph follows `PLAN.md` section 1.1; `cam`, `post`, `sim`, `kernel`, `naming`, `doc`, `store` never reach `render` or `ui` |
| `tools/lint_dmath.py` | Deterministic modules call `ray::dmath`, never the platform's trigonometric and exponential functions |
| `tools/lint_colours.py` | No colour literals outside `resources/themes/` |
| `tools/lint_licences.py` | `THIRD_PARTY.md` lists every vcpkg dependency; nothing is GPL-only |
| `tools/check_format.py` | clang-format 22 on `src/`, `apps/`, `tests/` |

## Licence

RayCAD is free software under the GNU Lesser General Public License, version 2.1 or (at your option) any
later version. See `LICENSE`. Third-party components and their licences are listed in `THIRD_PARTY.md`.
Contributions are accepted under the Developer Certificate of Origin; see `CONTRIBUTING.md`.
