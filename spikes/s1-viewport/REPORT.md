# S1 Viewport and glass panel on integrated graphics: report

Date: 2026-11-17. Spike definition: `PLAN.md` section 13, S1. Decision under test: `PLAN.md` 2.1, 2.3 and 6.

## 1. Question

Should the RayCAD viewport be

- **A**: our own renderer on Qt RHI inside a `QQuickRhiItem` (Direct3D 11 on Windows), which is the plan, or
- **B**: OCCT's `V3d_View` OpenGL viewer inside Qt Quick, with Qt Quick forced to its OpenGL backend, which is the brief's proposal?

Pass bar for A in the plan: 30 FPS or better on a UHD 620-class laptop, 500,000 triangles, 1080p, glass panels on.

## 2. Short answer

- **The data supports decision A.** Nothing measured argues for B.
- A on the integrated GPU available here (Intel UHD 770, Direct3D 11), glass on: **4.73 ms at the 95th percentile (211 FPS)**, median 3.70 ms. The budget is 33.3 ms.
- **The plan's pass bar is not confirmed, only indicated.** There was no UHD 620-class laptop. Estimate for UHD 620: 7 to 10 ms (100 to 140 FPS), see 5.2.
- B works. On the one adapter where both could be compared (RTX 3090, OpenGL), B needs 1.9 times the frame time of A with glass on and 1.5 times with glass off, at the 95th percentile.
- **B on the integrated GPU was not measured** (no way to select it for OpenGL without changing a Windows setting, see section 6).
- **A on macOS (Metal) was not measured** (no macOS machine).

## 3. Method

### 3.1 What was built

Everything is in `spikes/s1-viewport/`. No third-party source was fetched. Qt 6.12.0 (MSVC 2022 64-bit), OpenCASCADE 8.0.1 (vcpkg, `x64-windows`), MSVC from Visual Studio 2026, Release build.

| | A: `build/s1a.exe` | B: `build/s1b.exe` |
|-|-|-|
| Viewport item | `QQuickRhiItem` (`a/viewport.cpp`) | `QQuickFramebufferObject` (`b/occtview.cpp`) |
| Graphics API | Qt RHI. Direct3D 11 by default. The same code also ran on Qt's OpenGL backend (`QSG_RHI_BACKEND=opengl`) | OpenGL 4.5 core profile; the program sets `QSG_RHI_BACKEND=opengl` itself |
| Faces | One vertex buffer and one index buffer, 20 draw calls (one per body), per-pixel Blinn-Phong | One `Graphic3d_ArrayOfTriangles` with normals per body in a custom `AIS_InteractiveObject`, Phong shading model, OCCT default lights |
| Edges | One line-list buffer, 20 draw calls, faces pushed back with depth bias | One `Graphic3d_ArrayOfSegments` per body |
| Pick | ID-buffer pass on demand, 1x1 pixel read back with `QRhiReadbackDescription::setRect` | `AIS_InteractiveContext::MoveTo` with one `Select3D_SensitivePrimitiveArray` per face (CPU, BVH) |
| Glass | In the renderer: 4:1 downsample pass, then horizontal and vertical 17-tap Gaussian passes into ONE quarter-resolution texture (480x270). Exposed to QML as a texture provider. Three `ShaderEffect` panels sample it at their own screen position | In QML: `ShaderEffectSource` of the viewport at quarter resolution, ONE `MultiEffect` blur, a second `ShaderEffectSource`. The same three `ShaderEffect` panels sample that |
| Glass off | Blur passes skipped; panels are opaque rounded `Rectangle`s | Blur chain not instantiated; same `Rectangle`s |

B draws the triangles directly instead of through `AIS_Shape`, so that A and B draw exactly the same triangles. For a meshed solid, `AIS_Shape` hands the driver the same kind of arrays.

### 3.2 Scene

