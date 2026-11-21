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

To check a change, run `tools\verify.cmd [quick|full|ci]` with `VCPKG_ROOT` and `QT_ROOT_DIR` set (and
`VCPKG_DEFAULT_BINARY_CACHE` if you use one); "Which tests to run" below says which mode when. `quick` (the
default) configures `windows-msvc`, builds Release and runs the quick test tier; `full` runs every test after
the Release build; `ci` does what CI does on Windows: vcpkg install, then Debug with the quick tier, then
Release with every test. It stops at the first step that fails (the exit code says which).
`RAY_VCPKG_INSTALL_OPTIONS` passes extra options to vcpkg, such as `--x-buildtrees-root=C:\vb` when the
checkout's path is too long for OpenCASCADE's build tree. The Debug tests take several times as long as
Release ones (unoptimised OpenCASCADE); their CTest timeouts are four times longer (`tests/CMakeLists.txt`).

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

`tools/verify.sh [quick|full|ci] [preset]` is the POSIX counterpart of `tools\verify.cmd`, with the same modes
(preset `linux-gcc` or `macos-arm64` by default). `tools/linux-local.sh` builds a Windows checkout on Linux
through WSL.

### Presets

Configure presets: `windows-msvc`, `linux-gcc`, `linux-clang`, `macos-arm64` (each with `-debug`,
`-relwithdebinfo` and `-release` build and test presets), plus `linux-clang-asan` (AddressSanitizer and
UndefinedBehaviorSanitizer) and `linux-clang-tsan` (ThreadSanitizer). Build output goes to
`build/<preset>/`. Every test preset has a `-quick` twin that runs only the quick tier (for example
`windows-msvc-release-quick`); all of them run `ctest` in parallel.

Precompiled headers (`RAY_PCH`, on by default; `cmake/RayPch.cmake`) cover the standard library, Qt and
Catch2 in the Qt targets, `ray_api` and the tests. `linux-clang-asan` builds without them, so CI also compiles
every source the way it is written; configure with `-DRAY_PCH=OFF` to do the same locally.

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

### Which tests to run

Every test has one of two CTest labels (`tests/tiers.cmake`):

- `quick`: everything that guards ordinary changes, including the S4 naming scenarios, the naming
  acceptance and regression replays and a smoke block of the randomised naming sequences. A few minutes in
  Release. Run it on **every merge**: `tools\verify.cmd` (or `tools/verify.sh`), or
  `ctest --preset <config>-quick`.
- `slow`: the full randomised naming block, its six holdout blocks, the other-session replay and the sketch
  drag agreement sweep. Run every test (`tools\verify.cmd full`, `ctest --preset <config>`) **before a push**,
  and **after any change in `src/naming`, `src/kernel`, `ports/` or the naming oracle** (`tests/naming`).

CI runs the quick tier in Debug and every test in Release and under AddressSanitizer. In an MSVC Debug build
the randomised naming blocks are disabled (they would take hours there); ctest lists them as "Not Run
(Disabled)". Tests timed against the wall clock (cancel latencies, UI-thread stalls, start-up and drag
benchmarks) run alone. Tests that show a window take turns (the `desktop` resource lock: windows on one
desktop share the keyboard focus, the pointer and the screen), and a test that sets the system clipboard
holds the `clipboard` lock.

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
