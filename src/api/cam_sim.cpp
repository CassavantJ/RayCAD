// The simulation program of a setup, and the simulator that carries stock between setups (ray_api/cam_sim.h, T-075).
#include <ray_api/cam_sim.h>

#include <algorithm>
#include <new>
#include <utility>

namespace ray::api::cam_sim {

namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

std::string label(const cam::Setup& setup)
{
    return setup.name.empty() ? "setup " + setup.id.value.toString() : "setup '" + setup.name + "'";
}

const naming::ResolverBody* bodyOf(const cam::CamSnapshot& snap, const BodyId& id)
{
    const auto it =
        std::find_if(snap.bodies.begin(), snap.bodies.end(), [&](const naming::ResolverBody& b) { return b.id == id; });
    return it == snap.bodies.end() ? nullptr : &*it;
}

// `shape` triangulated within kSimSolidChordMm, moved by `xf` (model -> WCS), appended to `out`.
Status appendSoup(const kernel::Shape& shape, const Xform* xf, kernel::TriSoup& out, const std::string& what,
                  JobContext& ctx)
{
    auto soup = kernel::triangulateExact(shape, kSimSolidChordMm, ctx);
    if (!soup) {
        if (soup.error().code == ErrorCode::Cancelled) {
            return tl::unexpected(soup.error());
        }
        return failure(ErrorCode::KernelFailure, what + " could not be triangulated: " + soup.error().message);
    }
    const std::size_t base = out.positions.size() / 3;
    out.positions.reserve(out.positions.size() + soup->positions.size());
    for (std::size_t i = 0; i + 2 < soup->positions.size(); i += 3) {
        const double x = soup->positions[i], y = soup->positions[i + 1], z = soup->positions[i + 2];
        if (xf == nullptr) {
            out.positions.insert(out.positions.end(), {x, y, z});
            continue;
        }
        const double* m = xf->m;
        out.positions.insert(out.positions.end(),
                             {m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7],
                              m[8] * x + m[9] * y + m[10] * z + m[11]});
    }
    out.indices.reserve(out.indices.size() + soup->indices.size());
    for (const std::uint32_t v : soup->indices) {
        out.indices.push_back(static_cast<std::uint32_t>(base + v));
    }
    return {};
}

} // namespace