`common/scene.h`, used by both. 20 turned-part-like bodies (5 by 4 grid), 10 faces each (cylinders, a cone, annuli, a bore). **500,000 triangles** exactly, 301,200 vertices, 50,000 edge segments, 200 pickable faces. Perspective camera, one orbit revolution per 10 seconds. The three panels cover 32 % of the window. Screenshots: `results/a_glass.png`, `results/a_flat.png`, `results/b_glass.png`.

### 3.3 Measurement

- Machine: Core i9-13900K (24 cores), 64 GB RAM, Windows 11 Home 10.0.26200. NVIDIA RTX 3090, driver 32.0.16.1692 (OpenGL string "4.5.0 NVIDIA 616.92"). Intel UHD 770, driver 32.0.101.7088. All three monitors are connected to the RTX 3090. Primary monitor 2560x1440, 143 Hz, 150 % scaling.
- Render target: 1920x1080 physical pixels in every run (reported by the renderer, not assumed). The window is 1280x720 logical pixels at device pixel ratio 1.5.
- **Vsync was off in every run.** `QSurfaceFormat::setSwapInterval(0)`; the swap chain reported `QRhiSwapChain::NoVSync`. Frame time below is real swap-to-swap time, not a multiple of the refresh interval.
- Frame time = time between consecutive `QQuickWindow::frameSwapped` signals on the render thread, over a scripted 10-second orbit after 1.5 seconds of warm-up. Threaded render loop in every run.
- Each configuration was run 3 times. The table gives the median of the three runs and the range of the 95th percentile.
- GPU time: a separate 5-second run with `QQuickGraphicsConfiguration::setTimestamps(true)`, reading `QRhiCommandBuffer::lastCompletedGpuTime()`.
- Adapter check: every run records `QRhi::driverInfo()` (name, vendor id, device id) and the backend name. The tables use those recorded values.
- Adapter selection: `QT_D3D_ADAPTER_INDEX=0` (RTX 3090), `=1` (UHD 770), `QSG_RHI_PREFER_SOFTWARE_RENDERER=1` (WARP, reported as "Microsoft Basic Render Driver").
- Other spikes were running on the same machine and at times loaded every core. The benchmark process therefore runs at high priority, and each run records whole-machine CPU load during the orbit. During the reported runs that load was 18 to 78 % (WARP runs: 66 to 75 %, most of it WARP itself). An earlier set taken at normal priority under 100 % load is kept in `results/main_run0_machine_loaded.jsonl` and is not used.
- Reproduce: `spikes\build.cmd s1-viewport`, then in Git Bash `./measure.sh main` (three times), `./measure.sh gpu`, `./measure.sh extra`, `python summarize.py`. Raw data: `results/*.jsonl`. Generated tables: `results/summary.md`.

## 4. Results

### 4.1 Frame time, 10-second orbit, 1920x1080, 500,000 triangles, edges on, vsync off

| Variant | Adapter actually used (from QRhi) | Glass | Median ms | 95th percentile ms (range of 3 runs) | FPS at 95th percentile | Mean FPS | GPU time ms, median |
|-|-|-|-|-|-|-|-|
| A, Direct3D 11 | Intel UHD Graphics 770 (8086:a780) | on | 3.70 | **4.73** (4.69 to 4.74) | 211 | 270 | 2.40 |
| A, Direct3D 11 | Intel UHD Graphics 770 | off | 3.41 | **4.39** (3.57 to 4.40) | 228 | 293 | 2.13 |
| A, Direct3D 11 | NVIDIA RTX 3090 (10de:2204) | on | 0.15 | **0.34** (0.26 to 0.34) | 2976 | 5514 | 0.13 |
| A, Direct3D 11 | NVIDIA RTX 3090 | off | 0.14 | **0.39** (0.24 to 0.48) | 2577 | 5931 | 0.11 |
| A, Direct3D 11 | WARP, software (1414:8c) | on | 42.46 | **46.36** (45.40 to 50.47) | 22 | 23 | 42.31 |
| A, Direct3D 11 | WARP, software | off | 39.80 | **42.56** (41.99 to 42.81) | 23 | 25 | 39.75 |
| A, OpenGL | NVIDIA RTX 3090 | on | 0.21 | **0.34** (0.34 to 0.51) | 2941 | 4435 | 0.14 |
| A, OpenGL | NVIDIA RTX 3090 | off | 0.19 | **0.33** (0.32 to 0.59) | 3058 | 4901 | 0.12 |
| B, OpenGL | NVIDIA RTX 3090 | on | 0.49 | **0.64** (0.62 to 0.90) | 1570 | 2004 | 0.38 |
| B, OpenGL | NVIDIA RTX 3090 | off | 0.32 | **0.50** (0.47 to 0.67) | 2016 | 2900 | 0.27 |
| B, OpenGL | Intel UHD Graphics 770 | on, off | not measured | not measured | | | |

