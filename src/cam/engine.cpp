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
//
// T-045: listeners (subscribe) hear of every change of what the engine holds about an operation, on the UI thread:
// a generation started (a small job whose continuation posts the notice, so it never runs inside generate()), its
// progress (the job's onProgress, also kept in the running record for OpState::progress), its end (the continuation
// that files it) and a preview filed in the cache. statusAsync, validate and preview are jobs that hold the shared
// state, the registry and the snapshot function themselves (never the engine), so they may outlive it.
//
// T-075, in-process stock. An operation of a FromSetupStock setup needs the stock the earlier setup leaves
// (CarriedStock). prepare() asks obtainCarried for it, keyed by detail::carriedStockKey (the FromSetupStock part of the
// input hash):
//   - in the cache (Shared::carried, at most CamEngineOptions::carriedEntries, least recently used first out): taken;
//     a new key is the only invalidation, so an edit that cannot change that stock (a name, the later setup's own
//     operations, its WCS) never simulates again, and undo finds the old one while it is kept;
//   - being made by another job (Shared::carriedRunning): waited for, polling this job's own cancel every
//     kCarriedWaitPoll (the timing decides only who makes it, never what is made); when that job was cancelled, this
//     one makes it;
//   - else made here, inside this job (its progress and cancel): the earlier setup's toolpaths from the cache, or
//     generated here and filed in the cache by input hash as a preview is (so those operations show up to date, and
//     their listeners hear of it), its frame and starting stock (resolveSetup; recursively the carried stock of a
//     chain), then CamEngineOptions::simulateSetup. A generator failure of the earlier setup is remembered under that
//     operation's hash, as any failure is, and fails the later operation naming it.
// Nothing here waits on a queued job, so a pool of any size, one worker included, always gets through: the job that
// makes a stock runs it on its own thread.
#include "state.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
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
        double progress = -1; // T-045: the last progress the job reported
    };
    std::map<OperationId, Running> running;
    std::uint64_t nextTicket = 1;
    // Every generation whose continuation has not run yet (ticket -> operation), and those of them whose operation was
    // forgotten meanwhile: their result is cached by hash only, never made the operation's latest.
    std::map<std::uint64_t, OperationId> outstanding;
    std::set<std::uint64_t> dropped;
    // T-045: the listeners, by subscription id.
    std::map<int, ICamEngine::Listener> listeners;
    int nextListener = 1;
    // T-075: carried stocks by CarriedStock::key (bounded by carriedCapacity), and those being made.
    struct CarriedEntry {
        std::shared_ptr<const CarriedStock> stock;
        std::uint64_t used = 0;
    };
    std::map<Hash256, CarriedEntry> carried;
    std::size_t carriedCapacity = 4;
    struct PendingCarried {
        std::mutex mutex;
        std::condition_variable done;
        std::optional<Result<std::shared_ptr<const CarriedStock>>> result;
    };
    std::map<Hash256, std::shared_ptr<PendingCarried>> carriedRunning;
    void trimCarried()
    {
        while (carried.size() > carriedCapacity) {
            auto oldest = carried.begin();
            for (auto it = carried.begin(); it != carried.end(); ++it) {
                if (it->second.used < oldest->second.used) {
                    oldest = it;
                }
            }
            carried.erase(oldest);
        }
    }

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

    // UI thread, without the mutex: tells every listener about `id`. A listener may subscribe, unsubscribe or call
    // the engine: the calls go to a copy of the list.
    void notify(const OperationId& id)
    {
        std::vector<ICamEngine::Listener> copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& [n, l] : listeners) {
                copy.push_back(l);
            }
        }
        for (const ICamEngine::Listener& l : copy) {
            try {
                l(id);
            } catch (...) {
                // A listener's failure is its own; the others still hear.
            }
        }
    }

    bool hasListeners()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return !listeners.empty();
    }

    // Caller holds the mutex. The operation's last toolpath (for display) on a status that is not UpToDate.
    OpState withLatestLocked(const OperationId& id, OpState st)
    {
        if (st.status != OpStatus::Suppressed) {
            if (const auto it = latest.find(id); it != latest.end()) {
                st.toolpath = it->second;
            }
        }
        return st;
    }

    // Caller holds the mutex.
    OpState lookupLocked(const OperationId& id, const Hash256& hash)
    {
        if (const auto cached = toolpaths.find(hash); cached != toolpaths.end()) {
            cached->second.used = ++clock;
            return OpState{OpStatus::UpToDate, {}, cached->second.toolpath};
        }
        if (const auto run = running.find(id); run != running.end() && run->second.hash == hash) {
            OpState st = withLatestLocked(id, OpState{OpStatus::Generating, "Generating", nullptr});
            st.progress = run->second.progress;
            return st;
        }
        if (const auto failed = failures.find(hash); failed != failures.end()) {
            failed->second.used = ++clock;
            return withLatestLocked(id, OpState{OpStatus::Failed, failed->second.message, nullptr});
        }
        return withLatestLocked(id, OpState{OpStatus::Stale, "The toolpath is out of date", nullptr});
    }
};

