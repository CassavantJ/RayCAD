# CAD/CAM Architecture Prompt for Fable

Nov 16, 2026 · @Jake

## How to use this prompt

Download this doc as Markdown, delete the **Sources** section at the end, and save it as `BRIEF.md` in an empty project folder. Open that folder in Claude Code with Fable selected and say: "Read BRIEF.md and follow it." The constraints are already filled in.

## Role and context

You are the lead architect and technical product lead for a new desktop CAD/CAM application. Your job in this session is to produce a complete, buildable technical plan. Other AI agents (Claude Opus 5.5 subagents in Claude Code) will execute it task by task. Do not write implementation code yet; write interface signatures, schemas and pseudocode only where they make the plan unambiguous.

**About me.** I'm a software and robotics engineer (M.S. Computer Science) and a principal in a CNC machine tool distribution business serving aerospace, defense and medical shops in Arizona and Southern California. I know the code side and the shop floor. I will review your plan before any code is written.

**Target users.** CNC programmers, machinists and small-shop owners at job shops of 1 to 50 people. Many run CAM on aging office PCs or shop-floor laptops with integrated graphics. They care about getting correct, safe G-code to the machine fast, with minimal clicks and no surprises.

## Product vision and differentiators

The product is a fast, local-first, integrated CAD/CAM for job shops that matches the core workflows of Fusion, Mastercam and SolidWorks CAM and beats them on speed, hardware requirements and data ownership. Design every subsystem around these seven differentiators:

1. **Runs well on weak hardware; scales up on strong hardware.** Competitors are moving heavy work to the GPU (Mastercam 2026.R2 added GPU-accelerated simulation). We must deliver a fully usable experience with no discrete GPU, and use a GPU when present.
2. **Local-first, the user owns the data.** Files live on the user's disk and work fully offline. Cloud sync, if ever added, is optional. Autodesk is now building "Local Documents" for Fusion because manufacturers want control over where data lives; we start there.
3. **Shop knowledge is captured and reused.** Like CAMWorks' TechDB, the shop teaches the software its preferred tools, feeds, step-overs and entry moves per material and machine once. Feature recognition then applies them automatically.
4. **Automation the programmer can trust.** Every automatically generated operation is an ordinary, editable operation, never a black box. Rules-based automation comes first; AI assistance comes on top of it.
5. **Hybrid modeling.** Parametric history-based modeling plus direct editing of faces on dumb imported STEP solids, in the spirit of Siemens' synchronous technology. Customers send STEP files; programmers must be able to fix them without rebuilding.
6. **Version control built in.** Git-style history, named versions, branches and merges for parts and CAM programs, as Onshape does, but stored locally.
7. **Open source and scriptable.** Public API, scriptable custom features, plain-text post-processors, and a built-in MCP server so AI agents can drive the app.

**Non-goals:** competing on rendering, industrial-design surfacing, PLM, or electronics.

## Hardware tiers and performance budgets

One codebase, three presets. The app detects GPU, VRAM, RAM and core count on first launch, picks a tier, and lets the user override it. **Geometry, toolpaths and G-code must be bit-identical across tiers**; only display quality and simulation speed or resolution change. Add a CI test that proves it.

|Tier|Reference hardware|Viewport|CAM simulation|
|-|-|-|-|
|Low|Intel UHD 620-class integrated GPU (OpenGL 3.3), 4-core CPU, 8 GB RAM|Flat/Phong shading, coarse tessellation, no anti-aliasing, silhouette edges; flat opaque panels|CPU dexel or voxel stock at reduced resolution; fast backplot always available|
|Medium|GTX 1650-class GPU, 4 GB VRAM, 16 GB RAM|MSAA, finer tessellation, section views, transparency|Higher-resolution CPU simulation, multithreaded|
|High|RTX 3060-class or better, 8 GB+ VRAM, 32 GB RAM|Ambient occlusion, shadows, smooth edges; Liquid Glass panels on by default|GPU-compute material removal and collision checking at full resolution|

Liquid Glass (translucent, blurred floating panels over a full-window viewport) is on by default on High tier. Low and Medium use the same layout with flat opaque panels, and users can switch glass on or off on any tier. Implementation target: one shared, downsampled blur of the viewport per frame that every panel samples, re-blurred only when the view changes, never a separate blur per panel.

