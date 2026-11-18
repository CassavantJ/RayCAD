// Test support for the kernel tests (task T-004). See shape_testing.h.
#include "shape_testing.h"

#include "shape_impl.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

namespace ray::kernel::testing {

Result<Shape> makeCutBody()
{
    return detail::guarded("kernel::testing::makeCutBody", []() -> Result<Shape> {
        const TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 20.0).Shape();
        const TopoDS_Shape cylinder =
            BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(50, 30, -5), gp_Dir(0, 0, 1)), 8.0, 30.0).Shape();
        BRepAlgoAPI_Cut cut(box, cylinder);
        if (!cut.IsDone() || cut.HasErrors())
            return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, "cut failed", "testing"));
        return detail::wrap(cut.Shape());
    });
}

Result<Shape> makeLocatedCompound()
{
    return detail::guarded("kernel::testing::makeLocatedCompound", []() -> Result<Shape> {
        const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
        gp_Trsf move;
        move.SetTranslation(gp_Vec(200, 0, 0));
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        builder.Add(compound, box);
        builder.Add(compound, box.Moved(TopLoc_Location(move)));
        return detail::wrap(compound);
    });
}

Result<Shape> makePlanarFace()
{
    return detail::guarded("kernel::testing::makePlanarFace", []() -> Result<Shape> {
        const gp_Pln plane(gp_Pnt(0, 0, 5), gp_Dir(0, 0, 1));
        BRepBuilderAPI_MakeFace maker(plane, 0.0, 10.0, 0.0, 20.0);
        if (!maker.IsDone())
            return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, "face failed", "testing"));
        return detail::wrap(maker.Face());
    });
}

Status meshAndShare(const Shape& shape)
{
    return detail::guarded("kernel::testing::meshAndShare", [&]() -> Status {
        const TopoDS_Shape& stored = detail::occt(shape);
        if (stored.IsNull())
            return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, "null shape", "testing"));
        BRepMesh_IncrementalMesh mesher(stored, 0.5, false, 0.5, false);
        if (!mesher.IsDone())
            return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, "mesh failed", "testing"));
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        builder.Add(compound, stored);
        return std::monostate{};
    });
}

std::vector<std::byte> payloadOf(std::span<const std::byte> serialized)
{
    const std::span<const std::byte> payload = detail::payloadOf(serialized);
    return {payload.begin(), payload.end()};
}

std::vector<std::byte> seal(std::span<const std::byte> payload)
{
    return detail::sealPayload(payload);
}

} // namespace ray::kernel::testing
