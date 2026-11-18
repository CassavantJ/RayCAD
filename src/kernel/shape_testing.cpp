// Test support for the kernel tests (task T-004). See shape_testing.h.
#include "shape_testing.h"

#include "shape_impl.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRep_Builder.hxx>
#include <TopExp.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <vector>

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

namespace {

Error failed(const char* what)
{
    return detail::makeError(ErrorCode::KernelFailure, what, "testing");
}

// The distinct edges of `shape` (each once) for which `pick` is true.
template <class Pick>
std::vector<TopoDS_Edge> edgesWhere(const TopoDS_Shape& shape, Pick pick)
{
    detail::ShapeIndex edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    std::vector<TopoDS_Edge> picked;
    for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(edges.FindKey(i));
        if (pick(BRepAdaptor_Curve(edge)))
            picked.push_back(edge);
    }
    return picked;
}

Result<Shape> validSolid(const TopoDS_Shape& shape)
{
    if (shape.IsNull() || !BRepCheck_Analyzer(shape).IsValid())
        return tl::unexpected(failed("the test body is not valid"));
    return detail::wrap(shape);
}

} // namespace

Result<Shape> makeFilletedBody()
{
    return detail::guarded("kernel::testing::makeFilletedBody", []() -> Result<Shape> {
        const TopoDS_Shape box = BRepPrimAPI_MakeBox(80.0, 50.0, 20.0).Shape();
        BRepFilletAPI_MakeFillet corners(box);
        for (const TopoDS_Edge& edge : edgesWhere(box, [](const BRepAdaptor_Curve& curve) {
                 return curve.GetType() == GeomAbs_Line && std::abs(curve.Line().Direction().Z()) > 0.99;
             }))
            corners.Add(5.0, edge);
        corners.Build();
        if (!corners.IsDone())
            return tl::unexpected(failed("corner fillets failed"));

        TopoDS_Compound holes;
        BRep_Builder builder;
        builder.MakeCompound(holes);
        for (const double x : {20.0, 60.0})
            builder.Add(holes, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, 25, -5), gp_Dir(0, 0, 1)), 6.0, 30.0).Shape());
        BRepAlgoAPI_Cut cut(corners.Shape(), holes);
        if (!cut.IsDone() || cut.HasErrors())
            return tl::unexpected(failed("hole cut failed"));

        BRepFilletAPI_MakeFillet rims(cut.Shape());
        for (const TopoDS_Edge& edge : edgesWhere(cut.Shape(), [](const BRepAdaptor_Curve& curve) {
                 return curve.GetType() == GeomAbs_Circle && std::abs(curve.Circle().Radius() - 6.0) < 1e-6 &&
                        std::abs(curve.Circle().Location().Z() - 20.0) < 1e-6;
             }))
            rims.Add(1.5, edge);
        rims.Build();
        if (!rims.IsDone())
            return tl::unexpected(failed("rim fillets failed"));
        return validSolid(rims.Shape());
    });
}

Result<Shape> makeSphere(Vec3 centre, double radius)
{
    return detail::guarded("kernel::testing::makeSphere", [&]() -> Result<Shape> {
        return validSolid(BRepPrimAPI_MakeSphere(detail::toPnt(centre), radius).Shape());
    });
}

Result<Shape> makeTorus(double majorRadius, double minorRadius)
{
    return detail::guarded("kernel::testing::makeTorus", [&]() -> Result<Shape> {
        return validSolid(
            BRepPrimAPI_MakeTorus(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), majorRadius, minorRadius).Shape());
    });
}

Result<Shape> makeHolePlate(int rows, int cols)
{
    return detail::guarded("kernel::testing::makeHolePlate", [&]() -> Result<Shape> {
        const TopoDS_Shape plate = BRepPrimAPI_MakeBox(10.0 * cols, 10.0 * rows, 5.0).Shape();
        TopoDS_Compound holes;
        BRep_Builder builder;
        builder.MakeCompound(holes);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c)
                builder.Add(holes, BRepPrimAPI_MakeCylinder(
                                       gp_Ax2(gp_Pnt(10.0 * c + 5.0, 10.0 * r + 5.0, -1.0), gp_Dir(0, 0, 1)), 3.0, 7.0)
                                       .Shape());
        BRepAlgoAPI_Cut cut(plate, holes);
        if (!cut.IsDone() || cut.HasErrors())
            return tl::unexpected(failed("hole cut failed"));
        return validSolid(cut.Shape());
    });
}

Result<Shape> makeBodyWithUnmeshableFace()
{
    return detail::guarded("kernel::testing::makeBodyWithUnmeshableFace", []() -> Result<Shape> {
        BRepBuilderAPI_MakePolygon bowTie(gp_Pnt(0, 0, 50), gp_Pnt(10, 10, 50), gp_Pnt(10, 0, 50), gp_Pnt(0, 10, 50),
                                          /*Close=*/true);
        if (!bowTie.IsDone())
            return tl::unexpected(failed("bow tie wire failed"));
        BRepBuilderAPI_MakeFace face(bowTie.Wire(), /*OnlyPlane=*/true);
        if (!face.IsDone())
            return tl::unexpected(failed("bow tie face failed"));
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        builder.Add(compound, BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
        builder.Add(compound, face.Face());
        return detail::wrap(compound);
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