Reading the table:

- **Cost of glass in A**: 0.29 ms on UHD 770 (median 3.70 against 3.41), about 8 %. Between 0.01 and 0.02 ms on the RTX 3090.
- **Cost of glass in B**: 0.17 ms on the RTX 3090 (median 0.49 against 0.32). It adds about 50 % to B's frame.
- **A against B on the same adapter and the same API** (RTX 3090, OpenGL): B takes 2.3 times A's median frame time with glass on (0.49 against 0.21) and 1.7 times with glass off. At the 95th percentile: 1.9 times and 1.5 times. GPU time alone: 2.7 times and 2.3 times.
- On the RTX 3090 both variants are far inside every budget. The RTX 3090 rows say nothing about which variant survives on weak hardware.
- WARP on this 24-core desktop reaches 22 FPS. That is below the Low-tier budget of 30 FPS, and a laptop CPU has far fewer cores.
- The UHD 770 was rendering for a monitor attached to the RTX 3090, so each frame was copied between adapters. Swap-to-swap time (3.70 ms) is 1.3 ms above GPU time (2.40 ms). A laptop panel driven directly by the integrated GPU does not pay for that copy.

### 4.2 Extra runs (one 5-second run each)

| Run | Adapter | Median ms | 95th percentile ms | Mean FPS |
|-|-|-|-|-|
| A, Direct3D 11, MSAA 4x, glass on | Intel UHD 770 | 6.50 | 6.80 | 153 |
| A, Direct3D 11, MSAA 4x, glass on | NVIDIA RTX 3090 | 0.25 | 1.08 | 2657 |
| B with OCCT's own offscreen buffer plus blit (`useSystemBuffer = false`, as in the OCCT Qt samples), glass on | NVIDIA RTX 3090 | 0.52 | 0.78 | 1800 |
| Same, glass off | NVIDIA RTX 3090 | 0.34 | 0.55 | 2618 |