Starting performance budgets (refine these in the plan, per tier):

* Cold start to usable UI: under 3 s on Low.
* Open a 50 MB STEP file: under 10 s on Low, with a progress bar and the UI never frozen.
* Viewport: 30 FPS or better on Low with a 500k-triangle model; 60 FPS on High.
* Incremental feature rebuild for a typical 100-feature part: under 1 s.
* Memory: under 1.5 GB for a typical part with a full CAM program on Low.
* Toolpath generation and simulation: always on worker threads, cancellable, with progress.
* Installer under 300 MB; runs without admin rights.

## Proposed technology stack

This is my starting proposal. Validate or challenge every row, name the alternatives you rejected, and verify each license yourself before relying on it.

|Layer|Proposal|Notes to check|
|-|-|-|
|Language|C++20|OCCT 8.0 requires C++17 minimum. Consider Rust only for isolated modules.|
|Geometry kernel|Open CASCADE Technology 8.0.1|LGPL 2.1 with exception. 8.0.0 shipped May 2026 and 8.0.1 in July 2026; 8.0 restructured the source tree and added BRepGraph topology history, so use current APIs, not 7.x tutorials.|
|UI framework|Qt 6 with Qt Quick (QML) and Qt Quick Controls|LGPL v3, dynamic linking; the blur effect (Qt Quick Effects, MultiEffect) is LGPL v3 too. Chosen because Liquid Glass panels must blur the live 3D view behind them, which Qt Widgets can't do over an OpenGL viewport. Render OCCT into a Qt Quick item through a wrapped framebuffer, as OCCT's own Qt Quick sample does. Qt Quick 3D is GPL/commercial only and isn't needed, since OCCT draws the model. Fallback if integration fails the spike: draw panels in our own renderer with Dear ImGui (MIT), at a cost to accessibility and text input.|
|Rendering|Own renderer on OpenGL 3.3 core, sharing one context with Qt Quick; optional OpenGL 4.3 compute path for High tier (macOS stops at OpenGL 4.1)|Decide whether OCCT's visualization module is lean enough or whether we render tessellated meshes ourselves. Qt Quick defaults to Direct3D 11 on Windows; force its OpenGL backend so OCCT and the UI share one OpenGL context.|
|Sketch solver|PlaneGCS (from FreeCAD)|LGPL. SolveSpace's solver is GPLv3, usable only if our own license is GPL-compatible.|
|2D offsetting|Clipper2|Boost license.|
|3D toolpaths|OpenCAMLib, or our own drop-cutter and waterline on a BVH|LGPL 2.1. Judge its maintenance state.|
|Threading|oneTBB or a custom job system|Must support cancellation and progress reporting.|
|Post-processors and scripting|Embedded QuickJS (MIT) or Lua|Posts must be plain-text files editable in any editor, as Fusion's JavaScript posts are.|
|Native file format|SQLite container or zip of JSON plus BREP blobs|Must support fast partial loads, crash-safe saves and the version-history design.|
|AI and automation interface|Local MCP server over the public API|Exposes model, CAM setup and toolpath operations to AI agents.|

## CAD feature requirements

The CAD side exists to serve machining: model and repair parts, define stock and fixtures, and carry tolerances into CAM. The release column means v1 (first usable release), v2 (competitive release) or Later.

