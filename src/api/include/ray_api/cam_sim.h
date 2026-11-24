// The simulation program of a setup (sim.h SimProgram) built from a document, and the simulator the CAM engine uses to
// carry stock from one setup to the next (task T-075). Not one of the contract headers in include/ray/.
//
// WHY HERE. Building a SimProgram needs the CAM state (cam), the simulator's tool shapes (sim), and for the
// document-level form the document (doc). src/cam may not call src/sim (sim depends on cam, PLAN.md 1.1) and src/ui may
// link only api and render, so the one builder lives in src/api: the CAM engine reaches it through
// CamEngineOptions::simulateSetup (makeSetupSimulator below, set by the app), and the simulation panel (T-074) calls it
// directly, the same function for both.
//
// WHAT T-074 CAN RELY ON (buildSimProgram):
//   - SimProgram::setup, ops, machine and toolDefs point into the CamState the snapshot holds; SimProgramData keeps
//     that CamState alive (`cam`), so the program is valid exactly as long as the SimProgramData is, wherever it is
//     moved to. Toolpaths are shared_ptr (kept by the program itself); tool shapes and soups are values.
//   - ops, toolpaths, tools and toolDefs are parallel, in the order of `toolpaths` (each toolpath names its operation;
//     every one must be an operation of the setup). Pass the setup's operations in setup order to simulate the setup;
//     leaving one out simulates without it.
//   - stockSolid: the setup's starting stock in its WCS: the box, cylinder or solid stock triangulated within
//     kSimSolidChordMm (a curved stock's chords lie inside it by at most that), or for a FromSetupStock setup the
//     carried stock (ProgramOptions::startStock, from ICamEngine::carriedStock) as cam::resolveSetup puts it into the
//     setup's WCS. A FromSetupStock setup without startStock is InvalidArgument.
//   - partSolid (the part bodies united as one soup) and fixtureSolids (one soup per fixture body, in setup order) in
//   the
//     WCS, triangulated within kSimSolidChordMm = 0.009 mm, the bound sim.h asks of partSolid (src/sim/simulator.h
//     kGougePartChordMm), so the checks and the gouge test can run on it. Left empty when ProgramOptions::solids is
//     off (a run without checks and gouges, which reads neither).
//   - modelFromWcs: the setup's frame; `resolved` the whole ResolvedSetup (bounds, both transforms).
// Deterministic: the same document, setup and toolpaths give the same program, bit for bit. Runs kernel calls
// (triangulation takes the kernel's mesh lock): call it on a worker; it polls ctx.cancel between bodies.
#pragma once

#include <ray/api.h>
#include <ray/sim.h>

#include <memory>
#include <span>
#include <vector>

namespace ray::api::cam_sim {

// The chord tolerance of every soup in the program: sim.h's bound on partSolid (0.009 mm; src/sim/simulator.h
// kGougePartChordMm, the checks' part skin less the tool's chords).
inline constexpr double kSimSolidChordMm = 0.009;

struct ProgramOptions {
    bool solids = true;                            // partSolid and fixtureSolids (off: left empty)
    const cam::CarriedStock* startStock = nullptr; // the stock a FromSetupStock setup starts from
    const cam::ResolvedSetup* resolved = nullptr;  // the setup already resolved (with stockSolid, or carriedStock):
                                                   // used as it is instead of resolving it again
};

struct SimProgramData {
    std::shared_ptr<const cam::CamState> cam; // keeps what program points into alive
    cam::ResolvedSetup resolved;              // the setup's frame and stock (stockSolid and carriedStock dropped)
    sim::SimProgram program;
};

// The program of setup `setup` of the snapshot over `toolpaths` (see the header). NotFound for no such setup or a
// toolpath of an operation it does not list (or a tool, holder or machine missing from the document); the errors of
// cam::resolveSetup; KernelFailure when a body or the stock cannot be triangulated; Cancelled.
Result<SimProgramData> buildSimProgram(const cam::CamSnapshot& snapshot, const naming::IResolver& resolver,
                                       const SetupId& setup,
                                       std::span<const std::shared_ptr<const cam::Toolpath>> toolpaths,
                                       const ProgramOptions& options, JobContext& ctx);
// The same on a document snapshot and its build (doc::camSnapshot); InvalidArgument when the model is not built.
Result<SimProgramData> buildSimProgram(const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                                       const SetupId& setup,
                                       std::span<const std::shared_ptr<const cam::Toolpath>> toolpaths,
                                       const ProgramOptions& options, JobContext& ctx);

// CamEngineOptions::simulateSetup: the program (solids off; the request's resolved setup), run by a simulator of its
// own (sim::makeSimulator on `jobs`, a CPU tri-dexel model) at the request's cell (cam::kCarriedStockCellMm) with
// SimOptions::safetyChecks and gouges off, and its finalStock(). Nothing of the tier reaches it: the cell is the
// request's and the result does not depend on the worker count (sim.h). `jobs` must outlive the function.
cam::SimulateSetupFn makeSetupSimulator(IJobSystem& jobs);

} // namespace ray::api::cam_sim
