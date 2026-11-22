// The CAM engine (cam.h, ICamEngine and makeCamEngine; task T-032): operation status by input hash, and toolpath
// generation on workers.
//
// The engine keeps derived data only: toolpaths by input hash (a bounded cache, so undo, redo and edits that come
// back to an earlier state find their toolpath again), the latest toolpath of each operation (shown while it is
// stale), failures by input hash, and the generation running for each operation. The document holds no toolpath;
// an operation's status is a pure function of (snapshot, build, what the engine holds).
//
// Threads: status() reads under the mutex from any thread. generate() computes the input hash on the calling thread,
// submits the generator to the job system and records the running job; the job's continuation, posted to the UI
// thread by the job system, files the result under the hash it was made for. A result whose inputs changed while it
// ran is therefore filed under the old hash: the operation's current hash finds nothing and it stays Stale, which is
// what PLAN.md 7 asks for (the result goes into the cache and is otherwise ignored).
#include "state.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <new>
#include <utility>

namespace ray::cam {

using ToolpathPtr = std::shared_ptr<const Toolpath>;

// ---------- The stand-in setup resolution (until T-033) ----------

Result<ResolvedGeometry> resolveGeometryBasic(const GeometryRequest& request, JobContext& ctx)
{
    try {
        const CamSnapshot& snapshot = request.snapshot;
        const Setup& setup = request.setup;
        ResolvedGeometry geom;
        geom.wcsFromModel = Xform::identity();
        Box3 box{};
        bool any = false;
        const auto grow = [&](const kernel::Shape& shape) {
            const Box3 b = kernel::bounds(shape);
            if (!any) {
                box = b;
                any = true;
                return;
            }
            box.min = Vec3{std::min(box.min.x, b.min.x), std::min(box.min.y, b.min.y), std::min(box.min.z, b.min.z)};
            box.max = Vec3{std::max(box.max.x, b.max.x), std::max(box.max.y, b.max.y), std::max(box.max.z, b.max.z)};
        };
        for (const BodyId& id : setup.partBodies) {
            const naming::ResolverBody* body = detail::bodyOf(snapshot, id);
            if (body == nullptr) {
                return tl::unexpected(
                    Error{ErrorCode::ReferenceBroken,
                          "Part body " + id.value.toString() + " of setup " + detail::label(setup) + " is not built",
                          {}});
            }
            geom.partShapes.push_back(body->shape);
            grow(body->shape);
        }
        for (const BodyId& id : setup.fixtureBodies) {
            const naming::ResolverBody* body = detail::bodyOf(snapshot, id);
            if (body == nullptr) {
                return tl::unexpected(
                    Error{ErrorCode::ReferenceBroken,
                          "Fixture body " + id.value.toString() + " of setup " + detail::label(setup) + " is not built",
                          {}});
            }
            geom.fixtureShapes.push_back(body->shape);
        }
        RAY_CAM_TRY(ctx.cancel.check());
        for (std::size_t i = 0; i < request.op.geometry.size(); ++i) {
            const naming::TopoRef& ref = request.op.geometry[i];
            const auto part = std::find(setup.partBodies.begin(), setup.partBodies.end(), ref.body);
            const std::string what = "Geometry " + std::to_string(i + 1) + " of " + detail::label(request.op);
            if (part == setup.partBodies.end()) {
                return tl::unexpected(Error{
                    ErrorCode::ReferenceBroken, what + " is not on a part body of setup " + detail::label(setup), {}});
            }
            auto resolved = detail::resolveForCam(request.resolver, snapshot, ref, what);
            if (!resolved) {
                return tl::unexpected(resolved.error());
            }
            for (const kernel::SubId& e : resolved->elements) {
                geom.selected.push_back(e);
                geom.selectedShapeIndex.push_back(static_cast<int>(part - setup.partBodies.begin()));
            }
        }
        if (const auto* stock = std::get_if<BoxStock>(&setup.stock)) {
            box.min = Vec3{box.min.x - stock->xNeg, box.min.y - stock->yNeg, box.min.z - stock->zNeg};
            box.max = Vec3{box.max.x + stock->xPos, box.max.y + stock->yPos, box.max.z + stock->zPos};
        } else if (const auto* solid = std::get_if<SolidStock>(&setup.stock)) {
            if (const naming::ResolverBody* body = detail::bodyOf(snapshot, solid->body)) {
                box = kernel::bounds(body->shape);
            }
        }
        geom.stockBounds = box;
        return geom;
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while resolving a setup", {}});
    }
}

namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

OpStatus statusOf(const Error& e)
{
    return e.code == ErrorCode::ReferenceBroken ? OpStatus::ReferenceBroken : OpStatus::Failed;
}

// What the engine holds, shared with job continuations (which hold it weakly).
struct Shared {
    std::mutex mutex;
    std::size_t capacity = 256;
    std::uint64_t clock = 0; // use counter of the bounded maps
    struct Cached {
        ToolpathPtr toolpath;
        std::uint64_t used = 0;
    };
    std::map<Hash256, Cached> toolpaths; // by input hash
    struct Failure {
        std::string message;
        std::uint64_t used = 0;
    };
    std::map<Hash256, Failure> failures;       // by input hash
    std::map<OperationId, ToolpathPtr> latest; // each operation's last toolpath, whatever its hash
    struct Running {
        Hash256 hash;
        std::uint64_t ticket = 0;
        JobHandle<ToolpathPtr> job;
    };
    std::map<OperationId, Running> running;
    std::uint64_t nextTicket = 1;

