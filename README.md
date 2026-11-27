# RayCAD

RayCAD is an open-source desktop CAD/CAM application for machine shops: import a STEP file, repair it
with direct editing, program 2.5D milling, simulate, and post G-code. It is written in C++20 on Qt 6.12
(Qt Quick) and OpenCASCADE 8.0.1.

**Status: pre-release.** There has been no release yet, and the programs RayCAD writes have not been run on
a real machine. Read "Status and safety" below before you send a program to a machine. `PLAN.md` is the
technical plan (its section 12 lists the milestones; M1, M7, M2 and M3 are done); `BRIEF.md` is the original
brief.

## Status and safety

RayCAD is pre-release software. The post processors shipped with it (generic ISO, FANUC, Haas NGC, Haas
classic, Mazak Smooth and Mazak Matrix) have been checked only by the project's own G-code interpreter, which
reads every test program back and compares it with the toolpath it came from, and against the control
documentation cited in `docs/post_review`. None of them has been proven on a real machine, and the machine
definitions are generic ones taken from published specifications. The post dialog says so each time.

Before you cut with a program from RayCAD:

- dry-run it first: single block, with the feed hold in reach, above the part or with no tool in the spindle;
- check the work offsets, the tool lengths and what the tool changes do on your machine;
- read the program against your control's manual, as you would any program from a new post.

If you run one of these controls and can review a post or dry-run its programs, `docs/post_review/README.md`
says how to report what you find.

RayCAD comes with no warranty of any kind, to the extent the law allows; see sections 15 and 16 of the licence
(`LICENSE`).

## What works today

- **Open and look.** STEP and IGES import with progress and cancel, orbit, selection, measuring, sections, light
  and dark themes; documents saved as `.raycad` files, with recovery after a crash.
- **Model.** Sketches with constraints and dimensions (DXF import), extrude, cut, revolve, fillet, chamfer and
  hole features with named parameters, a feature tree with rollback, STEP export.
- **CAM.** Setups with stock and a work offset; a tool, holder, material and machine library; hole recognition;
  the operation panel, built for every operation type from its parameter schema, with a live toolpath preview;
  feeds and speeds calculated from the tool's presets for the material; shop templates; a cycle-time estimate;
  backplot; posting through the six shipped posts or your own, and a G-code viewer linked to the toolpath.

The nine operation types (`docs/operation_types.md` says how a type is written):

| Type | What it does |
|-|-|
| `drill` | Spot drilling, drilling, peck drilling, chip breaking, tapping, boring and reaming as canned cycles, at recognised holes |
| `face` | Faces the whole top of the stock flat with parallel strokes, zigzag or one way, at one or more depths |
| `contour2d` | Runs the tool beside a chain of picked edges at one or more depths, with leads, roughing and finish passes, tabs and cutter compensation |
| `pocket2d` | Clears the area above picked pocket floors with offset rings, islands left standing, entering by a helix, a ramp or a pre-drilled hole |
| `adaptive2d` | Roughs the area above picked floors with passes that hold the tool's radial load near a set value (adaptive clearing), entering by a helix, a ramp, a pre-drilled hole or from outside the stock, at one or more depths |
| `slot` | Cuts a slot along its centre line with an end mill as wide as the slot, ramping into each depth |
| `chamfer2d` | Chamfers or deburrs the top edges of walls with the flank of a chamfer mill or a spot drill |
| `engrave` | Engraves along picked edges and a line of text in a single-line font, at a depth below a plane |
| `threadmill` | Mills internal and external threads with a helix, from the hole's thread or entered values |

## Known limits

- The shipped posts have not been proven on a real machine (above), and the cycle-time estimate assumes
  accelerations, as no measured data is behind it yet.
- `pocket2d` takes the pocket's walls as vertical: drafted or undercut walls are not followed.
- `adaptive2d` takes the walls as vertical too; its passes are not all climb and it has no control of the smallest
  radius its passes turn at (its engine has none).
- `chamfer2d` and `engrave` take edges only; `contour2d` takes edges and planar faces.
- CAM geometry is picked on the model's faces and edges; it cannot reference a sketch.
- Each operation starts from the setup's stock: material removed by earlier operations is not carried to the next
  one (there is no stock simulation yet), so a later operation may cut air where an earlier one has been.
- macOS has never been built; the `macos-arm64` preset is untested. Windows and Linux are built and tested.

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
on `PATH`; CTest sets this for the tests. A test executable run directly without it fails every case that
reaches the kernel with "structured exception 0xc06d007e (an OpenCASCADE DLL or one of its functions could not
be loaded)", and Catch2 exits with the number of failed cases (`ray_cam_engine_tests.exe` in Debug: 9). That is
expected; run it through `ctest -R <name>` or put that folder on `PATH` first.

To check a change, run `tools\verify.cmd [quick|full|ci]` with `VCPKG_ROOT` and `QT_ROOT_DIR` set (and
`VCPKG_DEFAULT_BINARY_CACHE` if you use one); "Which tests to run" below says which mode when. `quick` (the
default) configures `windows-msvc`, builds Release and runs the quick test tier; `full` runs every test after
the Release build; `ci` does what CI does on Windows: vcpkg install, then Debug with the quick tier (less the
`debug-heavy` tests), then Release with every test. It stops at the first step that fails (the exit code says which).
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

