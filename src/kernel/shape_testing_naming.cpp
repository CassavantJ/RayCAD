// Test-only shape builders used by the naming tests (task T-007). See shape_testing.h.
#include "shape_impl.h"
#include "shape_testing.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <NCollection_List.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace ray::kernel::testing {

namespace {

Error testingError(const char* what)
{
    return detail::makeError(ErrorCode::KernelFailure, what, "testing");
}

// The only solid of `shape` (a boolean result is often a compound holding one solid); null if not exactly one.
TopoDS_Shape singleSolid(const TopoDS_Shape& shape)
{
    TopoDS_Shape found;
    int count = 0;
    for (TopExp_Explorer it(shape, TopAbs_SOLID); it.More(); it.Next()) {
        found = it.Current();
        ++count;
    }
    return count == 1 ? found : TopoDS_Shape();
}

Result<Shape> validSolid(const TopoDS_Shape& shape, const char* what)
{
    if (shape.IsNull() || shape.ShapeType() != TopAbs_SOLID || !BRepCheck_Analyzer(shape).IsValid())
        return tl::unexpected(testingError(what));
    return detail::wrap(shape);
}

// splitmix64: a fixed, platform-independent sequence for the face permutation.
std::uint64_t nextRandom(std::uint64_t& state)
{
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

Result<Shape> makePlateWithHoles(Vec3 size, double radius, std::span<const Vec2> centres)
{
    return detail::guarded("kernel::testing::makePlateWithHoles", [&]() -> Result<Shape> {
        const TopoDS_Shape plate = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(size.x, size.y, size.z)).Shape();
        if (centres.empty())
            return validSolid(plate, "plate failed");
        NCollection_List<TopoDS_Shape> arguments;
        arguments.Append(plate);
        NCollection_List<TopoDS_Shape> tools;
        for (const Vec2& c : centres)
            tools.Append(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(c.x, c.y, -1.0), gp_Dir(0, 0, 1)), radius, size.z + 2.0)
                             .Shape());
        BRepAlgoAPI_Cut cut;
        cut.SetArguments(arguments);
        cut.SetTools(tools);
        cut.SetRunParallel(false);
        cut.Build();
        if (!cut.IsDone() || cut.HasErrors())
            return tl::unexpected(testingError("cut failed"));
        return validSolid(singleSolid(cut.Shape()), "the holed plate is not one valid solid");
    });
}

Result<Shape> makeStarPrism(int points, double outer, double inner, double height)
{
    return detail::guarded("kernel::testing::makeStarPrism", [&]() -> Result<Shape> {
        if (points < 3)
            return tl::unexpected(testingError("a star needs at least 3 points"));
        BRepBuilderAPI_MakePolygon polygon;
        const int corners = 2 * points;
        for (int k = 0; k < corners; ++k) {
            const double angle = 3.14159265358979323846 * k / points;
            const double r = k % 2 == 0 ? outer : inner;
            polygon.Add(gp_Pnt(r * std::cos(angle), r * std::sin(angle), 0.0));
        }
        polygon.Close();
        if (!polygon.IsDone())
            return tl::unexpected(testingError("polygon failed"));
        BRepBuilderAPI_MakeFace face(polygon.Wire(), true);
        if (!face.IsDone())
            return tl::unexpected(testingError("star face failed"));
        BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(0, 0, height));
        prism.Build();
        if (!prism.IsDone())
            return tl::unexpected(testingError("prism failed"));
        // No BRepCheck here: its self-intersection test of the 2 * points-edge cap wires is quadratic and takes
        // seconds in a debug build. A prism of a simple polygon is valid by construction.
        if (prism.Shape().IsNull() || prism.Shape().ShapeType() != TopAbs_SOLID)
            return tl::unexpected(testingError("star prism is not a solid"));
        return detail::wrap(prism.Shape());
    });
}

Result<Shape> reorderFaces(const Shape& solid, std::uint64_t seed)
{
    return detail::guarded("kernel::testing::reorderFaces", [&]() -> Result<Shape> {
        const TopoDS_Shape& stored = detail::occt(solid);
        if (stored.IsNull() || stored.ShapeType() != TopAbs_SOLID)
            return tl::unexpected(testingError("reorderFaces needs a solid"));
        std::uint64_t state = seed;
        BRep_Builder builder;
        TopoDS_Solid out;
        builder.MakeSolid(out);
        // The iterators compose orientation and location, so the rebuilt shell and solid carry none of their own.
        for (TopoDS_Iterator child(stored); child.More(); child.Next()) {
            if (child.Value().ShapeType() != TopAbs_SHELL) {
                builder.Add(out, child.Value());
                continue;
            }
            std::vector<TopoDS_Shape> faces;
            for (TopoDS_Iterator face(child.Value()); face.More(); face.Next())
                faces.push_back(face.Value());
            for (std::size_t i = faces.size(); i > 1; --i)
                std::swap(faces[i - 1], faces[static_cast<std::size_t>(nextRandom(state) % i)]);
            TopoDS_Shell shell;
            builder.MakeShell(shell);
            for (const TopoDS_Shape& face : faces)
                builder.Add(shell, face);
            shell.Closed(child.Value().Closed());
            builder.Add(out, shell);
        }
        return validSolid(out, "the reordered solid is not valid");
    });
}

} // namespace ray::kernel::testing
