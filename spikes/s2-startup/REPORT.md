# S2 Cold start: report

Date: 2026-11-17. Status: **warm start measured; cold start and HDD not measured** (see "Not measured").

## Question

How long does a Qt Quick shell that links OpenCASCADE take from process start to its first frame,
and what reduces it: delay-loading the OCCT DLLs, compiling QML ahead of time, or something else?

## Short answer

- Warm start to first frame on this High-tier machine: **about 0.7 s median** (0.69 to 0.77 s in the two
  cleanest runs of 20, worst single run 0.77 to 1.13 s), for every build variant.
- **Neither delay-loading OCCT nor ahead-of-time QML changes the warm number by more than the noise** (about 5 ms and
  0 to 25 ms). Loading 27 OCCT DLLs from the file cache costs about 5 ms.
- The time is in Qt Quick itself: about 0.3 s to bring up an empty window with Direct3D 11, and about 0.3 s to
  create the QML user interface. One panel (the model tree side panel) accounted for about 140 ms.
- Delay-loading does help the first launch from a newly copied folder: time to `main()` 366 ms against 533 ms
  (medians of 5). It should help a cold start more; that is not measured.
- `qmltc` does not build with Qt Quick Controls in Qt 6.12.0. Not usable.
- Cold start after a reboot was not measured (no reboot allowed). No HDD exists in this machine. One command
  for Jake to run after a reboot is given below; the files it needs are already prepared.

## Machine

Core i9-13900K (24 cores), 64 GB RAM, RTX 3090 (2560x1440 at 143 Hz, driver 32.0.16.1692) and Intel UHD 770,
Windows 11 Home 10.0.26200, power plan "High performance", Microsoft Defender real-time protection on.
**High tier.** Any Low-tier figure in this report is an estimate and is labelled.

Drives: `C:` and `M:` are both Samsung SSD 990 PRO 2TB, NVMe (read with `Get-PhysicalDisk`). The repo and the
deploy folder are on `M:`. **There is no HDD in the machine**, so no HDD number exists.

**Load during measurement.** Other spikes ran on the same machine at the same time. Total CPU load from other
processes was 20 to 99 % during the runs (recorded per run below). Variants were interleaved round-robin so
the load hits all variants alike, but absolute numbers are higher and noisier than on an idle machine.
Differences under about 30 ms between variants are not meaningful.

Versions: Qt 6.12.0 msvc2022_64 (download.qt.io), OpenCASCADE 8.0.1 (vcpkg x64-windows, release), MSVC 14.51,
Release build, `/OPT:REF /OPT:ICF`. No third-party source was fetched.

## Method

Code: `src/main.cpp`, `src/occt_touch.cpp`, `qml/` (34 QML files), `CMakeLists.txt`, `bench.ps1`.

- **Shell.** `ApplicationWindow` 1600x900 with a menu bar (5 menus, about 45 actions), a ribbon toolbar with 5 tabs,
  a left side panel (model tree of 23 rows, operations list), a properties panel (4 sections, 15 editors),
  a viewport placeholder with a view cube and a floating toolbar, a start screen, a timeline bar, a status bar and
  a command palette popup. 34 QML components, Qt Quick Controls "Basic" style, Qt Quick Layouts.
- **OCCT.** `occtTouch()` calls into TKernel, TKMath, TKG2d, TKG3d, TKGeomBase, TKBRep, TKGeomAlgo, TKTopAlgo, TKPrim,
  TKBO, TKBool, TKFillet, TKOffset, TKShHealing, TKMesh, TKLCAF, TKCAF, TKXCAF, TKXSBase, TKDE, TKDESTEP, TKDEIGES
  (box, prism, fuse, cut, fillet, offset, heal, mesh, XCAF document, STEP and IGES readers and writers).
  It is reachable at run time (`--use-occt`), so the linker keeps the imports. The process counts its loaded
  modules: 27 `TK*.dll` are loaded (the 22 named plus TKCDF, TKVCAF, TKV3d, TKService, TKHLR, pulled in by TKXCAF).