`ctest` runs everything: the unit, golden and property tests of every module, the G-code round trip of
every shipped post, the tests that drive the desktop window, a standalone-compile test for every public
header, the module link-graph test, the OpenCASCADE smoke test (it calls into every toolkit the kernel
links, and on Windows also proves they are delay-loaded), the two application smoke tests, and the lint
tools with their self-tests.

### Which tests to run

Every test has one of two CTest labels (`tests/tiers.cmake`):

- `quick`: everything that guards ordinary changes, including the S4 naming scenarios, the naming
  acceptance and regression replays and a smoke block of the randomised naming sequences. A few minutes in
  Release. Run it on **every merge**: `tools\verify.cmd` (or `tools/verify.sh`), or
  `ctest --preset <config>-quick`.
- `slow`: the full randomised naming block, its six holdout blocks, the direct-edit blocks, the other-session replay,
  the sketch drag agreement sweep and the other long suites `tests/tiers.cmake` lists. CI runs them only nightly, so
  run every test (`tools\verify.cmd full`, `ctest --preset <config>`) **before a push**, and **after any change in
  `src/naming`, `src/kernel`, `ports/` or the naming oracle** (`tests/naming`).

CI runs the quick tier on every push and pull request (`build-test.yml`: Debug and Release on Windows, Linux and
macOS; `sanitizers.yml`: under AddressSanitizer), and the slow tier once a day (`nightly.yml`, at 06:23 UTC, skipped
when `main` has no new commit since the last scheduled run; also started by hand from the Actions tab, optionally on
another ref): per OS a Release job for the slow tests, one for the naming blocks (label `naming-blocks`) and one for
the direct-edit blocks (`directedit-blocks`), and the same three under AddressSanitizer, there all but the few
labelled `sanitizer-heavy` (too slow under ASan for the runner). Every test runs in exactly one of the two. A
milestone closes only after one manual run of `nightly.yml` is green on its closing commit (PLAN.md section 12).
Wrong answers of the direct-edit blocks come with an `[.evidence]` artifact per answer (`tools/ci_naming_evidence.py`).
`probe.yml` (by hand only) replays single naming sequences on a chosen platform and uploads their output, shapes and
evidence (`tools/ci_naming_probe.py` says how to write the cases). `stress.yml` (by hand only) runs one test
executable many times at once, under gdb on Linux and also under AddressSanitizer, and records every crash
(`tools/ci_stress.py`).
In an MSVC Debug build
the randomised naming blocks are disabled (they would take hours there); ctest lists them as "Not Run
(Disabled)". Debug CI also leaves out the tests labelled `debug-heavy` (the four S4 suites, the Release-scale
benchmarks and others that take 20 minutes or more each in an MSVC Debug build; `tests/tiers.cmake` lists them):
`ctest --preset <preset>-debug-ci`; Release CI runs them (`<preset>-release-quick-ci`; the nightly jobs use
`-release-slow-ci`, `-release-naming-ci` and `-release-directedit-ci`). No CI job runs the tests labelled `bench`, whose pass or
fail is a wall-clock budget (the `[wallclock]` splits and the benchmarks; the shared runners are slower than the
budgets assume); local runs (`tools\verify.cmd quick` and `full`) do. Windows CI does not run the tests that show a
window (label `desktop`): the runner's screen is smaller than their windows. Debug and Release run as separate CI jobs. Tests timed against the wall clock (cancel latencies, UI-thread stalls, start-up and drag
benchmarks) run alone. Tests that show a window take turns (the `desktop` resource lock: windows on one
desktop share the keyboard focus, the pointer and the screen), and a test that sets the system clipboard
holds the `clipboard` lock. The tests that show a window are also labelled `desktop`: `ctest --preset <config>-quick
-L desktop` runs just them, `-LE desktop` everything else.

To keep those windows off your primary monitor while you work, set `RAY_TEST_SCREEN` to the name of another
screen before running ctest or `tools\verify.cmd`: on Windows its display device name, such as `DISPLAY3`
(`\\.\DISPLAY3`, any case), elsewhere Qt's name of it (the output name, such as `DP-1`). Every window the tests
create, popups, dialogs and the `raycad` the start-up tests launch included, is then laid out on that screen before
it is first shown, and a test fails if any window of it touches the primary screen; each test process prints a line
(`RAY_TEST_SCREEN: windows on ...`) saying how many windows it showed. A name is used rather than a number because it
stays with the same monitor output whatever order the screens are listed in. A value that names no screen, or the
primary one, stops each window test at once with a list of the screens. Unset or empty, nothing changes (CI leaves
it unset). It does not stop a test window from taking the keyboard focus when it opens, and the pointer still
matters: pick a screen the pointer is not resting on, since a test window that opens or grows under it gets real
hover events (the viewport then picks under it, which the glass resize test counts as leaked GPU resources).
`tests/ui/screen/test_screen.h` says how it works.

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