    // Caller holds the mutex. Drops the least recently used entries above the capacity.
    template <class Map>
    void trim(Map& map)
    {
        while (map.size() > capacity) {
            auto oldest = map.begin();
            for (auto it = map.begin(); it != map.end(); ++it) {
                if (it->second.used < oldest->second.used) {
                    oldest = it;
                }
            }
            map.erase(oldest);
        }
    }
};

// One operation checked against a snapshot: its input hash, or the status it has without one.
struct Checked {
    std::optional<Hash256> hash;
    OpState blocked; // when there is no hash
};

class Engine final : public ICamEngine {
public:
    Engine(IJobSystem& jobs, std::shared_ptr<const OperationRegistry> types, CamSnapshotFn snapshot,
           CamEngineOptions options)
        : jobs_(jobs), types_(std::move(types)), snapshot_(std::move(snapshot)), s_(std::make_shared<Shared>())
    {
        resolveGeometry_ =
            options.resolveGeometry ? std::move(options.resolveGeometry) : ResolveGeometryFn(&resolveGeometryBasic);
        s_->capacity = std::max<std::size_t>(options.cacheEntries, 1);
        if (!types_) {
            types_ = std::make_shared<const OperationRegistry>();
        }
    }

    ~Engine() override
    {
        std::map<OperationId, Shared::Running> running;
        {
            std::lock_guard<std::mutex> lock(s_->mutex);
            running.swap(s_->running);
        }
        for (auto& [id, r] : running) {
            r.job.cancel();
        }
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::map<OperationId, OpState> status(doc::DocStatePtr state, doc::BuildResultPtr build) const override
    {
        std::map<OperationId, OpState> out;
        try {
            const CamSnapshot snap = snapshot_ ? snapshot_(state, build) : CamSnapshot{};
            if (!snap.cam) {
                return out;
            }
            const auto resolver = naming::makeResolver(snap.bodies);
            for (const auto& [id, op] : snap.cam->operations) {
                (void)op;
                const Checked checked = check(snap, resolver.get(), id);
                std::lock_guard<std::mutex> lock(s_->mutex);
                out[id] = checked.hash ? lookupLocked(id, *checked.hash) : withLatestLocked(id, checked.blocked);
            }
        } catch (...) {
            // Out of memory: report nothing rather than something partial and wrong.
            out.clear();
        }
        return out;
    }

    JobHandle<ToolpathPtr> generate(doc::DocStatePtr state, doc::BuildResultPtr build, OperationId id) override
    {
        try {
            auto snap = std::make_shared<const CamSnapshot>(snapshot_ ? snapshot_(state, build) : CamSnapshot{});
            if (!snap->cam || snap->cam->operations.count(id) == 0) {
                return failedJob(
                    Error{ErrorCode::NotFound, "Operation " + id.value.toString() + " is not in the document", {}});
            }
            std::shared_ptr<const naming::IResolver> resolver = naming::makeResolver(snap->bodies);
            if (!resolver) {
                return failedJob(Error{ErrorCode::Internal, "Out of memory", {}});
            }
            const Checked checked = check(*snap, resolver.get(), id);
            if (!checked.hash) {
                const ErrorCode code = checked.blocked.status == OpStatus::ReferenceBroken ? ErrorCode::ReferenceBroken
                                       : checked.blocked.status == OpStatus::Suppressed    ? ErrorCode::InvalidArgument
                                                                                           : ErrorCode::ToolpathFailed;
                return failedJob(Error{code, checked.blocked.message, {}});
            }
            const Hash256 hash = *checked.hash;

            std::lock_guard<std::mutex> lock(s_->mutex);
            if (auto cached = s_->toolpaths.find(hash); cached != s_->toolpaths.end()) {
                cached->second.used = ++s_->clock;
                ToolpathPtr toolpath = cached->second.toolpath;
                s_->latest[id] = toolpath;
                return jobs_.submit<ToolpathPtr>(JobInfo{"Toolpath", JobPriority::Interactive, false},
                                                 [toolpath](JobContext&) -> Result<ToolpathPtr> { return toolpath; });
            }
            if (auto run = s_->running.find(id); run != s_->running.end()) {
                if (run->second.hash == hash && !run->second.job.done()) {
                    return run->second.job; // the same inputs are being generated already
                }
                run->second.job.cancel(); // pointless now: its inputs are not the operation's any more
                s_->running.erase(run);
            }
            const std::uint64_t ticket = s_->nextTicket++;
            std::shared_ptr<const OperationRegistry> types = types_;
            ResolveGeometryFn resolve = resolveGeometry_;
            const std::string title = "Generate " + snap->cam->operations.at(id).name;
            JobHandle<ToolpathPtr> job = jobs_.submit<ToolpathPtr>(
                JobInfo{title, JobPriority::Normal, true},
                [snap, resolver, types, resolve, id, hash](JobContext& ctx) -> Result<ToolpathPtr> {
                    return run(*snap, *resolver, *types, resolve, id, hash, ctx);
                });
            s_->running[id] = Shared::Running{hash, ticket, job};
            std::weak_ptr<Shared> weak = s_;
            job.then([weak, id, ticket, hash](Result<ToolpathPtr> result) {
                if (auto s = weak.lock()) {
                    apply(*s, id, ticket, hash, std::move(result));
                }
            });
            return job;
        } catch (...) {
            return failedJob(Error{ErrorCode::Internal, "Out of memory while starting a toolpath", {}});
        }
    }

    void generateAllStale(doc::DocStatePtr state, doc::BuildResultPtr build) override
    {
        for (const auto& [id, st] : status(state, build)) {
            if (st.status == OpStatus::Stale) {
                (void)generate(state, build, id);
            }
        }
    }

private:
    // The input hash of `id`, or the status it has without one. `resolver` is over snap.bodies.
    Checked check(const CamSnapshot& snap, const naming::IResolver* resolver, const OperationId& id) const
    {
        Checked out;
        const Operation& op = snap.cam->operations.at(id);
        if (op.suppressed) {
            out.blocked = OpState{OpStatus::Suppressed, "Suppressed", nullptr};
            return out;
        }
        if (types_->find(op.type) == nullptr) {
            out.blocked = OpState{OpStatus::Failed, "Unknown operation type '" + op.type + "'", nullptr};
            return out;
        }
        if (!snap.built) {
            out.blocked = OpState{OpStatus::Stale, "Waiting for the model to build", nullptr};
            return out;
        }
        if (resolver == nullptr) {
            out.blocked = OpState{OpStatus::Failed, "Out of memory", nullptr};
            return out;
        }
        auto hash = operationInputHash(snap, *resolver, id);
        if (!hash) {
            out.blocked = OpState{statusOf(hash.error()), hash.error().message, nullptr};
            return out;
        }
        out.hash = *hash;
        return out;
    }

    // Caller holds the mutex.
    OpState lookupLocked(const OperationId& id, const Hash256& hash) const
    {
        if (const auto cached = s_->toolpaths.find(hash); cached != s_->toolpaths.end()) {
            cached->second.used = ++s_->clock;
            return OpState{OpStatus::UpToDate, {}, cached->second.toolpath};
        }
        if (const auto run = s_->running.find(id); run != s_->running.end() && run->second.hash == hash) {
            return withLatestLocked(id, OpState{OpStatus::Generating, "Generating", nullptr});
        }
        if (const auto failed = s_->failures.find(hash); failed != s_->failures.end()) {
            failed->second.used = ++s_->clock;
            return withLatestLocked(id, OpState{OpStatus::Failed, failed->second.message, nullptr});
        }
        return withLatestLocked(id, OpState{OpStatus::Stale, "The toolpath is out of date", nullptr});
    }

    // Caller holds the mutex. The operation's last toolpath (for display) on a status that is not UpToDate.
    OpState withLatestLocked(const OperationId& id, OpState st) const
    {
        if (st.status != OpStatus::Suppressed) {
            if (const auto latest = s_->latest.find(id); latest != s_->latest.end()) {
                st.toolpath = latest->second;
            }
        }
        return st;
    }

    JobHandle<ToolpathPtr> failedJob(Error error)
    {
        return jobs_.submit<ToolpathPtr>(JobInfo{"Toolpath", JobPriority::Interactive, false},
                                         [error](JobContext&) -> Result<ToolpathPtr> { return tl::unexpected(error); });
    }

    // The worker side: setup resolution, validation and the generator.
    static Result<ToolpathPtr> run(const CamSnapshot& snap, const naming::IResolver& resolver,
                                   const OperationRegistry& types, const ResolveGeometryFn& resolve,
                                   const OperationId& id, const Hash256& hash, JobContext& ctx)
    {
        RAY_CAM_TRY(ctx.cancel.check());
        const CamState& cam = *snap.cam;
        const Operation& op = cam.operations.at(id);
        const Setup* setup = detail::setupOf(cam, id);
        const IOperationType* type = types.find(op.type);
        if (setup == nullptr || type == nullptr) {
            return failure(ErrorCode::Internal, "The operation changed while its toolpath was being prepared");
        }
        auto items = detail::generatorItemsOf(cam, *setup, op); // only what the input hash covers (cam.h)
        if (!items) {
            return tl::unexpected(items.error());
        }
        Progress resolving = ctx.progress.sub(0.1);
        JobContext resolveCtx{ctx.cancel, resolving};
        resolving.set(0, "Resolving geometry");
        auto geom = resolve(GeometryRequest{snap, *setup, op, resolver}, resolveCtx);
        if (!geom) {
            return tl::unexpected(geom.error());
        }
        resolving.set(1);
        const GenerateInput input{op, items->tool, items->holder ? &*items->holder : nullptr, items->machine, *geom};
        if (auto valid = type->validate(input); !valid) {
            return tl::unexpected(valid.error());
        }
        RAY_CAM_TRY(ctx.cancel.check());
        Progress generating = ctx.progress.sub(0.9);
        JobContext genCtx{ctx.cancel, generating};
        auto made = type->generate(input, genCtx);
        if (!made) {
            return tl::unexpected(made.error());
        }
        made->operation = id;
        made->inputHash = hash;
        ctx.progress.set(1.0, "Done");
        return std::make_shared<const Toolpath>(std::move(*made));
    }

    // The UI-thread side: files the result under the hash it was made for.
    static void apply(Shared& s, const OperationId& id, std::uint64_t ticket, const Hash256& hash,
                      Result<ToolpathPtr> result)
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto run = s.running.find(id);
        const bool current = run != s.running.end() && run->second.ticket == ticket;
        if (current) {
            s.running.erase(run);
        }
        if (result) {
            s.toolpaths[hash] = Shared::Cached{*result, ++s.clock};
            s.failures.erase(hash);
            s.trim(s.toolpaths);
            if (current || s.latest.count(id) == 0) {
                s.latest[id] = *result;
            }
        } else if (result.error().code != ErrorCode::Cancelled) {
            s.failures[hash] = Shared::Failure{result.error().message, ++s.clock};
            s.trim(s.failures);
        }
    }

    IJobSystem& jobs_;
    std::shared_ptr<const OperationRegistry> types_;
    CamSnapshotFn snapshot_;
    ResolveGeometryFn resolveGeometry_;
    std::shared_ptr<Shared> s_;
};

} // namespace

std::unique_ptr<ICamEngine> makeCamEngine(IJobSystem& jobs, std::shared_ptr<const OperationRegistry> types,
                                          CamSnapshotFn snapshot, CamEngineOptions options)
{
    try {
        return std::make_unique<Engine>(jobs, std::move(types), std::move(snapshot), std::move(options));
    } catch (...) {
        return nullptr;
    }
}

} // namespace ray::cam