// One operation checked against a snapshot: its input hash, or the status it has without one.
struct Checked {
    std::optional<Hash256> hash;
    OpState blocked; // when there is no hash
};

// The input hash of `id`, or the status it has without one. `resolver` is over snap.bodies.
Checked check(const CamSnapshot& snap, const naming::IResolver* resolver, const OperationRegistry& types,
              const OperationId& id)
{
    Checked out;
    const Operation& op = snap.cam->operations.at(id);
    if (op.suppressed) {
        out.blocked = OpState{OpStatus::Suppressed, "Suppressed", nullptr};
        return out;
    }
    if (types.find(op.type) == nullptr) {
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

// The error a generation reports for an operation without an input hash.
Error blockedError(const Checked& checked)
{
    const ErrorCode code = checked.blocked.status == OpStatus::ReferenceBroken ? ErrorCode::ReferenceBroken
                           : checked.blocked.status == OpStatus::Suppressed    ? ErrorCode::InvalidArgument
                                                                               : ErrorCode::ToolpathFailed;
    return Error{code, checked.blocked.message, {}};
}

// The status of every operation of a snapshot (status() and statusAsync). `ctx` (may be null) stops it early.
std::map<OperationId, OpState> computeStatus(Shared& s, const OperationRegistry& types, const CamSnapshotFn& snapshot,
                                             const doc::DocStatePtr& state, const doc::BuildResultPtr& build,
                                             JobContext* ctx)
{
    std::map<OperationId, OpState> out;
    try {
        const CamSnapshot snap = snapshot ? snapshot(state, build) : CamSnapshot{};
        if (!snap.cam) {
            return out;
        }
        const auto resolver = naming::makeResolver(snap.bodies);
        for (const auto& [id, op] : snap.cam->operations) {
            (void)op;
            if (ctx != nullptr && ctx->cancel.cancelled()) {
                return {};
            }
            const Checked checked = check(snap, resolver.get(), types, id);
            std::lock_guard<std::mutex> lock(s.mutex);
            out[id] = checked.hash ? s.lookupLocked(id, *checked.hash) : s.withLatestLocked(id, checked.blocked);
        }
    } catch (...) {
        // Out of memory: report nothing rather than something partial and wrong.
        out.clear();
    }
    return out;
}

using CarriedPtr = std::shared_ptr<const CarriedStock>;

// What the worker side of a generation needs of the engine; jobs copy it (they hold the shared state, never the
// engine). `jobs` outlives the engine and its jobs (cam.h makeCamEngine).
struct Parts {
    IJobSystem* jobs = nullptr;
    std::shared_ptr<Shared> s;
    std::shared_ptr<const OperationRegistry> types;
    ResolveGeometryFn resolve;
    SimulateSetupFn simulate;
};

// How often a job waiting for a carried stock that another job makes looks at its own cancel token.
constexpr std::chrono::milliseconds kCarriedWaitPoll{20};
// The share of a generation's progress that making its carried stock takes, when it has to (the simulation and the
// earlier setup's toolpaths are most of the work then).
constexpr double kCarriedProgressWeight = 0.7;

Result<CarriedPtr> obtainCarried(const Parts& parts, const CamSnapshot& snap, const naming::IResolver& resolver,
                                 const Setup& setup, JobContext& ctx);

// What a generator is given, worked out on a worker: setup resolution and the reduced items.
struct Prepared {
    const Operation* op = nullptr;
    const IOperationType* type = nullptr;
    detail::GeneratorItems items;
    ResolvedGeometry geom;
    GenerateInput input() const
    {
        return GenerateInput{*op, items.tool, items.holder ? &*items.holder : nullptr, items.machine, geom};
    }
};

// Whether the setup of operation `id` carries its stock from an earlier one and the engine can make it.
bool needsCarried(const Parts& parts, const CamSnapshot& snap, const OperationId& id)
{
    const Setup* setup = detail::setupOf(*snap.cam, id);
    return setup != nullptr && parts.simulate && std::holds_alternative<FromSetupStock>(setup->stock);
}

// `weight`: the share of ctx.progress for resolving the geometry; `carriedWeight` that for the carried stock (T-075),
// used only when the setup has one.
Result<Prepared> prepare(const CamSnapshot& snap, const naming::IResolver& resolver, const Parts& parts,
                         const OperationId& id, JobContext& ctx, double weight, double carriedWeight)
{
    RAY_CAM_TRY(ctx.cancel.check());
    const CamState& cam = *snap.cam;
    Prepared out;
    out.op = &cam.operations.at(id);
    const Setup* setup = detail::setupOf(cam, id);
    out.type = parts.types->find(out.op->type);
    if (setup == nullptr || out.type == nullptr) {
        return failure(ErrorCode::Internal, "The operation changed while its toolpath was being prepared");
    }
    auto items = detail::generatorItemsOf(cam, *setup, *out.op); // only what the input hash covers (cam.h)
    if (!items) {
        return tl::unexpected(items.error());
    }
    out.items = std::move(*items);
    CarriedPtr carried; // T-075: kept for the resolution below
    if (needsCarried(parts, snap, id)) {
        Progress carrying = ctx.progress.sub(carriedWeight);
        JobContext carriedCtx{ctx.cancel, carrying};
        auto made = obtainCarried(parts, snap, resolver, *setup, carriedCtx);
        if (!made) {
            return tl::unexpected(made.error());
        }
        carrying.set(1);
        carried = std::move(*made);
    }
    Progress resolving = ctx.progress.sub(weight);
    JobContext resolveCtx{ctx.cancel, resolving};
    resolving.set(0, "Resolving geometry");
    auto geom = parts.resolve(GeometryRequest{snap, *setup, *out.op, resolver, carried.get()}, resolveCtx);
    if (!geom) {
        return tl::unexpected(geom.error());
    }
    resolving.set(1);
    out.geom = std::move(*geom);
    return out;
}

// The worker side of a generation: setup resolution, validation and the generator.
Result<ToolpathPtr> runGenerator(const CamSnapshot& snap, const naming::IResolver& resolver, const Parts& parts,
                                 const OperationId& id, const Hash256& hash, JobContext& ctx)
{
    const double carriedWeight = needsCarried(parts, snap, id) ? kCarriedProgressWeight : 0.0;
    const double resolveWeight = 0.1 * (1.0 - carriedWeight);
    auto prepared = prepare(snap, resolver, parts, id, ctx, resolveWeight, carriedWeight);
    if (!prepared) {
        return tl::unexpected(prepared.error());
    }
    const GenerateInput input = prepared->input();
    if (auto valid = prepared->type->validate(input); !valid) {
        return tl::unexpected(valid.error());
    }
    RAY_CAM_TRY(ctx.cancel.check());
    Progress generating = ctx.progress.sub(1.0 - carriedWeight - resolveWeight);
    JobContext genCtx{ctx.cancel, generating};
    auto made = prepared->type->generate(input, genCtx);
    if (!made) {
        return tl::unexpected(made.error());
    }
    made->operation = id;
    made->inputHash = hash;
    ctx.progress.set(1.0, "Done");
    return std::make_shared<const Toolpath>(std::move(*made));
}

// The UI-thread side of a generation: files the result under the hash it was made for.
void apply(Shared& s, const OperationId& id, std::uint64_t ticket, const Hash256& hash, Result<ToolpathPtr> result)
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

// ---------- In-process stock (T-075; the header of this file says how) ----------

// Tells the listeners about `id` from the UI thread soon, from a worker: the continuation of a job that does nothing.
void noticeFromWorker(const Parts& parts, const OperationId& id)
{
    if (parts.jobs == nullptr || !parts.s->hasListeners()) {
        return;
    }
    JobHandle<int> notice = parts.jobs->submit<int>(JobInfo{"CAM notice", JobPriority::Interactive, false},
                                                    [](JobContext&) -> Result<int> { return 0; });
    std::weak_ptr<Shared> weak = parts.s;
    notice.then([weak, id](Result<int>) {
        if (auto s = weak.lock()) {
            s->notify(id);
        }
    });
}

// Makes the stock FromSetupStock setup `setup` starts from, in this job (see the header of this file).
Result<CarriedPtr> makeCarried(const Parts& parts, const CamSnapshot& snap, const naming::IResolver& resolver,
                               const Setup& setup, const Hash256& key, JobContext& ctx)
{
    Shared& s = *parts.s;
    const CamState& cam = *snap.cam;
    const FromSetupStock& from = std::get<FromSetupStock>(setup.stock);
    const Setup* previous = detail::findSetup(cam, from.previous);
    if (previous == nullptr) {
        return failure(ErrorCode::NotFound, "The stock of setup " + detail::label(setup) + " comes from setup " +
                                                from.previous.value.toString() + ", which is not in the document");
    }
    const std::string what =
        "The stock of setup " + detail::label(setup) + " (what setup " + detail::label(*previous) + " leaves)";

    // 1. The earlier setup's toolpaths: every operation that is not suppressed, in order, as the key has them.
    std::vector<OperationId> ids;
    for (const OperationId& id : previous->operations) {
        const auto op = cam.operations.find(id);
        if (op != cam.operations.end() && !op->second.suppressed) {
            ids.push_back(id);
        }
    }
    Progress toolpathsProgress = ctx.progress.sub(0.4);
    toolpathsProgress.set(0, "Toolpaths of setup " + detail::label(*previous));
    std::vector<ToolpathPtr> toolpaths;
    for (const OperationId& id : ids) {
        RAY_CAM_TRY(ctx.cancel.check());
        const Operation& op = cam.operations.at(id);
        Progress one = toolpathsProgress.sub(1.0 / static_cast<double>(ids.size()));
        auto hash = operationInputHash(snap, resolver, id);
        if (!hash) {
            Error e = hash.error();
            e.message = what + " needs " + detail::label(op) + ": " + e.message;
            return tl::unexpected(std::move(e));
        }
        ToolpathPtr toolpath;
        JobHandle<ToolpathPtr> running; // that operation's own generation of these inputs, when one is under way
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (auto cached = s.toolpaths.find(*hash); cached != s.toolpaths.end()) {
                cached->second.used = ++s.clock;
                toolpath = cached->second.toolpath;
            } else if (auto failed = s.failures.find(*hash); failed != s.failures.end()) {
                failed->second.used = ++s.clock;
                return failure(ErrorCode::ToolpathFailed,
                               what + " needs " + detail::label(op) + ", which failed: " + failed->second.message);
            } else if (auto run = s.running.find(id); run != s.running.end() && run->second.hash == *hash) {
                running = run->second.job;
            }
        }
        if (!toolpath && running.valid()) {
            // Its own generation: run here when it has not started, else waited for (it needs nothing of this setup).
            auto theirs = running.wait();
            if (theirs) {
                toolpath = *theirs;
                std::lock_guard<std::mutex> lock(s.mutex);
                s.toolpaths[*hash] = Shared::Cached{toolpath, ++s.clock};
                s.trim(s.toolpaths);
            } else if (theirs.error().code != ErrorCode::Cancelled) {
                Error e = theirs.error();
                e.message = what + " needs " + detail::label(op) + ", which failed: " + e.message;
                return tl::unexpected(std::move(e));
            }
            // Cancelled (its inputs changed, or the user stopped it): made below unless this job is cancelled too.
        }
        if (!toolpath) {
            if (parts.types->find(op.type) == nullptr) {
                return failure(ErrorCode::ToolpathFailed, what + " needs " + detail::label(op) +
                                                              ", of an unknown operation type '" + op.type + "'");
            }
            JobContext oneCtx{ctx.cancel, one};
            auto made = runGenerator(snap, resolver, parts, id, *hash, oneCtx);
            if (!made) {
                if (made.error().code == ErrorCode::Cancelled) {
                    return tl::unexpected(made.error());
                }
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    s.failures[*hash] = Shared::Failure{made.error().message, ++s.clock};
                    s.trim(s.failures);
                }
                noticeFromWorker(parts, id);
                Error e = made.error();
                e.message = what + " needs " + detail::label(op) + ", which failed: " + e.message;
                return tl::unexpected(std::move(e));
            }
            toolpath = *made;
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.toolpaths[*hash] = Shared::Cached{toolpath, ++s.clock};
                s.failures.erase(*hash);
                s.trim(s.toolpaths);
            }
            noticeFromWorker(parts, id); // filed in the cache: that operation is up to date now
        }
        one.set(1);
        toolpaths.push_back(std::move(toolpath));
    }

    // 2. Its frame and starting stock (its own carried stock, for a chain).
    RAY_CAM_TRY(ctx.cancel.check());
    CarriedPtr before;
    if (std::holds_alternative<FromSetupStock>(previous->stock)) {
        auto made = obtainCarried(parts, snap, resolver, *previous, ctx);
        if (!made) {
            return tl::unexpected(made.error());
        }
        before = std::move(*made);
    }
    SetupResolveOptions options;
    options.stockSolid = true;
    options.carried = before.get();
    JobContext resolveCtx{ctx.cancel, Progress{}};
    auto resolved = resolveSetup(snap, *previous, resolver, resolveCtx, options);
    if (!resolved) {
        Error e = resolved.error();
        e.message = what + ": " + e.message;
        return tl::unexpected(std::move(e));
    }

    // 3. The simulation.
    Progress simulating = ctx.progress.sub(0.6);
    simulating.set(0,
                   "Simulating setup " + detail::label(*previous) + " for the stock of setup " + detail::label(setup));
    JobContext simCtx{ctx.cancel, simulating};
    SimulateSetupRequest request{snap, resolver, *previous, *resolved, toolpaths, kCarriedStockCellMm};
    auto solid = parts.simulate(request, simCtx);
    if (!solid) {
        Error e = solid.error();
        if (e.code != ErrorCode::Cancelled) {
            e.message = what + " could not be simulated: " + e.message;
        }
        return tl::unexpected(std::move(e));
    }
    if (solid->positions.empty() || solid->indices.empty()) {
        return failure(ErrorCode::ToolpathFailed, what + " is empty: that setup leaves no material");
    }
    auto out = std::make_shared<CarriedStock>();
    out->setup = previous->id;
    out->key = key;
    out->solid = std::make_shared<const kernel::TriSoup>(std::move(*solid));
    out->modelFromWcs = resolved->modelFromWcs;
    out->bounds = resolved->stockBounds;
    out->cellMm = kCarriedStockCellMm;
    simulating.set(1);
    return CarriedPtr(std::move(out));
}