Result<SimProgramData> buildSimProgram(const cam::CamSnapshot& snapshot, const naming::IResolver& resolver,
                                       const SetupId& setupId,
                                       std::span<const std::shared_ptr<const cam::Toolpath>> toolpaths,
                                       const ProgramOptions& options, JobContext& ctx)
{
    try {
        if (auto st = ctx.cancel.check(); !st) {
            return tl::unexpected(st.error());
        }
        if (!snapshot.cam) {
            return failure(ErrorCode::NotFound, "The document has no CAM state");
        }
        SimProgramData out;
        out.cam = snapshot.cam;
        const cam::CamState& cam = *out.cam;
        const auto setupIt =
            std::find_if(cam.setups.begin(), cam.setups.end(), [&](const cam::Setup& s) { return s.id == setupId; });
        if (setupIt == cam.setups.end()) {
            return failure(ErrorCode::NotFound, "The document has no setup " + setupId.value.toString());
        }
        const cam::Setup& setup = *setupIt;
        const bool carried = std::holds_alternative<cam::FromSetupStock>(setup.stock);

        // The frame and the starting stock.
        cam::ResolvedSetup resolved;
        if (options.resolved != nullptr) {
            resolved = *options.resolved;
        } else {
            if (carried && options.startStock == nullptr) {
                return failure(ErrorCode::InvalidArgument,
                               "The stock of " + label(setup) +
                                   " comes from an earlier setup: simulate that first (ICamEngine::carriedStock)");
            }
            cam::SetupResolveOptions ro;
            ro.stockSolid = true;
            ro.carried = options.startStock;
            auto r = cam::resolveSetup(snapshot, setup, resolver, ctx, ro);
            if (!r) {
                return tl::unexpected(r.error());
            }
            resolved = std::move(*r);
        }
        sim::SimProgram& p = out.program;
        p.setup = &setup;
        p.modelFromWcs = resolved.modelFromWcs;
        if (resolved.carriedStock) {
            p.stockSolid = *resolved.carriedStock;
            // The fixture check allows for its cells (sim.h SimProgram::stockCellMm).
            p.stockCellMm = options.startStock != nullptr ? options.startStock->cellMm : cam::kCarriedStockCellMm;
        } else if (!resolved.stockSolid.isNull()) {
            if (auto st = appendSoup(resolved.stockSolid, nullptr, p.stockSolid, "The stock of " + label(setup), ctx);
                !st) {
                return tl::unexpected(st.error());
            }
        } else {
            return failure(ErrorCode::InvalidArgument, "The stock of " + label(setup) + " has no solid");
        }

        // The machine, and each operation with its toolpath, tool shape and tool.
        const auto machine = cam.machines.find(setup.machine);
        if (machine == cam.machines.end()) {
            return failure(ErrorCode::NotFound, "The machine of " + label(setup) + " is not in the document");
        }
        p.machine = &machine->second.item;
        for (const std::shared_ptr<const cam::Toolpath>& path : toolpaths) {
            if (!path) {
                return failure(ErrorCode::InvalidArgument, "A toolpath of " + label(setup) + " is missing");
            }
            const auto op = cam.operations.find(path->operation);
            if (op == cam.operations.end() || std::find(setup.operations.begin(), setup.operations.end(),
                                                        path->operation) == setup.operations.end()) {
                return failure(ErrorCode::NotFound,
                               "Operation " + path->operation.value.toString() + " is not in " + label(setup));
            }
            const cam::Operation& o = op->second;
            const auto tool = cam.tools.find(o.tool.tool);
            if (tool == cam.tools.end()) {
                return failure(ErrorCode::NotFound, "The tool of operation '" + o.name + "' is not in the document");
            }
            const lib::Holder* holder = nullptr;
            if (o.tool.holder.value != Uuid{}) {
                const auto h = cam.holders.find(o.tool.holder);
                if (h == cam.holders.end()) {
                    return failure(ErrorCode::NotFound,
                                   "The holder of operation '" + o.name + "' is not in the document");
                }
                holder = &h->second.item;
            }
            p.ops.push_back(&o);
            p.toolpaths.push_back(path);
            p.tools.push_back(sim::makeToolShape(tool->second.item, holder, o.tool.stickOut));
            p.toolDefs.push_back(&tool->second.item);
        }

        // The part and the fixtures, for the checks and the gouge test.
        if (options.solids) {
            for (std::size_t i = 0; i < setup.partBodies.size(); ++i) {
                const naming::ResolverBody* body = bodyOf(snapshot, setup.partBodies[i]);
                const std::string what = "Part body " + std::to_string(i + 1) + " of " + label(setup);
                if (body == nullptr || body->shape.isNull()) {
                    return failure(ErrorCode::ReferenceBroken, what + " is not built");
                }
                if (auto st = appendSoup(body->shape, &resolved.wcsFromModel, p.partSolid, what, ctx); !st) {
                    return tl::unexpected(st.error());
                }
            }
            for (std::size_t i = 0; i < setup.fixtureBodies.size(); ++i) {
                const naming::ResolverBody* body = bodyOf(snapshot, setup.fixtureBodies[i]);
                const std::string what = "Fixture body " + std::to_string(i + 1) + " of " + label(setup);
                if (body == nullptr || body->shape.isNull()) {
                    return failure(ErrorCode::ReferenceBroken, what + " is not built");
                }
                kernel::TriSoup soup;
                if (auto st = appendSoup(body->shape, &resolved.wcsFromModel, soup, what, ctx); !st) {
                    return tl::unexpected(st.error());
                }
                p.fixtureSolids.push_back(std::move(soup));
            }
        }
        resolved.stockSolid = kernel::Shape{};
        resolved.carriedStock.reset();
        out.resolved = std::move(resolved);
        return out;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while building the simulation program");
    }
}

Result<SimProgramData> buildSimProgram(const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                                       const SetupId& setup,
                                       std::span<const std::shared_ptr<const cam::Toolpath>> toolpaths,
                                       const ProgramOptions& options, JobContext& ctx)
{
    if (!state || !build) {
        return failure(ErrorCode::InvalidArgument, "The model is not built yet");
    }
    const cam::CamSnapshot snap = doc::camSnapshot(state, build);
    const auto resolver = naming::makeResolver(snap.bodies);
    if (!resolver) {
        return failure(ErrorCode::Internal, "Out of memory");
    }
    return buildSimProgram(snap, *resolver, setup, toolpaths, options, ctx);
}

cam::SimulateSetupFn makeSetupSimulator(IJobSystem& jobs)
{
    IJobSystem* system = &jobs;
    return [system](const cam::SimulateSetupRequest& request, JobContext& ctx) -> Result<kernel::TriSoup> {
        ProgramOptions options;
        options.solids = false; // no checks and no gouges read them
        options.resolved = &request.resolved;
        Progress building = ctx.progress.sub(0.05);
        JobContext buildCtx{ctx.cancel, building};
        auto program =
            buildSimProgram(request.snapshot, request.resolver, request.setup.id, request.toolpaths, options, buildCtx);
        if (!program) {
            return tl::unexpected(program.error());
        }
        std::unique_ptr<sim::ISimulator> simulator = sim::makeSimulator(*system, nullptr);
        if (!simulator) {
            return failure(ErrorCode::Internal, "The simulator could not start");
        }
        sim::SimOptions run;
        run.resolution = sim::StockResolution{request.cellMm};
        run.safetyChecks = false;
        run.gouges = false;
        Progress running = ctx.progress.sub(0.95);
        JobContext runCtx{ctx.cancel, running};
        if (auto st = simulator->run(program->program, run, runCtx); !st) {
            return tl::unexpected(st.error());
        }
        return simulator->finalStock();
    };
}

} // namespace ray::api::cam_sim