- **Variants.** `shell_base` (OCCT as normal imports), `shell_delay` (same code, `/DELAYLOAD` for the 22 toolkits),
  `shell_noocct` (no OCCT, as a floor). QML is either the compiled-in module from `qt_add_qml_module`
  (qmlcachegen, "aot") or loose `.qml` files (`--loose`), with `QML_DISABLE_DISK_CACHE=1` ("nocache") or with
  the `.qmlc` disk cache warm ("qmlc").
- **Clock.** Milliseconds since the OS process creation time (`GetProcessTimes`) read with
  `GetSystemTimePreciseAsFileTime`. Stamps: `main()` entry, `QGuiApplication` constructed, QML loaded
  (root object created, window shown), first `QQuickWindow::frameSwapped`. The process then quits.
- **Deploy.** `build/deploy`: `windeployqt --release --no-compiler-runtime --no-translations --qmldir qml` plus the
  OCCT DLLs and their dependencies found with `file(GET_RUNTIME_DEPENDENCIES)`. All runs start from this folder.
- **Runs.** `bench.ps1`: 3 discarded warm-up rounds, then 20 rounds, each round runs every variant once.
  Diagnostic sets use 12 rounds. Raw data and summaries are in `results/`.
- Qt Quick backend in all runs unless stated: Direct3D 11 on the RTX 3090, threaded render loop.

Deviations from a shipped app, all deliberate:
- The QML disk cache is redirected to `build/s2cache` and the RHI shader disk cache is off
  (`Qt::AA_DisableShaderDiskCache`), because Qt otherwise creates `%LOCALAPPDATA%\<exe name>\cache`, which is
  outside the spike folder. With Direct3D 11 Qt wrote nothing into that cache in 250 runs before it was turned off.
- The VC++ runtime is taken from Windows, not shipped in the folder.
- SQLite is not linked. It is not in the vcpkg install on this machine and the plan links it statically, which
  adds no DLL. Clipper2, zstd, PlaneGCS and the other planned libraries are also not linked (not installed).

## Numbers

All times are ms from process creation to the first frame swapped, warm file cache.

### 1 to 4. Baseline, delay-load, QML ahead of time, combinations (20 runs each)

Run "run2": normal priority, CPU load from other processes 58 % at the start, 34 % at the end. This is the
quietest run.

| Variant | OCCT | QML | median | worst | best | `main()` at | TK DLLs loaded at first frame |
|-|-|-|-|-|-|-|-|
| base_aot (baseline) | imports | compiled in | 715 | 959 | 650 | 20.0 | 27 |
| delay_aot | delay-load | compiled in | 687 | 833 | 648 | 15.2 | 0 |
| noocct_aot | none | compiled in | 689 | 999 | 655 | 14.5 | 0 |
| base_loose_nocache | imports | loose, cache off | 715 | 810 | 678 | 19.7 | 27 |
| delay_loose_nocache | delay-load | loose, cache off | 700 | 769 | 666 | 14.9 | 0 |
| base_loose_qmlc | imports | loose, `.qmlc` warm | 703 | 975 | 662 | 18.7 | 27 |
| delay_loose_qmlc | delay-load | loose, `.qmlc` warm | 696 | 766 | 614 | 13.4 | 0 |
| base_aot_nocache | imports | compiled in, cache off | 738 | 894 | 677 | 18.2 | 27 |

Run "hp": the shell raises itself to High priority class at `main()` (`bench.ps1 -HighPriority`) to reduce the
effect of the other jobs. CPU load from other processes 90 % at the start, 77 % at the end.

| Variant | median | worst | best | `main()` at |
|-|-|-|-|-|
| base_aot (baseline) | 742 | 1010 | 635 | 21.3 |
| delay_aot | 728 | 1048 | 640 | 16.1 |
| noocct_aot | 728 | 1132 | 602 | 17.2 |
| base_loose_nocache | 752 | 1092 | 678 | 22.4 |
| delay_loose_nocache | 743 | 990 | 653 | 15.6 |
| base_loose_qmlc | 772 | 1050 | 661 | 20.3 |
| delay_loose_qmlc | 737 | 1017 | 653 | 14.6 |
| base_aot_nocache | 767 | 1056 | 670 | 20.9 |

