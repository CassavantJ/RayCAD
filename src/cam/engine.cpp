// The CAM engine (cam.h, ICamEngine and makeCamEngine; task T-032): operation status by input hash, and toolpath
// generation on workers.
//
// The engine keeps derived data only: toolpaths by input hash (a bounded cache, so undo, redo and edits that come
// back to an earlier state find their toolpath again), the latest toolpath of each operation (shown while it is
// stale), failures by input hash, and the generation running for each operation. The document holds no toolpath;
// an operation's status is a pure function of (snapshot, build, what the engine holds). forget() drops the latest
// toolpath and running record of operations whose document closed (the latest map would otherwise only grow); the
// bounded cache by hash stays, so a reopened document finds toolpaths of inputs it still has.
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
#include <set>
#include <span>
#include <utility>

namespace ray::cam {

using ToolpathPtr = std::shared_ptr<const Toolpath>;

// Setup resolution (resolveSetupGeometry, the default ResolveGeometryFn) is in setup.cpp (task T-033).

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
    // Every generation whose continuation has not run yet (ticket -> operation), and those of them whose operation was
    // forgotten meanwhile: their result is cached by hash only, never made the operation's latest.
    std::map<std::uint64_t, OperationId> outstanding;
    std::set<std::uint64_t> dropped;

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
            options.resolveGeometry ? std::move(options.resolveGeometry) : ResolveGeometryFn(&resolveSetupGeometry);
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
            s_->outstanding[ticket] = id;
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

    void forget(std::span<const OperationId> ops) override
    {
        const std::set<OperationId> gone(ops.begin(), ops.end());
        std::lock_guard<std::mutex> lock(s_->mutex);
        for (const OperationId& id : gone) {
            s_->latest.erase(id);
            s_->running.erase(id); // not cancelled here: the caller has done that (cam.h)
        }
        for (const auto& [ticket, id] : s_->outstanding) {
            if (gone.count(id) != 0) {
                s_->dropped.insert(ticket);
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
        s.outstanding.erase(ticket);
        const bool forgotten = s.dropped.erase(ticket) != 0; // its operation was forgotten while it ran
        if (result) {
            s.toolpaths[hash] = Shared::Cached{*result, ++s.clock};
            s.failures.erase(hash);
            s.trim(s.toolpaths);
            if (!forgotten && (current || s.latest.count(id) == 0)) {
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
