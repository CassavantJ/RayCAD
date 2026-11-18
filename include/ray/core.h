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
#include <span>
#include <string>
#include <string_view>
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
class CancelToken {
public:
    bool cancelled() const noexcept;
    // Returns Error{Cancelled} when cancelled. Long loops call this at least every 50 ms of work.
    Status check() const;
};
class CancelSource {
public:
    CancelToken token() const;
    void cancel() noexcept;
};
class Progress {
public:
    void set(double fraction01, std::string_view stage = {});   // thread-safe, rate-limited to 30 Hz
    Progress sub(double weight01);                              // child range for a nested step
};
struct JobContext {
    CancelToken cancel;
    Progress progress;
};

enum class JobPriority { Interactive, Normal, Background };
struct JobInfo { std::string title; JobPriority priority = JobPriority::Normal; bool userVisible = true; };

template <class T> class JobHandle {
public:
    void cancel();
    bool done() const;
    // Continuation runs on the UI thread (posted through IUiDispatcher).
    void then(std::function<void(Result<T>)> onUiThread);
    Result<T> wait();   // tests and workers only; never call on the UI thread
};

class IUiDispatcher { public: virtual ~IUiDispatcher() = default; virtual void post(std::function<void()>) = 0; };

class IJobSystem {
public:
    virtual ~IJobSystem() = default;
    template <class T>
    JobHandle<T> submit(JobInfo, std::function<Result<T>(JobContext&)> work);
    // Deterministic parallel loop: the range is split into fixed chunks of `grain` items regardless of
    // thread count; body(chunkIndex, begin, end) writes only to its own chunk's output slot; the caller
    // merges slots in chunk order. Thread count never changes the result.
    virtual Status parallelChunks(std::size_t count, std::size_t grain, JobContext&,
                                  const std::function<Status(std::size_t chunk, std::size_t begin, std::size_t end)>& body) = 0;
    virtual int workerCount() const = 0;
    virtual void setWorkerCount(int) = 0;   // tests force 1 and N to prove determinism
};

// ---------- Logging ----------
enum class LogLevel { Debug, Info, Warn, Error };
void log(LogLevel, std::string_view module, std::string_view message);

} // namespace ray