Two more runs at normal priority under heavier load are in `results/`: "run1" (about 55 % load) medians 739 to
830, worst 1240; "run3" (70 to 91 % load) medians 777 to 968, worst 4957. A set run at 99 % load gave medians
of 0.66 to 1.27 s and a worst of 3967 (`results/loaded99pct-parts-*`).

Reading:
- Delay-load against baseline: 28 ms (run2) and 14 ms (hp) lower median, of which about 5 ms is visible at
  `main()`. The rest is inside the noise.
- QML compiled in against loose files with the cache off: 0 and 13 ms (run2), 10 and 15 ms (hp). Inside the noise.
- Best case of the combination (delay-load plus compiled-in QML) against the worst case (imports plus loose QML,
  cache off): 28 ms (run2), 24 ms (hp).
- `QML_DISABLE_DISK_CACHE=1` on the compiled-in build cost 23 to 25 ms. Whether that variable also turns off Qt's own
  precompiled Controls QML was not verified (Qt's logging categories gave no output in this Qt build).

### Time attributable to OCCT DLL loading

| Situation | How measured | Result |
|-|-|-|
| Warm, at start | `main()` entry, base against delay, medians of 20 | 4.8 ms (run2), 5.2 ms (hp), 3.8 ms (run1) |
| Warm, at first use when delay-loaded | time of `occtTouch()` after the first frame, delay against base | 40.6 against 29.2 ms (run2), 41.8 against 29.8 ms (hp): about 12 ms |
| First launch from a newly copied folder | `main()` entry, base against delay, medians of 5 | 533 against 366 ms: about 170 ms (noisy, see below) |
| Cold after reboot | not measured | |

27 OCCT DLLs, 43.9 MB on disk. With delay-load the process has 129 modules at first frame instead of 163.
Linking all 22 toolkits with `/DELAYLOAD` produced no linker error (no data imports) and `occtTouch()` returned
the same result (1693) in both builds.

### First launch from a newly copied folder (not a cold start)

`bench.ps1 -Fresh 5`: copy the deploy folder to a new path, launch once, launch again. 5 copies per variant,
High priority, CPU load 53 to 96 %. The copy leaves the file data in the Windows file cache, so this is **not**
a cold start and must not be read as one. It shows the first launch after an install or update: Windows maps
each DLL as an image for the first time and Defender scans binaries it has not seen.

| Variant | launch | median | worst | best | `main()` median |
|-|-|-|-|-|-|
| base_aot | first | 1954 | 3145 | 1594 | 533 |
| base_aot | second | 1017 | 1552 | 786 | 28 |
| delay_aot | first | 2063 | 2354 | 1367 | 366 |
| delay_aot | second | 1090 | 1878 | 780 | 22 |

The first-frame medians do not separate the two variants at n=5 under this load. The `main()` stamp does.

### Where the warm time goes

Stage stamps, delay_aot, run2 medians: `main()` at 15 ms, `QGuiApplication` ready at 45 ms, QML loaded and
window shown at 683 ms, first frame swapped at 687 ms. So about 640 ms is between "application object ready"
and "QML loaded".

Diagnostic variants of delay_aot, three runs (run A 5 runs per variant, runs B and C 12; run C at High
priority under 53 to 96 % load):

| Variant | median run A | median run B | median run C (hp) |
|-|-|-|-|
| Direct3D 11 (default) | 657 | 683 | 760 |
| Empty `Window`, no content | 316 | 329 | 366 |
| Qt Quick software backend | 546 | 588 | 675 |
| Direct3D 11 on WARP (`QSG_RHI_PREFER_SOFTWARE_RENDERER=1`) | 559 | 585 | 680 |
| `QSG_RENDER_LOOP=basic` | 677 | 696 | 837 |
| Direct3D 12 | 807 | 857 | 971 |
| OpenGL | 797 | 875 | 978 |
| Vulkan | 944 | 934 | 1071 |

Removing one part of the UI at a time (loose QML, `.qmlc` warm, 12 runs, High priority, load 80 to 96 %,
`results/warm-parts-hp-*`):

| Variant | median |
|-|-|
| full UI | 655 |
| without menu bar | 684 |
| without command palette | 705 |
| without ribbon toolbar | 702 |
| without properties panel | 707 |
| without timeline | 644 |
| without start screen | 706 |
| without side panel (model tree, operations list) | 516 |
| viewport only (window, split view, viewport) | 450 |

Reading:
- An empty Qt Quick window needs 316 to 366 ms. That part is Qt, the window and the graphics device, not our QML.
- The full UI adds about 330 ms on top. A window with only the viewport adds about 100 to 130 ms.
- The side panel alone is worth about 140 ms. The other parts, removed one at a time, do not show a saving
  outside the noise (about 50 ms in this set). Why the side panel is expensive was not investigated.
- Direct3D 11 is the fastest hardware backend to start: Direct3D 12 adds 150 to 210 ms, OpenGL 115 to 220 ms,
  Vulkan 250 to 310 ms. Creating the Direct3D 11 device on the RTX 3090 costs about 75 to 110 ms more than WARP.

### Deployed size

`build/deploy` for one executable (without the two extra variant executables and the loose QML copy):

| | Files | Size |
|-|-|-|
| Total | 1409 | 132.3 MB |
| All DLLs | 115 | 127.9 MB |
| Qt6*.dll in the root | 31 | 58.8 MB |
| Qt plugin and QML plugin DLLs in subfolders | 48 | 4.2 MB |
| OCCT TK*.dll | 27 | 43.9 MB |
| OCCT dependencies (freetype, libpng16, libz, bz2, brotlicommon, brotlidec) | 6 | 1.2 MB |
| d3dcompiler_47.dll, dxcompiler.dll, dxil.dll | 3 | 19.6 MB |

Not needed by this shell and removable, not tested after removal: the Controls styles other than Basic
(27 DLLs, 18.1 MB) and `dxcompiler.dll` plus `dxil.dll` (15.1 MB, Direct3D 12 only).
TKXCAF pulls in the visualisation toolkits TKV3d, TKService, TKVCAF, TKHLR and freetype (5.3 MB with TKCDF)
although the product draws with its own renderer.

### qmltc

`cmake -DS2_QMLTC=ON` (`qmltc/CMakeLists.txt`) fails: 533 error lines, summary in `results/qmltc_build_errors.txt`.
The causes are "QtQuick.Controls uses optional imports which are not supported" and "Can't compile the QML base
type "Button" to C++ because it cannot be resolved" (the same for every Controls type). It was not tried with a
UI written only on `QtQuick.Templates`.

## Not measured

- **Cold start after a reboot.** Not measured: no reboot, no admin rights, no system changes were allowed, and
  Windows has no way to empty the file cache without admin rights. Copying the deploy folder to a new path does
  not make it cold (the copy itself fills the cache), so no approximation is offered as a cold number.
- **HDD, cold or warm.** No HDD exists in this machine.
- **Low tier and Medium tier.** No such machine was available.
- macOS and Linux.
- A lazy-loading version of the UI (`Loader`), a pruned deploy folder, SQLite and the other planned libraries.
- An idle machine. All runs had 20 to 99 % CPU load from other spikes.
- The product's own start-up work (settings, tool library, recent files, the real viewport item).

### Command for Jake after a reboot

Five private copies of the deploy folder are already prepared in `build\cold` (678 MB), one per variant, so each
variant reads its own Qt and OCCT files cold. Do not rebuild the spike before this (if you do, run
`bench.ps1 -PrepareCold` once before rebooting). Reboot, log in, wait two minutes, start nothing else, then run:

    powershell -ExecutionPolicy Bypass -File M:\RayCAD\spikes\s2-startup\bench.ps1 -Cold

It opens the shell window ten times (each closes by itself), prints a cold and a second-launch time per variant
and writes `results\cold-<time>.csv`. The first variant (delay_aot) is the true cold start, including cold
Windows system DLLs. The later variants are cold for their own files only: base_aot minus noocct_aot gives the
cold cost of the OCCT DLLs. One reboot gives one sample per variant; repeat for more. For an HDD number, copy
`spikes\s2-startup` to an HDD, run `bench.ps1 -PrepareCold` there, reboot, and run the same command from that path.

## Recommendation

### Budget for PLAN.md section 9

Replace the single "Cold start" row with two rows, both measured from OS process creation to the first frame
swapped, median of 10 launches after 2 discarded ones.

| Metric | Low | Medium | High | Basis |
|-|-|-|-|-|
| Warm start to interactive start screen | 2.5 s [estimate] | 1.5 s [estimate] | 1.0 s | Measured 0.69 to 0.77 s on High for a UI skeleton with no product start-up work, under load. Low and Medium are not measured: scaled by a guessed 2.5x and 1.5x slower single thread plus slower GPU device creation. |
| Cold start (first launch after boot) | 3.0 s [guess] | 2.0 s [guess] | 1.5 s [guess] | Unchanged and still unverified. Run the command above to replace the High guess with a number. |

The first launch after an install or update took 1.4 to 3.1 s on High. Do not gate CI on it.

The 3.0 s Low cold target stays a guess. An estimate, not a measurement: a Low machine with an HDD must read about
40 MB of Qt DLLs and, without delay-load, 44 MB of OCCT DLLs before the first frame; at typical HDD rates that
is 1 to 3 s of reading alone, so 3.0 s on an HDD is unlikely without delay-loading OCCT, and may not be reachable
with it.

### Build settings

1. **Delay-load every OCCT toolkit** (`/DELAYLOAD:TK*.dll` plus `delayimp.lib` on Windows). It costs nothing warm,
   links cleanly against OCCT 8.0.1, moves about 12 ms to the first OCCT call, saved about 170 ms before `main()`
   on a first launch, and keeps 44 MB of DLLs off the cold path. Consequences to plan for: no OCCT call may run
   before the first frame (the shell must not touch `kernel` during start), and a missing DLL shows up at first
   use instead of at launch, so the installer smoke test must call into every toolkit (the spike's `--use-occt`
   does this).
2. **Keep `qt_add_qml_module` with qmlcachegen for all QML.** It is the default and costs nothing, and it removes
   the dependency on a writable disk cache. Do not expect a start-up gain from it on High: measured 0 to 25 ms.
3. **Do not plan on qmltc.** It does not build with Qt Quick Controls in Qt 6.12.0.
4. **Stay on Direct3D 11 on Windows.** Direct3D 12, OpenGL and Vulkan start 115 to 310 ms slower here. This also
   counts against fallbacks (a) and (b) of PLAN section 2 (Qt forced to OpenGL).
5. **Treat QML creation as the start-up cost to manage.** About 330 ms of the 690 ms is creating the UI. Create at
   start only what the start screen shows; load the CAM tab, inactive ribbon tabs, the properties panel content
   and popups on first use. Profile the model tree delegate early (about 140 ms in this skeleton). Not tested here.
6. **Measure from process creation, not from `main()`.** `main()` is reached 15 to 20 ms after creation warm, but
   366 to 533 ms after creation on a first launch. `--bench-startup` should use the OS process creation time and
   report the median of several launches; single launches varied by 150 to 300 ms on a loaded machine.
7. **Prune the deploy folder** (unused Controls styles, `dxcompiler.dll`, `dxil.dll`): about 33 MB of 132 MB.
   Decide whether XCAF is worth shipping TKV3d, TKService and freetype.
8. Qt writes its QML and shader caches under `%LOCALAPPDATA%\<app name>\cache` by default. The product should
   set the location on purpose (`QML_DISK_CACHE_PATH`, organisation and application name) rather than inherit it.

## Files

- `CMakeLists.txt`, `copy_deps.cmake`, `src/`, `qml/`: the shell and its deploy step. Build: `spikes\build.cmd s2-startup`.
- `bench.ps1`: harness. `-Set main|diag|parts`, `-Runs`, `-HighPriority`, `-Fresh n`, `-PrepareCold`, `-Cold`.
- `make_exp.sh`: makes the QML folders for `-Set parts`. `qml_empty/`: the empty-window diagnostic.
- `qmltc/`: the failed qmltc attempt.
- `results/`: CSV, raw lines and summaries of every run quoted above.