|Capability|What it must do|Seen in|Release|
|-|-|-|-|
|Persistent references|Stable face, edge and vertex IDs so downstream features and toolpaths survive upstream edits. Design this on day one; FreeCAD needed about 20 years and a 1.0 release to mitigate it.|FreeCAD 1.0/1.1|v1|
|Sketcher|Lines, arcs, circles, splines, slots, construction geometry, auto-constraints, driving and driven dimensions, DXF import into a sketch|All|v1|
|Core features|Extrude, revolve, cut, fillet, chamfer, shell, draft, linear and circular pattern, mirror, split body|All|v1|
|Hole feature|Standard hole library: drilled, tapped (UNC/UNF/metric), counterbore, countersink, with thread specs stored for CAM|Mastercam 2026 solid holes, SolidWorks Hole Wizard|v1|
|Direct editing|Push/pull faces, move and rotate faces, delete face and heal, resize holes on dumb imported solids|Siemens synchronous technology, Plasticity|v1 basic, v2 full|
|Parameters|Named parameters and equations driving dimensions|All|v1|
|Configurations|Part families and variants from a parameter table, generated on demand|Fusion (July 2026 on-demand configurations), SolidWorks design tables|v2|
|Multi-body and simple assemblies|Multiple bodies per part; lightweight assemblies for vises, fixtures and plates in CAM setups|All|v2|
|Import/export|STEP AP203/214, IGES, STL, DXF in v1; STEP AP242 with PMI, 3MF and OBJ in v2. Note that Parasolid and native SolidWorks formats need commercial translators.|All|v1/v2|
|Measure and analysis|Distance, angle, radius, minimum internal corner radius (drives tool choice), draft analysis, mass properties, section views, bounding box for stock|All|v1|
|PMI and model-based definition|Tolerances, GD\&T, surface finish and thread callouts attached to geometry; automatic hole and thread annotation|Fusion (Sept 2026), SolidWorks MBD|v2|
|Long-operation control|Every long computation shows progress and cancels with Esc|SolidWorks 2026|v1|
|Drawings|2D drawings with automatic views and hole callouts|SolidWorks 2026 AI drawings|Later|
|Similar-part search|Find geometrically similar parts in the local library to reuse proven programs|Fusion (July 2026)|Later|

## CAM feature requirements

The CAM side is the product's core. Constant-engagement roughing, a strong tool library and reusable shop knowledge matter more than toolpath count.

|Capability|What it must do|Seen in|Release|
|-|-|-|-|
|Setups and stock|Box, cylinder, from-solid and offset stock; work coordinate systems; multiple setups per part; vises and fixtures as collision bodies; in-process stock carried between setups|All|v1|
|2.5D milling|Facing, 2D contour with lead-in/out and tabs, pocket, slot, chamfer and deburr, engraving, thread milling|All|v1|
|Hole making|Spot, drill, peck, tap, bore and ream cycles output as canned cycles; automatic hole grouping and shortest-path ordering|All|v1|
|Constant-engagement roughing|Adaptive or trochoidal roughing that holds a constant chip load using radial chip thinning, in 2D (v1) and 3D (v2). Treat it as the most important toolpath. Mastercam claims 25-75% faster roughing with Dynamic Motion.|Mastercam Dynamic Motion, Fusion Adaptive|v1 2D, v2 3D|
|Rest machining|Detect material left by previous tools automatically and clean it up|hyperMILL 2026, Fusion|v2|
|3D finishing|Parallel, scallop, waterline, pencil, steep-and-shallow|All|v2|
|Turning|Face, OD/ID rough and finish, groove, thread, part-off; automatic contour and machining-area detection|hyperMILL CAM Plan Turning, Fusion|v2|
|3+2 indexed machining|Several fixed tool orientations inside one clearing operation|Fusion (July 2026 3+2 clearing)|v2|
|Simultaneous 4/5-axis and mill-turn|Swarf, multiaxis contour, automatic tilting, polar machining|Mastercam 2026.R2, Fusion|Later|
|Feature recognition|Recognize pockets, holes, slots and bosses on imported solids, including dumb STEP, and propose operations for review|CAMWorks AFR, Fusion (exploratory)|v2|
|Shop knowledge database|Rules per feature type, material and machine: preferred tools, feeds, step-overs, entry moves. Recognized features get programmed the shop's way automatically.|CAMWorks TechDB|v1 templates, v2 rules|
|Tolerance-based machining|Read PMI and pick operations to match it, e.g. a tight-tolerance hole gets bored or reamed instead of only drilled|CAMWorks TBM|v2|
|Tool library|Mills, drills, taps, inserts and holders with full geometry; per-material cutting data; import vendor catalogs in ISO 13399/GTC; build holders from supplier models|Fusion (Sept 2026 holders), NX, Mastercam|v1 basic, v2 import|
|Feeds and speeds|Calculator with material database, radial chip thinning and machine limits (max RPM, HP, torque)|All|v1|
|Associativity|Toolpaths go stale and regenerate when the model changes, never silently wrong|CAMWorks, Fusion|v1|
|Linking and rapids|Minimize air-cutting and rapids; smart clearance planes; safe retracts|Mastercam 2026.R2|v1|
|Probing and tool setting|WCS probing cycles; laser tool-setter macros for height offsets, wear and breakage checks|Mastercam 2026.R2 (Blum)|v2|
|Setup sheets|Tool list, WCS, stock, fixture image, cycle-time estimate, exported as PDF or HTML|All|v1|
|Cycle-time estimate|Uses the machine's real feed, rapid and acceleration limits|All|v1 basic, v2 kinematic|

