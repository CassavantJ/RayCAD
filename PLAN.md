# RayCAD technical plan

Status: **revision 2, 2026-11-17. Revision 1 was approved by Jake and the M0 spikes were run; this revision folds their results in (section 0.5, `spikes/RESULTS.md`).**
Source brief: `BRIEF.md`. Interface contracts: `docs/interfaces/`.

How to read this file if you are an implementing agent: read section 0 (rules), your task entry in section 13, and the interface headers your task names. Everything you need is in those three places. If something is missing or contradictory, stop and report it; do not guess.

Confidence labels used throughout: **[verified]** checked against a primary source on 2026-11-17, **[design]** a decision made in this plan, **[guess]** an estimate that a spike or measurement must confirm, **[measured Sn]** a number from M0 spike Sn on the High-tier development PC (Core i9-13900K, RTX 3090 and UHD 770), taken while other spikes loaded the machine, so timings can be off by up to 2x.

---

## 0. Summary, changes from the brief, and standing rules

### 0.1 What v1 is

On a Low-tier laptop, a machinist imports a customer STEP file, repairs it with direct editing, programs a 2.5D part (facing, adaptive roughing, contours, pockets, drilling, tapping), simulates it with collision checks, posts it for Mazak (EIA/ISO), Haas NGC or Fanuc, and prints a setup sheet. Exit gate: three benchmark parts (section 12.3) cut on real machines with no G-code hand edits.

### 0.2 Where this plan departs from the brief

| # | Brief said | Plan says | Why |
|-|-|-|-|
| 1 | Render OCCT into Qt Quick through a shared OpenGL context; force Qt Quick to OpenGL on Windows | **Own renderer on Qt's RHI layer** (`QQuickRhiItem`): Direct3D 11 on Windows, Metal on macOS, OpenGL on Linux. OCCT's visualization module is not used. The brief's approach is the fallback. Spike S1 decides. | [verified] Native OpenGL inside Qt Quick works only on Qt's OpenGL backend; there is no supported way to bring a GL texture into a D3D11 or Metal scene. The only OCCT Qt Quick sample is a personal repo that names OCCT 7.7 and Qt 5, with reported resize flicker, black first frames and Qt 6 breakage. Forcing OpenGL puts our weakest users (old Intel drivers) on the least-tested path. The RHI route removes the macOS OpenGL problem entirely and gives High-tier compute on every OS including macOS. |
| 2 | Use OCCT 8.0 BRepGraph topology history | **Do not use BRepGraph.** Build naming on `BRepTools_History` plus our own element map. | [verified] BRepGraph had breaking reworks in three releases and was withdrawn from OCCT master on 2026-10-13 pending API review. |
| 3 | v1 includes shell, draft, patterns, mirror, split body | These ship in **v1.x**, after the exit gate. v1 has the sketcher, extrude, cut, revolve, fillet, chamfer, hole, parameters and direct editing. | Agreed with Jake (question 1). The exit gate does not need them. |
| 4 | Bit-identical geometry, toolpaths and G-code across tiers | Bit-identical across **tiers and thread counts on one build**. Across operating systems: **identical G-code text**, enforced by CI, with toolpath IR identical after quantisation. | Agreed (question 10). Different compilers and math libraries make bit-identical B-rep floating point across OSes impractical. Section 8 explains how the G-code still matches. |
| 5 | OpenCAMLib for 3D toolpaths | Not used in v1 (no 3D toolpaths in v1). For v2, default to our own drop-cutter and waterline on a BVH; re-evaluate then. | [verified] OpenCAMLib has no adaptive clearing, takes only STL, and has had no tagged release since January 2023. |
| 6 | OpenGL 4.3 compute for High tier | Compute through RHI (`IComputeBackend`), which maps to D3D11, Metal, OpenGL 4.3 and Vulkan. v2 work. | Follows from row 1. |
| 7 | QuickJS | **QuickJS-NG** 0.18. | [verified] Upstream QuickJS has no MSVC or CMake support; QuickJS-NG builds on MSVC in CI and ships CMake. Both MIT. |
| 8 | Multi-body is v2 | v1 supports **static imported fixture bodies** (collision only, not editable). Full multi-body modeling stays v2. | Agreed (question 3). v1 setups need vises as collision bodies. |
| 9 | Cold start under 3 s on Low | Kept as the target but marked [guess] until spike S2 measures it. | Qt Quick plus OCCT DLL load from a slow disk is unmeasured. |

### 0.3 Assumptions made where Jake gave no specific answer

Jake accepted every recommended default. Three questions had no default, so these are placeholders, repeated in section 15:

- **Target date:** none given. Section 12 gives an estimate instead of working back from a date.
- **Machine details** (models, control generations, 4th axis, rigid tapping, probes): unknown. v1 assumes three 3-axis vertical mills with rigid tapping and no probing. Posts are written against the control manuals and proven on the machines in milestone M8.
- **Low-tier CI hardware:** none confirmed. GPU frame-rate budgets cannot be enforced in CI until a UHD 620-class machine is available as a self-hosted runner. Until then they are measured by hand at each milestone.

### 0.4 Standing rules for every task

1. Only `src/kernel` includes OCCT headers. Only `src/ui` and `src/render` include Qt headers. CI checks this with an include scan.
2. `src/cam`, `src/post`, `src/sim` (CPU), `src/kernel`, `src/naming`, `src/doc` and `src/store` never link `src/render` or `src/ui`. CI checks the CMake dependency graph.
3. No exceptions cross a module boundary. Return `ray::Result<T>`.
4. Deterministic code (anything under `src/cam`, `src/post`, `src/naming`, `src/sketch`, and serialisation anywhere) follows section 8: no `<cmath>` trig (use `ray::dmath`), no iteration over unordered containers, no dependence on thread count, clock, locale, tier or pointer values.
5. Anything that can take more than 100 ms runs on a worker through `IJobSystem`, takes a `JobContext`, checks cancellation and reports progress.
6. No hard-coded colours, fonts or sizes in UI or viewport code. Use `render::Token` and theme metrics.
7. Every user-visible change to a document is a `doc::Command` executed through `api::IActionRegistry`.
8. Every user-visible string goes through `qsTr()` / `ray::tr()` so translation is possible later. English only in v1.
9. No code, assets, icons or file formats copied from any commercial CAD/CAM product. Code may be adapted from FreeCAD only where its licence is LGPL-2.1-or-later, with attribution kept in the file header and listed in `THIRD_PARTY.md`.
10. A task may touch only the files and directories its entry lists. If it needs a change to an interface header, it stops and reports; interface changes are separate SEQUENTIAL tasks approved by Jake.

### 0.5 What the M0 spikes changed (revision 2)

All ten spikes ran on 2026-11-17. Reports are in `spikes/<id>/REPORT.md`; `spikes/RESULTS.md` is the summary. Nothing was run on a Low-tier machine, on macOS or Linux, or on a real network share, so those results are still open (section 15).

| Spike | Result | Where the plan changed |
|-|-|-|
| S1 viewport | Own RHI renderer confirmed: 211 FPS at the 95th percentile on UHD 770 with glass on; OCCT's viewer 1.9x slower on the same card and API. UHD 620 not confirmed. | 2.3, 6.1, 9, T-013, T-016, `render.h` |
| S2 start-up | 0.7 s warm on High. Cold not measured. Delay-loading OCCT helps first launch; `qmltc` does not build with Qt Quick Controls. | 9, T-001, T-018, T-101 |
| S3 STEP import | 47 MiB file takes 9.7 s to a coarse first view and 21 s at fine tolerance. One mesh job per body crashed OCCT; `InParallel` is safe and bit-identical. Part of the read cannot be cancelled. | 7, 9, T-005, T-006, `kernel.h` |
| S4 naming | 3.3 as first written resolved 17 of 33,785 references to a wrong element. Amended rules: 0 wrong. | 3.3 rewritten, T-007, T-081, T-096, `naming.h`, `kernel.h` |
| S5 adaptive | Adapt FreeCAD main's `Adaptive.cpp`. Good engagement control, but no path for a narrow slot, polylines only, clock-driven branches, and a hang and a crash at 32 concurrent runs. | 2.1, 8, 9, T-060, T-061, new T-063, `cam.h` |
| S6 simulation | CPU tri-dexel passes Low and Medium with a wide margin. High at 0.05 mm takes 14.7 s per 100k moves on 4 threads. | 6.2, 9, T-070, T-072, `sim.h` |
| S7 sketch solver | PlaneGCS vendors with a 64-line shim. Drag at 200 coupled curves takes 0.4 to 0.8 s, not 16 ms. Bit-identical only under three adapter rules. | 8, 9, new spike S11, T-090, T-092, `sketch.h` |
| S8 post engine | QuickJS-NG kept: 1 M moves in 0.68 s, byte-identical. Four sandbox design changes. | 7, T-039, `post.h` |
| S9 direct editing | Booleans give 95% or better for hole and pocket removal, resize and push/pull. `BRepOffset_MakeOffset` is unusable. Fillet-run removal succeeded 18 of 35 times on real parts. | T-080 to T-083, `kernel.h` |
| S10 file format | 1,000 kills during save, 0 damaged files. A plain snapshot copy taken during a save is usually corrupt. Loopback SMB only. | 4.1 to 4.3, T-011, `store.h` |

### 0.6 What M1 changed (revision 3)

All 25 M1 tasks were merged on 2026-11-18. These are the places where the code now differs from this plan or from the headers in `docs/interfaces/`. The headers have not all been updated yet; where they disagree with this list, this list is right.

**Decided and built**

- File schema: `objects` and `thumbs` are rowid tables, not `WITHOUT ROWID` as section 4.1 says.
- Recovery journal: carries Execute, Undo, Redo and Snapshot records and can append objects. A document that was never saved is journalled through a backing file in `<userDataDir>/unsaved`.
- `IApp` open and save return job handles. The CAM, library and post accessors are nullable until those modules exist.
- The module rule stays "ui uses api and render only", so viewer, measure and settings helpers live in `src/api/include/ray_api/`.
- Imported bodies are named from their fingerprint (the fingerprint stands in for the parent feature).
- BREP is stored in a canonical form (`RAYBREP` v1); the payload is round-tripped once so the bytes are stable.
- One job system per process; one process-wide mesh lock shared by meshing and STL export; IGES reads are serialised.
- New theme tokens `DraftPositive`, `DraftNegative`, `DraftVertical`. T-016 needed no custom shader. Section caps use stencil parity in the main pass.
- Test corpus: 388 files, 204 committed. The ABC and Fusion gallery sets were rejected on licence grounds.

**Missing from the headers, to add in an interface task**

Per-focus shortcuts; `ActionInfo::appliesTo`; `AppEvent::SettingsChanged`; the tier explanation in `render.h`; optional import colour; vertex ids on `EdgeRange`; non-pickable lines; a public way to set the store's recovery folder (api uses a private one today).

**Known gaps carried forward**

- Import runs in the app's process. OCCT's readers crashed or looped on damaged files twice; both cases are now rejected before reading, which is not a guarantee. An out-of-process import worker is needed (risk 2).
- A 95 MiB STEP takes about 29 s to import against a 10 s budget; healing is sequential.
- No stored thumbnails. One document at a time. The unit toggle changes display only. Import options from the palette form are ignored. File shortcuts fire behind modal dialogs. Edits made during Save As are not journalled.
- `SurfaceInfo` gives the wrong plane normal for left-handed faces (measure works around it).
- Section panel state is not saved; section and draft shaders are unverified on llvmpipe and Metal.
- API tests read the real user profile.
- Three UI tests and one kernel timing test fail under machine load on Windows and pass alone.
- Linux and macOS have not passed on the final M1 tree.

**Open for Jake**

- Licence of the synthetic corpus parts: LGPL or CC0.
- `cam.h` against the section 1.1 dependency rule, before T-032.

---

## 1. Architecture overview

### 1.1 Modules

| Module | Responsibility | May depend on |
|-|-|-|
| `core` | Ids, hashing, `Result`, units, deterministic math, job system, logging | standard library, oneTBB, BLAKE3 |
| `kernel` | The only OCCT wrapper: import/export, construction, booleans, direct edits, queries, tessellation, slicing | core, OCCT |
| `naming` | Persistent names for faces, edges and vertices; reference resolution | core, kernel |
| `sketch` | Sketch entities, constraints, PlaneGCS solver, loop detection, DXF import | core, kernel (profile types), PlaneGCS, Eigen |
| `lib` | Tool, holder, material, machine, template and rule libraries in plain folders | core |
| `doc` | Immutable document snapshots, feature types, parameters, rebuild, commands, undo | core, kernel, naming, sketch, cam (types) |
| `store` | `.raycad` file, content-addressed objects, commits, recovery journal, migrations | core, doc, SQLite, zstd |
| `cam` | Setups, operations, toolpath IR, generators, linking, hole recognition, feeds, cycle time | core, kernel, naming, lib, Clipper2 |
| `sim` | Backplot, tri-dexel stock, analytic safety checks, timeline | core, kernel, cam, lib |
| `post` | QuickJS-NG host, post scripts, G-code output with line map, setup sheet HTML, file output | core, cam, lib, QuickJS-NG |
| `api` | Action registry, app session, headless app; the single entry point for UI, scripts and agents | all of the above |
| `render` | Tier detection, theme tokens, viewport renderer, blurred backdrop, GPU compute backend | core, kernel (mesh types), sim (backplot type), Qt RHI |
| `ui` | Qt Quick application: shell, panels, viewport item, tools, dialogs | api, render, Qt |
| `apps/raycad` | Desktop executable | ui |
| `apps/raycad-cli` | Headless executable for tests and golden files: import, generate, post, hash | api |

### 1.2 What calls what

```
                 +-------------------------------------------------------------+
                 |  ui (Qt Quick / QML)        start screen, palette, panels    |
                 |  ViewportItem (QQuickRhiItem)   tools: select, sketch, edit  |
                 +----------------+-------------------------+------------------+
                                  | actions (JSON args)      | scene items, picks
                                  v                          v
   scripts (v2) --+      +-----------------+        +-----------------------+
   MCP server(v2)-+----> |       api       |        |        render         |
   raycad-cli ----+      | IActionRegistry |        | tiers, themes,        |
                         | IApp            |        | IViewport, backdrop,  |
                         +--+----+----+----+        | IComputeBackend       |
                            |    |    |             +-----------^-----------+
            +---------------+    |    +-----------+             | display meshes,
            v                    v                v             | backplot (data only)
     +-------------+      +-------------+   +-----------+       |
     |    store    |<-----|     doc     |-->|    cam    |-------+
     | .raycad file|      | snapshots,  |   | setups,   |
     | objects,    |      | features,   |   | ops, IR,  |----> +---------+   +--------+
     | commits,    |      | rebuild,    |   | generators|      |   sim   |   |  post  |
     | journal     |      | undo        |   +-----+-----+      | dexel,  |   | QuickJS|
     +-------------+      +--+-------+--+         |            | checks  |   | G-code |
                             |       |            |            +----+----+   +---+----+
                             v       v            v                 |            |
                       +---------+ +--------+ +-------+             |            |
                       | naming  | | sketch | |  lib  |<------------+------------+
                       +----+----+ +---+----+ +-------+
                            |          |
                            v          v
                       +------------------------+
                       |   kernel (OCCT 8.0.1)  |
                       +-----------+------------+
                                   v
                       +------------------------+
                       | core: ids, hash, jobs, |
                       | dmath, units, Result   |
                       +------------------------+
```

Data flows one way: a user action becomes a command, the command produces a new document snapshot, the rebuilder produces shapes and element maps, the CAM engine marks operations stale and regenerates toolpaths, and the UI receives a notification and pushes display data to the renderer. The renderer never feeds anything back except pick results and camera state.

### 1.3 Processes and threads at a glance

One process. UI thread (Qt), render thread (Qt Quick scene graph), a oneTBB worker pool behind `IJobSystem`, and one short-lived thread per post run (QuickJS runtimes are single-threaded). Section 7 has the details.

---

## 2. Stack decisions

### 2.1 Final choices

