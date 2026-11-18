// Structural pre-check of shapes OCCT made, before BRepCheck sees them. See shape_impl.h.
//
// PRE-CHECK. BRepCheck_Analyzer classifies the wires of every face with BRepTopAdaptor_FClass2d. When the polygon it
// first makes of a wire is too coarse for the wire's area, FClass2d samples every edge again with
// GCPnts_QuasiUniformDeflection on the edge's pcurve and stops at the first edge that sampler refuses. The sampler
// refuses a B-spline or Bezier curve whose parametric resolution (the parameter step that moves the curve by
// Precision::Confusion()) is finer than one rounding step of its parameter values: it cannot place points on it.
// When that is the first edge of the wire, FClass2d is left with no point and indexes its point sequence at -1 and 0
// (OCCT 8.0.1 builds that file with No_Standard_OutOfRange): a read through a null pointer. On Linux that kills the
// process (found by the naming random test: a fillet's trial result in the size search); on Windows the kernel's
// structured-exception guard catches it, after OCCT has already run on garbage.
//
// So before any such call the kernel refuses a shape in which some face
//   - has no surface (FClass2d and BRepCheck dereference it);
//   - has an edge without a pcurve on it, or with a parameter range that is not finite and increasing;
//   - has an edge whose pcurve is a B-spline or Bezier curve that GCPnts_QuasiUniformDeflection refuses (the test
//     above, made exactly as that sampler makes it).
// A pcurve that cannot resolve Precision::Confusion() is degenerate geometry whatever BRepCheck would say, so this
// never refuses a shape that should have been accepted. It walks every edge of every face once: a small fraction of
// what BRepCheck itself costs.
#include "shape_impl.h"

#include <BRepAdaptor_Curve2d.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Tool.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAbs_CurveType.hxx>
#include <Precision.hxx>
#include <Standard_Real.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <cmath>

namespace ray::kernel::detail {

namespace {

std::optional<std::string> edgeFault(const TopoDS_Edge& edge, const TopoDS_Face& face)
{
    double first = 0.0;
    double last = 0.0;
    const occ::handle<Geom2d_Curve> pcurve = BRep_Tool::CurveOnSurface(edge, face, first, last);
    if (pcurve.IsNull())
        return std::string("an edge has no curve on its face");
    if (!std::isfinite(first) || !std::isfinite(last) || !(first < last))
        return std::string("an edge has an empty or unbounded parameter range");
    const BRepAdaptor_Curve2d curve(edge, face);
    const GeomAbs_CurveType type = curve.GetType();
    if (type == GeomAbs_BSplineCurve || type == GeomAbs_BezierCurve) {
        const double resolution = std::min(curve.Resolution(Precision::Confusion()), 1.e50);
        const double largest = std::max(std::abs(curve.FirstParameter()), std::abs(curve.LastParameter()));
        if (!(resolution >= Epsilon(largest)))
            return std::string("an edge's curve on its face is too steep to sample");
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> structuralFault(const TopoDS_Shape& shape)
{
    if (shape.IsNull())
        return std::string("the shape is empty");
    ShapeIndex faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
        const TopoDS_Face face = TopoDS::Face(faces.FindKey(i).Oriented(TopAbs_FORWARD));
        TopLoc_Location location;
        if (BRep_Tool::Surface(face, location).IsNull())
            return std::string("a face has no surface");
        for (TopoDS_Iterator wire(face); wire.More(); wire.Next()) {
            if (wire.Value().ShapeType() != TopAbs_WIRE)
                continue;
            for (TopoDS_Iterator it(wire.Value()); it.More(); it.Next())
                if (it.Value().ShapeType() == TopAbs_EDGE)
                    if (auto fault = edgeFault(TopoDS::Edge(it.Value()), face))
                        return fault;
        }
    }
    return std::nullopt;
}

bool checkedValid(const TopoDS_Shape& shape)
{
    if (structuralFault(shape))
        return false;
    return BRepCheck_Analyzer(shape).IsValid();
}

} // namespace ray::kernel::detail
