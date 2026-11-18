// Job system (task T-003): CancelToken, CancelSource, Progress, the shared state behind JobHandle, and the
// oneTBB implementation of IJobSystem. See PLAN.md section 7 (threading) and section 8 (determinism).
//
// Locks, in the only orders in which they are ever nested (none of them is held while job code runs):
//   ProgressRoot::mutex       -> (the listener) -> DispatchLink::mutex -> (IUiDispatcher::post)
//   JobStateBase::Impl::mutex -> DispatchLink::mutex -> (IUiDispatcher::post)
//   TbbJobSystem::mutex_      -> (oneTBB enqueue, task_arena and global_control construction)
// The dispatcher's post() therefore must only queue the function, never run it.
#include <ray/core.h>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/partitioner.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ray {

namespace {

Error cancelledError()
{
    return Error{ErrorCode::Cancelled, "Cancelled", {}};
}

Error internalError(std::string what, const std::string& where)
{
    Error error{ErrorCode::Internal, "Internal error: " + std::move(what), {}};
    if (!where.empty()) {
        error.context.push_back(where);
    }
    return error;
}

std::int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Raises `target` to at least `value`.
void atomicMax(std::atomic<double>& target, double value) noexcept
{
    double seen = target.load(std::memory_order_relaxed);
    while (seen < value && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

} // namespace

// ---------- Cancellation ----------
namespace detail {
struct CancelState {
    std::atomic<bool> flag{false};
    std::shared_ptr<const CancelState> parent;

    bool cancelled() const noexcept
    {
        for (const CancelState* state = this; state != nullptr; state = state->parent.get()) {
            if (state->flag.load(std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }
};
} // namespace detail

CancelToken::CancelToken(std::shared_ptr<const detail::CancelState> state) noexcept : state_(std::move(state)) {}

bool CancelToken::cancelled() const noexcept
{
    return state_ != nullptr && state_->cancelled();
}

Status CancelToken::check() const
{
    if (cancelled()) {
        return tl::unexpected<Error>(cancelledError());
    }
    return Status{};
}

CancelSource::CancelSource() : state_(std::make_shared<detail::CancelState>()) {}

CancelSource::CancelSource(const CancelToken& parent) : CancelSource()
{
    state_->parent = parent.state_;
}

CancelToken CancelSource::token() const
{
    return CancelToken(state_);
}

void CancelSource::cancel() noexcept
{
    if (state_ != nullptr) {
        state_->flag.store(true, std::memory_order_release);
    }
}

bool CancelSource::cancelled() const noexcept
{
    return state_ != nullptr && state_->cancelled();
}

// ---------- Progress ----------
namespace detail {

// What every Progress of one tree reports into.
struct ProgressRoot {
    static constexpr std::int64_t kIntervalNs = static_cast<std::int64_t>(1e9 / Progress::kMaxRateHz) + 1;

    std::mutex mutex;
    ProgressListener listener;   // guarded by mutex
    double current = 0.0;        // guarded: the largest fraction set so far
    double reported = -1.0;      // guarded: the fraction last given to the listener
    std::string stage;           // guarded: the latest non-empty stage
    std::string reportedStage;   // guarded
    bool finalDelivered = false; // guarded
    std::atomic<std::int64_t> nextEmitNs{0};
    std::atomic<double> skipped{0.0}; // largest fraction dropped by the lock-free fast path

    void report(double fraction, std::string_view newStage)
    {
        const std::int64_t now = nowNs();
        const bool isFinal = fraction >= 1.0;
        if (!isFinal && newStage.empty() && now < nextEmitNs.load(std::memory_order_relaxed)) {
            atomicMax(skipped, fraction); // too soon after the last call; seen by the next one that emits
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (finalDelivered) {
            return;
        }
        current = std::max({current, fraction, skipped.load(std::memory_order_relaxed)});
        if (!newStage.empty() && newStage != stage) {
            stage.assign(newStage);
        }
        if (current >= 1.0) {
            current = 1.0;
        } else if (now < nextEmitNs.load(std::memory_order_relaxed) ||
                   (current == reported && stage == reportedStage)) {
            return;
        }
        nextEmitNs.store(now + kIntervalNs, std::memory_order_relaxed);
        reported = current;
        reportedStage = stage;
        finalDelivered = current >= 1.0;
        if (listener) {
            try {
                listener(current, stage);
            } catch (...) {
                // A listener must not throw; if it does, the update is lost and the job goes on.
            }
        }
    }
};

struct ProgressNode {
    std::shared_ptr<ProgressRoot> root;
    double offset = 0.0;           // where this range starts in the root's [0, 1]
    double scale = 1.0;            // its length
    std::atomic<double> used{0.0}; // share of this range already given to sub()
};

} // namespace detail

Progress::Progress(ProgressListener listener) : node_(std::make_shared<detail::ProgressNode>())
{
    node_->root = std::make_shared<detail::ProgressRoot>();
    node_->root->listener = std::move(listener);
}

Progress::Progress(std::shared_ptr<detail::ProgressNode> node) noexcept : node_(std::move(node)) {}

void Progress::set(double fraction01, std::string_view stage)
{
    if (node_ == nullptr) {
        return;
    }
    if (!(fraction01 >= 0.0)) {
        fraction01 = 0.0; // also NaN
    }
    fraction01 = std::min(fraction01, 1.0);
    double global = node_->offset + node_->scale * fraction01;
    if (global >= 1.0 - 1e-9) {
        global = 1.0; // the end of the last sub-range is the end of the job, whatever the rounding
    }
    node_->root->report(global, stage);
}

Progress Progress::sub(double weight01)
{
    if (node_ == nullptr) {
        return Progress();
    }
    if (!(weight01 >= 0.0)) {
        weight01 = 0.0;
    }
    double start = node_->used.load(std::memory_order_relaxed);
    double end = 0.0;
    do {
        end = std::min(1.0, start + weight01);
    } while (!node_->used.compare_exchange_weak(start, end, std::memory_order_relaxed));
    auto child = std::make_shared<detail::ProgressNode>();
    child->root = node_->root;
    child->offset = node_->offset + node_->scale * start;
    child->scale = node_->scale * (end - start);
    return Progress(std::move(child));
}

void Progress::setListener(ProgressListener listener)
{
    if (node_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(node_->root->mutex);
    node_->root->listener = std::move(listener);
}

// ---------- Job state ----------
namespace detail {

// The job system's link to the UI dispatcher. Cut when the job system is destroyed; later posts are dropped.
struct DispatchLink {
    std::mutex mutex;
    IUiDispatcher* dispatcher = nullptr; // guarded by mutex

    void post(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (dispatcher != nullptr) {
            try {
                dispatcher->post(std::move(fn));
            } catch (...) {
                // A dispatcher that cannot take more work drops it; nothing may escape into a worker.
            }
        }
    }
    void detach()
    {
        std::lock_guard<std::mutex> lock(mutex);
        dispatcher = nullptr;
    }
};

struct JobStateBase::Impl {
    enum : int { kUnpublished = 0, kPublished = 1, kClaimed = 2 };

    // Set by the job system before the job is published; then read only by the thread that claims it.
    std::function<void(JobContext&)> work;
    std::string title;
    CancelSource cancel;
    Progress progress{ProgressListener{}};
    std::shared_ptr<DispatchLink> link;

    // kPublished is stored (release) after the fields above are set; claiming reads it (acquire). Exactly one
    // thread claims a job: the worker that runs it, a thread that waits for it, or a thread that cancels it.
    std::atomic<int> claim{kUnpublished};

    mutable std::mutex mutex;
    std::condition_variable doneChanged;
    bool done = false;                                // guarded by mutex
    std::vector<std::function<void()>> continuations; // guarded by mutex
    std::atomic<bool> doneFlag{false};                // done, readable without the lock

    void publish() noexcept { claim.store(kPublished, std::memory_order_release); }
    bool tryClaim() noexcept
    {
        int expected = kPublished;
        return claim.compare_exchange_strong(expected, kClaimed, std::memory_order_acq_rel);
    }
    void post(std::function<void()> fn) const
    {
        if (link != nullptr) {
            link->post(std::move(fn));
        }
    }
};

namespace {

// Posting under the job's lock keeps continuations in the order then() was called, also when one is
// attached while the job completes.
void markDone(JobStateBase::Impl& impl)
{
    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.done = true;
    impl.doneFlag.store(true, std::memory_order_release);
    for (auto& continuation : impl.continuations) {
        impl.post(std::move(continuation));
    }
    impl.continuations.clear();
    impl.doneChanged.notify_all();
}

// Runs a claimed job on the calling thread and completes it. Never throws.
void execute(JobStateBase& state) noexcept
{
    JobStateBase::Impl& impl = state.impl();
    try {
        std::function<void(JobContext&)> work = std::move(impl.work);
        impl.work = nullptr;
        JobContext ctx{impl.cancel.token(), impl.progress};
        if (!ctx.cancel.cancelled()) {
            try {
                if (!work) {
                    throw std::logic_error("job has no work");
                }
                work(ctx);
            } catch (const std::exception& e) {
                state.setError(internalError(e.what(), impl.title));
            } catch (...) {
                state.setError(internalError("unknown exception", impl.title));
            }
        }
        work = nullptr; // releases what the work captured, before anyone is told the job is done
        if (ctx.cancel.cancelled()) {
            state.setError(cancelledError()); // a cancelled job never reports a value
        } else if (state.succeeded()) {
            ctx.progress.set(1.0);
        }
    } catch (...) {
        try {
            state.setError(internalError("job bookkeeping failed", impl.title));
        } catch (...) {
            // Out of memory while reporting a failure; the result keeps whatever was stored last.
        }
    }
    markDone(impl);
}

// Completes a claimed job as cancelled without running it.
void completeCancelled(JobStateBase& state) noexcept
{
    JobStateBase::Impl& impl = state.impl();
    try {
        impl.work = nullptr;
        state.setError(cancelledError());
    } catch (...) {
        // See execute().
    }
    markDone(impl);
}

} // namespace

JobStateBase::JobStateBase() : impl_(std::make_unique<Impl>()) {}

JobStateBase::~JobStateBase() = default;

void JobStateBase::cancel()
{
    impl_->cancel.cancel();
    if (impl_->tryClaim()) {
        completeCancelled(*this); // not started: done now, instead of when a worker gets to it
    }
}

bool JobStateBase::done() const noexcept
{
    return impl_->doneFlag.load(std::memory_order_acquire);
}

void JobStateBase::wait()
{
    if (impl_->tryClaim()) {
        execute(*this); // not started: run it here rather than block a thread that a worker may need
        return;
    }
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->doneChanged.wait(lock, [this] { return impl_->done; });
}

void JobStateBase::then(std::function<void()> onUiThread)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->done) {
        impl_->post(std::move(onUiThread));
    } else {
        impl_->continuations.push_back(std::move(onUiThread));
    }
}

void JobStateBase::onProgress(std::function<void(double fraction01, std::string stage)> onUiThread)
{
    std::shared_ptr<DispatchLink> link = impl_->link;
    impl_->progress.setListener([link, fn = std::move(onUiThread)](double fraction, std::string_view stage) {
        if (link != nullptr) {
            link->post([fn, fraction, text = std::string(stage)] { fn(fraction, text); });
        }
    });
}

} // namespace detail

// ---------- The oneTBB job system ----------
namespace {

using tbb::task_arena;

// One arena per priority, each with the same concurrency, and a task group per arena so that the job
// system can wait until every task it enqueued has finished running.
struct ArenaSet {
    explicit ArenaSet(int workers)
        : interactive(workers, 0, task_arena::priority::high), normal(workers, 0, task_arena::priority::normal),
          background(workers, 0, task_arena::priority::low)
    {
    }
    ArenaSet(const ArenaSet&) = delete;
    ArenaSet& operator=(const ArenaSet&) = delete;
    ~ArenaSet() { waitAll(); }

    void waitAll() noexcept
    {
        try {
            interactive.wait_for(interactiveJobs);
            normal.wait_for(normalJobs);
            background.wait_for(backgroundJobs);
        } catch (...) {
            // Job tasks never throw (execute() catches everything), so this is not reached.
        }
    }
    task_arena& arena(JobPriority priority)
    {
        switch (priority) {
        case JobPriority::Interactive:
            return interactive;
        case JobPriority::Background:
            return background;
        case JobPriority::Normal:
            break;
        }
        return normal;
    }
    tbb::task_group& group(JobPriority priority)
    {
        switch (priority) {
        case JobPriority::Interactive:
            return interactiveJobs;
        case JobPriority::Background:
            return backgroundJobs;
        case JobPriority::Normal:
            break;
        }
        return normalJobs;
    }

    task_arena interactive;
    task_arena normal;
    task_arena background;
    tbb::task_group interactiveJobs;
    tbb::task_group normalJobs;
    tbb::task_group backgroundJobs;
};

// The whole process gets `workers` oneTBB worker threads, shared by the three arenas, no more and no fewer
// (oneTBB otherwise caps them at the hardware thread count minus one, and tests ask for 16 workers on small
// machines). The +1 is the slot of a thread outside the pool that joins an arena, as parallelChunks does.
std::unique_ptr<tbb::global_control> workerLimit(int workers)
{
    return std::make_unique<tbb::global_control>(tbb::global_control::max_allowed_parallelism,
                                                 static_cast<std::size_t>(workers) + 1);
}

class TbbJobSystem final : public IJobSystem {
public:
    TbbJobSystem(IUiDispatcher& dispatcher, int workers)
        : link_(std::make_shared<detail::DispatchLink>()), workers_(std::max(1, workers)),
          limit_(workerLimit(workers_.load())), arenas_(std::make_shared<ArenaSet>(workers_.load()))
    {
        link_->dispatcher = &dispatcher;
    }

    TbbJobSystem(const TbbJobSystem&) = delete;
    TbbJobSystem& operator=(const TbbJobSystem&) = delete;

    ~TbbJobSystem() override
    {
        shutdown_.cancel(); // every job's token derives from this one
        // Jobs may submit more jobs while we wait; those start cancelled and finish at once. Wait until a
        // whole round saw no new submission.
        for (;;) {
            std::vector<std::shared_ptr<ArenaSet>> sets;
            std::uint64_t submittedBefore = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sets = retired_;
                sets.push_back(arenas_);
                submittedBefore = submitted_;
            }
            for (const auto& set : sets) {
                set->waitAll();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (submitted_ == submittedBefore) {
                break;
            }
        }
        link_->detach();
        std::lock_guard<std::mutex> lock(mutex_);
        retired_.clear();
        arenas_.reset();
        limit_.reset();
    }

    Status parallelChunks(std::size_t count, std::size_t grain, JobContext& ctx,
                          const std::function<Status(std::size_t, std::size_t, std::size_t)>& body) override
    {
        if (grain == 0) {
            return tl::unexpected<Error>(
                Error{ErrorCode::InvalidArgument, "parallelChunks: the grain must be at least 1", {}});
        }
        if (count == 0) {
            return Status{};
        }
        const std::size_t chunks = count / grain + (count % grain != 0 ? 1 : 0);
        constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
        std::vector<std::optional<Error>> errors(chunks); // slot c is written by chunk c only
        std::atomic<std::size_t> firstError{kNone};
        std::atomic<std::size_t> completed{0};
        const CancelToken& token = ctx.cancel;
        Progress& progress = ctx.progress;

        // Besides what oneTBB guarantees, the release store here and the acquire load at the start of every chunk,
        // and the acq_rel count of completed chunks read with acquire at the end, order the slots and the
        // captured state between this thread and the chunks with C++ atomics that ThreadSanitizer sees even
        // when oneTBB itself is not instrumented.
        firstError.store(kNone, std::memory_order_release);

        auto runChunk = [&](std::size_t chunk) {
            // A chunk after a failed one cannot change the result; a chunk before it still must run.
            if (chunk > firstError.load(std::memory_order_acquire) || token.cancelled()) {
                return;
            }
            const std::size_t begin = chunk * grain;
            const std::size_t end = std::min(count, begin + grain);
            Status status;
            try {
                status = body(chunk, begin, end);
            } catch (const std::exception& e) {
                status = tl::unexpected<Error>(internalError(e.what(), "parallelChunks"));
            } catch (...) {
                status = tl::unexpected<Error>(internalError("unknown exception", "parallelChunks"));
            }
            if (!status) {
                errors[chunk] = std::move(status.error());
                std::size_t seen = firstError.load(std::memory_order_relaxed);
                while (chunk < seen && !firstError.compare_exchange_weak(seen, chunk, std::memory_order_relaxed)) {
                }
            }
            const std::size_t done = completed.fetch_add(1, std::memory_order_acq_rel) + 1;
            progress.set(static_cast<double>(done) / static_cast<double>(chunks));
        };
        auto loop = [&] {
            // isolate: while this thread waits for chunks it must not pick up an unrelated queued job.
            tbb::this_task_arena::isolate([&] {
                tbb::parallel_for(
                    tbb::blocked_range<std::size_t>(0, chunks, 1),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                        for (std::size_t chunk = range.begin(); chunk != range.end(); ++chunk) {
                            runChunk(chunk);
                        }
                    },
                    tbb::simple_partitioner());
            });
        };

        try {
            if (tbb::this_task_arena::current_thread_index() != task_arena::not_initialized) {
                loop(); // already on a worker (a job, or a chunk of an outer loop): stay in its arena
            } else {
                std::shared_ptr<ArenaSet> arenas = currentArenas();
                if (arenas == nullptr) {
                    return tl::unexpected<Error>(cancelledError()); // the job system is shutting down
                }
                arenas->normal.execute(loop);
            }
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what(), "parallelChunks"));
        } catch (...) {
            return tl::unexpected<Error>(internalError("unknown exception", "parallelChunks"));
        }

        (void)completed.load(std::memory_order_acquire);
        if (token.cancelled()) {
            return tl::unexpected<Error>(cancelledError());
        }
        const std::size_t first = firstError.load(std::memory_order_acquire);
        if (first != kNone) {
            return tl::unexpected<Error>(std::move(*errors[first]));
        }
        return Status{};
    }

    int workerCount() const override { return workers_.load(std::memory_order_relaxed); }

    void setWorkerCount(int workers) override
    {
        workers = std::max(1, workers);
        std::lock_guard<std::mutex> lock(mutex_);
        if (arenas_ == nullptr || workers == workers_.load(std::memory_order_relaxed)) {
            return;
        }
        // Jobs already queued finish in the old arenas, which live until the job system is destroyed.
        retired_.push_back(std::move(arenas_));
        arenas_ = std::make_shared<ArenaSet>(workers);
        std::unique_ptr<tbb::global_control> limit = workerLimit(workers);
        limit_ = std::move(limit); // the old limit goes after the new one exists
        workers_.store(workers, std::memory_order_relaxed);
    }

protected:
    void submitErased(JobInfo info, std::shared_ptr<detail::JobStateBase> state,
                      std::function<void(JobContext&)> run) override
    {
        detail::JobStateBase::Impl& impl = state->impl();
        impl.work = std::move(run);
        impl.title = std::move(info.title);
        impl.cancel = CancelSource(shutdown_.token());
        impl.link = link_;
        impl.publish();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (arenas_ != nullptr) {
                auto task = [state] {
                    if (state->impl().tryClaim()) {
                        detail::execute(*state);
                    }
                };
                arenas_->arena(info.priority).enqueue(std::move(task), arenas_->group(info.priority));
                ++submitted_;
                return;
            }
        }
        state->cancel(); // submitted after the job system finished shutting down: never runs
    }

private:
    std::shared_ptr<ArenaSet> currentArenas() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return arenas_;
    }

    std::shared_ptr<detail::DispatchLink> link_;
    CancelSource shutdown_;
    std::atomic<int> workers_;
    std::unique_ptr<tbb::global_control> limit_; // guarded by mutex_ after construction
    mutable std::mutex mutex_;
    std::shared_ptr<ArenaSet> arenas_;               // guarded by mutex_
    std::vector<std::shared_ptr<ArenaSet>> retired_; // guarded by mutex_
    std::uint64_t submitted_ = 0;                    // guarded by mutex_
};

} // namespace

std::unique_ptr<IJobSystem> makeJobSystem(IUiDispatcher& dispatcher, int workerCount)
{
    if (workerCount <= 0) {
        workerCount = tbb::info::default_concurrency();
    }
    return std::make_unique<TbbJobSystem>(dispatcher, workerCount);
}

} // namespace ray