## Simulation, verification and post-processing

Trust is the product. A programmer must be able to prove a program is safe before it touches a machine, on any tier.

|Capability|What it must do|Seen in|Release|
|-|-|-|-|
|Backplot|Instant toolpath preview on every tier, colored by move type and feed|All|v1|
|Material-removal simulation|Dexel or voxel stock on CPU for Low/Medium; GPU compute at full resolution on High. Mastercam's GPU simulation cut one verification from about 90 to 22 minutes.|Mastercam 2026.R2, Vericut|v1 CPU, v2 GPU|
|Collision and gouge checks|Tool, shank and holder against part, stock and fixtures; rapids into stock; minimum safe stick-out suggestion|All|v1|
|Simulation timeline|Scrub through the program; errors and warnings marked on the timeline; click a marker to jump to that move and its G-code line|Vericut 9.5|v1|
|As-machined vs design comparison|Color map of leftover and gouged material against the design model|Vericut AUTO-DIFF, Fusion|v2|
|G-code-driven simulation|Simulate the posted G-code itself through a control emulator for Mazak EIA/ISO, Haas and Fanuc, not only the internal toolpath|Vericut|v2|
|Machine simulation|Full kinematic model: axes, travel limits, tool changer, turrets, fixtures|Fusion (turn-mill simulation planned), hyperMILL VIRTUAL Machining|v2 mill, Later turn-mill|
|Feed optimization|Adjust feed per move from actual engagement and material removed|Vericut Force and OptiPath|Later|
|Post-processors|Plain-text scripted posts with user-editable properties (Fusion's are editable JavaScript). Ship Mazak (EIA/ISO), Haas (classic and NGC), Fanuc and a generic ISO post in v1; Okuma, Siemens 840D and Heidenhain in v2. Golden-file tests for every post.|Fusion, Mastercam|v1/v2|
|Machine definitions|One file per machine: axes, limits, max RPM, power, rapid and accel rates, tool changer, control dialect. Ship definitions for Mazak, Haas and Fanuc-controlled machines first, since those are what I can test on.|All|v1|
|Send to machine|Save to network share or USB with naming rules; DNC and MTConnect monitoring later|All|v1 basic, Later DNC|

## UX, data management, AI and extensibility

**User experience (v1)**

* Start into a recent-files and templates screen, not an empty document; Fusion moved to this in April 2026.
* Keyboard-first: a command palette that searches every command, fully remappable shortcuts, and context menus at the cursor. Plasticity shows how far Blender-style bindings and context widgets cut clicks.
* Live previews for every feature and operation before committing; minimal modal dialogs.
* Selection filters (faces, edges, features, bodies), as SolidWorks 2026 expanded.
* Inch and metric everywhere, set per document, with inch as the US default.
* Autosave, crash recovery and unlimited undo/redo across CAD and CAM.
* High-DPI and display-scaling support from the start.
* **Light and dark themes, both first-class from v1.** Follow the OS setting by default and switch at runtime without a restart. Every color comes from semantic theme tokens, including the 3D viewport, toolpath colors and G-code highlighting; no hard-coded colors. Thin lines such as toolpaths and accent outlines need darker values in light mode to stay visible. Text must meet 4.5:1 contrast in both themes.
* **Optional Liquid Glass look.** Floating translucent panels over a full-window 3D viewport, with background blur, top-edge highlights and capsule-shaped controls, in both light and dark themes. On by default on High tier only; Low and Medium get the same layout with flat opaque panels at almost no GPU cost. A settings toggle overrides it. Glass must never drop text contrast below 4.5:1.
* **Visual direction (approved in my style preview).** Graphite dark theme plus a matching light theme; Inter at 13 px for UI text; JetBrains Mono for numbers and G-code; one amber accent (#F5A524, darkened to about #B36A00 for thin lines in light mode) for primary actions and cutting moves; blue reserved for selection (#4C9AFF dark, #1F6FE0 light); dashed red for rapids. Treat these as starting tokens kept in one theme file.

**Data management**

* Git-style local history: every save is a microversion; named versions; branch and merge for both model and CAM program; restore any point. Onshape built its PDM this way, without check-in/check-out locks. (v2; design the storage for it in v1.)
* A visual diff: which features, operations and G-code changed between two versions. (v2)
* Shared libraries (tools, templates, posts, machines, shop rules) live in plain folders so a small shop can share them over a network drive. (v1)
* Fully offline by default; optional sync is a Later decision.
* Comments pinned to geometry, as Onshape does. (Later)

**AI assistance (rules first, AI on top)**

* Every AI or automated action is an ordinary, inspectable, undoable command. Anything that changes toolpaths or G-code needs user confirmation. Fusion's stated direction for AI CAM follows the same rule.
* Natural-language commands for routine edits, such as "drop all roughing feeds 15% for 4140". Mastercam Copilot already changes feeds and speeds across operations by voice or text with confirmation prompts. (v2)
* Learn the shop's preferences from the edits programmers make to automated output; Fusion says its upcoming AI CAM will do this. (Later)
* A built-in local MCP server over the public API, so external agents such as Claude can drive the app. Fusion is betaing a Compute MCP server with over 7,000 planned endpoints, and Onshape exposes an MCP server for Claude. (v2)
* AI is optional: a part must be fully programmable with no AI and no network. No data leaves the machine without explicit opt-in.

**Extensibility**

* One public API that built-in features and operations also use, so the API is never second-class. Onshape writes its own features in its public FeatureScript language. (v1 internal, v2 public)
* Custom features, operations and posts in the embedded scripting language. (v2)
* Reusable, versioned workflow macros, like Fusion's planned System Modeler. (Later)

## Release scope

The release tags in the tables above roll up into three releases. Plan milestones inside each one; v1 is the only release that needs a detailed task breakdown now.

1. **v1, "first part off the machine":** on a Low-tier laptop, a machinist imports a customer STEP file, repairs it with direct editing, programs a 2.5D part with adaptive roughing, drilling and tapping, simulates it with collision checks, posts it for Mazak, Haas or Fanuc, and prints a setup sheet. **Exit gate:** three benchmark parts that you propose and I approve are programmed and cut on a real machine with no G-code hand edits.
2. **v2, "competitive":** 3D roughing and finishing, rest machining, turning, 3+2 indexing, feature recognition with the shop rules database, PMI and tolerance-based machining, GPU simulation, G-code-driven and machine simulation, Git-style version history, public API and MCP server.
3. **Later:** simultaneous 4/5-axis, mill-turn, drawings, feed optimization, DNC and machine monitoring, similar-part search, optional cloud sync.

**Out of scope entirely:** photorealistic rendering, FEA, sheet metal, electronics and enterprise PLM.

## Constraints

My answers, as of November 17, 2026.

* **Target OS:** Windows, Linux and macOS, with Windows as the primary target. macOS caps OpenGL at 4.1 and has deprecated it, and Qt Quick defaults to Metal there: plan how OCCT and Qt Quick share OpenGL on Mac, and what degrades if they can't.
* **Intent:** open source. Recommend the project's own license and explain the trade-off (for example LGPL like FreeCAD and OCCT, versus GPL v3). Check every dependency against that license; GPL-only components such as SolveSpace's solver or Qt Quick 3D are usable only if our license is GPL-compatible.
* **My time:** 20+ hours per week, plus AI agents doing most of the implementation.
* **Budget for paid components:** $0. Open-source components only, no paid translators. Unsigned builds trigger Windows SmartScreen and macOS Gatekeeper warnings, so propose free code-signing and notarization options for open-source projects.
* **Machines I can test on:** a Mazak (Mazatrol control running EIA/ISO programs), a Haas with the NGC control, and a Fanuc-controlled machine.
* **Benchmark parts:** propose three typical 2.5D job-shop parts for my approval. Together they must cover facing, adaptive pocketing, contours, drilling, tapping and a second setup.
* **Legal:** no proprietary code, assets, icons or file formats copied from any competitor. Matching workflows and ideas is fine.

## Required plan deliverables

The plan must contain all of the following:

1. **Architecture overview:** modules, their responsibilities, and what calls what. Include a text diagram.
2. **Stack decisions:** a final choice per layer, the rationale, rejected alternatives and license obligations.
3. **Core data model:** document, feature tree, persistent naming scheme, sketch, body, setup, stock, tool, holder, operation, toolpath, machine, post, shop rule. Give schemas.
4. **Native file format and version-history storage**, including crash safety and migration between format versions.
5. **Module interfaces:** C++ headers or signatures for every boundary subagents will build against.
6. **Rendering and tier system:** hardware detection, presets, the rendering abstraction, and how High-tier GPU compute plugs in without forking code. Include the theme-token system for light and dark modes, and the Liquid Glass panel effect with its flat fallback.
7. **Threading model:** what runs on the UI thread versus workers, cancellation, progress, and the thread safety of OCCT calls.
8. **Determinism strategy:** how we guarantee identical toolpaths and G-code across tiers, thread counts and machines.
9. **Performance budgets per tier** and how CI measures them.
10. **Repository layout, build system and dependency management:** for example CMake with vcpkg or Conan, plus CI on every target OS.
11. **Testing strategy:** unit tests; geometry regression tests; toolpath and G-code golden files; simulation correctness tests; a corpus of real-world STEP files.
12. **Milestone roadmap for v1.** Every milestone ends in a runnable build I can try, and demos one user-visible workflow.
13. **Task breakdown for v1** in the format below.
14. **Risk register:** the top 10 risks with likelihood, impact and how to reduce each. Cover at least persistent naming, OCCT robustness on bad STEP files, adaptive roughing quality, post-processor correctness, and embedding the OCCT viewport in Qt Quick.
15. **Open questions for me.**

**Format for every task:**

* ID and title
* Goal in one sentence
* Inputs, outputs and the exact files or directories it may touch
* Interfaces it implements or consumes, by name
* Dependencies (task IDs)
* **PARALLEL-SAFE** or **SEQUENTIAL** (anything touching the kernel wrapper, feature tree, persistent naming, document model or renderer core is SEQUENTIAL)
* Acceptance tests with pass/fail criteria an agent can run itself
* Estimated size: S (under 1 day of agent work), M (1-3 days) or L (split it)

## How to work

1. Read this whole brief first. Before planning, ask me every question whose answer would change the architecture, grouped and numbered. Wait for my answers.
2. Push back on anything in this brief you think is wrong, including the stack, the scope and the budgets. Prefer a boring, proven choice over a clever one.
3. Prototype risky decisions before committing to them. If a choice hinges on a fact, such as OCCT tessellation speed on integrated graphics, PlaneGCS stability, or OCCT rendering inside Qt Quick with a blurred glass panel at 30+ FPS on Intel UHD 620, propose a small spike task to measure it first.
4. Write the final plan to `PLAN.md` in the repo root, with interface headers under `docs/interfaces/`. Each task must be completable by an agent that reads only `PLAN.md`, its task entry and the named interfaces.
5. Keep the plan honest: mark guesses as guesses, and give a confidence level on schedule estimates.
6. Do not start implementation until I approve the plan.

Toolkit decision sources:

* [Qt MultiEffect source (license header)](https://codebrowser.dev/qt6/qtdeclarative/src/effects/qquickmultieffect.cpp.html): Qt Quick Effects is LGPL v3
* [Qt MultiEffect docs](https://doc.qt.io/qt/qml-qtquick-effects-multieffect.html): blur, combining with ShaderEffectSource
* [Qt for Windows graphics](https://doc.qt.io/qt-6/windows-graphics.html): Qt Quick defaults to Direct3D 11; how to request OpenGL
* [OCCT forum: rendering into a QQuickItem](https://dev.opencascade.org/content/render-qquikitem): wrapping Qt's framebuffer for OCCT
* [OCCT Qt Quick sample](https://dev.opencascade.org/doc/overview/html/samples_qml_android_occt.html): official 3D Viewer sample in QtQuick/QML
* [Qt Quick 3D license files (Debian)](https://sources.debian.org/src/qt6-quick3d/6.8.2-5/LICENSES): GPL-3.0 and commercial only