| Layer | Choice | Rationale | Rejected | Licence and obligations |
|-|-|-|-|-|
| Language | C++20. MSVC 2022, Clang 18+, GCC 13+. No Rust in v1. | OCCT and Qt are C++; one toolchain keeps agent tasks simple. | C++23 (uneven `std::expected` support; we use `tl::expected` instead). Rust core (two FFI boundaries around OCCT and Qt). | n/a |
| Geometry kernel | OCCT **8.0.1**, pinned. [verified: released 2026-07-30; vcpkg and Conan both ship 8.0.1] | Only open-source B-rep kernel with STEP, booleans, fillets and defeaturing. | Writing a kernel; older OCCT 7.9 (misses 8.0 thread-safety fixes). | LGPL 2.1 (2.1 only) with Open CASCADE exception. Dynamic link, ship licence text, offer source, allow relinking. |
| Topology history | `BRepTools_History` and `Modified/Generated/IsDeleted`, normalised in `kernel::ShapeHistory` | Stable, documented mechanism. | BRepGraph (withdrawn upstream); OCAF `TNaming` (pulls in OCAF document model, one per thread, not ours). | part of OCCT |
| UI framework | Qt **6.12 LTS**, Qt Quick, Qt Quick Controls, Qt Quick Effects. [verified: 6.12 released 2026-09-30, supports Windows 10 1809+, 11, macOS 14.4+; it is the last Qt to support Windows 10] | Live blur behind panels, high-DPI, accessibility and text input for free. | Qt Widgets (no blur over a 3D view); Dear ImGui (no accessibility, weak text input) kept only as the last-resort fallback; Qt Quick 3D (GPL/commercial, not needed). | LGPL v3. Dynamic link, ship licence text and notice, provide Qt source or written offer, allow replacing Qt DLLs, no anti-reverse-engineering terms. |
| Rendering | **Own renderer on Qt RHI inside a `QQuickRhiItem`.** D3D11 on Windows, Metal on macOS, OpenGL 3.3+ on Linux. Tessellated meshes from `kernel::tessellate`. | See 0.2 row 1. Our scene is simple (meshes, edges, polylines, an ID buffer) and CAM needs a custom renderer for toolpaths and dexel stock anyway. | OCCT TKOpenGl/AIS in Qt Quick with forced OpenGL (fallback, see 2.3). Raw OpenGL 3.3 renderer (same shared-context problem). bgfx/Vulkan directly (second graphics stack next to Qt's). | Qt RHI is part of Qt GUI (LGPL v3). **Risk:** RHI has no source/binary compatibility promise between Qt minor versions [verified]; we pin Qt 6.12 and keep all RHI code in `src/render/rhi`. Shaders are compiled at build time with Qt's `qsb` tool, which is GPL v3/commercial [verified]; it is a build tool we run, not code we ship, and the runtime library is LGPL. [design: I read this as fine; listed in open questions for confirmation.] |
| Sketch solver | PlaneGCS, vendored from FreeCAD into `third_party/planegcs` with a small shim replacing FreeCAD's `Base/` headers | Proven in FreeCAD; 11 files; needs Eigen, header-only Boost.Graph and `boost/math/constants` [measured S7: builds unmodified at FreeCAD tag 1.1.4 with a 64-line shim, Eigen 3.4.0]. Drag speed limits are in section 9. | SolveSpace solver (GPL v3 only); writing our own. | LGPL-2.1-or-later. Keep headers, list in `THIRD_PARTY.md`. A script re-syncs from upstream. |
| 2D polygons | Clipper2 **2.0.1** from the new home `PolyClipping/Clipper2` [verified: the original repo was retired in 2026] | Integer coordinates make offsets deterministic. | CGAL (GPL/commercial for the parts we need); Boost.Polygon. | Boost Software License 1.0. |
| Arcs | Our own deterministic arc fitting after Clipper2 (Clipper2 is polygon-only [verified]) | G-code quality and file size need G2/G3. | Emitting only line segments. | ours |
| Adaptive roughing | A reworked fork of FreeCAD **main**'s `Adaptive.cpp` (the Clipper2 version, commit 56062b61 or later), in `src/cam/ops/adaptive2d`. Not vendored unmodified; work list in T-060. | [measured S5] Median engagement within 1.1 deg of target, 98.5 to 100% of cut length within 1.1x target, nothing left uncut. Correction to revision 1: only FreeCAD's unreleased main branch is on Clipper2 (ported about 2026-11-03); every release up to 1.1.4 uses Clipper 6.4.2 and is worse on every quality measure. The code is LGPL-2.1-or-later. | OpenCAMLib (has no adaptive clearing). | LGPL-2.1-or-later if adapted; attribution to Kresimir Tusek. |
| 3D toolpaths (v2) | Own drop-cutter and waterline on a BVH [guess, revisit at v2] | See 0.2 row 5. | OpenCAMLib for now. | n/a |
| Threading | oneTBB behind `IJobSystem` | Mature, has task groups and arenas; OCCT can use it too. | Custom pool (cancellation and nested parallelism are easy to get wrong). | Apache 2.0. |
| Scripting and posts | QuickJS-NG **0.18** [verified: released 2026-11-01, MIT, MSVC CI] | Programmers who edit posts already know JavaScript posts. Small, embeddable, no JIT, deterministic. [measured S8: 1 M moves in 0.68 s, script heap 0.19 MB, byte-identical output.] Built as the plain `qjs` library without `quickjs-libc`, with a one-line vendor patch (`JS_INTERRUPT_COUNTER_INIT` 100). | Lua (smaller audience among post editors); V8 (huge); upstream QuickJS (no MSVC). | MIT. |
| Native file | One SQLite file per document (`.raycad`), rollback-journal mode, exclusive locking, plus a lock file | Transactions give crash-safe saves; partial loads by key; one file to email. | Zip of JSON and BREP (no atomic partial update; rewrite whole file on each save); a folder (not emailable). | Public domain. |
| Text formats | Canonical JSON (nlohmann/json) for every plain-text file: libraries, machines, themes, manifests | One format, one parser, schema-validated. | TOML/YAML (second parser, ambiguous number formatting). | MIT. |
| Hash, compression | BLAKE3, zstd | Fast content addressing; fast compression of BREP and toolpaths. | SHA-256 (slower); zlib. | BLAKE3 CC0/Apache 2.0; zstd BSD. |
| Tests | Catch2 3.x | Simple, header-light, good CTest integration. | GoogleTest. | BSL 1.0. |
| MCP server (v2) | Our own thin server over stdio and Streamable HTTP, mapping MCP tools to `IActionRegistry` | [verified] There is no official C++ MCP SDK; community ones lag the 2026-07-28 spec. | Community SDKs; a sidecar in another language. | ours |
| Tool catalog import (v2) | Our own GTC reader | [verified] No maintained open-source parser. ISO 13399 itself is a paid standard; GTC documents need free registration, licence terms unclear. | n/a | check GTC terms before v2 |
| Fonts | Inter, JetBrains Mono, bundled | Approved visual direction. | n/a | SIL OFL 1.1; ship licence text. |
| Build | CMake 3.28+ presets; vcpkg manifest mode with a pinned baseline; Qt from prebuilt binaries (`install-qt-action`/aqtinstall), not from vcpkg | Building Qt from source in CI is slow; vcpkg has OCCT 8.0.1 and everything else. | Conan (equally viable; one tool is enough); FetchContent for everything (slow OCCT builds on every clean CI run). | n/a |
| Packaging | Windows: Inno Setup per-user installer (no admin) plus portable zip. Linux: AppImage. macOS: dmg. | All free; no admin rights. | MSIX (signing required); MSI (admin by default). | Inno Setup licence permits this use. |

### 2.2 Project licence recommendation

**Recommend LGPL-2.1-or-later for RayCAD, with a Developer Certificate of Origin and no contributor licence agreement.**

- It matches OCCT and FreeCAD, so we can adapt FreeCAD's LGPL code (PlaneGCS, the adaptive algorithm, element-map ideas) with no friction.
- A tooling vendor or machine builder can ship a closed plug-in or integrate RayCAD libraries, which helps adoption in shops.
- Post scripts, templates and rules a shop writes are data run by the app and are not covered by our licence either way.
- The cost: someone can build a closed product on top of our libraries, as long as they publish changes to the libraries themselves. GPL v3 would prevent that and would also unlock GPL-only components (SolveSpace's solver, Qt Quick 3D), but we need none of those.
- SignPath's free signing forbids commercial dual licensing [verified], so a CLA that would enable dual licensing buys nothing.

Compatibility: an LGPL-2.1-or-later application that dynamically links Qt (LGPL v3) and OCCT (LGPL 2.1 with exception) and includes LGPL-2.1-or-later source is standard practice (FreeCAD does exactly this). [The FSF's own compatibility page could not be reached today, so this rests on established practice, not a statement I re-read.]

### 2.3 Rendering fallback, if spike S1 fails

Spike S1 supports the RHI renderer [measured S1: 500k triangles with edges at 1080p, glass on, 4.7 ms at the 95th percentile on Intel UHD 770 and 0.34 ms on RTX 3090; OCCT's viewer in a `QQuickFramebufferObject` was 1.9x slower on the same card and API]. The UHD 620 bar (30 FPS) is indicated with a wide margin but **not confirmed**, and Metal was not run. If a real UHD 620 or a Mac fails it, fall back in this order: (a) the same renderer on Qt's OpenGL backend, which is a configuration switch (the spike code ran on it with one backend-specific line), at a start-up cost of 115 to 220 ms [measured S2]; (b) OCCT's viewer in a `QQuickFramebufferObject`, as the brief proposed; (c) Dear ImGui panels. `render.h` names no graphics API, so none of these changes any other module.

### 2.4 Dependency licence ledger

`THIRD_PARTY.md` lists every dependency with version, licence, source URL and how it is linked. A CI job fails if a vcpkg dependency appears that is not in the ledger, or if any dependency's licence is GPL-only.

---

## 3. Core data model

All persistent objects serialise to **canonical JSON** (UTF-8, sorted keys, no extra whitespace, shortest round-trip doubles) or to a documented binary layout. An object's identity in storage is the BLAKE3 hash of those bytes. Lengths are millimetres, angles are radians, unless a field is an expression string with its own unit.

### 3.1 Document

```json
{
  "schema": 1,
  "settings": { "units": "inch", "decimals": 4, "material": "<material uuid>" },
  "parameters": [ { "id": "<uuid>", "name": "width", "expression": "4 in", "kind": "length" } ],
  "features":   [ "<hash of feature object>", "..." ],
  "bodies":     [ { "id": "<uuid>", "name": "Bracket", "role": "part", "color": [0.72,0.74,0.78,1], "visible": true } ],
  "cam":        "<hash of cam object>",
  "embedded":   { "tools": {"<uuid>": "<hash>"}, "holders": {}, "machines": {}, "materials": {} }
}
```

The manifest holds hashes, not content, so editing one feature rewrites one small object plus the manifest. `embedded` holds copies of every library item the document uses, so a file opens identically on a machine that has no shared library.

### 3.2 Feature tree

A feature is `{ "id", "type", "name", "suppressed", "props" }`. The list order is the rebuild order. v1 feature types and their `props`:

| Type | Props (all dimensions are expression strings) |
|-|-|
| `import` | `blob` (hash of BREP object), `sourceFile`, `sourceUnits`, `body` (BodyId), `role` (`part`/`fixture`) |
| `datum_plane` | `base` (`xy`/`xz`/`yz` or TopoRef to a planar face), `offset` |
| `sketch` | `plane` (datum feature id or TopoRef), `sketch` (hash of sketch object) |
| `extrude` | `sketch`, `regions` (loop ids), `distance`, `direction`, `symmetric`, `draft`, `operation` (`new`/`join`/`cut`), `target` (BodyId) |
| `revolve` | `sketch`, `regions`, `axis` (SketchEntId), `angle`, `operation`, `target` |
| `fillet`, `chamfer` | `edges` ([TopoRef]), `radius` or `distance` |
| `hole` | `face` (TopoRef), `positions` (sketch id or points), `standard` (e.g. `"1/4-20 UNC"`, `"M6x1"`), `kind` (`drilled`/`tapped`/`counterbore`/`countersink`), `diameter`, `depth`, `through`, `cbore` {diameter, depth}, `csink` {diameter, angle}, `thread` {spec, depth, class} |
| `delete_face` | `faces` ([TopoRef]) |
| `offset_face` | `faces` ([TopoRef]), `distance` |
| `resize_hole` | `face` (TopoRef), `diameter` |
| `transform_body` | `body`, `xform` |

Direct edits are ordinary features in the same history. That is the whole hybrid-modeling design: an imported solid is feature 1, and every direct edit or parametric feature after it is replayable and undoable. Thread specs on `hole` features are stored so CAM can pick the tap without asking.

v1.x adds `shell`, `draft`, `pattern_linear`, `pattern_circular`, `mirror`, `split_body`. v2 adds `move_face`, configurations and PMI.

### 3.3 Persistent naming

This is the highest-risk design in the project. Spike S4 tested the revision 1 rules on a 40-feature part with 677 references and 47 edit scenarios (33,785 resolutions). They resolved **17 references to a wrong element**: 8 silently, 9 flagged `Repaired`, one of which built a chamfer on the wrong edge. The rules below are the amended set that gave **0 wrong** on the same scenarios (97.8% `Exact`, 0.1% `Split`, 0.2% `Merged`, 0.1% `Repaired`, 1.8% `Broken`, of which 483 of 572 were elements that no longer existed). That is one part and one author; the residual risk is in section 14.

**Name.** Every face, edge and vertex of every body has one or more `TopoName`s (see `naming.h`): the feature that created it, a role string, the sorted names of the elements it was generated from, a discriminator, an ordinal and a sibling count. The stored reference is the hash of those fields. A name describes how the element came to exist, so it does not change when unrelated features are edited. The **family** of a name is the hash of (kind, origin, role, parents); names that differ only in discriminator and ordinal are siblings of one family.

**How names are assigned.**

1. *Sweeps (extrude, revolve).* The kernel reports which sketch curve produced each side face and which sketch loop produced each cap (`SweepFaceOrigin`). Side face name = (feature, `extrude.side`, [sketch entity id]). Caps = (feature, `extrude.startcap` / `extrude.endcap`, [loop id]). Edges and vertices are named from the two or three faces that meet there. This requires **stable sketch entity ids and stable loop ids**; hole walls are named from a stable position id of the hole feature.
2. *Modifying operations (boolean, fillet, chamfer, direct edits).* For each `HistoryEntry`: a `Modified` element keeps its input name. If one input maps to several outputs, it is a split: each output gets (feature, `split`, [input name]) and a `LineageEvent::Split` is recorded. If several inputs map to one output (same-domain unification), the output **carries every input name**. `Generated` elements get (feature, role such as `fillet.blend`, [source names]). `Deleted` records a lineage event.
3. *Elements with no history.* Named from their neighbours: an edge from its adjacent faces, a vertex from its faces. [measured S4] In OCCT 8.0.1 this is needed for the 4 boundary edges and 4 vertices of every fillet or chamfer blend and for prism edges and vertices; every result *face* had history. Neighbour names were never wrong, but they are fragile: when a neighbouring face is split or renamed, the element's name changes although the element did not.
4. *Telling siblings apart.* Siblings are first told apart by a **topological discriminator**: the sorted names of the faces around the element that are not among its parents (for a face, its adjacent faces; for an edge, the faces at its end vertices), preferring faces made by the current feature. Only siblings whose discriminators are identical get an ordinal, by quantised geometric fingerprint (centroid, then area, then axis), never by OCCT index. The **sibling count is part of the hashed name**, so a name issued alone and a name issued as one of n are different names. A name that needed an ordinal is *ambiguous*.
5. *Imported solids.* No history exists. Faces are named (import feature, `import.face`, [], ordinal by fingerprint sort) and carry their fingerprint. When the user replaces the imported file with a new revision, `rematchImported` matches old names to new faces by fingerprint and adjacency; anything unmatched is reported, never guessed. [Not tested by S4.]

**Resolution.** `IResolver::resolve` returns one of five states (`naming.h`):

- `Exact`: the name exists. An ambiguous name is trusted only while the element's fingerprint still matches the one stored at pick time (compared with a tolerance, not bit equality); otherwise it is `Broken`.
- `Split`: the name is gone but its family now has more members, or a lineage split is recorded. Returns all pieces. A fillet on a split edge fillets all pieces; a CAM contour asks for review.
- `Merged`: the name is gone because its family is whole again, or the element was unified with others. Returns the element that now contains what was picked, plus more. Consumers treat it like `Split`.
- `Repaired`: the name is gone, exactly one candidate matches the stored fingerprint **and** is surrounded by exactly the faces the stored neighbour names resolve to. It is shown to the user as a suggestion. **A `Repaired` reference is never consumed by a feature rebuild or a posted operation until the user confirms it**; until then the feature fails or the operation is `ReferenceBroken`.
- `Broken`: anything else. A feature with a broken reference fails visibly. A CAM operation with a broken or unconfirmed reference is `ReferenceBroken` and **cannot be posted**.

The trade is deliberate: the amended rules turn some recoverable cases into `Broken` (89 of 31,819 references were lost although their element still existed, against 61 with the unsafe rules). Users will sometimes see "reference broken" after an edit that left their element untouched, mostly on edges and vertices next to a face that was split.

**Storage.** Each body's `ElementMap` is a derived object stored in the file cache; it also records family membership for the current build. Names are rebuilt deterministically on every rebuild, and the lineage of the current build was sufficient in S4; lineage is not kept across rebuilds. [measured S4] `propagate` cost 0.22 ms per feature, about 1.3% of the OCCT time, and a 141-face part has 708 names.

**Kernel facts the naming code relies on** [measured S4, OCCT 8.0.1]: boolean, `SimplifyResult`, standalone unify, prism, fillet and chamfer history contained no wrong entry in 39 audited operations. One logical intersection that comes out in several pieces (a hole rim broken by the part edge, a cylinder-cylinder intersection cut at seams) carries no hint that the pieces belong together; the family rule handles it. Seam-induced pieces can only be told apart by ordinal and go `Broken` when their geometry changes.

**Acceptance (carried by T-081 and T-096):** on the S4 part and its 47 scenarios, at least 95% of references whose element still exists resolve `Exact`, `Split` or `Merged` in every scenario, and **zero resolve to a wrong element**, judged by an oracle that does not use the naming code. The second number is the one that matters. Not yet covered and to be added by those tasks: imports and rule 5, direct edits, revolve, more than one part.

### 3.4 Sketch

```json
{ "schema": 1,
  "entities": [
    { "id": "<uuid>", "type": "point",  "x": 0, "y": 0 },
    { "id": "<uuid>", "type": "line",   "a": "<pt>", "b": "<pt>", "construction": false },
    { "id": "<uuid>", "type": "arc",    "center": "<pt>", "start": "<pt>", "end": "<pt>", "ccw": true },
    { "id": "<uuid>", "type": "circle", "center": "<pt>", "radius": 6.35 },
    { "id": "<uuid>", "type": "spline", "points": ["<pt>"], "degree": 3, "closed": false },
    { "id": "<uuid>", "type": "slot",   "a": "<pt>", "b": "<pt>", "width": 6.35 } ],
  "constraints": [
    { "id": "<uuid>", "type": "distance", "refs": ["<pt>", "<pt>"], "expression": "width / 2", "driven": false } ] }
```

Entities are stored sorted by id. Solved coordinates are stored with the sketch so opening a file never requires a solve.

### 3.5 Body

A body is identity plus display attributes in the manifest. Its shape and element map are derived by the rebuild and cached. An imported body's source BREP is the `blob` on its `import` feature, which is the one place geometry is a source of truth and not a cache.

### 3.6 CAM objects and libraries

**Setup, stock, WCS** (see `cam.h`):

```json
{ "id": "<uuid>", "name": "Op 1 - top", "partBodies": ["<body>"], "fixtureBodies": ["<body>"],
  "stock": { "type": "box", "xNeg": 2.54, "xPos": 2.54, "yNeg": 2.54, "yPos": 2.54, "zNeg": 0, "zPos": 1.27 },
  "wcs": { "zFace": "<TopoRef>", "xEdge": "<TopoRef>", "origin": { "mode": "stockCorner", "corner": 0 }, "workOffset": 1 },
  "machine": "<machine uuid>", "post": "haas-ngc", "programNumber": 1001, "operations": ["<op uuid>"] }
```

Stock types: `box` (offsets from the part bounding box), `cylinder`, `solid` (a body), `fromSetup` (the simulated result of an earlier setup, which is how in-process stock is carried to the second setup).

**Operation:**

```json
{ "id": "<uuid>", "type": "adaptive2d", "name": "Rough pocket",
  "tool": { "tool": "<uuid>", "holder": "<uuid>", "number": 3, "lengthOffset": 3, "diameterOffset": 3, "stickOut": 31.75 },
  "cutting": { "rpm": 10000, "feedCut": 2540, "feedPlunge": 500, "feedRamp": 1270, "coolant": "flood" },
  "heights": { "clearance": 25.4, "retract": 5.08, "feedPlane": 2.54, "top": 0, "bottom": -12.7 },
  "geometry": ["<TopoRef>"],
  "params": { "optimalLoad": 1.27, "stepDown": 12.7, "stockToLeaveRadial": 0.25, "stockToLeaveAxial": 0.13,
              "entry": "helix", "helixDiameter": 9.5, "rampAngle": 2, "direction": "climb", "minCuttingRadius": 0.6 },
  "template": "<template uuid or empty>" }
```

Each operation type publishes a JSON Schema with defaults (`IOperationType::paramsSchemaJson`); the UI builds the form from it, and the API validates against it.

**Toolpath** (binary, canonical little-endian, the golden-file format): header (`RCTP`, version, operation id, input hash, move count, start point), then fixed-size `Move` records exactly as in `cam.h`. Coordinates are integer nanometres; feeds are integer micrometres per minute. No floating point is stored.

**Tool** (`tools/<uuid>.tool.json`):

```json
{ "schema": 1, "id": "<uuid>", "name": "1/2 3FL carbide EM", "vendor": "", "productId": "",
  "type": "flatEndMill", "flutes": 3, "diameter": 12.7, "shankDiameter": 12.7, "fluteLength": 31.75,
  "shoulderLength": 38.1, "overallLength": 76.2, "cornerRadius": 0, "bodyMaterial": "carbide",
  "presets": [ { "material": "<uuid 6061>", "opClass": "rough", "surfaceSpeed": 300, "chipLoad": 0.089,
                 "maxRadialFraction": 0.1, "maxAxialFraction": 2.0, "rampAngle": 2 } ] }
```

**Holder:** `{ id, name, vendor, gaugeLength, segments: [ { height, lowerDiameter, upperDiameter } ] }`.

**Machine** (`machines/<uuid>.machine.json`):

```json
{ "schema": 1, "id": "<uuid>", "name": "Haas VF-2 (NGC)", "vendor": "Haas", "model": "VF-2", "kind": "mill3",
  "axes": [ { "letter": "X", "min": 0, "max": 762, "rapid": 25400, "maxFeed": 16500, "accel": 5000 } ],
  "spindle": { "maxRpm": 8100, "minRpm": 0, "powerKw": 22.4, "torqueNm": 122 },
  "toolChanger": { "capacity": 20, "changeSeconds": 4.2 },
  "control": { "dialect": "haas-ngc", "defaultPost": "haas-ngc", "rigidTapping": true, "maxProgramNameLength": 8 },
  "options": { "throughSpindleCoolant": false, "probe": false, "toolSetter": false },
  "postProperties": { "useG187": true } }
```

Shipped definitions use published catalogue values and are marked `"verified": false` until Jake checks them against the real machines.

**Post:** one `posts/<name>.post.js` file calling `definePost` (`post_api.d.ts`). Properties declared in the script appear in the UI and can be overridden per machine and per setup.

**Operation template (v1 shop knowledge)** and **shop rule (v2):**

```json
{ "schema": 1, "id": "<uuid>", "name": "Tap 1/4-20 in 6061", "opType": "drill", "featureKind": "hole.tapped",
  "material": "<uuid 6061>", "machine": null,
  "toolQuery": { "type": "tap", "threadSpec": "$feature.threadSpec" },
  "params": { "cycle": "tap", "depth": "$feature.threadDepth + 2 * $tool.threadPitch" } }

{ "schema": 1, "id": "<uuid>", "name": "Tapped holes", "priority": 100,
  "when": { "feature": "hole.tapped", "diameter": { "max": 12.7 } },
  "templates": ["<spot template>", "<tap drill template>", "<tap template>"] }
```

The v1 template format is the unit the v2 rule engine composes, so v1 shop knowledge carries forward unchanged.

---

## 4. Native file format and version-history storage

### 4.1 Container

One SQLite database per document, extension `.raycad`.

```sql
PRAGMA application_id = 0x52415943;   -- 'RAYC'
PRAGMA user_version   = 1;            -- format version
PRAGMA journal_mode   = DELETE;       -- rollback journal; WAL is unsafe on network shares
PRAGMA synchronous    = FULL;
PRAGMA locking_mode   = EXCLUSIVE;

CREATE TABLE meta    (key TEXT PRIMARY KEY, value TEXT NOT NULL);           -- file id (uuid), created-by version, units
CREATE TABLE objects (hash BLOB PRIMARY KEY, kind INTEGER NOT NULL,
                      raw_size INTEGER NOT NULL, data BLOB NOT NULL) WITHOUT ROWID;   -- zstd-compressed
CREATE TABLE commits (id BLOB PRIMARY KEY, root BLOB NOT NULL, author TEXT, time_ms INTEGER NOT NULL,
                      message TEXT, named INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID;
CREATE TABLE commit_parents (commit_id BLOB NOT NULL, ord INTEGER NOT NULL, parent BLOB NOT NULL,
                      PRIMARY KEY (commit_id, ord)) WITHOUT ROWID;
CREATE TABLE refs    (name TEXT PRIMARY KEY, commit_id BLOB NOT NULL);       -- heads/main, heads/<branch>, tags/<name>
CREATE TABLE derived (input_hash BLOB PRIMARY KEY, object_hash BLOB NOT NULL, kernel_version TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE thumbs  (commit_id BLOB PRIMARY KEY, png BLOB NOT NULL) WITHOUT ROWID;
```

The write lock is taken at open with an empty `BEGIN EXCLUSIVE; COMMIT`, because exclusive locking mode only takes effect at the first write [measured S10; the fix itself is untested]. In this mode SQLite keeps `<name>.raycad-journal` beside the document for the whole session and leaves it after a crash; the app, its cleanup code and any sync tool must never delete it or separate it from the document.

This is Git's model (content-addressed objects, commits with parents, movable refs) inside one file. v1 writes a commit on every save, so the storage for microversions, named versions, branches and merges exists from day one; v2 adds the UI, the visual diff and `merge3`.

**Source versus derived.** Source objects (manifest, features, sketches, CAM, imported BREP, embedded library items) are reachable from a commit root. Derived objects (rebuilt shapes, element maps, toolpaths, display meshes) live in `derived`, keyed by the hash of their inputs plus the kernel version. Deleting the whole `derived` table loses nothing but time. A kernel upgrade changes the key, so stale geometry is never reused.

**Partial load.** Opening reads `refs`, one commit row, the manifest object and the thumbnail (target under 200 ms regardless of file size). Shapes and toolpaths load from `derived` on demand, visible bodies first.

### 4.2 Crash safety

- **Save** is one SQLite transaction: insert new objects, insert the commit, move the ref. A crash mid-save leaves the previous commit intact (rollback journal, `synchronous=FULL`). [measured S10: 1,000 checked process kills during save, 347 with a hot journal, on local NTFS and loopback SMB, zero damaged files. Saving a 50 KB change to a 200 MB file took 8 to 17 ms and wrote 132 to 259 KB. A process kill does not test power loss; that and a real network share are still open.]
- **Between saves**, every executed command is appended to a recovery journal on the local disk (`<user data>/recovery/<file id>.journal`), fsync'd. On next launch the app offers to reopen the file at its last commit and replay the journal. Because commands are pure functions of (snapshot, args), replay reproduces the exact state. This is also the autosave: no timer, no partial files on the network share.
- **Corruption check.** Every object read verifies its hash. `PRAGMA integrity_check` runs on open if the previous session did not close the file cleanly.

### 4.3 Network shares and concurrent access

SQLite's own guidance is that locking over network file systems is unreliable [verified]. So: one writer per file, guarded by a lock file as well as SQLite's lock.

- **Lock file.** Opening for write creates `<name>.raycad.lock` with create-new semantics (fails if it exists). It holds user, machine, process id, start time, a heartbeat time rewritten every 30 s, and a **save counter**. The writer keeps the handle open without delete sharing, so a dead owner on the same server is detected at once.
- **Second opener** gets read-only mode and sees who holds the lock and since when.
- **Stale lock.** Heartbeat age is measured on the file server's clock (the modification time of a probe file created beside the lock, minus the lock file's modification time), never by comparing two clients' clocks. A lock older than 2 minutes, or one whose owner is provably dead, can be broken after a prompt: rename the stale file away, then create a new one.
- **Read-only access is a verified snapshot copy, not a live connection.** The writer's exclusive lock returns `SQLITE_BUSY` to everyone else for the whole session. A plain file copy taken during a save is usually corrupt [measured S10: 126 of 137 under continuous saving; copying the journal too does not help]. So the writer sets the save counter to "saving" before each save and to "idle, n+1" after it; the reader reads the counter, copies the file to a local temp path, reads the counter again, retries if a save overlapped, and runs the hash and integrity checks on the copy before opening it [measured S10: 0 bad of 194].
- Shared libraries avoid the problem by being one small file per item with atomic replace (`libraries.h`).

[measured S10] All of this passed on local NTFS and on a loopback SMB path. **It has not been run on a real file server or NAS**, with power loss, a pulled cable, clock skew or two machines racing to break a lock. T-011 repeats the S10 programs on a real share before M1 ends (section 15).

### 4.4 Format migration

- `user_version` is the container version; every JSON object has its own `"schema"` integer.
- Migrations are forward-only single steps (`Migration{from,to}`), run inside a transaction on a copy. The original is kept as `<name>.v<N>.bak` until the migrated file passes an integrity check and a load test.
- A file newer than the app returns `FormatTooNew` and is never modified. The message names the version needed.
- Unknown JSON keys are preserved on round-trip, so a newer minor version's data survives an older app that can still read the file.
- CI keeps one fixture file per released format version and proves each still opens and rebuilds to the same geometry hash.

### 4.5 History growth

Microversions older than a configurable age (default 90 days) that are not named versions are pruned by `compact()`, which also drops unreachable objects and vacuums. Named versions are never pruned. Content addressing means an unchanged 40 MB imported BREP is stored once no matter how many commits reference it.

---

## 5. Module interfaces

The contracts are the headers in `docs/interfaces/`. When implementation starts, task T-001 copies them to `include/ray/` and they become the real headers; from then on a change to any of them is its own SEQUENTIAL task.

| Header | Key names tasks refer to |
|-|-|
| `core.h` | `Id<>`, `Hash256`, `Hasher`, `Result`, `Error`, `Nm`, `dmath`, `CancelToken`, `Progress`, `JobContext`, `IJobSystem`, `IUiDispatcher` |
| `kernel.h` | `Shape`, `SubId`, `ShapeHistory`, `OpResult`, `importStep`, `extrude`, `boolean`, `fillet`, `deleteFaces`, `blendRun`, `offsetFaces`, `resizeHole`, `Heal`, `tessellate`, `triangulateExact`, `sliceAt`, `silhouette` |
| `naming.h` | `TopoName` (with `discriminator`, `siblingCount`, `family`), `Fingerprint`, `ElementMap`, `LineageEvent`, `propagate`, `nameImported`, `rematchImported`, `TopoRef`, `ResolveState` (`Exact`, `Split`, `Merged`, `Repaired`, `Broken`), `Resolved::usable`, `IResolver` |
| `sketch.h` | `Sketch`, `Entity`, `Constraint`, `ISketchSolver`, `IDragSession`, `inferConstraints`, `closedLoops`, `importDxf`, `exportDxf` |
| `document.h` | `DocState`, `Feature`, `Parameter`, `IFeatureType`, `FeatureRegistry`, `IRebuilder`, `BuildResult`, `Command`, `ICommandHandler`, `IDocument`, `canonicalJson` |
| `store.h` | `IObjectStore`, `IDocumentFile`, `OpenInfo`, `LockInfo`, `breakStaleLock`, `Commit`, `Ref`, `IRecoveryJournal`, `Migration`, `merge3` |
| `cam.h` | `Setup`, `StockDef`, `Wcs`, `Operation`, `Move`, `Toolpath`, `IOperationType`, `OperationRegistry`, `ICamEngine`, `linkPasses`, `recognizeHoles`, `computeFeeds`, `estimateCycleTime` |
| `libraries.h` | `Tool`, `Holder`, `Material`, `MachineDef`, `OpTemplate`, `ShopRule`, `ILibrary`, `LibraryStack` |
| `sim.h` | `Backplot`, `IStockModel` (`cutBatch`, `snapshot`), `ToolShape`, `SimEvent`, `ISimulator` |
| `post.h`, `post_api.d.ts` | `PostInfo`, `PostJob`, `PostOutput`, `IPostEngine`, `formatNumber`, `writeProgram`, `renderSetupSheetHtml`, `definePost` |
| `render.h` | `HardwareInfo`, `Tier`, `TierPreset`, `Token`, `Theme`, `IViewport`, `PickHit`, `IComputeBackend` |
| `api.h` | `ActionInfo`, `IActionRegistry`, `IApp`, `makeHeadlessApp` |

These headers are a plan draft, revised after the spikes (each revised header says so in its status line). They have been read for consistency but **not compiled**; T-001 makes them compile and reports any signature it had to change.

---

## 6. Rendering and tier system

### 6.1 Hardware detection and tier choice

`detectHardware()` runs once after the first window exists and reads the adapter name, driver, VRAM, RAM, core count and RHI feature flags. On Windows the adapter type and VRAM come from DXGI, because Qt's RHI reports device type `Unknown` for real adapters and gives no VRAM [measured S1]; macOS and Linux use the platform equivalents. A software rasteriser (WARP, llvmpipe) is always Low, with MSAA and glass off and a visible notice: WARP drew the 500k-triangle scene at 22 FPS on a 24-core desktop [measured S1], below the Low budget. `chooseTier`:

- **Low** if the renderer is software, or the GPU is integrated, or VRAM is under 3 GB, or RAM is under 12 GB.
- **High** if the GPU is discrete, VRAM is 8 GB or more, RAM is 24 GB or more and compute shaders are available.
- **Medium** otherwise.

On first launch the app then renders a fixed 500k-triangle test scene for one second. If the measured frame time misses the tier's budget, it drops one tier. The result and the driver version are cached; a driver change triggers a re-check. Settings show the detected hardware, the chosen tier, and let the user override the tier or any single option. Overrides live in user settings, never in documents.

### 6.2 Presets (one codebase, data only)

| Setting | Low | Medium | High |
|-|-|-|-|
| Mesh chord / angle tolerance (display) | 0.2 mm / 20° | 0.05 mm / 12° | 0.01 mm / 6° |
| Anti-aliasing | off; edges drawn as 1 px lines | MSAA 4x | MSAA 4x plus smooth edges |
| Shading | Flat/Phong, silhouette edges | plus transparency, section caps | plus ambient occlusion, shadows |
| Liquid Glass default | off (flat opaque panels) | off | on |
| Simulation cell size (upper bound; snapped per axis so the stock is spanned exactly) | 0.2 mm [estimate from S6] | 0.1 mm [interpolated] | 0.05 mm |
| Simulation backend | CPU tri-dexel | CPU tri-dexel, multithreaded | GPU compute (v2); CPU on 8 or more threads until then |
| Backplot display limit | 2 M segments | 8 M | 32 M |

Display numbers are starting values [guess]. [measured S1] On UHD 770, MSAA 4x plus glass still gave 147 FPS, so frame rate is not the reason glass defaults to off on Low and Medium; the default stays off until a UHD 620 run, and it is Jake's call. [measured S6] Simulation cell sizes: 100k moves took 0.18 s at 0.5 mm, 0.88 s at 0.2 mm and 14.7 s at 0.05 mm on 4 threads, with 2, 13 and 214 MB of stock; the Low value assumes a laptop 3.5x slower. A preset is a `TierPreset` struct. Code never tests `tier == Low`; it reads a preset field. That is what "without forking code" means here.

### 6.3 Rendering abstraction

`render::IViewport` (`render.h`) is the only thing the rest of the app sees. It takes meshes, polylines and a backplot; it returns picks and stats. Behind it:

- `src/render/rhi/` holds every Qt RHI call: pipelines, buffers, the ID-buffer pass, the blur pass.
- Shaders are written once in Vulkan-style GLSL and compiled at build time by `qsb` to HLSL, MSL and GLSL.
- **Draw on demand.** The viewport renders only when the scene, camera, theme or highlight changes. An idle app uses no GPU, which matters on laptops.
- **Picking.** A second render target writes (item id, sub-shape index) per pixel. It is re-rendered only when the camera or scene changed, then read back around the cursor. No CPU ray casting against B-rep.
- **Large models.** One vertex buffer and one draw call per body, with per-face ranges for highlight; edges as a separate line buffer. 500k triangles is a few draw calls.
- **Dexel stock display.** `IStockModel::toMesh` returns only the dirty region; `updateMesh` patches the buffer.

### 6.4 GPU compute without forking code

`sim::IStockModel` has two implementations behind one interface: `makeTriDexelCpu` (v1) and `makeTriDexelGpu` (v2) on `render::IComputeBackend`. The simulator, timeline, collision checks and UI call `IStockModel` and never know which one they have. The safety verdicts that gate posting are computed analytically from the toolpath and exact triangle meshes (`ISimulator::verifyFast`), so they are identical on every tier. The GPU model must agree with the CPU model at the same cell size to within one cell; a v2 CI test checks this.

### 6.5 Theme tokens

- A theme is a JSON file in `resources/themes/` (`graphite-dark.json`, `graphite-light.json`) mapping every `render::Token` to a colour, plus metrics (radii, spacing, font sizes) and font families. One file per theme; nothing else in the codebase contains a colour literal. A CI grep fails the build on hex colours outside `resources/themes/`.
- Starting tokens from the approved style preview: accent `#F5A524`; accent thin lines in light mode `#B36A00`; selection `#4C9AFF` dark / `#1F6FE0` light; rapids dashed red; Inter 13 px; JetBrains Mono for numbers and G-code.
- QML reads tokens from a `Theme` singleton whose properties notify on change, so switching theme at runtime repaints everything with no restart. The viewport gets the same theme through `IViewport::setTheme`. The default follows the OS setting and tracks it live.
- `loadTheme` validates contrast: text tokens at least 4.5:1 against panel backgrounds, thin-line tokens at least 3:1 against the viewport background. A theme that fails does not load, and a unit test runs this on both shipped themes.

### 6.6 Liquid Glass and its flat fallback

- **Layout is identical in both modes.** Panels float over a full-window viewport. Only the panel background component differs: `GlassPanel` or `FlatPanel`, chosen by one setting.
- **One blur per frame, shared.** After the viewport renders, if the backdrop is enabled and the image changed, the renderer downsamples it to quarter resolution and runs a two-pass separable blur into one texture (`IViewport::blurredBackdropTexture`). Every glass panel is a small shader that samples that texture at its own screen position, adds the theme tint, a top-edge highlight and a border. There is no per-panel blur and no `MultiEffect` per panel.
- **Re-blur only when the view changes.** Because the viewport draws on demand, a static view costs nothing; panels re-composite from the cached texture.
- **Contrast.** The tint opacity is set so the worst-case backdrop (pure white or pure black behind the panel) still gives 4.5:1 for text. `loadTheme` checks this arithmetic.
- **Flat mode** draws an opaque `PanelBg` rectangle with the same radius and border, and the backdrop pass is switched off entirely.
- Qt's own guidance is that blur is among the heaviest effects and should not be regenerated per frame [verified]; this design follows it. [measured S1] The shared blur cost 0.29 ms per changed frame on UHD 770 (about 8%). `QQuickWindow::createTextureFromRhiTexture()` takes ownership of the texture it is given, which caused a double free on resize in the spike; the ownership rule is in `render.h`.

---

## 7. Threading model

| Thread | Runs | Rules |
|-|-|-|
| UI thread | QML, input, `IDocument::execute`, undo/redo, snapshot pointer swaps, scheduling jobs | Never blocks for more than 16 ms. No OCCT calls except trivial queries on cached data. |
| Render thread | Qt Quick scene graph, `src/render/rhi` | Receives immutable mesh buffers at the sync point. Never touches document or kernel objects. |
| Worker pool (oneTBB) | STEP import, rebuild, tessellation, toolpath generation, simulation, hashing, save | Reads immutable snapshots. Returns results to the UI thread through `JobHandle::then`. |
| Post thread | One QuickJS-NG runtime per post run | Runtime is created and destroyed on that thread. JS stack limit 256 KB on a thread stack of at least 1 MB. Cancel and the CPU limit act through the engine's interrupt poll and can lag by the length of one native call (section 13, T-039). |
| Sketch drag worker | One live PlaneGCS system per drag session | Always solves for the newest pointer position and drops stale ones; the UI thread never waits for it. |

**Snapshots make this simple.** `DocState` and `BuildResult` are immutable and shared by `shared_ptr<const>`. Workers never see a document change under them. When a job finishes, the UI thread checks whether its result still matches the current snapshot's input hash; if not, the result goes into the cache and is otherwise ignored.

**Cancellation and progress.** Every job has a `CancelSource`. Esc cancels the foreground job. A newer edit cancels a now-pointless rebuild or preview automatically. Jobs check the token in their loops; kernel calls pass a `Message_ProgressIndicator` subclass whose `UserBreak()` reads the token. [verified] Booleans, meshing, offsets and STEP transfer accept a progress range; **the STEP model-load stage does not.** [measured S3] Reading through `ReadStream` with our own stream buffer makes the first quarter of the read cancellable within 5 ms; the rest (about 30 ms per MiB: 1.2 s at 47 MiB, 2.5 s at 95 MiB) cannot be cancelled and runs under an OCCT global mutex. On cancel during that stage the job is **abandoned**: the UI returns at once and the result is discarded when the call ends. Import shows "Reading file" as an indeterminate step, then real progress for transfer, healing and meshing. Worst cancel latencies seen: transfer 0.1 s, healing 0.8 s, meshing 1.9 s at the Low tolerance and 13.9 s at the High tolerance (it follows the slowest face), so a cancelled refinement mesh is also abandoned, not waited for.

**OCCT thread-safety rules** [verified from the 8.0 sources and release notes; the user guide makes no blanket promise]:

1. A `kernel::Shape` is immutable after creation. Operations build new shapes.
2. Different shapes may be processed on different threads at the same time. Handle reference counts are atomic.
3. The same shape is never passed to two concurrent mutating calls. Tessellation writes triangulation into the shape's faces, and [measured S3] OCCT 8.0.1 BRepMesh is **not safe when two shapes are meshed at the same time**: one mesh job per body crashed with an access violation in 2 of 15 runs on a 95 MiB file and once left 119 faces without triangles with no error. So `tessellate` and `triangulateExact` take a **process-wide mesh lock**: one mesh call at a time, with OCCT's `InParallel` on inside it (bit-identical to single-threaded in 83 runs, 8x faster). The kernel caps the number of faces meshed concurrently to bound memory (15.6 GB peak was seen with 32 threads at the High tolerance), uses the default (Watson) mesher only, never Delabella in parallel, and reports faces that received no triangles, because BRepMesh's status flags cannot be trusted. Results are copied into our own `Mesh`/`TriSoup`; nothing outside the kernel reads OCCT's triangulation.
4. OCCT's internal parallel flags (`SetRunParallel`, `InParallel`) are **off** for everything that produces geometry, for determinism, with the one exception of meshing in rule 3, which CI checks by comparing mesh hashes against a single-threaded run. Parallelism otherwise comes from running independent features, bodies and operations side by side.
5. STEP parameters are passed per call, not through global statics.
6. Every kernel entry point wraps OCCT in `try/catch` (`Standard_Failure` derives from `std::exception` in 8.0) and returns `KernelFailure`. Catching is not enough: [measured S9] `BRepAlgoAPI_Defeaturing` reported success while doing nothing in 69 of 489 attempts and returned an invalid or wrong body in 22, and `ShapeUpgrade_UnifySameDomain` turned 10 of 496 valid results into invalid solids. Every modifying kernel function therefore **validates its result** (`BRepCheck`, closed solid, volume changed in the expected direction) and **detects no-ops**, and returns a failure otherwise. `BRepOffset_MakeOffset` is not used: it raised stack overflows and access violations.
7. A CI stress test runs fillets, booleans and imports on 8 threads over the corpus under ThreadSanitizer on Linux. OCCT 8.0.1 fixed a shared-state bug in concurrent fillets, so we treat this as "intended but verify".

**Rebuild parallelism.** Features of one body form a chain and run in order. Chains of different bodies run in parallel. The result is assembled in feature order, so thread count never changes the output.

---

## 8. Determinism strategy

**Guarantee.** For a given RayCAD build, the same document produces byte-identical toolpath IR and G-code regardless of hardware tier, thread count, GPU, or machine. Across Windows, Linux and macOS builds, the G-code text is identical. B-rep internals may differ in the last bits across operating systems.

How each source of nondeterminism is closed:

| Source | Mitigation |
|-|-|
| Hardware tier | Tier presets live in `src/render` and are fed only to display tessellation and simulation resolution. `src/cam` and `src/post` cannot link `src/render` (CMake check). CAM triangulation tolerance comes from the operation's own tolerance parameter. |
| Thread count | `IJobSystem::parallelChunks` splits work by a fixed grain, not by thread count, and merges in chunk order. No parallel reductions on floating point. OCCT internal parallelism off. |
| Floating-point contraction and fast math | `/fp:strict` on MSVC; `-ffp-contract=off -fno-fast-math` on Clang and GCC; no x87; FMA disabled in deterministic modules. |
| Math library differences | CAM, sim-verdict and post code call `ray::dmath` (vendored, identical on all platforms), never `<cmath>` trig. Lint rule in CI. |
| 2D geometry | Offsets and pocketing run in Clipper2 integer coordinates at 1 nm resolution. Integer results are exact and identical everywhere. The adaptive engine runs on its own coarser integer grid chosen from its tolerance (480 units per mm at the default, about 2 µm) because its step constants are tied to that grid; its output is converted to integer nanometres at the boundary [measured S5]. |
| Adaptive engine (forked code) | The upstream code decides by `clock()`, uses 32-bit `long` on Windows, calls `<cmath>` trig, shares file-scope counters between instances and returns uninitialised fields on failure [measured S5]. T-060 removes each of these; CI runs 32 concurrent instances under a time limit. |
| Coordinates leaving CAM | Every toolpath coordinate is rounded to integer nanometres (`toNm`, round-half-even) at the `cam::Toolpath` boundary. Posts format from those integers. Sub-nanometre differences in B-rep evaluation between operating systems vanish here, except when a value sits exactly on a rounding boundary, which the cross-OS CI comparison will catch. |
| Container ordering | No iteration over `unordered_map`/`unordered_set` or pointer-keyed containers in deterministic code; sorted vectors or `std::map`. Ties in sorts are always broken by a stable key (id or fingerprint). |
| Ids and time | Ids are random UUIDs but never influence geometry or ordering of output. Generators receive no clock. Posts get the date only when `includeDate` is on. |
| Number formatting | `post::formatNumber` does decimal rounding itself; it never calls `printf` with locale. |
| Kernel version | Derived-data cache keys include the OCCT version; golden files are regenerated and reviewed when OCCT is upgraded. |
| Sketch solver | PlaneGCS with fixed iteration limits and a fixed starting point (stored coordinates). [measured S7] Results are bit-identical only if the adapter (1) keeps all solver parameters in one contiguous array in canonical entity order, because PlaneGCS sorts by pointer value, (2) calls `Eigen::setCpuCacheSizes` with fixed values at start-up, because Eigen sizes its blocks from the detected cache and P-cores and E-cores of one CPU gave different bits, and (3) is built with one fixed set of floating-point and SIMD flags. PlaneGCS's `diagnose()` uses `std::async` internally; results did not depend on it. |

**CI proof** (required by the brief):

1. `determinism-tiers`: for every corpus part, generate all toolpaths and G-code with the Low, Medium and High presets. Hashes must match exactly.
2. `determinism-threads`: same with 1, 2 and 16 worker threads.
3. `determinism-repeat`: 5 runs in separate processes with ASLR on. Hashes must match.
4. `determinism-cross-os`: Windows, Linux and macOS jobs upload their G-code; a final job compares the text byte for byte. Toolpath IR hashes are compared too and any mismatch is reported; a mismatch in IR with matching G-code is a warning to investigate, a G-code mismatch fails the build.

---

## 9. Performance budgets per tier

| Metric | Low | Medium | High | How CI measures it |
|-|-|-|-|-|
| Warm start to interactive start screen | 2.5 s [estimate] | 1.5 s [estimate] | 1.0 s [measured S2: 0.7 s for a UI skeleton] | `--bench-startup`: time from OS process creation (not `main()`) to the first frame swapped; median of 10 launches after 2 discarded. |
| Cold start (first launch after boot) | 3.0 s [guess] | 2.0 s [guess] | 1.5 s [guess] | Same, after a reboot. Not measured by S2. First launch after an install took 1.4 to 3.1 s on High and is not gated. |
| Open 50 MB STEP to first shaded view (meshed at 0.2 mm / 20° on every tier) | 40 s [estimate] | 15 s [estimate] | 10 s [measured S3: 9.7 s] | `raycad-cli bench import` on the corpus file; plus UI-thread stall monitor must show no stall over 50 ms. Provisional: S3 used synthetic files; no real file above 6 MiB was measured. |
| Background refinement of that file to the tier tolerance | n/a (already at it) | 10 s more [estimate] | 15 s more [measured S3: 15.5 s] | Same harness, second timestamp. |
| Import peak memory, 50 MB STEP | 3 GB | 4 GB | 6 GB | Peak working set. The kernel caps concurrent faces to stay inside this (S3 saw 2.7 to 15.6 GB uncapped). |
| Open a saved `.raycad` (typical part, CAM program) | 1.0 s | 0.7 s | 0.5 s | `--bench-open`. |
| Viewport, 500k triangles, orbit [measured S1: 211 FPS on UHD 770 at 1080p; UHD 620 estimated 105 to 141 FPS, not confirmed] | 30 FPS | 60 FPS | 60 FPS at 4K | Scripted orbit, `IViewport::lastFrameStats`, 95th percentile frame time. Needs real GPUs. |
| Same with glass panels on | 30 FPS (when enabled) | 60 FPS | 60 FPS | As above with backdrop enabled. |
| Idle GPU use | 0 frames/s | 0 | 0 | Frame counter over 5 s idle must be 0. |
| Incremental rebuild, 100-feature part, edit feature 50 | 1.0 s | 0.7 s | 0.5 s | `raycad-cli bench rebuild`. |
| Sketch drag step, one coupled group of up to 50 curves | 33 ms [estimate] | 16 ms | 16 ms [measured S7: 8 to 16 ms] | Micro-benchmark on the S7 sketch. Larger coupled groups are solved on the drag worker and follow the pointer late: 54 to 81 ms at 100 curves, 0.4 to 0.8 s at 200 [measured S7]. The UI thread never blocks. Spike S11 looks for a faster drag. |
| Sketch full solve with diagnosis, 200 curves | 2 s [estimate] | 1 s | 1 s [measured S7: 0.65 s] | Micro-benchmark. Always on a worker. |
| 2D adaptive, BP2 circular pocket | 2 s | 1 s | 0.5 s [measured S5: 0.26 s] | `bench toolpath`. |
| 2D adaptive, 90 x 60 mm pocket with two islands | 12 s | 6 s | 3 s [measured S5: 4.3 s today] | `bench toolpath`. |
| Backplot build, 1 M moves | 150 ms | 100 ms | 60 ms | Micro-benchmark. |
| Material-removal sim, 100k moves at tier resolution | 20 s [S6 estimate: 3 s] | 15 s | 15 s on 4 threads, 10 s on 8 or more [measured S6: 14.7 s and 8.8 s] | `bench sim`. |
| Timeline seek | 250 ms | 150 ms | 100 ms, which depends on T-070's keyframes not being full copies (a full 0.05 mm copy is 214 MB and 57 ms [measured S6]) | `bench seek`. |
| Post 1 M moves | 5 s | 3 s | 2 s [measured S8: 0.68 s for a toy post] | `bench post`. |
| Memory, typical part with full CAM program | 1.5 GB | 2.5 GB | 4 GB | Peak working set from the bench harness. |
| UI-thread stall | 50 ms max | 50 ms | 33 ms | Watchdog thread logs any event-loop gap. |
| Installer size | under 300 MB | same | same | CI checks the artefact size. |

**Measurement reality.** GitHub's free runners have no GPU and are noisy [verified]. So:

- CPU-only budgets run on hosted runners on every pull request, compared against a stored baseline; a regression over 20% fails. Absolute pass/fail against the table is checked on the reference machine.
- GPU and absolute budgets need a **self-hosted Low-tier runner** (UHD 620-class laptop). Until one exists, an agent cannot verify them; they are checked by hand at each milestone demo. This is open question 3.
- Rendering correctness (not speed) is tested on hosted runners with software rasterisers: WARP on Windows, Mesa llvmpipe on Linux.

---

## 10. Repository layout, build and dependencies

```
RayCAD/
  BRIEF.md  PLAN.md  LICENSE  THIRD_PARTY.md  README.md  CONTRIBUTING.md
  CMakeLists.txt  CMakePresets.json  vcpkg.json  vcpkg-configuration.json
  docs/interfaces/            contract drafts (this plan)
  include/ray/                public headers (copied from docs/interfaces by T-001)
  src/
    core/  kernel/  naming/  sketch/  lib/  doc/  store/  cam/  sim/  post/  api/
    cam/ops/                  one file pair per operation type
    doc/features/             one file pair per feature type
    render/  render/rhi/  render/shaders/
    ui/  ui/qml/  ui/qml/panels/  ui/qml/controls/
  apps/raycad/  apps/raycad-cli/
  third_party/planegcs/  third_party/dmath/
  resources/
    themes/  fonts/  icons/        (icons are original or permissively licensed; ledger in THIRD_PARTY.md)
    library/tools/ holders/ materials/ machines/ posts/ templates/
  schemas/                    JSON Schemas for every plain-text file type
  tests/
    unit/<module>/  golden/toolpaths/  golden/gcode/  golden/geometry/
    corpus/                   STEP/IGES/DXF test files with a manifest and licences
    benchmarks/  ui/
  spikes/                     throwaway spike code; never linked into the product
  tools/                      golden-file updater, include checker, licence checker
  packaging/windows/ linux/ macos/
  .github/workflows/
```

**Build.** CMake presets `windows-msvc`, `linux-clang`, `linux-gcc`, `macos-arm64`, each in Debug, RelWithDebInfo and Release, plus `linux-clang-tsan` and `linux-clang-asan`. One static library per module, plus the two executables. Module dependency rules from section 1.1 are written as `target_link_libraries(... PRIVATE ...)` and checked by a script.

**Dependencies.** `vcpkg.json` with a pinned baseline: opencascade 8.0.1, tbb, eigen3, sqlite3, zstd, blake3, nlohmann-json, clipper2, quickjs-ng, catch2, tl-expected, boost-graph. Binary caching through a NuGet feed on GitHub Packages (the old GitHub Actions cache backend was removed [verified]). Qt 6.12 comes from prebuilt binaries via `install-qt-action`, cached. PlaneGCS and dmath are vendored.

**CI on every pull request** (GitHub Actions, free for public repositories [verified]):

| Job | Runs on | Does |
|-|-|-|
| build-test | windows-2025, ubuntu-26.04, macos-26 | Configure, build, unit tests, golden tests |
| determinism | all three, then a compare job | Section 8 |
| sanitizers | ubuntu | ASan/UBSan unit tests; TSan kernel stress |
| lint | ubuntu | clang-format, include rules, dmath rule, colour-literal rule, licence ledger |
| bench | ubuntu, windows | CPU budgets against baseline |
| render-smoke | ubuntu (llvmpipe), windows (WARP) | Offscreen render of test scenes, image compare with tolerance |
| package | all three, on main and tags | Installer, AppImage, dmg; size check |
| nightly | self-hosted Low-tier runner, when available | Absolute budgets, GPU frame rates, full corpus |

**Signing.**

- *Windows:* apply to SignPath Foundation after the first public release. [verified] It is free for open source, but requires a public repo, an OSI licence with no commercial dual licensing, MFA for all maintainers, a published code-signing policy, builds from CI, and a manual approval per release; acceptance is at their discretion; the publisher shows as "SignPath Foundation". A signed build still shows SmartScreen warnings until reputation accumulates over weeks of installs. No other free option exists.
- *macOS:* unsigned until v1 ships (agreed, question 7). [verified] Users must go to System Settings, Privacy and Security, and click Open Anyway; the old Control-click shortcut is gone. The download page will explain this. Notarization needs the $99/year programme.
- *Linux:* AppImage with a published SHA-256 and a detached signature from a project key.

---

## 11. Testing strategy

| Layer | What | Where | Pass rule |
|-|-|-|-|
| Unit | Every module, every public function | `tests/unit/<module>` | All pass on three OSes |
| Kernel robustness | Import, heal, tessellate, slice and hash every file in the STEP corpus | `tests/corpus` | No crash, no hang over the time limit; a tracked pass-rate that may only go up |
| Geometry regression | Build reference parts from feature lists; compare volume, area, bounding box, face/edge counts, and element-map hash | `tests/golden/geometry` | Volume and area within 1e-9 relative; counts and map hash exact per OS |
| Naming | Edit-and-resolve scenarios from section 3.3: scripted upstream edits, then assert each reference's `ResolveState` and that it still points at the element with the expected fingerprint | `tests/unit/naming` | Zero wrong-element resolutions, ever |
| Property tests | Random sketches solve to satisfied constraints; random pockets produce toolpaths that stay inside the boundary and never exceed the engagement limit | `tests/unit/sketch`, `tests/unit/cam` | 10,000 seeded cases |
| Toolpath golden | Canonical `Toolpath` bytes for every operation type on fixed inputs | `tests/golden/toolpaths` | Byte-identical. Changes need a reviewed update via `tools/update-golden` with a rendered before/after image |
| Toolpath invariants | Independent checker, not sharing code with generators: no rapid below the feed plane inside stock; no move outside containment plus tolerance; engagement angle within limit for adaptive; lead-ins tangent; cutter comp balanced | `tests/unit/cam/invariants` | All hold on all golden and property inputs |
| G-code golden | Every shipped post against every benchmark part and a per-post feature matrix (each cycle, arc quadrants, helical, tool changes, units, sequence numbers on/off) | `tests/golden/gcode` | Byte-identical on all three OSes |
| G-code round-trip | A small independent G-code interpreter (test-only in v1; grows into the v2 control emulator) parses posted output back to moves and compares to the toolpath IR | `tests/unit/post/roundtrip` | End points within 1 output unit of least count; no unknown words |
| Simulation correctness | Analytic cases: slot volume, drilled hole volume, pocket volume within one cell layer of exact; known-collision and known-gouge programs must raise exactly the expected events; known-safe programs raise none | `tests/unit/sim` | As stated |
| File format | Save/load round trip equals by hash; kill-during-save loop (terminate the process at random points 500 times, file must always open at old or new commit); journal replay; migration fixtures | `tests/unit/store` | As stated |
| Determinism | Section 8 | CI | As stated |
| UI | Qt Quick Test for panels and controls; scripted end-to-end runs through `raycad --script` driving `IActionRegistry` (import, program, post, compare G-code) | `tests/ui` | Pass on Windows and Linux with software rendering |
| Performance | Section 9 | `tests/benchmarks` | As stated |
| On-machine | Benchmark parts cut on the Mazak, Haas and Fanuc machines | Milestone M8 checklist | Jake signs off |

**STEP corpus.** Target 300 files by M1 and 1,000 by M8, with a manifest recording source, licence and known defects: NIST PMI test models and the CAx-IF test-case library (public), FreeCAD and OCCT regression files (LGPL), ABC-dataset samples (check its licence per file), exports we generate from FreeCAD in several AP versions, and anonymised customer-style parts Jake can contribute. Each file is tagged with the exporting CAD system, because failures cluster by exporter. Files we cannot redistribute are fetched by script and not committed.

---

## 12. Milestone roadmap for v1

Every milestone ends in a build Jake can install and a demo of one workflow. Durations assume about 20 hours a week of Jake's review time with agents implementing. Review, not typing, is the bottleneck.

| # | Name | Demo at the end | Estimate [guess] |
|-|-|-|-|
| M0 | Spikes and skeleton | Ten spike reports with measured numbers (**done 2026-11-17**, `spikes/RESULTS.md`); an empty app window on all three OSes from CI. | 2 weeks left |
| M1 | Open and look | Start screen. Open a 50 MB STEP with progress and cancel. Orbit, select faces and edges, measure, section, switch theme and tier at runtime, toggle glass. Save and reopen as `.raycad`. Kill the app and recover. | 6 weeks |
| M7 | Model it (**runs second**, see below) | Sketch with constraints and dimensions, DXF import, extrude, cut, revolve, fillet, chamfer, hole feature, named parameters. Model a simple fixture plate from scratch, save it and export STEP. Programming it waits for M2. | 9 weeks |
| M2 | First G-code | Create a setup with stock and WCS on an imported part. Holes are recognised; spot, drill and tap them with tools from the library. Backplot. Post for Haas, Fanuc, Mazak and generic ISO. | 6 weeks |
| M3 | 2.5D milling | Face, contour with leads and tabs, pocket, slot, chamfer, engrave, thread mill. Feeds and speeds calculator. Save an operation as a shop template and reuse it on another part. | 5 weeks |
| M4 | Adaptive | 2D adaptive roughing with helix entry on the benchmark pockets, and a ramp entry in a narrow slot; engagement plot shows constant load; cycle time compared against the plain pocket. | 6 weeks |
| M5 | Prove it | Material-removal simulation with timeline; collisions and rapids into stock flagged and clickable through to the G-code line; second setup starts from the first setup's simulated stock; setup sheet PDF; save to a network folder with naming rules. | 6 weeks |
| M6 | Fix the model | On a customer STEP: delete a fillet, resize a hole, push a face. Toolpaths go stale and regenerate; nothing is silently wrong. Replace the STEP with a new revision and review what changed. | 5 weeks |
| M8 | Harden and cut | Budgets met on Low tier, installers for three OSes, benchmark parts cut on the Mazak, Haas and Fanuc machines with no hand edits. **v1 exit gate.** | 6 weeks |

**Total: about 50 weeks from now, so roughly 11 to 12 months.** M4 grew by two weeks because the adaptive code needs reworking before it is safe to run in parallel and it has no narrow-slot entry (S5); the spikes themselves took a day, not three weeks. Confidence: about 50% that the exit gate lands within 12 to 16 months. The spikes removed the renderer risk and confirmed the file format, but they also showed that naming needs more states and more review UI than planned, that sketch dragging needs a second look (S11), and that direct editing cannot promise fillet-run removal. The three things most likely to move it are persistent naming (M6, M7), adaptive quality (M4), and on-machine proving (M8), which depends on machine availability I cannot estimate. If the schedule must shrink, cut M7 down to "sketch plus extrude/cut" before touching anything else.

**Execution order (revision 3, 2026-11-18, Jake's decision): M1, M7, M2, M3, M4, M5, M6, M8.** Milestone numbers are kept as stable labels, like task ids, so nothing else in this plan is renumbered. Jake asked to be able to make a part before CAM. What that changes:

- Parametric modeling now comes before direct editing, the reverse of the first plan. Naming therefore meets sweeps, booleans and blends first, and the history-free direct-edit case later. T-081 is split so its core no longer waits for T-080 (see T-081a, T-081b).
- The hole feature ships in M7 without CAM thread pickup; that part moves to T-097b in M2.
- S11 runs first, then T-090. T-094 starts at once.
- M7 grows by about a week for the T-081a work pulled into it; M6 shrinks by the same amount. The total is unchanged.
- The v1 exit gate (M8) does not move in content, but the first G-code arrives about nine weeks later than first planned.

**v1.x after the gate:** shell, draft, linear and circular pattern, mirror, split body, Okuma/Siemens/Heidenhain posts.

### 12.3 Proposed benchmark parts (need Jake's approval)

All three are 2.5D, inch, held in a 6-inch vise, and designed so each machine cuts at least one.

| Part | Material and stock | Setup 1 | Setup 2 | Covers |
|-|-|-|-|-|
| **BP1 Fixture plate** | 6061-T6, 6.25 x 4.25 x 0.75 in saw-cut | Face top. Adaptive rough then finish-contour the 6.00 x 4.00 perimeter, full depth, with four tabs. Two closed rectangular pockets 0.375 deep with 0.25 corner radii (adaptive, then finish wall and floor). Four 1/4-20 UNC tapped through holes (spot, #7 drill, tap). Four 0.266 through holes with 0.44 x 0.26 counterbores. 0.02 chamfer on top edges. | Flip. Face to 0.625 thickness. Chamfer the perimeter. | Facing, adaptive pocketing, contour with tabs, drilling, counterbore, rigid tapping, chamfer, second setup, in-process stock |
| **BP2 Manifold block** | 6061-T6, 3.25 x 3.25 x 2.1 in | Face. Adaptive rough the outside to 3.00 square, 1.25 deep, then finish. One open-sided pocket (step) 0.5 deep. A circular pocket 1.500 diameter, 0.75 deep (helix entry adaptive, finish contour). Four M6x1 tapped blind holes 0.5 deep. One 0.500 reamed hole (spot, drill 31/64, ream). Engrave a part number. | Flip onto the finished square. Face to 2.000. Adaptive and finish the remaining 0.75 of the outside. Two 3/8-16 UNC holes thread-milled 0.6 deep. | Open and closed adaptive, helix entry, metric taps, reaming, engraving, thread milling, deep contour, second setup located on machined faces |
| **BP3 Steel bracket** | 4140 pre-hard (28-32 HRC), 4.25 x 2.25 x 1.0 in | Face. Adaptive rough a stepped profile with a 0.375-wide through slot. Finish contour with a spring pass, cutter compensation in control. Two 10-32 UNF tapped holes, peck drilled. 0.03 chamfer. | Flip. Face to 0.875. Pocket 0.25 deep on the back face. Deburr chamfer. | Steel feeds and speeds with chip thinning and power limits, slotting, wear compensation (G41/G42), peck cycles, UNF taps, finishing strategy |

Suggested assignment: BP1 on the Haas, BP2 on the Mazak, BP3 on the Fanuc machine, then BP1 on all three as the common part. Tolerances for sign-off: general ±0.005 in, the reamed hole +0.0005/-0, pocket depth ±0.002 in, threads gauge with go/no-go.

---

## 13. Task breakdown for v1

Format: every task lists goal, files it may touch, interfaces, dependencies, mode, acceptance and size. **SEQUENTIAL** tasks run one at a time across the whole project, in ID order; **PARALLEL-SAFE** tasks may run alongside anything once their dependencies are done. Sizes: S under 1 day of agent work, M 1 to 3 days. Nothing is L; anything that grew was split.

Every task also has these acceptance criteria, not repeated below: builds on all three OS presets, `ctest` passes, lint job passes, new code has unit tests, and no file outside the listed paths changed.

### M0: spikes (all PARALLEL-SAFE, code lives only in `spikes/<id>/`, output is `spikes/<id>/REPORT.md`)

**S1 to S10 were run on 2026-11-17.** The entries below are kept as the record of what was asked; results are in section 0.5 and `spikes/RESULTS.md`. Acceptance items that could not be met on the one available machine: S1 on UHD 620 and macOS and option B on the integrated GPU; S2 cold start and HDD; S5 and S7 on other operating systems; S10 on a real network share. "Sn approved" in a task's dependencies now means "read `spikes/<id>/REPORT.md` first".

**S1 Viewport and glass panel on integrated graphics** · Size M
- Goal: prove or disprove the RHI renderer decision with numbers.
- Files: `spikes/s1-viewport/`. Interfaces: none (informs `render.h`). Depends: none.
- Work: (A) `QQuickRhiItem` drawing a 500k-triangle mesh with edges, an ID-buffer pick, and one shared quarter-resolution blur sampled by three floating panels. (B) the brief's alternative: OCCT `V3d_View` in a `QQuickFramebufferObject` with Qt forced to OpenGL, same scene.
- Acceptance: report gives 95th-percentile frame time for A and B, glass on and off, at 1080p on (1) a UHD 620-class laptop if Jake supplies one, otherwise the weakest GPU available plus WARP, and (2) a discrete GPU; resize, first-frame and high-DPI behaviour noted; macOS result for A. Pass for decision A: 30 FPS or better on UHD 620 with glass on. States clearly which decision the data supports.

**S2 Cold start** · Size S
- Goal: measure start-up time of a Qt Quick shell that links OCCT and the other libraries.
- Files: `spikes/s2-startup/`. Depends: none.
- Acceptance: report gives cold (after reboot) and warm times to first interactive frame on HDD and SSD, with and without delay-loading OCCT DLLs and with QML compiled ahead of time (`qmlcachegen`/`qmltc`). Recommends a budget.

**S3 STEP import and tessellation speed** · Size S
- Goal: measure import, heal, and tessellation on large files, and how long the uncancellable parse stage lasts.
- Files: `spikes/s3-step/`. Depends: none.
- Acceptance: report with times per stage for five files from 5 to 100 MB at the three tier tolerances, single-threaded and with one face-mesh job per body; peak memory; cancel latency per stage.

**S4 Persistent naming prototype** · Size M
- Goal: validate section 3.3 on real OCCT history before any feature is built.
- Files: `spikes/s4-naming/`. Interfaces: prototypes `naming::propagate`, `IResolver`. Depends: none.
- Acceptance: a 40-feature scripted part (extrudes, cuts, fillets, chamfers, holes) and the edit scenarios of section 3.3. Report gives percent `Exact`/`Split`/`Repaired`/`Broken`, and **the count of wrong-element resolutions, which must be 0**. Lists every OCCT operation whose history was missing or wrong, and the neighbour-naming fallback result. Recommends changes to `naming.h` if needed.

**S5 Adaptive roughing: port or rewrite** · Size M
- Goal: decide how to build `adaptive2d` and set its acceptance numbers.
- Files: `spikes/s5-adaptive/`. Depends: none.
- Acceptance: FreeCAD `Adaptive.cpp` built standalone against Clipper2 2.0.1 and run on six pockets (BP1 and BP2 pockets, a pocket with islands, a narrow slot, an open pocket, a thin-wall case). Report gives engagement-angle histogram from an independent 2D cutter-sweep checker, cut and link lengths, run time, and determinism across thread counts and OSes. Recommends port, adapt or rewrite, with reasons.

**S6 Tri-dexel simulation** · Size M
- Goal: confirm CPU tri-dexel meets Low-tier speed and memory.
- Files: `spikes/s6-dexel/`. Depends: none.
- Acceptance: report gives moves per second and memory for a 6 x 4 x 1 in stock at 0.5, 0.2 and 0.05 mm with flat and ball tools on 4 cores; volume error on analytic cases; display-mesh extraction time for a dirty region.

**S7 PlaneGCS standalone** · Size S
- Goal: vendor PlaneGCS outside FreeCAD and check speed and determinism.
- Files: `spikes/s7-planegcs/`. Depends: none.
- Acceptance: builds on three OSes with a shim under 200 lines; 200-entity sketch drag solve time; identical results over 100 repeated runs and across thread settings; list of FreeCAD headers stubbed.

**S8 QuickJS-NG post host** · Size S
- Goal: confirm post speed and sandbox limits.
- Files: `spikes/s8-quickjs/`. Depends: none.
- Acceptance: a toy post emits 1 M moves; report gives time, peak memory, and proof that the heap limit, the CPU-time interrupt, and removal of `Date`, `Math.random` and I/O all work; error messages carry script line numbers.

**S9 Direct editing success rate** · Size M
- Goal: learn how often OCCT defeaturing and face offset succeed on real parts.
- Files: `spikes/s9-directedit/`. Depends: none.
- Acceptance: on 50 corpus solids, attempt delete-fillet, delete-hole, delete-pocket, resize-hole and offset-planar-face on automatically chosen targets. Report gives success rate per edit type, failure categories, time, and whether history was returned. Recommends which edits v1 can promise.

**S10 SQLite file on a network share** · Size S
- Goal: confirm the save, lock-file and recovery design on SMB.
- Files: `spikes/s10-sqlite-smb/`. Depends: none.
- Acceptance: 500 kill-during-save iterations on a local disk and on an SMB share with zero unreadable files; second-opener behaviour with the lock file; save time for a 200 MB file when only 50 KB changed.

**S11 Faster sketch drag** · Size S · follow-up spike, run before T-090
- Goal: find a drag solve that holds 16 ms beyond 50 coupled curves, or show there is none without forking PlaneGCS.
- Files: `spikes/s11-sketch-drag/`. Depends: S7 report.
- Work: S7 found that declaring only the still-free parameters as unknowns (`getDependentParams`) made a drag step about 500 times faster when it worked, and silently left the point unmoved when the remaining constraints were not linearly independent. Try a row-independent subset of constraints for the reduced system, falling back to the full solve when the reduced one does not move the point; also profile the full drag solve to see which decomposition dominates.
- Acceptance: on the S7 sketches at 50, 100, 150 and 200 curves, report median and worst step time and the count of steps where the result differs from the full solve by more than 1e-9 mm, which must be 0. Recommends: adopt, fork PlaneGCS, or keep the section 9 budget as it is.

### M1: open and look

**T-001 Repository skeleton, build and CI** · SEQUENTIAL · Size M
- Goal: a building, testing, empty project on three OSes.
- Files: repo root files, `include/ray/` (copied from `docs/interfaces/`), `src/*/CMakeLists.txt` with stub sources, `tools/`, `.github/workflows/`, `THIRD_PARTY.md`, `LICENSE`.
- Interfaces: all headers (must compile). Depends: M0 decisions approved.
- Acceptance: all presets configure and build; every header in `include/ray/` compiles standalone in a test translation unit; CI jobs build-test, lint (include rules, dmath rule, colour-literal rule, licence ledger) and sanitizers are green; the report lists every signature changed from the drafts. On Windows every OCCT toolkit is linked with `/DELAYLOAD` [measured S2: no warm cost, about 170 ms saved before `main()` on a first launch] and a smoke test calls into every toolkit, since a missing DLL now fails at first use. QML is built with `qt_add_qml_module` (qmlcachegen); `qmltc` is not used (it does not build with Qt Quick Controls in Qt 6.12.0). `vcpkg.json` includes `boost-graph` and `boost-math` for PlaneGCS.

**T-002 core: identity, hashing, results, units, dmath** · PARALLEL-SAFE · Size M
- Goal: implement the non-threading half of `core.h`.
- Files: `src/core/` (except `jobs*`), `third_party/dmath/`, `tests/unit/core/`.
- Interfaces: implements `Uuid`, `Id`, `Hash256`, `Hasher`, `Error`, `Result`, `toNm`, `dmath`, `formatLength`, `parseLength`. Depends: T-001.
- Acceptance: BLAKE3 test vectors pass; `dmath` results are bit-identical across the three OS jobs for a stored table of 100,000 inputs and within 1 ulp of the reference; `parseLength` handles `0.5`, `1/2`, `12.7mm`, `0.5in`, and rejects garbage; `toNm` round-half-even cases pass.

**T-003 core: job system** · PARALLEL-SAFE · Size M
- Goal: implement `IJobSystem`, `JobHandle`, `CancelToken`, `Progress` on oneTBB.
- Files: `src/core/jobs*`, `tests/unit/core/jobs*`.
- Interfaces: implements `IJobSystem`, `CancelSource`, `Progress`, `IUiDispatcher` (test double). Depends: T-001.
- Acceptance: `parallelChunks` gives identical merged output with 1, 2 and 16 workers; a cancelled job returns `Cancelled` within 100 ms in a test loop; `then` continuations run on the dispatcher thread; progress callbacks are rate-limited; TSan clean.

**T-004 kernel: shapes, serialisation, queries** · SEQUENTIAL · Size M
- Goal: the `Shape` handle, canonical traversal order, BREP serialisation and geometry queries.
- Files: `src/kernel/shape*`, `src/kernel/query*`, `tests/unit/kernel/`.
- Interfaces: implements `Shape`, `SubId`, `serialize`, `deserialize`, `surfaceInfo`, `curveInfo`, `vertexPoint`, `adjacent`, `bounds`, `massProps`, `minDistance`, `makeBox`, `makeCylinder`. Depends: T-001.
- Acceptance: serialise then deserialise then serialise is byte-identical; `SubId` order is identical across runs and thread counts; box and cylinder mass properties match closed-form values to 1e-9 relative; every entry point returns `KernelFailure` (never throws) on a null or corrupt shape.

**T-005 kernel: import and export** · SEQUENTIAL · Size M
- Goal: STEP and IGES import with healing, progress and cancel; STEP, IGES and STL export.
- Files: `src/kernel/io*`, `tests/unit/kernel/io*`.
- Interfaces: implements `importStep`, `importIges`, `exportStep`, `exportIges`, `exportStl`. Depends: T-004, T-003, T-025.
- Acceptance: corpus run has zero crashes and zero hangs past 120 s per file; the pass rate (valid solid produced) is recorded as the baseline; units and body names and colours are read on the NIST files; export then re-import preserves volume to 1e-6 relative on 20 parts. From S3: STEP is read through `ReadStream` with our own stream buffer; cancel returns to the caller within 100 ms in every stage, by abandoning the job during the uncancellable model-load stage (about 30 ms per MiB) and letting it finish in the background; healing follows `ImportOptions::heal` (`None`, `Reader`, `Full`) and is never skipped by default, because unhealed files left B-spline faces without triangles; healing runs one job per solid.

**T-006 kernel: tessellation** · SEQUENTIAL · Size M
- Goal: display meshes with per-face and per-edge ranges, and exact triangulation for CAM.
- Files: `src/kernel/mesh*`, `tests/unit/kernel/mesh*`.
- Interfaces: implements `tessellate`, `triangulateExact`. Depends: T-004.
- Acceptance: every face and edge of the input appears in `faces`/`edges` exactly once; mesh is watertight on closed solids in the corpus subset (no boundary edges) for `triangulateExact`; `triangulateExact` output is identical regardless of any tier preset. From S3 (section 7 rule 3): one mesh call at a time under a process-wide lock, with `InParallel` inside; a test starts 8 threads tessellating 8 different shapes and gets results identical to serial with no crash and no TSan report; mesh hash with `InParallel` equals the single-threaded hash on the corpus subset; the default mesher only; faces that receive no triangles are listed in `Mesh::failedFaces`; peak memory on the 50 MB corpus file stays inside the section 9 row by capping concurrent faces; a cancelled mesh call is abandoned by the caller within 100 ms.

**T-007 naming: element maps for imported bodies and the resolver** · SEQUENTIAL · Size M
- Goal: names, fingerprints, serialisation and resolution for history-free bodies.
- Files: `src/naming/`, `tests/unit/naming/`.
- Interfaces: implements `TopoName` (with `discriminator`, `siblingCount`, `family`), `Fingerprint`, `ElementMap` (several names per element, family lookup), `nameImported`, `TopoRef` (all names, families, neighbour names), `IResolver` (states `Exact`, `Broken`). Depends: T-004, S4 approved.
- Acceptance: importing the same file twice gives identical element-map bytes; names are unchanged when OCCT sub-shape order is shuffled in a test hook; map serialisation round-trips; a reference to a missing name resolves `Broken`, never to another element; an ambiguous name whose stored fingerprint no longer matches (tolerance compare) resolves `Broken`.

**T-008 doc: state, canonical JSON, parameters** · SEQUENTIAL · Size M
- Goal: the immutable `DocState`, its serialisation and the expression evaluator.
- Files: `src/doc/state*`, `src/doc/json*`, `src/doc/expr*`, `schemas/doc*.schema.json`, `tests/unit/doc/`.
- Interfaces: implements `DocState`, `Feature`, `Body`, `Parameter`, `evaluate`, `canonicalJson`, `stateHash`. Depends: T-002.
- Acceptance: `canonicalJson` is idempotent and key-order independent; state round-trips with equal hash; unknown JSON keys survive a round trip; expressions handle units, precedence, functions, parameter references, and report cycles and unit mismatches with a message.

**T-009 doc: commands, undo and previews** · SEQUENTIAL · Size M
- Goal: `IDocument` with unlimited undo across CAD and CAM.
- Files: `src/doc/document*`, `src/doc/commands/`, `tests/unit/doc/commands*`.
- Interfaces: implements `Command`, `ICommandHandler`, `IDocument`; handlers `feature.add/edit/remove/reorder/suppress`, `param.set`, `body.set`. Depends: T-008.
- Acceptance: 1,000 random commands then 1,000 undos returns the initial state hash; redo is cleared by a new command; `beginPreview` never changes `state()` or the undo stack; listeners fire on the dispatcher thread exactly once per change.

**T-010 doc: rebuilder and the import feature** · SEQUENTIAL · Size M
- Goal: incremental, cancellable rebuild with caching, and the `import` feature type.
- Files: `src/doc/rebuild*`, `src/doc/features/import*`, `tests/unit/doc/rebuild*`.
- Interfaces: implements `IRebuilder`, `FeatureRegistry`, `IFeatureType` (for `import`), `RebuildContext`. Depends: T-005, T-007, T-009, T-003.
- Acceptance: unchanged features report `cacheHit`; editing feature N recomputes only N and later features of the same body; a failed feature marks later ones `NotBuilt` and leaves earlier output intact; cancel returns within 1 s; output is identical for 1 and 16 workers.

**T-011 store: document file, objects and commits** · PARALLEL-SAFE · Size M
- Goal: the `.raycad` container.
- Files: `src/store/file*`, `src/store/objects*`, `tests/unit/store/`.
- Interfaces: implements `IObjectStore`, `IDocumentFile` (all but `compact`), `Commit`, `Ref`, lock file. Depends: T-008, S10 approved.
- Acceptance: schema matches section 4.1; save is one transaction; the write lock is taken at open; `loadHead` on a 500 MB synthetic file is under 200 ms; hash verification detects a flipped byte; kill-during-save 500 times never yields an unreadable file. Lock file exactly as section 4.3: create-new, open handle, server-clock heartbeat age, save counter, break by rename; a second opener gets `OpenInfo::lock` with user, host, since and stale flag, and read-only access through a counter-guarded, verified snapshot copy (a test copies during continuous saving and never opens a bad copy). The S10 programs (`spikes/s10-sqlite-smb`) are rerun against a real network share when Jake supplies a path, and the result is added to that report; until then the network claim stays unverified.

**T-012 store: recovery journal, migrations, compaction** · PARALLEL-SAFE · Size M
- Goal: crash recovery and format evolution.
- Files: `src/store/journal*`, `src/store/migrate*`, `src/store/compact*`, `tests/unit/store/`.
- Interfaces: implements `IRecoveryJournal`, `Migration`, `registerMigration`, `IDocumentFile::compact`. Depends: T-011, T-009.
- Acceptance: kill the process after N random commands, replay, state hash equals the pre-kill hash for 200 trials; `append` under 2 ms median; a fabricated v0 fixture migrates to v1 and keeps a `.bak`; a file with `user_version` 99 returns `FormatTooNew` and its bytes are unchanged; `compact` keeps named versions and drops unreachable objects.

**T-013 render: hardware detection, tiers and themes** · PARALLEL-SAFE · Size M
- Goal: tier selection and the theme-token system with both shipped themes.
- Files: `src/render/tier*`, `src/render/theme*`, `resources/themes/`, `resources/fonts/`, `schemas/theme.schema.json`, `tests/unit/render/`.
- Interfaces: implements `HardwareInfo`, `detectHardware`, `chooseTier`, `presetFor`, `Token`, `Theme`, `loadTheme`, `contrastRatio`. Depends: T-001.
- Acceptance: `chooseTier` unit cases for the three reference machines and for software rasterisers (always Low, MSAA and glass off); adapter type and VRAM come from DXGI on Windows, not from QRhi; both themes load; a theme missing a token or failing 4.5:1 text contrast or 3:1 thin-line contrast is rejected with the token named; the brief's starting colours are present.

**T-014 render: RHI viewport core** · SEQUENTIAL · Size M
- Goal: draw shaded meshes and edges with a camera, on demand.
- Files: `src/render/viewport*`, `src/render/rhi/`, `src/render/shaders/`, `tests/unit/render/`.
- Interfaces: implements `IViewport` (mesh items, transforms, styles, camera, theme, preset, `requestFrame`, `captureImage`, `lastFrameStats`). Depends: T-006, T-013, S1 approved.
- Acceptance: offscreen renders of five test scenes match reference images within tolerance on WARP and llvmpipe; zero frames rendered during 5 s idle; switching theme and preset at runtime changes the next frame with no restart; MSAA and edge modes follow the preset.

**T-015 render: picking and highlight** · SEQUENTIAL · Size M
- Goal: ID-buffer picking for faces, edges, vertices and bodies, with hover and selection highlight.
- Files: `src/render/pick*`, `src/render/rhi/`, `src/render/shaders/`.
- Interfaces: implements `IViewport::pick`, `pickRect`, `setHighlight`, `PickFilter`. Depends: T-014.
- Acceptance: scripted picks at known pixels return the expected `SubId` on test scenes; edges and vertices are pickable within a 6 px tolerance; filters work; the ID buffer is re-rendered only when camera or scene changed (counter test).

**T-016 render: blurred backdrop for glass panels** · SEQUENTIAL · Size S
- Goal: one shared, downsampled blur texture per changed frame.
- Files: `src/render/backdrop*`, `src/render/rhi/`, `src/render/shaders/`.
- Interfaces: implements `IViewport::blurredBackdropTexture`, `setBackdropEnabled`. Depends: T-014.
- Acceptance: blur pass count per frame is exactly 1 with any number of panels and 0 when disabled or when the viewport image is unchanged (counter test); output matches a reference image; the backdrop handle follows the ownership and orientation rules in `render.h` and 200 scripted window resizes with glass on produce no crash (S1 hit a double free here).

**T-017 api: action registry, app session, headless app and CLI** · SEQUENTIAL · Size M
- Goal: the single entry point, usable without a window.
- Files: `src/api/`, `apps/raycad-cli/`, `tests/unit/api/`.
- Interfaces: implements `IActionRegistry`, `IApp`, `makeHeadlessApp`, `ConfirmHook`, `describeDocumentJson`; registers `file.*` and document actions. Depends: T-010, T-011, T-012.
- Acceptance: `raycad-cli` can import a STEP, save, reopen and print the state hash; invalid args are rejected by schema with a path to the bad field; an action flagged `changesToolpathsOrGcode` invoked as `Caller::Agent` calls the confirm hook and does nothing when it returns false; `search("sav")` ranks `file.save` first, deterministically.

**T-018 ui: application shell, theme binding and panel components** · PARALLEL-SAFE · Size M
- Goal: the window, the `Theme` QML singleton, glass and flat panels, and the base controls.
- Files: `src/ui/app*`, `src/ui/qml/controls/`, `src/ui/qml/Shell.qml`, `src/ui/qml/panels/PanelBase.qml`, `apps/raycad/`, `tests/ui/controls/`.
- Interfaces: consumes `Theme`, `IApp`. Depends: T-013, T-017.
- Acceptance: theme follows the OS and switches live; glass/flat toggle swaps panel backgrounds with identical layout (geometry snapshot test); controls render correctly at 100%, 150% and 200% scale; no colour or font literal in QML (lint); Qt Quick is not forced to OpenGL. From S2: only what the start screen shows is created at start; other tabs, panel contents and popups load on first use (`Loader`), and no kernel call runs before the first frame; warm start meets the section 9 row; the QML and shader cache location is set explicitly.

**T-019 ui: start screen, file handling, progress and recovery** · PARALLEL-SAFE · Size M
- Goal: launch into recent files and templates; open, save, save-as; job progress with Esc cancel; crash-recovery prompt.
- Files: `src/ui/qml/StartScreen.qml`, `src/ui/qml/panels/JobsPanel.qml`, `src/ui/files*`, `tests/ui/start/`.
- Interfaces: consumes `IApp::recentFiles`, `open`, `save`, `IRecoveryJournal::findPending`, `JobHandle`. Depends: T-018.
- Acceptance: app starts on the start screen with thumbnails; opening a STEP shows staged progress and Esc cancels; drag-and-drop opens files; a simulated crash offers recovery on next launch and restores the state hash; UI-thread watchdog records no stall over 50 ms during import.

**T-020 ui: viewport item, navigation, selection, model tree** · PARALLEL-SAFE · Size M
- Goal: the 3D view inside Qt Quick with mouse, touchpad and keyboard navigation, selection filters, and the feature and body tree.
- Files: `src/ui/viewport*`, `src/ui/qml/Viewport.qml`, `src/ui/qml/panels/ModelTree.qml`, `src/ui/selection*`.
- Interfaces: consumes `IViewport`, `IDocument`, `IResolver::makeRef`. Depends: T-015, T-016, T-018.
- Acceptance: orbit, pan, zoom-to-cursor, fit, standard views and a view cube work; selection filters for faces, edges, vertices and bodies; selecting in the tree highlights in the view and the reverse; navigation presets (RayCAD default plus two common industry styles) are selectable; end-to-end script selects a face by pixel and reads back its `TopoRef`.

**T-021 ui: command palette, shortcuts and context menus** · PARALLEL-SAFE · Size M
- Goal: keyboard-first control of every action.
- Files: `src/ui/qml/CommandPalette.qml`, `src/ui/shortcuts*`, `src/ui/qml/ContextMenu.qml`, `src/ui/qml/panels/ShortcutSettings.qml`.
- Interfaces: consumes `IActionRegistry::all`, `search`, `invoke`. Depends: T-018.
- Acceptance: every registered action appears in the palette (test iterates the registry); shortcuts are remappable, conflicts are reported, and settings persist; the context menu at the cursor lists actions valid for the current selection.

**T-022 Measure and analysis tools** · PARALLEL-SAFE · Size M
- Goal: distance, angle, radius, minimum internal corner radius, mass properties, bounding box.
- Files: `src/kernel/analysis*` is NOT touched (functions exist from T-004); `src/api/measure*`, `src/ui/qml/panels/Measure.qml`, `tests/unit/api/measure*`.
- Interfaces: consumes `minDistance`, `surfaceInfo`, `curveInfo`, `massProps`, `bounds`; adds actions `measure.*`. Depends: T-020. (`minInternalCornerRadius` arrives with T-050; the UI entry is enabled then.)
- Acceptance: measurements on a reference part match known values to 1e-6 mm; results display in document units and switch live with the unit setting.

**T-023 render: section view and draft analysis** · SEQUENTIAL · Size S
- Goal: a clipping plane with optional cap, and a draft-angle colour mode.
- Files: `src/render/section*`, `src/render/rhi/`, `src/render/shaders/`, `src/ui/qml/panels/Section.qml`.
- Interfaces: implements `IViewport::setSection`; adds a draft-analysis style. Depends: T-015.
- Acceptance: reference-image tests for a sectioned part with and without cap (cap only when the preset allows); draft colours match computed angles on a test wedge.

**T-024 Settings, units and tier override** · PARALLEL-SAFE · Size S
- Goal: user settings storage and UI for units, theme, glass, tier and per-option overrides.
- Files: `src/api/settings*`, `src/ui/qml/panels/Settings.qml`.
- Interfaces: consumes `TierPreset`, `HardwareInfo`, `UnitFormat`. Depends: T-018.
- Acceptance: settings persist per user without admin rights; new documents default to inch; changing tier applies without restart; the panel shows detected hardware and the reason for the chosen tier.

**T-025 Test corpus and tooling** · PARALLEL-SAFE · Size M
- Goal: the STEP/IGES/DXF corpus with manifest, plus golden-file tools.
- Files: `tests/corpus/`, `tools/corpus*`, `tools/update-golden*`.
- Interfaces: none. Depends: T-001.
- Acceptance: at least 300 files with source, licence and exporter recorded; a fetch script for files not redistributable; `update-golden` writes new goldens only with an explicit flag and prints a diff summary.

### M2: first G-code

**T-030 lib: library files and layered lookup** · PARALLEL-SAFE · Size M
- Goal: read, validate, write and watch plain-folder libraries.
- Files: `src/lib/`, `schemas/tool|holder|material|machine|optemplate.schema.json`, `tests/unit/lib/`.
- Interfaces: implements `ILibrary`, `LibraryStack`, all structs in `libraries.h`. Depends: T-002.
- Acceptance: a malformed file becomes a `LibraryIssue` and does not block the rest; writes are atomic (kill test); an edit made on disk by another process is detected and `save` returns `Locked`; `queryTools` ordering is deterministic; later roots override earlier by id.

**T-031 lib: starter content** · PARALLEL-SAFE · Size S
- Goal: a usable built-in library.
- Files: `resources/library/`.
- Interfaces: data conforming to the schemas. Depends: T-030.
- Acceptance: about 60 generic tools (end mills, drills by number/letter/fraction/metric, spot drills, UNC/UNF/metric taps, chamfer mills, face mill, reamers, thread mills), 8 holders, 8 materials (6061, 7075, 1018, 4140, 4140PH, 304, 17-4, Ti-6Al-4V) with presets, and three machine definitions marked `"verified": false`; all validate; values cite a public source in a comment field.

**T-032 cam: CAM state, commands and the staleness engine** · SEQUENTIAL · Size M
- Goal: setups and operations inside the document, and `ICamEngine`.
- Files: `src/cam/state*`, `src/cam/engine*`, `src/doc/commands/cam*`, `tests/unit/cam/engine*`.
- Interfaces: implements `CamState`, `Setup`, `Operation`, `OperationRegistry`, `ICamEngine`; commands `cam.setup.*`, `cam.operation.*`. Depends: T-010, T-030.
- Acceptance: an operation's input hash changes when its params, tool, cutting data, referenced geometry or stock change, and not when an unrelated body or the tier changes; a model edit marks exactly the affected operations `Stale`; a `Broken` reference yields `ReferenceBroken`; CAM commands undo and redo with the rest of the document.

**T-033 cam: setup resolution** · PARALLEL-SAFE · Size M
- Goal: turn a `Setup` into a WCS frame, stock solid and `ResolvedGeometry`.
- Files: `src/cam/setup*`, `tests/unit/cam/setup*`.
- Interfaces: consumes `IResolver`, `kernel::bounds`, `makeBox`, `makeCylinder`; produces `ResolvedGeometry`. Depends: T-032.
- Acceptance: WCS from a face and edge follows the model when it is edited; all eight stock corners and top-centre origins are correct on a reference part; box, cylinder and solid stock produce the expected bounds.

**T-034 cam: hole recognition and ordering** · PARALLEL-SAFE · Size M
- Goal: find drillable holes on any solid, including imported ones.
- Files: `src/cam/holes*`, `tests/unit/cam/holes*`.
- Interfaces: implements `recognizeHoles`, `orderHolesShortestPath`. Depends: T-033.
- Acceptance: on 20 corpus parts with hand-labelled holes, 100% of plain, counterbored, countersunk, blind and through holes aligned with setup Z are found with correct diameter, depth and bottom type, and zero non-holes are reported; holes on `hole` features carry their thread spec (enabled when T-097 lands); ordering is deterministic and no worse than 1.25 times nearest-neighbour length.

**T-035 cam: drilling operation** · PARALLEL-SAFE · Size M
- Goal: spot, drill, peck, chip-break, tap, bore and ream as canned-cycle moves.
- Files: `src/cam/ops/drill*`, `schemas/ops/drill.schema.json`, `tests/golden/toolpaths/drill*`.
- Interfaces: implements `IOperationType` for `drill`. Depends: T-034.
- Acceptance: golden toolpaths for each cycle; tip-angle depth compensation and through-hole breakthrough are correct to 1 µm; tapping feed equals pitch times rpm exactly in IR units; `validate` rejects a tap whose spec does not match the hole thread; invariant checker passes.

**T-037 sim: backplot data** · PARALLEL-SAFE · Size S
- Goal: fast line data from a toolpath.
- Files: `src/sim/backplot*`, `tests/unit/sim/backplot*`.
- Interfaces: implements `buildBackplot`. Depends: T-032.
- Acceptance: 1 M moves in under 150 ms on the CI baseline machine; arcs flattened within chord tolerance; `moveFirstVertex` maps every move.

**T-038 render: lines, backplot and tool glyph** · SEQUENTIAL · Size M
- Goal: draw toolpaths by move type and feed, with a range for scrubbing, and a tool shape.
- Files: `src/render/lines*`, `src/render/rhi/`, `src/render/shaders/`.
- Interfaces: implements `IViewport::addBackplot`, `setBackplotRange`, `addLines`, `addToolGlyph`; toolpath picking (`PickFilter::Toolpath`). Depends: T-015, T-037.
- Acceptance: reference images in both themes show cut, rapid (dashed), plunge, ramp, lead and link colours from tokens; thin lines meet 3:1 contrast in light mode; 2 M segments render at the Low budget on the reference machine or, without it, under the baseline frame time on WARP; picking a segment returns its `moveIndex`.

**T-039 post: QuickJS-NG host** · PARALLEL-SAFE · Size M
- Goal: run post scripts deterministically and safely.
- Files: `src/post/engine*`, `src/post/writer*`, `src/post/format*`, `tests/unit/post/`.
- Interfaces: implements `IPostEngine`, `PostInfo`, `PostOutput`, `formatNumber`, and the script side of `post_api.d.ts`. Depends: T-032, S8 approved.
- Acceptance: scripts cannot reach `Date`, `Math.random`, timers, `eval`, `Function("...")`, `import` or I/O (tests try each; `spikes/s8-quickjs/tests` is the starting set). From S8: QuickJS-NG is built as the plain `qjs` library without `quickjs-libc`, with `JS_INTERRUPT_COUNTER_INIT` patched to 100 and the patch recorded in `THIRD_PARTY.md`; scripts are compiled in a second, throwaway context of the same runtime and moved across as bytecode, so the script context never has the Eval intrinsic; Promise is present but jobs are never run, and a handler that returns a promise is an error; a soft heap limit at 7/8 of the hard limit is checked in the interrupt handler so an out-of-memory script reports a line; output size and warning count have their own limits; a failed run frees the runtime to zero bytes and the next run is unaffected. Heap, CPU and output limits abort with a script error; the line number is given when the engine has one (not for a thrown non-`Error` value). Known and documented: a script stuck inside one native call can overrun the CPU limit by the length of that call; `formatNumber` snaps to integer nanometres and rounds with integers. The host splits arcs to `maxArcDegrees`, converts units, expands cycles the post does not declare, and tracks modal state; the line map covers every emitted motion line; output is byte-identical across the three OS jobs.

**T-040 post: generic ISO and Fanuc posts** · PARALLEL-SAFE · Size M
- Goal: two shipped posts with full golden coverage.
- Files: `resources/library/posts/generic-iso.post.js`, `fanuc.post.js`, `tests/golden/gcode/generic-iso/`, `fanuc/`.
- Interfaces: `definePost`. Depends: T-039, T-035.
- Acceptance: the post feature matrix (each cycle, arcs in four quadrants and full circles, helical, tool change with pre-select, G43 length offsets, G41/G42, coolant modes, inch and metric, sequence numbers on/off, program number and comment sanitising) matches goldens that Jake has reviewed line by line once; safe start block and end-of-program block follow the control manual, cited in comments.

**T-041 post: Haas NGC and Haas classic** · PARALLEL-SAFE · Size S
- Files: `resources/library/posts/haas-ngc.post.js`, `haas-classic.post.js`, `tests/golden/gcode/haas-*/`. Interfaces: `definePost`. Depends: T-040.
- Goal and acceptance: as T-040 for Haas, including G187 smoothing property, G84 rigid tapping, G83 peck with Haas I/J/K options off by default.

**T-042 post: Mazak EIA/ISO** · PARALLEL-SAFE · Size S
- Files: `resources/library/posts/mazak-eia.post.js`, `tests/golden/gcode/mazak-eia/`. Interfaces: `definePost`. Depends: T-040.
- Goal and acceptance: as T-040 for Mazatrol controls running EIA/ISO programs, with the control generation as a property; work-offset and tool-length handling per the EIA manual, cited in comments. Open question 2 (control generation) must be answered first.

**T-043 Test-only G-code interpreter and round-trip test** · PARALLEL-SAFE · Size M
- Goal: an independent reader that turns posted G-code back into moves.
- Files: `tests/unit/post/roundtrip/` only (no product code).
- Interfaces: consumes `PostOutput`, `Toolpath`. Depends: T-040.
- Acceptance: for every golden program of every post, parsed end points equal the IR within one least-count unit, feeds match, cycles expand to the same holes, and no unknown word is found. The interpreter shares no code with `src/post`.

**T-044 ui: CAM workspace, setups** · PARALLEL-SAFE · Size M
- Goal: CAM tab with the setup tree and setup editing with live preview.
- Files: `src/ui/qml/cam/SetupPanel.qml`, `src/ui/qml/cam/CamTree.qml`, `src/ui/cam/setup*`.
- Interfaces: consumes `cam.setup.*` actions, `ICamEngine::status`, `beginPreview`. Depends: T-033, T-020.
- Acceptance: creating a setup takes at most four interactions on an imported part (pick top face, accept default stock, pick origin, OK); stock and WCS triad preview live; status badges (up to date, stale, generating, failed, broken reference) match the engine.

**T-045 ui: operation editing from schemas** · PARALLEL-SAFE · Size M
- Goal: one generic operation panel built from each type's JSON Schema, with geometry pickers and heights.
- Files: `src/ui/qml/cam/OperationPanel.qml`, `src/ui/qml/cam/SchemaForm.qml`, `src/ui/cam/operation*`.
- Interfaces: consumes `IOperationType::paramsSchemaJson`, `cam.operation.*`, `ICamEngine::generate`. Depends: T-044, T-035.
- Acceptance: every registered operation type gets a working form with no type-specific QML (test iterates the registry); fields accept expressions and units; edits preview the toolpath on a worker and can be cancelled; generation shows progress.

**T-046 ui: tool library manager** · PARALLEL-SAFE · Size M
- Files: `src/ui/qml/cam/ToolLibrary.qml`, `src/ui/cam/tools*`. Interfaces: consumes `LibraryStack`, `ILibrary::save`. Depends: T-030, T-018.
- Goal and acceptance: browse, filter, create, duplicate and edit tools, holders and per-material presets with a profile preview; edits to a shared library show a clear conflict message on `Locked`; tools used by the open document are embedded on first use.

**T-047 ui: posting, G-code viewer and file output** · PARALLEL-SAFE · Size M
- Files: `src/ui/qml/cam/PostDialog.qml`, `src/ui/qml/cam/GcodeView.qml`, `src/post/output*`, `tests/unit/post/output*`.
- Interfaces: implements `writeProgram`, `OutputRule`; consumes `IPostEngine`, `PostOutput::lineMap`. Depends: T-039, T-044.
- Goal and acceptance: post a setup; posting is blocked with a named reason when any operation is stale, failed or has a broken reference; viewer highlights with G-code tokens and is read-only; selecting a line highlights the move and the reverse; naming patterns and overwrite rules work on a local folder and an SMB path; writes are atomic.

**T-048 Feeds and speeds calculator** · PARALLEL-SAFE · Size M
- Files: `src/cam/feeds*`, `src/ui/qml/cam/FeedsPanel.qml`, `tests/unit/cam/feeds*`.
- Interfaces: implements `computeFeeds`. Depends: T-030.
- Goal and acceptance: rpm and feed from surface speed and chip load with radial chip thinning, clamped by machine rpm, feed, power and torque, with flags saying which limit applied; 30 hand-calculated cases match to 0.1%; the operation panel offers "use calculated" and shows the result without overwriting user-entered values silently.

### M3: 2.5D milling

**T-050 cam: shared 2D geometry core** · SEQUENTIAL · Size M
- Goal: boundaries from faces and edges, integer conversion, offsetting helpers and deterministic arc fitting.
- Files: `src/cam/geom2d*`, `src/kernel/slice*`, `tests/unit/cam/geom2d*`.
- Interfaces: implements `kernel::sliceAt`, `silhouette`, `minInternalCornerRadius`; internal `cam::geom2d` helpers used by all milling ops. Depends: T-033.
- Acceptance: slices of reference parts match analytic loops to 1 µm with arcs kept as arcs; integer round trip error under 1 nm; arc fitting reproduces known arcs exactly and never deviates more than tolerance (property test, 10,000 cases); results identical across OS jobs.

**T-051 cam: linking** · PARALLEL-SAFE · Size M
- Files: `src/cam/link*`, `tests/unit/cam/link*`. Interfaces: implements `linkPasses`, `Heights`, `LinkOptions`. Depends: T-050.
- Goal and acceptance: order passes and add leads, ramps, helices, retracts and stay-down links; property test proves no rapid below the feed plane inside stock bounds and no link crossing uncut stock; rapid distance on the benchmark parts is within 10% of a brute-force best ordering for small cases.

**T-052 cam: facing** · PARALLEL-SAFE · Size S
- Files: `src/cam/ops/face*`, `schemas/ops/face.schema.json`, `tests/golden/toolpaths/face*`. Interfaces: `IOperationType` `face`. Depends: T-051.
- Goal and acceptance: zigzag and one-way facing over the stock boundary with stepover, overhang and multiple depths; golden plus invariants; covers the whole stock top (coverage check by 2D sweep).

**T-053 cam: 2D contour** · PARALLEL-SAFE · Size M
- Files: `src/cam/ops/contour2d*`, schema, goldens. Interfaces: `IOperationType` `contour2d`. Depends: T-051.
- Goal and acceptance: open and closed chains, climb or conventional, tangent arc lead-in/out, multiple depths and roughing passes, finish and spring passes, tabs (rectangular and triangular), compensation in computer or in control (emits `compensation` on moves with a linear lead); golden plus invariants; offsets correct to 1 µm on arcs and lines.

**T-054 cam: 2D pocket** · PARALLEL-SAFE · Size M
- Files: `src/cam/ops/pocket2d*`, schema, goldens. Interfaces: `IOperationType` `pocket2d`. Depends: T-051.
- Goal and acceptance: offset pocketing with islands, open edges, helix or ramp entry (never a plunge with a non-centre-cutting tool unless a pre-drill point is given), floor and wall finish passes, stock to leave; coverage check proves no uncut area larger than the scallop limit; invariants pass.

**T-055 cam: slot and chamfer/deburr** · PARALLEL-SAFE · Size M
- Files: `src/cam/ops/slot*`, `src/cam/ops/chamfer2d*`, schemas, goldens. Interfaces: `IOperationType` `slot`, `chamfer2d`. Depends: T-051.
- Goal and acceptance: centre-line slotting with ramping; chamfering edges with a chamfer mill at a chosen width and tip offset, with collision check of the tool tip against the walls below; goldens plus invariants.

**T-056 cam: engraving and thread milling** · PARALLEL-SAFE · Size M
- Files: `src/cam/ops/engrave*`, `src/cam/ops/threadmill*`, schemas, goldens, `resources/fonts/` (one single-line engraving font, original or permissively licensed). Interfaces: `IOperationType` `engrave`, `threadmill`. Depends: T-051.
- Goal and acceptance: engrave sketch curves and text on a plane; helical thread milling for internal and external threads, single- and multi-form, climb, with radial lead-in; pitch and diameter exact in IR units; goldens plus invariants.

**T-057 Operation templates (v1 shop knowledge)** · PARALLEL-SAFE · Size M
- Files: `src/cam/templates*`, `src/ui/qml/cam/Templates.qml`, `resources/library/templates/`, `tests/unit/cam/templates*`.
- Interfaces: consumes `OpTemplate`, `LibraryStack::templatesFor`, `queryTools`; action `cam.template.apply`, `cam.template.saveFrom`. Depends: T-045, T-034.
- Goal and acceptance: save any operation as a template; apply templates to selected geometry or to all recognised holes of a kind; the result is ordinary operations with `templateId` set; expressions over `feature.*` and `tool.*` evaluate; applying "tapped holes" to BP1 produces spot, drill and tap operations with correct tools in one action; shipped starter templates for the eight materials.

**T-058 Toolpath invariant checker** · PARALLEL-SAFE · Size M
- Files: `tests/unit/cam/invariants/` only. Interfaces: consumes `Toolpath`, `ResolvedGeometry`. Depends: T-035.
- Goal and acceptance: an independent checker (2D cutter sweep on its own grid, no shared code with generators) for the invariants in section 11; seeded property tests feed it every operation type; a deliberately broken generator in the test suite is caught for each invariant.

**T-059 Cycle-time estimate** · PARALLEL-SAFE · Size S
- Files: `src/cam/cycletime*`, tests. Interfaces: implements `estimateCycleTime`. Depends: T-032.
- Goal and acceptance: trapezoidal per-axis acceleration, rapid and feed limits, tool-change time; analytic cases match to 1%; to be compared with real machine times in M8 and the error recorded.

### M4: adaptive

**T-060 cam: constant-engagement engine** · PARALLEL-SAFE · Size M
- Goal: a deterministic, re-entrant fork of FreeCAD main's `Adaptive.cpp` that generates 2D passes holding a target radial engagement.
- Files: `src/cam/ops/adaptive2d/engine*`, `tests/unit/cam/adaptive*`, `tests/tools/sweepcheck/` (the S5 checker, promoted), `THIRD_PARTY.md`.
- Interfaces: internal engine consumed by T-061 and T-063; uses `geom2d`. Depends: T-050, S5 approved (read `spikes/s5-adaptive/REPORT.md` sections 5, 7 and 8).
- Work, in this order: (1) reproduce and diagnose the S5 failure where 32 concurrent instances hung once and crashed once; if the cause is in the algorithm's convergence and not in shared globals or clock limits, stop and report, because that reopens the port-or-rewrite decision. (2) Start from FreeCAD main commit 56062b61 or later against Clipper2 2.0.1 built with `USINGZ`; keep the LGPL-2.1-or-later header and the attribution to Kresimir Tusek. (3) Remove every `clock()` decision in favour of iteration and node-count limits; replace `long` with `std::int64_t` and truncation with rounding, with a range check; remove the file-scope counters; initialise every output; route trig through `ray::dmath`; replace console output with warnings; poll the cancel token per step.
- Acceptance, measured by the sweep checker on the S5 pockets other than the slot, at 10% and 20% load with a 12.7 mm tool: median engagement within 2 deg of target; 99th percentile at most target + 6 deg; at most 0.5% of cut length above target + 10 deg and at most 0.1% above 1.5x target; reachable area left uncut 0.00 mm2; gouge beyond the input polygon at most 10 µm at the default tolerance (S5 measured up to 8.5 µm; 2 µm is the goal once a finer wall grid exists); output byte-identical over 20 processes, 20 in-process repeats and 1, 2, 16 and 32 concurrent jobs, and across the three OS jobs; 32 concurrent instances finish under a time limit in CI; cancel returns within 50 ms at any point; a failed start returns a defined error with no uninitialised data. Run time meets the section 9 rows on the baseline machine.

**T-063 cam: adaptive entry, slots and link moves** · PARALLEL-SAFE · Size M
- Goal: close the gaps S5 found in what the engine can cut.
- Files: `src/cam/ops/adaptive2d/entry*`, `src/cam/ops/adaptive2d/engine*`, tests. Interfaces: internal; fills `Toolpath::warnings`. Depends: T-060.
- Acceptance: a slot 14.5 mm wide with a 12.7 mm tool produces a toolpath at 10% and 20% load, entering by a ramp along the slot axis (or at a pre-drilled point when one is given), with 99th-percentile engagement at most 75 deg and never above 100 deg; a slot too narrow for any entry returns a failed result naming the minimum width; tool-down link moves remove 0 mm2 of material on the islands pocket (S5 measured 1.28 mm2), or are reclassified as cuts; wherever engagement exceeds 1.25x target the move carries a reduced-feed flag for T-061 to apply; air cutting (cut-flagged moves under 2 deg) is at most 10% on the BP1 and BP2 pockets and 15% on the others.

**T-061 cam: adaptive2d operation** · PARALLEL-SAFE · Size M
- Files: `src/cam/ops/adaptive2d/op*`, schema, goldens. Interfaces: `IOperationType` `adaptive2d`. Depends: T-060, T-063, T-051.
- Goal and acceptance: helix, ramp or pre-drill entry with a minimum helix diameter and automatic fall-back between them, open-pocket entry from outside stock, multiple depths, stock to leave, minimum cutting radius, stay-down links with lifted feed moves, feed reduction on flagged moves, arc fitting (at most 1 move per mm of cut on average; S5 measured 2 to 3 points per mm unfitted), and use of `stockSlices` when present so it clears only remaining material; engine coordinates are converted to integer nanometres at the `Toolpath` boundary; `Toolpath::Stats` reports 99th-percentile engagement and air-cut length; goldens plus invariants. The operation must be safe to generate in parallel with other operations (`generateAllStale`).

**T-062 Adaptive quality review pack** · PARALLEL-SAFE · Size S
- Files: `tests/benchmarks/adaptive/`, `docs/reports/adaptive.md`. Interfaces: none. Depends: T-061, T-059.
- Goal and acceptance: for BP1 to BP3, a report with engagement histograms, cycle time against plain pocketing, rapid percentage and images, for Jake to judge before M5. No pass/fail beyond the report existing with all fields.

### M5: prove it

**T-070 sim: tool shapes and CPU tri-dexel stock** · PARALLEL-SAFE · Size M
- Files: `src/sim/toolshape*`, `src/sim/tridexel*`, `tests/unit/sim/`. Interfaces: implements `ToolShape`, `makeToolShape`, `IStockModel`, `makeTriDexelCpu`. Depends: T-006, S6 approved.
- Goal and acceptance: analytic volume cases within one cell layer; `toMesh` returns only the dirty region and honours a triangle budget (a full-resolution 0.05 mm mesh is 12.4 M triangles); identical stock hash for 1, 4 and 16 workers and for any batch size. From S6 (`spikes/s6-dexel/REPORT.md`): moves are applied through `cutBatch` in batches, with each grid split into fixed strips so the result cannot depend on thread count; the cell is snapped per axis so the stock is spanned exactly; a dexel holds two inline intervals with overflow on the heap; `snapshot` for keyframes is cheaper than a full copy (copy-on-write strips or deltas) and under 50 ms for the benchmark stock at every tier resolution; `intersects` is documented as resolution-dependent; 100k moves meet the section 9 row; memory for the 6 x 4 x 1 in stock is under 20 MB at 0.2 mm and under 300 MB at 0.05 mm. Arcs, general revolved profiles, flute-length limits and non-box stock were not covered by S6 and are part of this task.

**T-071 sim: analytic safety checks** · PARALLEL-SAFE · Size M
- Files: `src/sim/verify*`, `tests/unit/sim/verify*`. Interfaces: implements `ISimulator::verifyFast`, `SimEvent`. Depends: T-070 (tool shapes), T-032.
- Goal and acceptance: `RapidIntoStock` against the **initial** stock is analytic and tier-independent; against in-process cut stock it is a separate, resolution-labelled event computed at a fixed resolution that does not come from the tier preset (S6: the dexel query misses material thinner than one cell). Rapid into stock, holder or shank against stock, part and fixtures, travel limits, plunge with non-centre-cutting tool, tool too short with a minimum stick-out suggestion; each has a known-bad program that raises exactly the expected events and a known-good one that raises none; results do not depend on the tier preset (test runs all three).

**T-072 sim: simulator, keyframes and gouge detection** · PARALLEL-SAFE · Size M
- Files: `src/sim/simulator*`, tests. Interfaces: implements `ISimulator` (rest), `makeSimulator`. Depends: T-070, T-071.
- Goal and acceptance: full run with progress and cancel, using `cutBatch`; `seek` anywhere within the tier's section 9 budget using `snapshot` keyframes; gouges beyond tolerance reported with depth and move index on a seeded-gouge program; `finalStock` is a closed solid.

**T-073 render: simulation stock display** · SEQUENTIAL · Size S
- Files: `src/render/stock*`, `src/render/rhi/`. Interfaces: implements `IViewport::updateMesh` with dirty regions; cut-face and collision colour tokens. Depends: T-038, T-070.
- Acceptance: updating a dirty region uploads only that region (byte counter test); reference images in both themes.

**T-074 ui: simulation panel and timeline** · PARALLEL-SAFE · Size M
- Files: `src/ui/qml/cam/SimPanel.qml`, `src/ui/qml/cam/Timeline.qml`, `src/ui/cam/sim*`. Interfaces: consumes `ISimulator`, `SimEvent`, `PostOutput::lineMap`. Depends: T-072, T-073, T-047.
- Goal and acceptance: play, pause, speed, scrub; error and warning markers on the timeline; clicking a marker jumps to the move, shows the tool there, and highlights the G-code line; stock, tool, holder and fixture visibility toggles; an end-to-end script on a seeded-collision part finds the marker and verifies the line.

**T-075 cam: in-process stock between setups** · SEQUENTIAL · Size M
- Files: `src/cam/stock*`, `src/cam/engine*`, tests. Interfaces: implements `FromSetupStock`, fills `ResolvedGeometry::stockSlices`. Depends: T-072, T-061.
- Goal and acceptance: setup 2 of BP1 starts from setup 1's simulated stock, re-oriented; changing a setup 1 operation marks setup 2 operations stale; slices of the carried stock are deterministic at a fixed CAM resolution that does not depend on the tier (test runs all three presets and compares toolpath hashes).

**T-076 Setup sheets** · PARALLEL-SAFE · Size M
- Files: `src/post/setupsheet*`, `resources/setupsheet/`, `src/ui/setupsheet*`, tests. Interfaces: implements `renderSetupSheetHtml`; UI converts to PDF. Depends: T-059, T-047.
- Goal and acceptance: self-contained HTML with program number, WCS and stock, fixture image from `captureImage`, tool list with stick-out and holders, per-operation summary and cycle time; PDF export on all three OSes; golden HTML (with fixed date) is byte-identical.

**T-077 Fixture bodies** · PARALLEL-SAFE · Size M
- Files: `src/doc/features/transform_body*`, `src/ui/qml/cam/Fixtures.qml`, `resources/library/fixtures/` (original simple vise and parallels models). Interfaces: `import` with role `fixture`, `transform_body` feature type. Depends: T-044, T-071.
- Goal and acceptance: import a STEP as a fixture, position it with a mate-to-face helper and numeric offsets; fixtures are never selectable as machining geometry; a holder passing through the vise jaw raises `HolderHitsFixture`.

### M6: fix the model

**T-080 kernel: direct-edit operations** · SEQUENTIAL · Size M
- Files: `src/kernel/directedit*`, tests, `tests/corpus/directedit/` (the S9 set). Interfaces: implements `deleteFaces`, `offsetFaces`, `resizeHole`, `unifySameDomain`, each returning `ShapeHistory`. Depends: T-004, S9 approved (read `spikes/s9-directedit/REPORT.md`).
- How, from S9: `deleteFaces` uses `BRepAlgoAPI_Defeaturing`; for a hole with a counterbore, countersink or chamfer it removes the whole stack and falls back to the bore alone. `resizeHole` expands the picked face to the whole cylinder (real holes are often two or more faces), enlarges by boolean cut, and shrinks by fusing a sleeve with a fall-back to defeature-then-cut. `offsetFaces` accepts planar faces only and is built as prism plus boolean; when a neighbour is not square to the face the result is a slab or notch with a step, and the call returns a warning saying so. `BRepOffset_MakeOffset` is not used. `unifySameDomain` validates its result and returns the input unchanged if unifying made it invalid. Every function validates its result and detects no-ops (section 7 rule 6). History is rewritten so the edited face is `Modified`, not "deleted plus a new face from the tool body", and an unchanged face with no entry means unchanged.
- Acceptance: on the S9 set, success rates are at least: delete plain hole 95%; delete hole with its counterbore, countersink or chamfer 95%; delete closed pocket 95%; resize hole larger 97% and smaller 95%; push/pull planar face 95% each way; delete a fillet that touches no other blend 75% on the real parts; delete a whole blend run 50% on the real parts (S9 measured 18 of 35). Every success is a valid closed solid with volume change of the expected sign; every failure returns `KernelFailure` with a message a machinist can act on (for example "this fillet joins other blends; select the whole run" or "neighbouring faces cannot be extended to close the gap"); no call reports success for a no-op; cancel is honoured within 10 s and the caller is released within 100 ms (S9 saw fillet removals run past 150 s and take up to 9.4 s to honour cancel); structured exceptions are contained on Windows. Add 20 or more real job-shop STEP files to the set before M6 and re-measure; the fillet and push/pull rates are the most likely to move.

**T-081 was split in revision 3.** "T-081" elsewhere in this plan means both halves unless it says otherwise.

**T-081a naming: propagation core** · SEQUENTIAL · Size M · **runs in M7, after T-095**
- Files: `src/naming/propagate*`, tests, `tests/naming/oracle/` (the S4 oracle and scenarios, promoted). Interfaces: implements `propagate`, `LineageEvent`, families, resolver states `Split`, `Merged` and `Repaired` exactly as section 3.3. Depends: T-007, T-094, T-095 (read `spikes/s4-naming/REPORT.md` sections 6, 9 and 10).
- Acceptance: scenarios scripted with the T-094 and T-095 operations only (extrude, cut, boolean, fillet, chamfer): an unrelated cut leaves all other references `Exact`; a cut that splits a referenced face gives `Split` with both pieces; a fuse that unifies two faces gives `Merged`; a cut that removes a referenced face gives `Broken`; the three S4 failure cases (ordinal swap after an unrelated cut, hole rim broken into two arcs, slot moved by its own width) resolve `Exact`, `Split` and `Broken` respectively and never to another element. A `Repaired` result is never returned to a rebuild or to CAM as usable until confirmed. Zero wrong-element resolutions across 1,000 randomised sequences of those operations on 20 corpus parts, judged by an oracle that does not share code with the naming module.

**T-081b naming: propagation through direct edits** · SEQUENTIAL · Size S
- Files: `src/naming/propagate*` (fixes only), tests, `tests/naming/oracle/`. Interfaces: none new. Depends: T-080, T-081a.
- Acceptance: scripted scenarios: delete an unrelated fillet (all other references `Exact`); resize a hole referenced by a drill op (`Exact`); offset a face that splits a neighbour (`Split` with both pieces); push a face until two faces unify (`Merged`); delete a referenced face (`Broken`). The T-081a randomised test is extended with the T-080 edits and still gives zero wrong-element resolutions.

**T-082 doc: direct-edit feature types** · SEQUENTIAL · Size S
- Files: `src/doc/features/delete_face*`, `offset_face*`, `resize_hole*`, tests. Interfaces: `IFeatureType` for the three types. Depends: T-081, T-010.
- Acceptance: each rebuilds, undoes and caches like any feature; editing an earlier direct edit replays later ones.

**T-083 ui: direct-edit tools and reference review** · PARALLEL-SAFE · Size M
- Files: `src/ui/qml/model/DirectEdit*.qml`, `src/ui/qml/panels/ReferenceReview.qml`, `src/ui/model/directedit*`. Interfaces: consumes actions for the three features, `beginPreview`, `ResolveState`. Depends: T-082, T-045.
- Goal and acceptance: select faces and press Delete, or drag an arrow to push/pull with numeric entry, with live preview; selecting one fillet face offers "select the whole blend run"; push/pull is offered on planar faces and warns before producing a stepped slab; a kernel refusal is shown with its message, never as a silent no-op; a review panel lists `Split`, `Merged`, `Repaired` and `Broken` references with before/after highlighting and lets the user confirm or re-pick; stale operations are obvious in the CAM tree and one action regenerates them.

**T-084 Replace imported geometry** · SEQUENTIAL · Size M
- Files: `src/naming/rematch*`, `src/doc/commands/replace_import*`, `src/ui/qml/model/ReplaceImport.qml`, tests. Interfaces: implements `rematchImported`; command `feature.import.replace`. Depends: T-081.
- Goal and acceptance: replace a part's STEP with a revised file; unchanged faces keep their names; changed and new faces are listed and coloured; affected operations go stale or `ReferenceBroken`; on five revision pairs made by editing parts in FreeCAD, zero wrong matches.

### M7: model it

**T-090 sketch: model and solver** · PARALLEL-SAFE · Size M
- Files: `src/sketch/model*`, `src/sketch/solver*`, `third_party/planegcs/`, `tests/unit/sketch/`. Interfaces: implements `Sketch`, `Entity`, `Constraint`, `ISketchSolver`, `makePlaneGcsSolver`. Depends: T-002, S7 approved.
- Acceptance: PlaneGCS is vendored unmodified at a pinned FreeCAD tag with the S7 shim (`spikes/s7-planegcs/shim`); all constraint types solve on unit cases; conflicting, redundant and partially redundant constraints are identified by id through an int-tag-to-`Uuid` table; a drag is a session (`beginDrag`, `move`, `end`) that keeps the solver alive; a drag step meets the section 9 row for a coupled group of up to 50 curves, or the better figure if S11 found one; full solve and diagnosis only ever run on a worker; results are bit-identical over 100 runs, across thread settings, and between a P-core and an E-core, under the three adapter rules of section 8; property test of 10,000 random well-constrained sketches satisfies every constraint to 1e-9. Depends also on S11.

**T-091 sketch: loops, inference and DXF** · PARALLEL-SAFE · Size M
- Files: `src/sketch/loops*`, `src/sketch/infer*`, `src/sketch/dxf*`, tests, DXF corpus subset. Interfaces: implements `closedLoops`, `inferConstraints`, `importDxf`, `exportDxf`. Depends: T-090.
- Acceptance: nested loops and shared edges produce the right regions with curve tags; inference proposes coincident, horizontal, vertical, tangent, parallel and perpendicular within tolerance and nothing spurious on test drawings; 30 DXF files from different exporters import with correct units, arcs and polyline bulges; a sketch exported with `exportDxf` and re-imported is geometrically identical to 1e-9 mm.

**T-092 ui: sketch drawing tools** · PARALLEL-SAFE · Size M
- Files: `src/ui/qml/sketch/`, `src/ui/sketch/tools*`. Interfaces: consumes `sketch.*` actions, `ISketchSolver::beginDrag` and `IDragSession`. Depends: T-091, T-020, T-096 (sketch feature).
- Goal and acceptance: dragging runs on the drag worker, which always solves for the newest pointer position, so the UI stays at frame rate on large sketches while geometry may follow late; line, arc (three kinds), circle, rectangle, slot, spline, construction toggle, trim, extend, offset, fillet corner; snaps with inferred-constraint glyphs; chained drawing from the keyboard; sketch colours from tokens (under-, fully-, over-constrained).

**T-093 ui: constraints and dimensions** · PARALLEL-SAFE · Size M
- Files: `src/ui/qml/sketch/Constraints*.qml`, `src/ui/sketch/dimensions*`. Interfaces: as T-092. Depends: T-092.
- Goal and acceptance: apply and delete all constraint types; smart dimension tool that picks distance, radius, diameter or angle from the selection; dimensions accept expressions and parameters; driven dimensions; remaining degrees of freedom shown; conflicting constraints highlighted from solver output.

**T-094 kernel: sweeps and booleans** · SEQUENTIAL · Size M
- Files: `src/kernel/sweep*`, `src/kernel/boolean*`, tests. Interfaces: implements `extrude`, `revolve`, `boolean`, `transformed` with `ShapeHistory` and `SweepFaceOrigin`. Depends: T-004.
- Acceptance: volumes match closed-form values to 1e-9 on 20 cases; every side face maps to its profile curve tag; boolean history covers every result face (no orphan faces) on the test set, or the orphan is reported in `warnings`; failures return `KernelFailure`.

**T-095 kernel: fillet and chamfer** · SEQUENTIAL · Size S
- Files: `src/kernel/blend*`, tests. Interfaces: implements `fillet`, `chamfer`. Depends: T-094.
- Acceptance: history marks blend faces `Generated` from their edges and neighbours `Modified`; radius too large returns a clear failure; 8-thread stress over the corpus is TSan clean.

**T-096 doc and naming: parametric feature types** · SEQUENTIAL · Size M
- Files: `src/doc/features/sketch*`, `datum_plane*`, `extrude*`, `revolve*`, `fillet*`, `chamfer*`, `src/naming/sweep*`, `tests/golden/geometry/`. Interfaces: `IFeatureType` for the six types; `propagate` with `sweepOrigins`. Depends: T-094, T-095, T-081a, T-090.
- Acceptance: sketch entity ids and loop ids are stable through trims, splits and solves (test), and hole positions have stable ids; the S4 40-feature part rebuilds with all 47 S4 edit scenarios at 95% or better `Exact`/`Split`/`Merged` among references whose element still exists, and zero wrong-element resolutions by the T-081a oracle; revolve is added to the scenario set; 100-feature synthetic part incremental rebuild meets the Low budget on the baseline machine; geometry goldens stored.

**T-097 Hole feature and thread standards** · SEQUENTIAL · Size M
- Files: `src/doc/features/hole*`, `resources/standards/threads.json`, tests. Interfaces: `IFeatureType` `hole`. Depends: T-096.
- Acceptance: UNC, UNF and metric coarse and fine tables with tap-drill sizes, cited to a public standard summary; drilled, tapped, counterbored and countersunk holes build correctly; the thread spec is stored on the feature where T-097b can read it.

**T-097b cam: thread pickup from hole features** · SEQUENTIAL · Size S · **runs in M2, after T-034** (split from T-097 in revision 3)
- Files: `src/cam/holes*` (thread pickup only), tests. Interfaces: `RecognizedHole::threadSpec`. Depends: T-097, T-034.
- Acceptance: hole recognition returns the thread spec of a hole made by the hole feature, and the "tapped holes" template picks the matching tap with no user input.

**T-098 ui: feature tools and parameter table** · PARALLEL-SAFE · Size M
- Files: `src/ui/qml/model/`, `src/ui/model/features*`. Interfaces: consumes `feature.*`, `param.*` actions, `IFeatureType::propsSchemaJson`, `beginPreview`. Depends: T-096, T-097, T-093.
- Goal and acceptance: extrude, cut, revolve, fillet, chamfer and hole tools with in-view drag handles, numeric fields accepting expressions, and live preview; feature tree supports rename, suppress, reorder, edit and rollback; parameter table with add, rename (references update) and delete (blocked when used); failed features show their message.

### M8: harden and cut

**T-100 Performance pass** · SEQUENTIAL · Size M
- Files: any under `src/` as profiling directs, listed in the task report; `tests/benchmarks/`.
- Interfaces: none changed. Depends: all M1 to M7 tasks.
- Acceptance: every row of the section 9 table is measured on the Low reference machine and either met or reported with the measured value and a proposal; no golden file changes.

**T-101 Packaging** · PARALLEL-SAFE · Size M
- Files: `packaging/`, `.github/workflows/package*`. Depends: T-018.
- Goal and acceptance: unused Qt Quick Controls styles and the Direct3D 12 compiler DLLs (`dxcompiler.dll`, `dxil.dll`) are pruned (about 33 MB of a 132 MB folder in S2), and the installer smoke test calls into every delay-loaded OCCT toolkit. Windows per-user installer and portable zip that install and run on a clean Windows 10 22H2 and Windows 11 VM without admin rights; AppImage runs on Ubuntu 22.04 and later; macOS arm64 dmg runs after the documented Gatekeeper step; each under 300 MB; LGPL notices, Qt and OCCT licence texts and source offers included; Qt and OCCT are replaceable shared libraries.

**T-102 Signing and release process** · PARALLEL-SAFE · Size S
- Files: `docs/release/`, `CONTRIBUTING.md`, `.github/workflows/release*`. Depends: T-101.
- Goal and acceptance: code-signing policy page, maintainer roles and MFA checklist as SignPath requires; the application is drafted for Jake to submit; release workflow builds from a tag reproducibly and attaches checksums.

**T-103 Accessibility, high-DPI and contrast audit** · PARALLEL-SAFE · Size S
- Files: `src/ui/qml/` fixes, `docs/reports/accessibility.md`. Depends: M7 UI tasks.
- Acceptance: every control reachable by keyboard and labelled for screen readers; automated screenshot contrast check passes 4.5:1 for text in both themes with glass on over black and white backdrops; layouts correct at 100 to 250% scaling.

**T-104 Benchmark documents and proving checklist** · PARALLEL-SAFE · Size S
- Files: `tests/benchmarks/parts/BP1..BP3/`, `docs/proving/`. Depends: T-098, T-075, T-076.
- Goal and acceptance: each benchmark part modelled and fully programmed in RayCAD with setup sheets; a per-machine proving checklist (air cut above the part, single block through the first tool, wax or foam first article, then metal); posted programs are stored as goldens.

**T-105 Machine verification and post proving** · SEQUENTIAL (needs Jake at the machines) · Size M
- Files: `resources/library/machines/`, `resources/library/posts/`, `tests/golden/gcode/`, `docs/proving/results.md`. Depends: T-104.
- Goal and acceptance: machine definitions corrected against the real machines and marked verified; a post-proving program per control exercising every cycle and code the posts emit, run in air; any post fix lands with an updated golden; **exit gate: BP1 to BP3 cut with no hand edits, measured against the section 12.3 tolerances, signed off by Jake.**

**T-106 User documentation** · PARALLEL-SAFE · Size S
- Files: `docs/user/`. Depends: M6.
- Goal and acceptance: getting started (import to G-code in 15 minutes), post editing guide with the full `post_api.d.ts` reference, library sharing on a network drive, file format and recovery notes; every screenshot generated by a script so it can be refreshed.

### Task count and parallelism

10 M0 spikes (done), 1 follow-up spike (S11) and 86 tasks. 28 tasks are SEQUENTIAL (kernel, naming, document, renderer core, API hub, CAM engine); they form the critical path. The rest can run two to four at a time behind them. Suggested lanes after M7: lane A kernel/naming/doc, lane B CAM operations, lane C posts and libraries, lane D UI. During M7 there are two lanes: kernel and naming (T-094, T-095, T-081a, T-096, T-097) and sketch (S11, T-090, T-091), which join at T-096; the UI tasks follow.

---

## 14. Risk register

| # | Risk | Likelihood | Impact | How we reduce it |
|-|-|-|-|-|
| 1 | **Persistent naming** resolves a reference to the wrong face after an edit, or breaks too often to be usable | High | Critical: wrong toolpath on the wrong geometry | S4 showed the first rules were unsafe (17 wrong of 33,785) and that amended rules reach 0 wrong on the same scenarios. That is one part, one author and an oracle that shares the script's idea of identity, so the risk stays High. Design rule: fail loudly, never guess; `Repaired` is a suggestion until confirmed; broken references block posting. The S4 oracle and scenarios become permanent tests; randomised edit tests with a zero-wrong-match gate (T-081, T-096); a second part and imports are added there. Since revision 3 sweeps and blends (M7) come before direct edits (M6), so the zero-wrong-match gate of T-081a is the first real test of naming and must not be relaxed to keep M7 moving. BRepGraph avoided. Expect user-visible `Broken` references on about 0.3% of surviving elements per edit. |
| 2 | **OCCT fails on bad STEP files** (crash, hang, invalid solids), or direct edits fail on real parts | High | High: the first thing every user does is import | Corpus of 300 to 1,000 files tagged by exporter, run in CI with a pass rate that may only rise. Healing and sewing on import; everything wrapped to return errors; watchdog time limits; import into a worker so the app survives. S9 measures direct-edit success so v1 only promises what works. A failed import still shows surfaces so the user can see what arrived. |
| 3 | **Adaptive roughing quality**: engagement spikes, air cutting, slow generation, or broken tools on steel | Medium | High: it is the headline toolpath | S5 chose to adapt FreeCAD main's code: engagement control is good, but the base is two weeks old upstream, hung and crashed at 32 concurrent runs for an undiagnosed reason, and has no slot entry. Diagnosis is the first step of T-060 and can reopen the rewrite option. Independent sweep checker with numeric gates (T-060, T-063). Review pack for Jake before simulation work (T-062). BP3 in 4140 is in the exit gate. Conservative shipped presets. |
| 4 | **Post-processor error crashes a machine** | Medium | Critical: safety and reputation | Golden files per post per feature, reviewed line by line once by Jake. Independent G-code round-trip interpreter (T-043). Posts cannot post stale or unverified operations. Analytic safety checks on every toolpath. Proving checklist: air cut, single block, soft material, then metal. Machine definitions marked unverified until checked. Posts are plain text so shops can read every line. |
| 5 | **Viewport in Qt Quick**: RHI renderer too slow on integrated graphics, `QQuickRhiItem` unstable, or RHI API churn | Low to Medium | High: schedule, and Low-tier promise | S1 measured both approaches and supports ours (211 FPS on UHD 770); UHD 620 and Metal are still unmeasured. `render.h` is API-neutral so fallbacks (2.3) touch only `src/render`. Qt pinned to 6.12 LTS; all RHI code in one directory. Software-rasteriser image tests in CI. |
| 6 | **Determinism leaks**: output differs across tiers, threads or OSes | Medium | High: destroys trust, breaks golden tests | Section 8 rules enforced by lint and link checks; integer geometry in CAM; quantised IR; four CI determinism jobs from M2 onward so leaks are caught the week they appear. |
| 7 | **Scope and schedule**: v1 is still large for one reviewer | High | Medium | Milestones each deliver a usable workflow; M7 can shrink without touching the exit gate; v1.x list already cut; SEQUENTIAL lane keeps review load predictable. Honest estimate with 50% confidence stated. |
| 8 | **Agent-built code drifts from the architecture** (boundary violations, duplicated helpers, subtly different conventions) | High | Medium | Interfaces fixed up front; tasks list the files they may touch; include and link rules checked in CI; acceptance tests an agent can run; interface changes only through approved SEQUENTIAL tasks; `/code-review` on every merge. |
| 9 | **Simulation misses a collision or gouge on Low tier** because of coarse resolution | Medium | High | Safety verdicts are analytic and tier-independent (T-071); resolution-dependent results are labelled with their tolerance; seeded-fault tests on all three presets. |
| 10 | **Low-tier performance** (start-up, 50 MB STEP, memory) misses budget | Medium | Medium | S2, S3 and S6 measured on a High-tier PC only, so every Low number is still an estimate; import is the tightest (first view of a 50 MB STEP estimated at 40 s on Low); lazy loading from the derived cache; draw on demand; a reference laptop in nightly CI as soon as one exists; budgets tracked from M1 so regressions surface early. |
| 11 | **File corruption or lost work**, especially on network shares | Low | High | Single-writer lock file, rollback journal, local recovery journal, hash-verified objects, 500-iteration kill tests (S10: 0 damaged in 1,000), counter-guarded snapshot copies for readers. S10 ran on loopback SMB only; a real share, power loss and a pulled cable are untested until Jake supplies a path. |
| 12 | **Licence or distribution problem**: a GPL-only dependency slips in, SignPath declines, Qt LTS patches stop for open-source users | Low | Medium | Licence ledger enforced in CI; unsigned builds documented as the fallback; Qt version isolated behind `src/ui` and `src/render`; revisit Qt version at each release. |
| 13 | **OCCT misbehaves quietly**: meshing two shapes at once crashes or drops faces; defeaturing reports success for a no-op or returns an invalid body; unify invalidates a valid solid (all seen in S3 and S9) | High | High | One mesh call at a time under a process-wide lock; every modifying kernel call validates its result and detects no-ops; faces without triangles are reported; structured exceptions contained on Windows; the default mesher only. These are acceptance items in T-005, T-006 and T-080. |
| 14 | **Sketch dragging feels slow** on large, tightly linked sketches (0.4 to 0.8 s per step at 200 coupled curves in S7) | Medium | Medium | Drag on a worker that always takes the newest pointer position, so the UI never blocks; budget stated per coupled group of 50 curves; spike S11 looks for a faster reduced solve; typical job-shop sketches split into small independent groups, which solved in under 1 ms. |
| 15 | **Direct editing disappoints on real parts**: fillet runs come out about half the time, push/pull makes a step next to angled or blended neighbours, and only 11 of the 50 S9 solids were real | Medium | Medium | Promise only what S9 measured; refuse clearly with a reason; run-selection in the UI; add 20 or more real parts before M6 and re-measure. |

---

## 15. Open questions for Jake

**Need an answer before the named task can start**

1. **Approve revision 2 of this plan** (section 0.5): the amended naming rules, the adaptive fork, the narrowed direct-edit promises, the revised budgets in section 9 and the new estimate in section 12. The departures in section 0.2 were approved with revision 1.
2. **Machine details** for the three machines: model, control generation (for the Mazak, which Mazatrol generation runs the EIA/ISO programs), axis count and any 4th axis, rigid tapping, probe and tool setter, through-spindle coolant, tool capacity. Blocks T-042 and T-105; T-031 ships unverified values until then.
3. **Low-tier reference laptop.** Can you supply a UHD 620-class machine as a self-hosted CI runner, or at least for milestone demos? Every Low-tier number in this plan is an estimate scaled from a High-tier PC. A Mac is needed once for the S1 Metal check.
3a. **A folder on the shop file server or a NAS** (writable, about 1 GB) to finish S10 on a real network share. Needed before M1 ends (T-011).
3b. **Cold start:** after your next reboot, wait two minutes and run `powershell -ExecutionPolicy Bypass -File M:\RayCAD\spikes\s2-startup\bench.ps1 -Cold`.
4. **Benchmark parts** (section 12.3): approve, or change materials, features and machine assignment. Are 4140 pre-hard and reaming realistic for the Fanuc machine?
5. **Licence:** confirm LGPL-2.1-or-later with DCO and no CLA (section 2.2). This choice is hard to change once outside contributors arrive.

**Would change scope or schedule**

6. **Target date** for the exit gate, if you have one. The estimate is 12 to 16 months at 50% confidence.
7. **Is M7 order acceptable?** Answered 2026-11-18: no. M7 runs straight after M1, ahead of CAM (section 12, revision 3).
8. **Cutter compensation:** which do your customers' shops use by default, wear compensation in control (G41/G42 with small offsets) or computer-only? It sets the default in `contour2d` and the posts.
9. **Probing in v1?** The brief tags it v2. If your machines have probes and you want WCS probing in the benchmark runs, it needs adding.
10. **Customer STEP files:** can you contribute anonymised real parts to the test corpus? They are worth more than any public set. Two spikes were limited by this: S3 had no real file above 6 MiB (needs a few at 20 to 100 MB) and S9 had only 11 real solids (needs 20 or more typical job-shop parts before M6).
10a. **Liquid Glass default on Low and Medium:** frame rate does not force it off (S1). Keep it off by default on those tiers, or on?

**Facts I could not fully verify**

11. Qt's `qsb` shader tool is GPL v3/commercial. I believe running it at build time does not affect our licence, as with any GPL build tool, but it is worth a second opinion.
12. Whether Qt's OpenGL backend is still supported on macOS in 6.12 could not be confirmed. It only matters if we fall back from RHI.
13. The FSF's licence-compatibility page was unreachable; section 2.2 rests on what FreeCAD and KiCad do in practice.
14. OCCT publishes no blanket thread-safety guarantee. Section 7 relies on source-level evidence and our own stress tests.
15. GTC tool-catalog licence terms are unstated on their site. Check before v2 tool import.
16. Shipped feeds, speeds and machine limits come from public catalogue values and are placeholders until you check them.

**Not verified in this plan**

- The interface headers have not been compiled.
- The spikes ran on one High-tier Windows PC, all at once. Nothing was measured on a Low- or Medium-tier machine, on macOS or Linux, or on a real network share; cross-OS determinism is untested for the adaptive engine, PlaneGCS and QuickJS-NG.
- Numbers marked [measured Sn] are from throwaway prototypes and synthetic or small data sets. Numbers marked [guess] or [estimate] are not measurements.
- Three spike agents (S1, S3, S9) wrote their `REPORT.md` by a route around a harness rule that subagents return text; the content was not affected.
- The task sizes are estimates with no track record behind them yet. After M1, re-estimate from actuals.

**Found by T-001 (2026-11-17), to settle before the named task**

- **`cam.h` against section 1.1 (blocks T-032):** `ICamEngine` takes `doc::DocStatePtr` and `doc::BuildResultPtr`, but 1.1 lets `doc` depend on `cam` and not the reverse, so `src/cam` cannot include `document.h`. Proposed fix: keep the operation types, toolpath IR and generators in `cam`, and move the `ICamEngine` implementation (staleness tracking over document snapshots) into `doc`, declared in `document.h`. Needs Jake's approval as an interface change.
- **`document.h` `RebuildContext`** has reference members and no constructor; T-010 adds one.
- **`sim.h` `makeTriDexelGpu`** takes a `render::IComputeBackend&` while rule 2 forbids `sim` linking `render`; the v2 implementation must live in `src/render` or a new module.
- **Binary cache in CI** uses `actions/cache` on vcpkg's archive folder, not NuGet on GitHub Packages as section 10 says (NuGet needs mono on Linux and macOS and write access that fork pull requests do not get).
- **CI runners** are `windows-latest`, `ubuntu-latest` and `macos-latest` (GitHub no longer offers `macos-14`), not the images named in section 10.
- **gperf (GPL-3.0-or-later)** is pulled in on Linux as a host build tool by OCCT's freetype feature. The licence lint allows GPL for rows marked "Build tool only". Building OCCT without freetype would remove it.
- From T-001 on, `include/ray/` holds the real headers; `docs/interfaces/` is kept identical as the reviewable copy.
