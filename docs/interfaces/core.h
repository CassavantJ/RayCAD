// RayCAD interface contract: core
// Module: src/core   Depends on: C++20 standard library, tl::expected (CC0)
// Status: PLAN DRAFT. Signatures only; no implementation lives in docs/interfaces.
//
// Rules for every module:
//  - Internal units are millimetres and radians, stored as double.
//  - No module except src/kernel may include an OCCT header.
//  - No module except src/ui and src/render may include a Qt header.
//  - No exceptions cross a module boundary; return ray::Result<T>.
#pragma once
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <tl/expected.hpp>

namespace ray {

// ---------- Identity ----------
struct Uuid {
    std::array<std::uint8_t, 16> bytes{};
    auto operator<=>(const Uuid&) const = default;
    static Uuid generate();                    // random v4; never used inside deterministic code paths
    static Uuid fromString(std::string_view);  // canonical 8-4-4-4-12 hex
    std::string toString() const;
};

template <class Tag>
struct Id {
    Uuid value;
    auto operator<=>(const Id&) const = default;
};
using FeatureId   = Id<struct FeatureTag>;
using BodyId      = Id<struct BodyTag>;
using SketchEntId = Id<struct SketchEntTag>;
using ParamId     = Id<struct ParamTag>;
using SetupId     = Id<struct SetupTag>;
using OperationId = Id<struct OperationTag>;
using ToolId      = Id<struct ToolTag>;
using HolderId    = Id<struct HolderTag>;
using MachineId   = Id<struct MachineTag>;
using MaterialId  = Id<struct MaterialTag>;

// Content hash (BLAKE3-256). Used for object storage, cache keys and staleness.
struct Hash256 {
    std::array<std::uint8_t, 32> bytes{};
    auto operator<=>(const Hash256&) const = default;
    std::string hex() const;
};
class Hasher {
public:
    Hasher() noexcept;
    ~Hasher();
    Hasher(Hasher&&) noexcept;
    Hasher& operator=(Hasher&&) noexcept;
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    Hasher& bytes(std::span<const std::byte>);   // raw bytes, no length
    Hasher& str(std::string_view);               // 8-byte little-endian length, then the bytes
    Hasher& f64(double);          // hashes the IEEE-754 bit pattern; -0.0 is normalised to +0.0
    Hasher& i64(std::int64_t);    // 8 bytes, little-endian
    Hash256 finish();             // does not reset; more input may follow
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------- Errors ----------
enum class ErrorCode {
    Cancelled, InvalidArgument, NotFound, IoError, FormatError, FormatTooNew,
    KernelFailure,        // OCCT threw or returned an invalid shape
    ReferenceBroken,      // a TopoRef could not be resolved
    SolverFailed, Overconstrained,
    ToolpathFailed, PostFailed, ScriptError,
    Locked, Internal
};
struct Error {
    ErrorCode code;
    std::string message;                 // user-presentable, English source string
    std::vector<std::string> context;    // innermost first, for logs
};
template <class T> using Result = tl::expected<T, Error>;
using Status = Result<std::monostate>;

// ---------- Math ----------
struct Vec2 { double x{}, y{}; };
struct Vec3 { double x{}, y{}, z{}; };
struct Xform { double m[12]; static Xform identity(); };   // row-major 3x4 rigid transform
struct Box3  { Vec3 min, max; };

// Toolpath coordinates are quantised to integer nanometres at the cam::Toolpath boundary.
using Nm = std::int64_t;
constexpr double kNmPerMm = 1'000'000.0;
Nm     toNm(double mm);       // round-half-even
double toMm(Nm);

// Deterministic math. CAM, sim and post code must call these, never <cmath> trig/exp,
// so results do not depend on the platform libm. Implementation is vendored (musl-derived, MIT).
namespace dmath {
double sin(double); double cos(double); double tan(double);
double asin(double); double acos(double); double atan2(double, double);
double sqrt(double);          // IEEE-exact; thin wrapper
double hypot(double, double);
}

// ---------- Units (display only) ----------
enum class UnitSystem { Inch, Metric };
struct UnitFormat { UnitSystem system; int decimals; };
std::string formatLength(double mm, UnitFormat);
Result<double> parseLength(std::string_view text, UnitSystem defaultSystem);   // accepts "0.5", "0.5in", "12.7mm", "1/2"

// ---------- Jobs: cancellation and progress ----------
// Implementation: src/core/jobs.cpp, on oneTBB. Everything in this section is thread-safe unless noted.
namespace detail {
struct CancelState;    // defined in src/core/jobs.cpp
struct ProgressNode;   // defined in src/core/jobs.cpp
}

class CancelToken {
public:
    CancelToken() = default;   // never cancelled
    bool cancelled() const noexcept;
    // Returns Error{Cancelled} when cancelled. Long loops call this at least every 50 ms of work.
    Status check() const;
private:
    friend class CancelSource;
    explicit CancelToken(std::shared_ptr<const detail::CancelState> state) noexcept;
    std::shared_ptr<const detail::CancelState> state_;
};
class CancelSource {
public:
    CancelSource();                                    // a new source, not cancelled
    explicit CancelSource(const CancelToken& parent);  // also reads as cancelled once `parent` is cancelled
    CancelToken token() const;
    void cancel() noexcept;
    bool cancelled() const noexcept;
private:
    std::shared_ptr<detail::CancelState> state_;   // copies share one state
};

// Receives progress of a whole job as one fraction in [0, 1]. It is called on the thread that called
// Progress::set, one call at a time, at most 30 times a second, with a fraction that never decreases;
// a fraction of 1.0 is always delivered, once. It must not call set() on the same Progress tree.
using ProgressListener = std::function<void(double fraction01, std::string_view stage)>;
class Progress {
public:
    Progress() = default;                          // a no-op sink: set() and sub() do nothing
    explicit Progress(ProgressListener listener);  // a root covering [0, 1]
    void set(double fraction01, std::string_view stage = {});   // thread-safe, rate-limited to 30 Hz
    // Child range for a nested step: the next `weight01` of this range, after the ranges of earlier sub()
    // calls on this Progress (or a copy of it). Its set(f) reports start + f * weight in this range.
    Progress sub(double weight01);
    void setListener(ProgressListener listener);   // replaces the root's listener; no effect on a no-op sink
    static constexpr double kMaxRateHz = 30.0;
private:
    explicit Progress(std::shared_ptr<detail::ProgressNode> node) noexcept;
    std::shared_ptr<detail::ProgressNode> node_;   // copies share one node
};
struct JobContext {
    CancelToken cancel;
    Progress progress;
};

enum class JobPriority { Interactive, Normal, Background };
struct JobInfo { std::string title; JobPriority priority = JobPriority::Normal; bool userVisible = true; };

namespace detail {
// Shared state of one job, without the result type. Owned by JobHandle copies and the job system.
class JobStateBase {
public:
    JobStateBase();
    virtual ~JobStateBase();
    JobStateBase(const JobStateBase&) = delete;
    JobStateBase& operator=(const JobStateBase&) = delete;
    void cancel();
    bool done() const noexcept;
    // Blocks until the job is done. A job that has not started yet is run on the calling thread.
    void wait();
    // Posts `onUiThread` through the dispatcher once the job is done, at once if it is done already.
    // Dropped if the job system was destroyed before the job finished.
    void then(std::function<void()> onUiThread);
    void onProgress(std::function<void(double fraction01, std::string stage)> onUiThread);
    virtual void setError(Error) = 0;            // the job system stores a failure (cancel, exception)
    virtual bool succeeded() const noexcept = 0; // a value was stored
    struct Impl;                                 // defined in src/core/jobs.cpp
    Impl& impl() noexcept { return *impl_; }
private:
    std::unique_ptr<Impl> impl_;
};

template <class T>
class JobState final : public JobStateBase {
public:
    // Written by the thread that runs the job before the job is marked done, read only after that.
    std::optional<Result<T>> result;
    void setError(Error e) override { result.emplace(tl::unexpected<Error>(std::move(e))); }
    bool succeeded() const noexcept override { return result.has_value() && result->has_value(); }
    // A copy of the result, or the result itself when Result<T> cannot be copied (then only one
    // wait() or then() may read it).
    Result<T> take() {
        if constexpr (std::is_copy_constructible_v<Result<T>>) {
            return *result;
        } else {
            return std::move(*result);
        }
    }
};
}

template <class T> class JobHandle {
public:
    JobHandle() = default;   // empty: done() is true, wait() returns InvalidArgument, the rest do nothing
    explicit JobHandle(std::shared_ptr<detail::JobState<T>> state) noexcept : state_(std::move(state)) {}
    bool valid() const noexcept { return state_ != nullptr; }
    // Requests cancellation. A job that has not started never runs; a running one sees its token cancelled.
    // Either way the result is Error{Cancelled}, also if the work returns a value after the request.
    void cancel() { if (state_) state_->cancel(); }
    bool done() const { return !state_ || state_->done(); }
    // Continuation runs on the UI thread (posted through IUiDispatcher).
    void then(std::function<void(Result<T>)> onUiThread) {
        if (!state_ || !onUiThread) return;
        state_->then([state = state_, fn = std::move(onUiThread)]() { fn(state->take()); });
    }
    // Progress of the job, posted through IUiDispatcher (rate-limited as ProgressListener describes).
    void onProgress(std::function<void(double fraction01, std::string stage)> onUiThread) {
        if (state_ && onUiThread) state_->onProgress(std::move(onUiThread));
    }
    Result<T> wait() {   // tests and workers only; never call on the UI thread
        if (!state_) return tl::unexpected<Error>(Error{ErrorCode::InvalidArgument, "Empty job handle", {}});
        state_->wait();
        return state_->take();
    }
private:
    std::shared_ptr<detail::JobState<T>> state_;
};

class IUiDispatcher { public: virtual ~IUiDispatcher() = default; virtual void post(std::function<void()>) = 0; };

class IJobSystem {
public:
    virtual ~IJobSystem() = default;   // cancels every job, waits for the running ones, then returns
    // Runs `work` on a worker. An exception thrown by `work` becomes Error{Internal}; it never escapes.
    template <class T>
    JobHandle<T> submit(JobInfo, std::function<Result<T>(JobContext&)> work);
    // Deterministic parallel loop: the range is split into fixed chunks of `grain` items regardless of
    // thread count; body(chunkIndex, begin, end) writes only to its own chunk's output slot; the caller
    // merges slots in chunk order. Thread count never changes the result.
    // If chunks fail, the error of the lowest failing chunk index is returned (chunks after it may be
    // skipped); an exception from body is that chunk's Error{Internal}. Chunks are skipped once ctx.cancel is
    // cancelled, and the call then returns Error{Cancelled}. Completed chunks are reported to ctx.progress
    // (pass ctx.progress.sub(w) when the loop is one step of a larger job). grain 0 is InvalidArgument.
    virtual Status parallelChunks(std::size_t count, std::size_t grain, JobContext&,
                                  const std::function<Status(std::size_t chunk, std::size_t begin, std::size_t end)>& body) = 0;
    virtual int workerCount() const = 0;
    virtual void setWorkerCount(int) = 0;   // tests force 1 and N to prove determinism
protected:
    // The type-erased hook behind submit(): runs `run` once with the job's context, then completes `state`.
    virtual void submitErased(JobInfo info, std::shared_ptr<detail::JobStateBase> state,
                              std::function<void(JobContext&)> run) = 0;
};

template <class T>
JobHandle<T> IJobSystem::submit(JobInfo info, std::function<Result<T>(JobContext&)> work) {
    auto state = std::make_shared<detail::JobState<T>>();
    detail::JobState<T>* slot = state.get();   // the job system keeps `state` alive while `run` can be called
    submitErased(std::move(info), state, [slot, work = std::move(work)](JobContext& ctx) {
        slot->result.emplace(work(ctx));
    });
    return JobHandle<T>(std::move(state));
}

// The oneTBB job system. `workerCount` 0 means one worker per hardware thread. It sets oneTBB's process-wide
// worker thread limit to workerCount, so a process has one job system at a time. `dispatcher` must outlive it,
// and its post() must queue the function, never run it before returning.
std::unique_ptr<IJobSystem> makeJobSystem(IUiDispatcher& dispatcher, int workerCount = 0);

// ---------- Logging ----------
enum class LogLevel { Debug, Info, Warn, Error };
void log(LogLevel, std::string_view module, std::string_view message);

} // namespace ray