Result<CarriedPtr> obtainCarried(const Parts& parts, const CamSnapshot& snap, const naming::IResolver& resolver,
                                 const Setup& setup, JobContext& ctx)
{
    if (!parts.simulate) {
        return failure(ErrorCode::InvalidArgument,
                       "The stock of setup " + detail::label(setup) +
                           " comes from an earlier setup, and this CAM engine cannot simulate one");
    }
    auto key = detail::carriedStockKey(snap, resolver, setup);
    if (!key) {
        return tl::unexpected(key.error());
    }
    Shared& s = *parts.s;
    for (;;) {
        RAY_CAM_TRY(ctx.cancel.check());
        std::shared_ptr<Shared::PendingCarried> pending;
        bool mine = false;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (auto cached = s.carried.find(*key); cached != s.carried.end()) {
                cached->second.used = ++s.clock;
                return cached->second.stock;
            }
            std::shared_ptr<Shared::PendingCarried>& slot = s.carriedRunning[*key];
            if (!slot) {
                slot = std::make_shared<Shared::PendingCarried>();
                mine = true;
            }
            pending = slot;
        }
        if (mine) {
            Result<CarriedPtr> made = failure(ErrorCode::Internal, "The in-process stock could not be made");
            try {
                made = makeCarried(parts, snap, resolver, setup, *key, ctx);
            } catch (const std::bad_alloc&) {
                made = failure(ErrorCode::Internal, "Out of memory while simulating the in-process stock");
            } catch (...) {
                made = failure(ErrorCode::Internal, "Unexpected failure while simulating the in-process stock");
            }
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                if (made) {
                    s.carried[*key] = Shared::CarriedEntry{*made, ++s.clock};
                    s.trimCarried();
                }
                s.carriedRunning.erase(*key);
            }
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                pending->result = made;
            }
            pending->done.notify_all();
            return made;
        }
        // Another job is making it: wait, answering this job's own cancel.
        Result<CarriedPtr> theirs = failure(ErrorCode::Internal, "The in-process stock could not be made");
        {
            std::unique_lock<std::mutex> lock(pending->mutex);
            while (!pending->result) {
                if (ctx.cancel.cancelled()) {
                    return tl::unexpected(ctx.cancel.check().error());
                }
                pending->done.wait_for(lock, kCarriedWaitPoll);
            }
            theirs = *pending->result;
        }
        if (theirs || theirs.error().code != ErrorCode::Cancelled) {
            return theirs;
        }
        // That job was cancelled and this one is not: make it here (the loop finds no pending entry now).
    }
}

