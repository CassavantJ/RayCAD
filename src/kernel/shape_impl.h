// Private to src/kernel: the OpenCASCADE side of ray::kernel::Shape (task T-004).
// Kernel sources include this to take the TopoDS_Shape out of a Shape, to wrap a result back, to map
// between OCCT sub-shapes and SubId, and to run OCCT code with every exception turned into an Error.
// Nothing outside src/kernel may include it (it includes OCCT headers; PLAN.md 0.4 rule 1).
#pragma once
#include <ray/kernel.h>

#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_IndexedMap.hxx>
#include <NCollection_List.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_XYZ.hxx>

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ray::kernel {

namespace detail {

using ShapeIndex = NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;
using AncestorIndex = NCollection_IndexedDataMap<TopoDS_Shape, NCollection_List<TopoDS_Shape>, TopTools_ShapeMapHasher>;

inline constexpr std::size_t kSubKindCount = 4;

// Slot of a SubKind in per-kind arrays (Vertex 0, Edge 1, Face 2, Solid 3); kSubKindCount if the value is
// not one of the enumerators.
constexpr std::size_t kindSlot(SubKind kind) noexcept
{
    const auto slot = static_cast<std::size_t>(kind);
    return slot < kSubKindCount ? slot : kSubKindCount;
}
constexpr bool validKind(SubKind kind) noexcept
{
    return kindSlot(kind) < kSubKindCount;
}
TopAbs_ShapeEnum occtKind(SubKind kind) noexcept; // precondition: validKind(kind)

} // namespace detail

// The stored shape plus its canonical sub-shape maps.
//
// CANONICAL ORDER (SubId). For each kind, SubId{kind, i} is the (i+1)-th entry of
// TopExp::MapShapes(shape, kind) on the stored shape: the distinct sub-shapes of that kind (distinct by
// TShape and location; orientation is ignored) in the order a depth-first, pre-order walk meets them first,
// visiting the children of every TShape in their stored order (TopoDS_Iterator / TopExp_Explorer). The
// stored shape itself counts if it is of that kind. Nothing in this order depends on pointer values, hash
// buckets, threads, clocks or the operating system: it is a function of the topology graph alone, so the
// same graph gives the same SubIds in every run. serialize() writes children in stored order and
// deserialize() rebuilds them in the same order, so a round trip keeps every SubId.
//
// An Impl is immutable once constructed. The adjacency maps and the content hash are built on first use,
// each exactly once (std::call_once), so concurrent queries on one Shape are safe.
struct Shape::Impl {
    explicit Impl(TopoDS_Shape stored); // builds the per-kind maps; may throw Standard_Failure
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    TopoDS_Shape shape;                                         // never null
    std::array<detail::ShapeIndex, detail::kSubKindCount> subs; // canonical order per kind, 1-based (OCCT)

    // Every sub-shape of kind `of`, with the sub-shapes of kind `wanted` that contain it (each listed once),
    // from TopExp::MapShapesAndUniqueAncestors. Precondition: both kinds valid. May throw.
    const detail::AncestorIndex& ancestors(SubKind of, SubKind wanted) const;

    // BLAKE3 of serialize(); computed once. Never throws.
    Hash256 contentHash() const;

    // Facts the kernel established about the stored shape; they describe it and never change it, so setting them on
    // a shared Shape is safe (atomics). `validated`: the shape passed validateSolids or checkedValid (the structural
    // pre-check and BRepCheck), so an operation whose result keeps some of its faces unchanged need not check those
    // faces again (boolean_internal.h validateSolids). `volume`: the summed volume of its solids, once measured
    // (NaN until then), so the next operation on it does not integrate it again (detail::volumeOf).
    mutable std::atomic<bool> validated{false};
    mutable std::atomic<double> volume{std::numeric_limits<double>::quiet_NaN()};

private:
    mutable std::array<std::once_flag, detail::kSubKindCount * detail::kSubKindCount> ancestorOnce_;
    mutable std::array<detail::AncestorIndex, detail::kSubKindCount * detail::kSubKindCount> ancestor_;
    mutable std::once_flag hashOnce_;
    mutable Hash256 hash_{};
};

namespace detail {

// ---------- Errors ----------
Error makeError(ErrorCode code, std::string message, std::string_view where);

// Runs thunk(context). Standard_Failure, std::exception, any other C++ exception and, on Windows, a
// structured exception (access violation, stack overflow, a delay-loaded DLL that is missing) are caught
// and described in `message`. Returns true when thunk returned normally. Never throws.
using GuardedThunk = void (*)(void*);
bool runGuarded(GuardedThunk thunk, void* context, std::string& message) noexcept;

// How many structured exceptions runGuarded() has caught in this process (always 0 off Windows, where nothing
// catches them). A caught access violation inside OCCT may leave OCCT state corrupt, so the kernel's own checks
// must keep OCCT from faulting in the first place; the tests use this count to prove that they do.
std::uint64_t structuredExceptionCount() noexcept;

// Runs `body` (returning a Result<T>) and turns anything it throws into ErrorCode::KernelFailure with
// `where` as context. Every public kernel entry point runs its OCCT work through this.
template <class F>
auto guarded(std::string_view where, F&& body) -> std::invoke_result_t<F&>
{
    using R = std::invoke_result_t<F&>;
    std::optional<R> result;
    auto run = [&]() {
        result.emplace(body());
    };
    std::string message;
    const bool ok = runGuarded([](void* context) { (*static_cast<decltype(run)*>(context))(); }, &run, message);
    if (!ok || !result.has_value())
        return tl::unexpected(makeError(ErrorCode::KernelFailure, "The geometry kernel failed: " + message, where));
    return std::move(*result);
}

// ---------- Checking shapes OCCT made (shape_check.cpp) ----------
// A cheap structural check of a shape that came out of an OCCT algorithm or a file, to run BEFORE
// BRepCheck_Analyzer or other OCCT code that classifies points on its faces. OCCT 8.0.1 reads out of bounds on
// some shapes that it should merely call invalid, which kills the process off Windows (shape_check.cpp says which).
// Returns why the shape must be refused, or nothing if OCCT may look at it. May throw (call inside guarded()).
std::optional<std::string> structuralFault(const TopoDS_Shape& shape);

// BRepCheck_Analyzer(shape).IsValid(), but false without running BRepCheck when structuralFault() refuses the
// shape. Every kernel check of a shape it did not build itself goes through this. May throw.
bool checkedValid(const TopoDS_Shape& shape);

// ---------- Shape <-> TopoDS_Shape ----------
// Wraps an OCCT shape; a null TopoDS_Shape gives a null Shape. Builds the canonical maps, so it may throw:
// call it inside guarded().
Shape wrap(const TopoDS_Shape& shape);

// The stored OCCT shape; a null TopoDS_Shape for a null Shape.
const TopoDS_Shape& occt(const Shape& shape) noexcept;

// KernelFailure for a null shape.
Status requireShape(const Shape& shape, std::string_view where);

// The sub-shape a SubId names, with the orientation and location it has inside the stored shape.
// Errors: KernelFailure for a null shape; InvalidArgument for an unknown kind or an index out of range.
Result<TopoDS_Shape> resolve(const Shape& shape, SubId id, std::string_view where);

// The SubId of an OCCT sub-shape of `shape` (matched by TShape and location, orientation ignored); empty if
// it is not a vertex, edge, face or solid of `shape`. Used to normalise OCCT history to SubIds.
std::optional<SubId> subIdOf(const Shape& shape, const TopoDS_Shape& sub);

// ---------- Serialisation ----------
// serialize() on an OCCT shape (see shape_serialize.cpp for the format). Empty for a null shape or if OCCT
// failed. Never throws.
std::vector<std::byte> serializeOcct(const TopoDS_Shape& shape) noexcept;

// The envelope around a payload (magic, version, length, checksum). May throw std::bad_alloc.
std::vector<std::byte> sealPayload(std::span<const std::byte> payload);
// The payload of well-formed serialized bytes; empty if the envelope is not well formed.
std::span<const std::byte> payloadOf(std::span<const std::byte> bytes) noexcept;

// ---------- Small conversions ----------
inline Vec3 toVec3(const gp_XYZ& xyz) noexcept
{
    return Vec3{xyz.X(), xyz.Y(), xyz.Z()};
}
inline Vec3 toVec3(const gp_Pnt& point) noexcept
{
    return toVec3(point.XYZ());
}
inline Vec3 toVec3(const gp_Dir& dir) noexcept
{
    return toVec3(dir.XYZ());
}
inline gp_Pnt toPnt(const Vec3& v) noexcept
{
    return gp_Pnt(v.x, v.y, v.z);
}

// ---------- Hashing ----------
// BLAKE3-256 of `data` (ray::Hasher).
Hash256 blake3(std::span<const std::byte> data) noexcept;

} // namespace detail
} // namespace ray::kernel