The main B rows use `useSystemBuffer = true` (OCCT draws straight into Qt's framebuffer object), which was the faster setting for B.

### 4.3 Other observations

| Item | A | B |
|-|-|-|
| Time to first frame: process creation to first swap, warm file cache, includes building the mesh | Direct3D 11: 338 ms on RTX 3090, 827 ms on UHD 770, 231 ms on WARP. OpenGL on RTX 3090: 476 ms | 544 ms (glass on), 501 ms (glass off) on RTX 3090 |
| Pick latency, request on the UI thread to answer on the UI thread, viewport idle (median / 95th percentile) | 1.6 / 2.3 ms on RTX 3090. 5.7 / 6.5 ms on UHD 770. 83.6 / 86.2 ms on WARP | 4.6 / 5.6 ms on RTX 3090 |
| Pick latency while the viewport renders continuously | 1.7 / 2.2 ms on RTX 3090. 14.9 / 15.4 ms on UHD 770. 230 / 233 ms on WARP | 0.7 / 1.5 ms (glass on), 1.7 / 2.2 ms (glass off) on RTX 3090 |
| Pick correctness | 120 of 120 scripted picks returned the expected face id in each of the ten configurations, on Direct3D 11 and on OpenGL | 120 of 120 |
| Resize: window stepped 60 times in 2 s between 1920x1080 and 1280x720 while orbiting; frame time median / 95th percentile / worst | RTX 3090: 0.14 / 0.55 / 3.7 ms. UHD 770: 2.6 / 7.3 / 12.4 ms. WARP: 38 / 43 / 184 ms. No crash after the fix in 4.4 | RTX 3090: 0.50 / 0.98 / 5.9 ms |
| High-DPI: device pixel ratio 1.0, 1.5 and 3.0 (forced with `QT_SCALE_FACTOR` on the 150 % display) | Target stays 1920x1080 physical pixels, panels and text scale, 40 of 40 picks correct at each ratio | Same |
| Idle: frames drawn in 2 s with nothing changing | 0 | 0 |
| Clean exit | Yes | OCCT prints `wglMakeCurrent() has failed. The handle is invalid.` on shutdown. Exit code 0 |

A's pick is one full frame plus the ID pass, so it scales with frame time. B's pick is a CPU ray test and does not.

### 4.4 Problems found while building

A:

- **`QQuickWindow::createTextureFromRhiTexture()` takes ownership.** The `QSGTexture` it returns deletes the `QRhiTexture` it wraps. Holding that texture in a `unique_ptr` as well caused a double free and a crash on the first resize. Fixed by letting the wrapper own the backdrop texture. This matters for `blurredBackdropTexture()` in `render.h`.
- `QRhiDriverInfo::deviceType` is `Unknown` for both the RTX 3090 and the UHD 770 on Direct3D 11 (WARP reports `Cpu`). On OpenGL the vendor id and device id are 0. QRhi alone cannot tell integrated from discrete.
- On OpenGL the backdrop texture is Y-up, so the panel shader needs a flip flag. That was the only backend-specific line.
- Qt warnings do not reach a pipe on Windows unless `QT_FORCE_STDERR_LOGGING=1` is set.

B:

- OCCT's first-frame initialisation rebinds framebuffer 0. `OpenGl_FrameBuffer::InitWrapper` then fails and the view stays black. Fix: bind Qt's framebuffer object again before wrapping it, every frame.
- `QQuickOpenGLUtils::resetOpenGLState()` is needed after every OCCT redraw.
- OCCT logs `Unhandled GL error: GL_INVALID_ENUM` twice during initialisation, in core and in compatibility profile. Rendering was correct afterwards.
- Colours passed as `Quantity_TOC_RGB` came out far too bright. Passed as `Quantity_TOC_sRGB` they matched A.
- B exists only on OpenGL. Qt Quick must be forced to OpenGL on every platform.

### 4.5 Code size

Counted with `wc -l`, comments and blank lines included. The measurement harness (`common/bench.h`, 359 lines), the scene generator (`common/scene.h`, 114), the panel (`qml/Panel.qml`, 46) and the panel shader (`shaders/panel.frag`, 29) are shared and not counted.

| | A | B |
|-|-|-|
| Viewport C++ | 409 (`a/viewport.cpp`, `a/viewport.h`) | 254 (`b/occtview.cpp`, `b/occtview.h`) |
| Shaders | 139 (8 files) | 0 |
| `main.cpp` | 46 | 47 |
| Main QML, including B's blur chain | 24 | 57 |
| **Total** | **618** | **358** |

B is 42 % smaller for this scope. The gap will grow in a real viewport: OCCT already has highlighting, clipping planes with caps, transparency and selection of edges and vertices, and A has to write each of them. This spike did not measure that cost.

## 5. Limits

### 5.1 Not measured

- **A UHD 620-class laptop.** None was available. The plan's pass bar is therefore not confirmed.
- **macOS and Metal for A.** No macOS machine. `qsb` generated the Metal shader variants at build time; they were never run. The acceptance item "macOS result for A" is not met.
- **B on the Intel UHD 770.** See section 6.
- B on WARP. WARP is a Direct3D device; B needs OpenGL.
- Linux, Vulkan, Direct3D 12.
- What resize looks like. The resize test is scripted and records frame times only. Nobody watched it, so flicker or a stale frame during a mouse drag was not judged.
- Moving a window between monitors with different scaling, and a scaling change while running.
- 4K, real STEP models, transparency, section caps, backplots, a model with thousands of bodies, `updateMesh` patching, `pickRect`, the pick point in 3D.
- Cold start. First-frame times are with a warm file cache.
- Stability over hours.

### 5.2 Estimate for UHD 620 (labelled estimate, not a measurement)

Assumption given for this spike: UHD 770 is roughly 1.5 to 2 times faster than UHD 620.

| | UHD 770, measured | UHD 620, estimate at 1.5x to 2x |
|-|-|-|
| A, glass on, 95th percentile | 4.73 ms (211 FPS) | 7.1 to 9.5 ms (105 to 141 FPS) |
| A, glass off, 95th percentile | 4.39 ms (228 FPS) | 6.6 to 8.8 ms (114 to 152 FPS) |
| A, glass on, MSAA 4x, 95th percentile | 6.80 ms (147 FPS) | 10.2 to 13.6 ms (74 to 98 FPS) |

The pass bar needs 33.3 ms. A would have to be 7 times slower on a UHD 620 than on the UHD 770 to miss it. A 15 W laptop with single-channel memory can be slower than the 1.5 to 2 times assumed here; how much slower is a guess. The result is: indicated with a wide margin, not confirmed.

## 6. How the integrated GPU was selected, and why B was not run on it

- Direct3D 11 (A): `QT_D3D_ADAPTER_INDEX=1`. Qt's log lists adapter 1 as "Intel(R) UHD Graphics 770", and each run's `QRhi::driverInfo()` confirms vendor 8086, device a780.
- OpenGL (B, and A on OpenGL): **the Intel adapter could not be selected.** WGL has no adapter choice. Windows picks the OpenGL driver from the monitor the window is on, and all monitors on this machine are connected to the RTX 3090. Every OpenGL run reported "NVIDIA GeForce RTX 3090".
- The remaining route is the Windows per-application GPU preference (Settings, System, Display, Graphics, stored under the `UserGpuPreferences` registry key). That is a Windows setting. It was not changed.
- To get the missing rows: set `build\s1b.exe` to "Power saving" on that Settings page (or plug a monitor into the motherboard output), run `./run.sh b --glass 1 --out results/b_intel.jsonl` and the same with `--glass 0`, and check the `device` field of the output.

Guess, not a measurement: B's GPU time on the RTX 3090 is 2.3 to 2.7 times A's. If that ratio held on the UHD 770, B would need about 5 to 6.5 ms of GPU time per frame there, which is still inside the budget. Ratios from a GPU this fast do not transfer reliably.

## 7. Acceptance items

| Acceptance item from PLAN.md S1 | Status |
|-|-|
| 95th-percentile frame time for A, glass on and off, 1080p | Pass |
| Same for B | Partly: RTX 3090 only |
| Weakest GPU available plus WARP (no UHD 620 laptop supplied) | Pass for A (UHD 770 and WARP). Fail for B (run on neither) |
| Discrete GPU | Pass (RTX 3090, A and B) |
| Resize behaviour noted | Partly: scripted timing only, not judged by eye |
| First-frame behaviour noted | Pass |
| High-DPI behaviour noted | Pass for ratios 1.0, 1.5 and 3.0. Mixed-DPI monitors not tested |
| macOS result for A | Fail: not measured |
| Pass bar for decision A: 30 FPS or better on UHD 620 with glass on | Not confirmed. Indicated: 211 FPS at the 95th percentile on UHD 770; estimate 105 to 141 FPS on UHD 620 |
| States which decision the data supports | A, see section 8 |

## 8. Recommendation

**Keep decision A: own renderer on Qt RHI in a `QQuickRhiItem`.**

Reasons, from the data:

1. A meets the Low-tier budget on the weakest GPU available with a margin of 7 times (4.73 ms against 33.3 ms), glass on.
2. The shared quarter-resolution blur costs 0.29 ms on the UHD 770. Glass is not what decides the frame rate.
3. Where A and B could be compared directly, B is 1.5 to 1.9 times slower at the 95th percentile, and its QML blur chain costs 0.17 ms against 0.02 ms for A's blur passes.
4. A's code ran on Direct3D 11, on WARP and on OpenGL, with correct picks on all three. B runs only on OpenGL.
5. B needed a framebuffer rebind and a state reset every frame to coexist with Qt Quick, and still logs errors at start and at exit.

What argues for B: 42 % less code in this spike, and OCCT's ready-made highlighting, clipping, transparency and selection. That is a cost question for the M1 tasks, not a performance question.

Two things stay open and should be closed before M1 is called done: a run of `s1a.exe` on a real UHD 620-class laptop, and a build and run of A on a Mac.

### Changes proposed for PLAN.md

- **2.3**: fallback (a), our renderer on OpenGL, is cheaper than the text implies. The S1 renderer ran on Qt's OpenGL backend with one backend-specific line (the Y flip for the backdrop) and the same speed on the RTX 3090. State that (a) is a configuration switch (`QSG_RHI_BACKEND=opengl`), not a rewrite.
- **6.1**: "reads the adapter name, driver, VRAM ... and RHI feature flags" is not enough. QRhi reports device type `Unknown` for both real adapters on Direct3D 11 and gives no VRAM figure. `detectHardware()` needs DXGI directly on Windows (adapter description, dedicated video memory) to fill `discreteGpu` and `vramBytes`.
- **6.1 and 9**: say what happens on a software rasteriser. WARP gave 22 FPS on a 24-core desktop, below the Low budget of 30 FPS, with a pick latency of 84 ms. "Drops one tier" has nowhere to go below Low. Proposed: software rendering is a supported but slow fallback with a visible warning, excluded from the frame-rate budgets, with hover picking throttled.
- **6.2**: frame rate cannot be the reason for "Liquid Glass default off" on Low and Medium: glass costs 8 % on the UHD 770, and MSAA 4x still gives 147 FPS there. Either keep the defaults for a stated other reason (battery, readability) or turn glass on for Medium. Decide after the UHD 620 run. This is Jake's call.
- **6.3, Picking**: add that the read-back is a small rectangle around the cursor, not the whole ID buffer (S1 read 1x1), and that the answer arrives one frame later (1.6 ms on RTX 3090, 5.7 ms on UHD 770).
- **6.6**: add that S1 rebuilt the blur targets, the ID target and three pipelines on every size change (60 times in the 2-second resize test). The worst frame was 12 ms on the UHD 770 and 184 ms on WARP. The real renderer should allocate these in size steps or delay the rebuild until the resize settles.
- **9, measurement**: record that frame-time benchmarks on a shared machine need the process at high priority and a load reading; the first S1 run under full load from other spikes was unusable. Also record that `QRhiCommandBuffer::lastCompletedGpuTime()` works on Direct3D 11 and OpenGL, so `FrameStats::gpuMs` can be filled there (Metal not checked).
- **13, S1**: mark the pass bar "indicated, not confirmed" and keep the UHD 620 and macOS runs as open items (open question 3 already covers the laptop).

### Changes proposed for docs/interfaces/render.h

- `virtual void* blurredBackdropTexture() = 0;` needs a stated ownership and thread rule. In Qt the texture reaches QML as a `QSGTexture` made by `QQuickWindow::createTextureFromRhiTexture()`, and that wrapper deletes the texture it wraps. Proposed wording: the handle is owned by the viewport, is valid only on the render thread, and is replaced whenever the viewport is resized. Add a way to learn that it changed (S1 used `QSGTextureProvider::textureChanged`).
- The backdrop needs an orientation rule. On OpenGL the texture is Y-up and the panel shader must flip V. Either add `bool backdropIsYUp() const` or state that the renderer always delivers a top-down image.
- `HardwareInfo::discreteGpu` and `vramBytes`: note in the comment that these come from the platform API (DXGI on Windows), not from Qt RHI.
- `PickHit::point`: the ID buffer alone does not give a 3D point. It needs the depth value at the pixel as well. S1 did not build this; the comment should say where the point comes from.
- The header comment "this header does not change if the fallback is chosen" held up: neither A nor B needed a graphics API name in the interface.