class Engine final : public ICamEngine {
public:
    Engine(IJobSystem& jobs, std::shared_ptr<const OperationRegistry> types, CamSnapshotFn snapshot,
           CamEngineOptions options)
        : jobs_(jobs), types_(std::move(types)), snapshot_(std::move(snapshot)), s_(std::make_shared<Shared>())
    {
        resolveGeometry_ =
            options.resolveGeometry ? std::move(options.resolveGeometry) : ResolveGeometryFn(&resolveSetupGeometry);
        s_->capacity = std::max<std::size_t>(options.cacheEntries, 1);
        s_->carriedCapacity = std::max<std::size_t>(options.carriedEntries, 1);
        if (!types_) {
            types_ = std::make_shared<const OperationRegistry>();
        }
        parts_ = Parts{&jobs_, s_, types_, resolveGeometry_, std::move(options.simulateSetup)};
    }

    ~Engine() override
    {
        std::map<OperationId, Shared::Running> running;
        {
            std::lock_guard<std::mutex> lock(s_->mutex);
            running.swap(s_->running);
            s_->listeners.clear(); // jobs that outlive the engine tell no one
        }
        for (auto& [id, r] : running) {
            r.job.cancel();
        }
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::map<OperationId, OpState> status(doc::DocStatePtr state, doc::BuildResultPtr build) const override
    {
        return computeStatus(*s_, *types_, snapshot_, state, build, nullptr);
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
            const Checked checked = check(*snap, resolver.get(), *types_, id);
            if (!checked.hash) {
                return failedJob(blockedError(checked));
            }
            const Hash256 hash = *checked.hash;

            std::lock_guard<std::mutex> lock(s_->mutex);
            if (auto cached = s_->toolpaths.find(hash); cached != s_->toolpaths.end()) {
                cached->second.used = ++s_->clock;
                ToolpathPtr toolpath = cached->second.toolpath;
                const bool changed = s_->latest[id] != toolpath;
                s_->latest[id] = toolpath;
                JobHandle<ToolpathPtr> done =
                    jobs_.submit<ToolpathPtr>(JobInfo{"Toolpath", JobPriority::Interactive, false},
                                              [toolpath](JobContext&) -> Result<ToolpathPtr> { return toolpath; });
                if (changed && !s_->listeners.empty()) {
                    std::weak_ptr<Shared> weak = s_;
                    done.then([weak, id](Result<ToolpathPtr>) {
                        if (auto s = weak.lock()) {
                            s->notify(id);
                        }
                    });
                }
                return done;
            }
            if (auto run = s_->running.find(id); run != s_->running.end()) {
                if (run->second.hash == hash && !run->second.job.done()) {
                    return run->second.job; // the same inputs are being generated already
                }
                run->second.job.cancel(); // pointless now: its inputs are not the operation's any more
                s_->running.erase(run);
            }
            const std::uint64_t ticket = s_->nextTicket++;
            const Parts parts = parts_;
            const std::string title = "Generate " + snap->cam->operations.at(id).name;
            // The job starts by taking the engine's lock once: this function holds it until the job's onProgress
            // below is attached, and a progress reported before that is lost (Progress::setListener replaces, it does
            // not replay), which left a blocked generation's status without a progress on a slow machine.
            std::weak_ptr<Shared> barrier = s_;
            JobHandle<ToolpathPtr> job = jobs_.submit<ToolpathPtr>(
                JobInfo{title, JobPriority::Normal, true},
                [snap, resolver, parts, id, hash, barrier](JobContext& ctx) -> Result<ToolpathPtr> {
                    if (auto s = barrier.lock()) {
                        const std::lock_guard<std::mutex> wait(s->mutex);
                    }
                    return runGenerator(*snap, *resolver, parts, id, hash, ctx);
                });
            s_->running[id] = Shared::Running{hash, ticket, job, -1};
            s_->outstanding[ticket] = id;
            std::weak_ptr<Shared> weak = s_;
            job.onProgress([weak, id, ticket](double fraction, std::string) {
                auto s = weak.lock();
                if (!s) {
                    return;
                }
                bool current = false;
                {
                    std::lock_guard<std::mutex> l(s->mutex);
                    if (auto r = s->running.find(id); r != s->running.end() && r->second.ticket == ticket) {
                        r->second.progress = fraction;
                        current = true;
                    }
                }
                if (current) {
                    s->notify(id);
                }
            });
            job.then([weak, id, ticket, hash](Result<ToolpathPtr> result) {
                if (auto s = weak.lock()) {
                    apply(*s, id, ticket, hash, std::move(result));
                    s->notify(id);
                }
            });
            if (!s_->listeners.empty()) {
                postNotice(id); // Generating
            }
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

    // ---------- T-045 ----------
    int subscribe(Listener listener) override
    {
        if (!listener) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(s_->mutex);
        const int n = s_->nextListener++;
        s_->listeners.emplace(n, std::move(listener));
        return n;
    }

    void unsubscribe(int n) override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->listeners.erase(n);
    }

    JobHandle<std::map<OperationId, OpState>> statusAsync(doc::DocStatePtr state, doc::BuildResultPtr build) override
    {
        std::shared_ptr<Shared> s = s_;
        std::shared_ptr<const OperationRegistry> types = types_;
        CamSnapshotFn snapshot = snapshot_;
        return jobs_.submit<std::map<OperationId, OpState>>(
            JobInfo{"CAM status", JobPriority::Interactive, false},
            [s, types, snapshot, state, build](JobContext& ctx) -> Result<std::map<OperationId, OpState>> {
                RAY_CAM_TRY(ctx.cancel.check());
                auto out = computeStatus(*s, *types, snapshot, state, build, &ctx);
                RAY_CAM_TRY(ctx.cancel.check());
                return out;
            });
    }

    JobHandle<OpValidation> validate(doc::DocStatePtr state, doc::BuildResultPtr build, OperationId id) override
    {
        std::shared_ptr<Shared> s = s_;
        std::shared_ptr<const OperationRegistry> types = types_;
        CamSnapshotFn snapshot = snapshot_;
        const Parts parts = parts_;
        return jobs_.submit<OpValidation>(
            JobInfo{"Check operation", JobPriority::Interactive, false},
            [s, types, snapshot, parts, state, build, id](JobContext& ctx) -> Result<OpValidation> {
                RAY_CAM_TRY(ctx.cancel.check());
                const CamSnapshot snap = snapshot ? snapshot(state, build) : CamSnapshot{};
                if (!snap.cam || snap.cam->operations.count(id) == 0) {
                    return failure(ErrorCode::NotFound, "Operation " + id.value.toString() + " is not in the document");
                }
                const auto resolver = naming::makeResolver(snap.bodies);
                const Checked checked = check(snap, resolver.get(), *types, id);
                OpValidation out;
                if (!checked.hash) {
                    out.status = checked.blocked.status;
                    if (checked.blocked.status != OpStatus::Stale) {
                        out.error = blockedError(checked);
                    }
                    return out;
                }
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (auto cached = s->toolpaths.find(*checked.hash); cached != s->toolpaths.end()) {
                        out.status = OpStatus::UpToDate;
                        out.warnings = cached->second.toolpath->warnings;
                        return out; // its warnings are those of this exact input
                    }
                }
                auto prepared = prepare(snap, *resolver, parts, id, ctx, 0.1, 0.9);
                if (!prepared) {
                    if (prepared.error().code == ErrorCode::Cancelled) {
                        return tl::unexpected(prepared.error());
                    }
                    out.status = statusOf(prepared.error());
                    out.error = prepared.error();
                    return out;
                }
                RAY_CAM_TRY(ctx.cancel.check());
                auto warnings = prepared->type->check(prepared->input());
                if (!warnings) {
                    out.status = statusOf(warnings.error());
                    out.error = warnings.error();
                    return out;
                }
                out.status = OpStatus::Stale;
                out.warnings = std::move(*warnings);
                return out;
            });
    }

