// ray::kernel::Shape, the canonical sub-shape order, and the primitive constructors (task T-004).
#include "shape_impl.h"

#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <gp_Ax2.hxx>

#include <cmath>

namespace ray::kernel {

// ---------- Shape::Impl ----------

Shape::Impl::Impl(TopoDS_Shape stored) : shape(std::move(stored))
{
    for (std::size_t slot = 0; slot < detail::kSubKindCount; ++slot)
        TopExp::MapShapes(shape, detail::occtKind(static_cast<SubKind>(slot)), subs[slot]);
}

const detail::AncestorIndex& Shape::Impl::ancestors(SubKind of, SubKind wanted) const
{
    const std::size_t slot = detail::kindSlot(of) * detail::kSubKindCount + detail::kindSlot(wanted);
    std::call_once(ancestorOnce_[slot], [&] {
        detail::AncestorIndex built;
        TopExp::MapShapesAndUniqueAncestors(shape, detail::occtKind(of), detail::occtKind(wanted), built);
        ancestor_[slot] = std::move(built);
    });
    return ancestor_[slot];
}

Hash256 Shape::Impl::contentHash() const
{
    std::call_once(hashOnce_, [&] {
        // Returns no bytes if OCCT failed, which then hashes like a null shape.
        hash_ = detail::blake3(detail::serializeOcct(shape));
    });
    return hash_;
}

// ---------- Shape ----------

Shape::Shape() = default;

bool Shape::isNull() const
{
    return impl == nullptr;
}

std::uint32_t Shape::count(SubKind kind) const
{
    if (impl == nullptr || !detail::validKind(kind))
        return 0;
    return static_cast<std::uint32_t>(impl->subs[detail::kindSlot(kind)].Extent());
}

Hash256 Shape::contentHash() const
{
    if (impl == nullptr)
        return detail::blake3({}); // serialize() of a null shape is empty
    return impl->contentHash();
}

// ---------- detail ----------

namespace detail {

TopAbs_ShapeEnum occtKind(SubKind kind) noexcept
{
    switch (kind) {
    case SubKind::Vertex:
        return TopAbs_VERTEX;
    case SubKind::Edge:
        return TopAbs_EDGE;
    case SubKind::Face:
        return TopAbs_FACE;
    case SubKind::Solid:
        return TopAbs_SOLID;
    }
    return TopAbs_SHAPE;
}

Shape wrap(const TopoDS_Shape& shape)
{
    Shape result;
    if (!shape.IsNull())
        result.impl = std::make_shared<const Shape::Impl>(shape);
    return result;
}

const TopoDS_Shape& occt(const Shape& shape) noexcept
{
    static const TopoDS_Shape null;
    return shape.impl != nullptr ? shape.impl->shape : null;
}

Status requireShape(const Shape& shape, std::string_view where)
{
    if (shape.impl == nullptr)
        return tl::unexpected(makeError(ErrorCode::KernelFailure, "The shape is empty.", where));
    return std::monostate{};
}

Result<TopoDS_Shape> resolve(const Shape& shape, SubId id, std::string_view where)
{
    if (auto ok = requireShape(shape, where); !ok)
        return tl::unexpected(ok.error());
    if (!validKind(id.kind))
        return tl::unexpected(makeError(ErrorCode::InvalidArgument, "Unknown sub-shape kind.", where));
    const ShapeIndex& map = shape.impl->subs[kindSlot(id.kind)];
    if (id.index >= static_cast<std::uint32_t>(map.Extent()))
        return tl::unexpected(
            makeError(ErrorCode::InvalidArgument, "The sub-shape does not exist in this shape.", where));
    return map.FindKey(static_cast<int>(id.index) + 1);
}

std::optional<SubId> subIdOf(const Shape& shape, const TopoDS_Shape& sub)
{
    if (shape.impl == nullptr || sub.IsNull())
        return std::nullopt;
    SubKind kind{};
    switch (sub.ShapeType()) {
    case TopAbs_VERTEX:
        kind = SubKind::Vertex;
        break;
    case TopAbs_EDGE:
        kind = SubKind::Edge;
        break;
    case TopAbs_FACE:
        kind = SubKind::Face;
        break;
    case TopAbs_SOLID:
        kind = SubKind::Solid;
        break;
    default:
        return std::nullopt;
    }
    const int index = shape.impl->subs[kindSlot(kind)].FindIndex(sub);
    if (index <= 0)
        return std::nullopt;
    return SubId{kind, static_cast<std::uint32_t>(index - 1)};
}

Hash256 blake3(std::span<const std::byte> data) noexcept
{
    Hasher hasher;
    hasher.bytes(data);
    return hasher.finish();
}

} // namespace detail

// ---------- Construction ----------

namespace {

bool finite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// A freshly built primitive must be a valid solid; OCCT's own status is not trusted (kernel.h contract).
Result<Shape> checkedSolid(const TopoDS_Shape& built, std::string_view where)
{
    if (built.IsNull() || built.ShapeType() != TopAbs_SOLID || !BRepCheck_Analyzer(built).IsValid())
        return tl::unexpected(
            detail::makeError(ErrorCode::KernelFailure, "The geometry kernel built an invalid solid.", where));
    return detail::wrap(built);
}

} // namespace

Result<Shape> makeBox(Vec3 min, Vec3 max)
{
    constexpr std::string_view where = "kernel::makeBox";
    if (!finite(min) || !finite(max))
        return tl::unexpected(detail::makeError(ErrorCode::InvalidArgument, "Box corners must be finite.", where));
    const double tolerance = Precision::Confusion();
    if (!(max.x - min.x > tolerance) || !(max.y - min.y > tolerance) || !(max.z - min.z > tolerance))
        return tl::unexpected(detail::makeError(
            ErrorCode::InvalidArgument, "A box needs a positive size along x, y and z (max greater than min).", where));
    return detail::guarded(where, [&]() -> Result<Shape> {
        BRepPrimAPI_MakeBox maker(detail::toPnt(min), detail::toPnt(max));
        maker.Build();
        if (!maker.IsDone())
            return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, "The box could not be built.", where));
        return checkedSolid(maker.Solid(), where);
    });
}

Result<Shape> makeCylinder(Vec3 base, Vec3 axis, double radius, double height)
{
    constexpr std::string_view where = "kernel::makeCylinder";
    if (!finite(base) || !finite(axis) || !std::isfinite(radius) || !std::isfinite(height))
        return tl::unexpected(
            detail::makeError(ErrorCode::InvalidArgument, "Cylinder parameters must be finite.", where));
    const double tolerance = Precision::Confusion();
    const double axisLength = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (!(axisLength > tolerance))
        return tl::unexpected(
            detail::makeError(ErrorCode::InvalidArgument, "The cylinder axis must not be a zero vector.", where));
    if (!(radius > tolerance) || !(height > tolerance))
        return tl::unexpected(detail::makeError(ErrorCode::InvalidArgument,
                                                "A cylinder needs a positive radius and a positive height.", where));
    return detail::guarded(where, [&]() -> Result<Shape> {
        const gp_Ax2 frame(detail::toPnt(base), gp_Dir(axis.x, axis.y, axis.z));
        BRepPrimAPI_MakeCylinder maker(frame, radius, height);
        maker.Build();
        if (!maker.IsDone())
            return tl::unexpected(
                detail::makeError(ErrorCode::KernelFailure, "The cylinder could not be built.", where));
        return checkedSolid(maker.Solid(), where);
    });
}

} // namespace ray::kernel