    JobHandle<ToolpathPtr> preview(doc::DocStatePtr state, doc::BuildResultPtr build, OperationId id) override
    {
        std::shared_ptr<Shared> s = s_;
        std::shared_ptr<const OperationRegistry> types = types_;
        CamSnapshotFn snapshot = snapshot_;
        const Parts parts = parts_;
        JobHandle<ToolpathPtr> job = jobs_.submit<ToolpathPtr>(
            JobInfo{"Toolpath preview", JobPriority::Interactive, false},
            [s, types, snapshot, parts, state, build, id](JobContext& ctx) -> Result<ToolpathPtr> {
                RAY_CAM_TRY(ctx.cancel.check());
                const CamSnapshot snap = snapshot ? snapshot(state, build) : CamSnapshot{};
                if (!snap.cam || snap.cam->operations.count(id) == 0) {
                    return failure(ErrorCode::NotFound, "Operation " + id.value.toString() + " is not in the document");
                }
                const auto resolver = naming::makeResolver(snap.bodies);
                const Checked checked = check(snap, resolver.get(), *types, id);
                if (!checked.hash) {
                    return tl::unexpected(blockedError(checked));
                }
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (auto cached = s->toolpaths.find(*checked.hash); cached != s->toolpaths.end()) {
                        cached->second.used = ++s->clock;
                        return cached->second.toolpath;
                    }
                }
                auto made = runGenerator(snap, *resolver, parts, id, *checked.hash, ctx);
                if (made) {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    s->toolpaths[*checked.hash] = Shared::Cached{*made, ++s->clock};
                    s->failures.erase(*checked.hash);
                    s->trim(s->toolpaths);
                }
                return made;
            });
        std::weak_ptr<Shared> weak = s_;
        job.then([weak, id](Result<ToolpathPtr> r) {
            auto s = weak.lock();
            if (s && r) {
                s->notify(id); // filed in the cache: a committed operation with these inputs is up to date now
            }
        });
        return job;
    }

    // ---------- T-075 ----------
    JobHandle<CarriedPtr> carriedStock(doc::DocStatePtr state, doc::BuildResultPtr build, SetupId setupId) override
    {
        CamSnapshotFn snapshot = snapshot_;
        const Parts parts = parts_;
        return jobs_.submit<CarriedPtr>(
            JobInfo{"Simulate the stock of a setup", JobPriority::Normal, true},
            [snapshot, parts, state, build, setupId](JobContext& ctx) -> Result<CarriedPtr> {
                RAY_CAM_TRY(ctx.cancel.check());
                const CamSnapshot snap = snapshot ? snapshot(state, build) : CamSnapshot{};
                const Setup* setup = snap.cam ? detail::findSetup(*snap.cam, setupId) : nullptr;
                if (setup == nullptr) {
                    return failure(ErrorCode::NotFound,
                                   "Setup " + setupId.value.toString() + " is not in the document");
                }
                if (!std::holds_alternative<FromSetupStock>(setup->stock)) {
                    return failure(ErrorCode::InvalidArgument, "The stock of setup " + detail::label(*setup) +
                                                                   " is not carried from an earlier setup");
                }
                if (!snap.built) {
                    return failure(ErrorCode::InvalidArgument, "The model is not built yet");
                }
                const auto resolver = naming::makeResolver(snap.bodies);
                if (!resolver) {
                    return failure(ErrorCode::Internal, "Out of memory");
                }
                return obtainCarried(parts, snap, *resolver, *setup, ctx);
            });
    }

private:
    JobHandle<ToolpathPtr> failedJob(Error error)
    {
        return jobs_.submit<ToolpathPtr>(JobInfo{"Toolpath", JobPriority::Interactive, false},
                                         [error](JobContext&) -> Result<ToolpathPtr> { return tl::unexpected(error); });
    }

    // Tells the listeners about `id` from the UI thread soon (the continuation of a job that does nothing), never
    // inside the engine call that changed it.
    void postNotice(const OperationId& id)
    {
        JobHandle<int> notice = jobs_.submit<int>(JobInfo{"CAM notice", JobPriority::Interactive, false},
                                                  [](JobContext&) -> Result<int> { return 0; });
        std::weak_ptr<Shared> weak = s_;
        notice.then([weak, id](Result<int>) {
            if (auto s = weak.lock()) {
                s->notify(id);
            }
        });
    }

    IJobSystem& jobs_;
    std::shared_ptr<const OperationRegistry> types_;
    CamSnapshotFn snapshot_;
    ResolveGeometryFn resolveGeometry_;
    std::shared_ptr<Shared> s_;
    Parts parts_; // T-075: what the worker side of a generation needs (the members above, and simulateSetup)
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
