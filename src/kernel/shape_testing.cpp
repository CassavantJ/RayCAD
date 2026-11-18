// Test support for the kernel tests (task T-004). See shape_testing.h.
#include "shape_testing.h"

#include "io_internal.h"
#include "shape_impl.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRepPrimAPI_MakeWedge.hxx>
#include <BRep_Builder.hxx>
#include <DESTEP_Parameters.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <IGESCAFControl_Writer.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESData_GlobalSection.hxx>
#include <IGESData_IGESModel.hxx>
#include <NCollection_HArray1.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <UnitsMethods_LengthUnit.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_ColorType.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XSControl_WorkSession.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <fstream>
#include <initializer_list>
#include <iterator>

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

// ---------- Import / export tests (task T-005) ----------

namespace {

Result<Shape> failed(const char* what)
{
    return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, what, "testing"));
}

TopoDS_Shape cutShape(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    BRepAlgoAPI_Cut cut(a, b);
    return cut.IsDone() && !cut.HasErrors() ? cut.Shape() : TopoDS_Shape();
}

TopoDS_Shape boxShape(double x0, double y0, double z0, double x1, double y1, double z1)
{
    return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), gp_Pnt(x1, y1, z1)).Shape();
}

TopoDS_Shape cylinderShape(double x, double y, double z, double radius, double height)
{
    return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, z), gp_Dir(0, 0, 1)), radius, height).Shape();
}

// A planar face in the XZ plane bounded by the polyline `points` (closed automatically).
TopoDS_Face polygonFace(std::initializer_list<gp_Pnt> points)
{
    BRepBuilderAPI_MakePolygon polygon;
    for (const gp_Pnt& p : points)
        polygon.Add(p);
    polygon.Close();
    return BRepBuilderAPI_MakeFace(polygon.Wire(), true).Face();
}

// A closed planar face bounded by a B-spline through `points` (z = 0) and the straight segment closing it.
TopoDS_Face splineFace(std::initializer_list<gp_Pnt> points)
{
    occ::handle<NCollection_HArray1<gp_Pnt>> poles =
        new NCollection_HArray1<gp_Pnt>(1, static_cast<int>(points.size()));
    int i = 1;
    for (const gp_Pnt& p : points)
        poles->SetValue(i++, p);
    GeomAPI_Interpolate interpolate(poles, false, 1e-7);
    interpolate.Perform();
    const occ::handle<Geom_BSplineCurve> curve = interpolate.Curve();
    const TopoDS_Edge spline = BRepBuilderAPI_MakeEdge(curve).Edge();
    const TopoDS_Edge closing = BRepBuilderAPI_MakeEdge(*std::prev(points.end()), *points.begin()).Edge();
    BRepBuilderAPI_MakeWire wire(spline, closing);
    return BRepBuilderAPI_MakeFace(wire.Wire(), true).Face();
}

TopoDS_Shape filletAll(const TopoDS_Shape& shape, double radius)
{
    BRepFilletAPI_MakeFillet fillet(shape);
    for (TopExp_Explorer it(shape, TopAbs_EDGE); it.More(); it.Next())
        fillet.Add(radius, TopoDS::Edge(it.Current()));
    fillet.Build();
    return fillet.IsDone() ? fillet.Shape() : TopoDS_Shape();
}

TopoDS_Shape chamferAll(const TopoDS_Shape& shape, double distance)
{
    BRepFilletAPI_MakeChamfer chamfer(shape);
    for (TopExp_Explorer it(shape, TopAbs_EDGE); it.More(); it.Next())
        chamfer.Add(distance, TopoDS::Edge(it.Current()));
    chamfer.Build();
    return chamfer.IsDone() ? chamfer.Shape() : TopoDS_Shape();
}

TopoDS_Shape buildTestPart(int index)
{
    switch (index) {
    case 0:
        return boxShape(0, 0, 0, 10, 20, 30);
    case 1:
        return cylinderShape(0, 0, 0, 5, 20);
    case 2:
        return BRepPrimAPI_MakeSphere(gp_Pnt(1, 2, 3), 7).Shape();
    case 3:
        return BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 8, 3, 12).Shape();
    case 4:
        return BRepPrimAPI_MakeTorus(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 15, 4).Shape();
    case 5:
        return cutShape(boxShape(0, 0, 0, 100, 60, 20), cylinderShape(50, 30, -5, 8, 30));
    case 6:
        return filletAll(boxShape(0, 0, 0, 30, 20, 10), 2.0);
    case 7:
        return chamferAll(boxShape(0, 0, 0, 30, 20, 10), 1.5);
    case 8:
        return BRepPrimAPI_MakeWedge(20, 10, 15, 5).Shape();
    case 9: {
        const TopoDS_Face profile =
            polygonFace({gp_Pnt(5, 0, 0), gp_Pnt(12, 0, 0), gp_Pnt(9, 0, 15), gp_Pnt(5, 0, 15)});
        return BRepPrimAPI_MakeRevol(profile, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
    }
    case 10: {
        const TopoDS_Face profile = splineFace({gp_Pnt(0, 0, 0), gp_Pnt(10, 6, 0), gp_Pnt(20, -3, 0), gp_Pnt(30, 5, 0),
                                                gp_Pnt(40, 0, 0), gp_Pnt(20, -15, 0)});
        return BRepPrimAPI_MakePrism(profile, gp_Vec(0, 0, 12)).Shape();
    }
    case 11: {
        BRepAlgoAPI_Fuse fuse(boxShape(0, 0, 0, 40, 40, 10), cylinderShape(20, 20, 5, 9, 25));
        return fuse.IsDone() && !fuse.HasErrors() ? fuse.Shape() : TopoDS_Shape();
    }
    case 12:
        return cutShape(cylinderShape(0, 0, 0, 10, 30), cylinderShape(0, 0, -1, 7, 32));
    case 13: {
        gp_Trsf rotation;
        rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 1, 0)), 0.5235987755982988);
        gp_Trsf move;
        move.SetTranslation(gp_Vec(-30, 12, 7));
        return BRepBuilderAPI_Transform(boxShape(0, 0, 0, 25, 15, 8), move * rotation, true).Shape();
    }
    case 14: {
        TopoDS_Shape plate = boxShape(0, 0, 0, 80, 80, 6);
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                plate = cutShape(plate, cylinderShape(20 + 20 * column, 20 + 20 * row, -1, 3 + row, 8));
        return plate;
    }
    case 15: {
        BRepAlgoAPI_Common common(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 10).Shape(),
                                  boxShape(-6, -6, -20, 20, 20, 4));
        return common.IsDone() && !common.HasErrors() ? common.Shape() : TopoDS_Shape();
    }
    case 16: {
        const gp_Circ circle(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 10);
        const gp_Elips ellipse(gp_Ax2(gp_Pnt(2, 1, 25), gp_Dir(0, 0, 1)), 12, 6);
        BRepOffsetAPI_ThruSections loft(true, false);
        loft.AddWire(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circle).Edge()).Wire());
        loft.AddWire(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(ellipse).Edge()).Wire());
        loft.Build();
        return loft.IsDone() ? loft.Shape() : TopoDS_Shape();
    }
    case 17: {
        const gp_Elips ellipse(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 14, 5);
        const TopoDS_Face face =
            BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(ellipse).Edge()).Wire()).Face();
        return BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, 9)).Shape();
    }
    case 18:
        return cutShape(BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 12, 6, 18).Shape(),
                        BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 18), 5).Shape());
    case 19: {
        const TopoDS_Face profile = splineFace({gp_Pnt(4, 0, 0), gp_Pnt(11, 0, 2), gp_Pnt(9, 0, 12), gp_Pnt(5, 0, 18),
                                                gp_Pnt(6, 0, 25), gp_Pnt(4, 0, 25)});
        return BRepPrimAPI_MakeRevol(profile, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 4.71238898038469).Shape();
    }
    default:
        return TopoDS_Shape();
    }
}

// Opens a UTF-8 path for writing.
bool openForWriting(const std::string& path, std::ofstream& out)
{
    const auto fsPath = detail::pathFromUtf8(path, "testing");
    if (!fsPath)
        return false;
    out.open(*fsPath, std::ios::binary | std::ios::trunc);
    return out.is_open();
}

occ::handle<TDocStd_Document> newMillimetreDocument()
{
    occ::handle<TDocStd_Document> doc = new TDocStd_Document("BinXCAF");
    XCAFDoc_DocumentTool::Set(doc->Main(), false);
    XCAFDoc_DocumentTool::SetLengthUnit(doc, 1.0, UnitsMethods_LengthUnit_Millimeter);
    return doc;
}

TopLoc_Location moved(double x, double y, double z)
{
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(x, y, z));
    return TopLoc_Location(trsf);
}

} // namespace

Result<Shape> makeTestPart(int index)
{
    return detail::guarded("kernel::testing::makeTestPart", [&]() -> Result<Shape> {
        const TopoDS_Shape shape = buildTestPart(index);
        if (shape.IsNull() || !BRepCheck_Analyzer(shape).IsValid())
            return failed("test part not built");
        // Booleans return a compound holding one solid; hand out the solid itself.
        TopExp_Explorer solids(shape, TopAbs_SOLID);
        if (!solids.More())
            return failed("test part has no solid");
        const TopoDS_Shape solid = solids.Current();
        solids.Next();
        if (solids.More())
            return failed("test part has more than one solid");
        return detail::wrap(solid);
    });
}

Result<Shape> makeLooseBoxFaces()
{
    return detail::guarded("kernel::testing::makeLooseBoxFaces", []() -> Result<Shape> {
        const TopoDS_Shape box = boxShape(0, 0, 0, 10, 20, 30);
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        for (TopExp_Explorer it(box, TopAbs_FACE); it.More(); it.Next())
            builder.Add(compound, BRepBuilderAPI_Copy(it.Current()).Shape());
        return detail::wrap(compound);
    });
}

Status writeStepAssembly(const std::string& path, bool inches)
{
    return detail::guarded("kernel::testing::writeStepAssembly", [&]() -> Status {
        occ::handle<TDocStd_Document> doc = newMillimetreDocument();
        const occ::handle<XCAFDoc_ShapeTool> shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        const occ::handle<XCAFDoc_ColorTool> colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());

        const TDF_Label bracket = shapes->AddShape(boxShape(0, 0, 0, 40, 20, 10), false);
        TDataStd_Name::Set(bracket, "Bracket");
        colors->SetColor(bracket, Quantity_Color(0.8, 0.1, 0.1, Quantity_TOC_sRGB), XCAFDoc_ColorSurf);

        const TDF_Label pin = shapes->AddShape(cylinderShape(0, 0, 0, 3, 30), false);
        TDataStd_Name::Set(pin, "Pin");
        colors->SetColor(pin, Quantity_Color(0.1, 0.2, 0.9, Quantity_TOC_sRGB), XCAFDoc_ColorSurf);

        const TopoDS_Shape plateShape = boxShape(0, 0, 0, 100, 50, 5);
        const TDF_Label plate = shapes->AddShape(plateShape, false);
        TDataStd_Name::Set(plate, "Plate");
        TopExp_Explorer firstFace(plateShape, TopAbs_FACE);
        const TDF_Label face = shapes->AddSubShape(plate, firstFace.Current());
        if (face.IsNull())
            return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, "sub-shape label failed", "testing"));
        colors->SetColor(face, Quantity_Color(0.1, 0.7, 0.2, Quantity_TOC_sRGB), XCAFDoc_ColorSurf);

        const TDF_Label fixture = shapes->NewShape();
        TDataStd_Name::Set(fixture, "Fixture");
        shapes->AddComponent(fixture, bracket, TopLoc_Location());
        gp_Trsf turn;
        turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5707963267948966);
        gp_Trsf shift;
        shift.SetTranslation(gp_Vec(100, 0, 0));
        shapes->AddComponent(fixture, bracket, TopLoc_Location(shift * turn));
        shapes->AddComponent(fixture, pin, moved(10, 10, 10));
        shapes->AddComponent(fixture, plate, moved(0, 0, -5));
        shapes->UpdateAssemblies();

        STEPCAFControl_Writer writer;
        writer.SetColorMode(true);
        writer.SetNameMode(true);
        DESTEP_Parameters params;
        params.WriteUnit = inches ? UnitsMethods_LengthUnit_Inch : UnitsMethods_LengthUnit_Millimeter;
        if (!writer.Transfer(doc, params, STEPControl_AsIs))
            return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, "STEP transfer failed", "testing"));
        std::ofstream out;
        if (!openForWriting(path, out))
            return tl::unexpected(detail::makeError(ErrorCode::IoError, "cannot write", "testing"));
        if (writer.WriteStream(out) != IFSelect_RetDone)
            return tl::unexpected(detail::makeError(ErrorCode::IoError, "STEP write failed", "testing"));
        return std::monostate{};
    });
}

Status writeIgesColoredBox(const std::string& path, bool inches)
{
    return detail::guarded("kernel::testing::writeIgesColoredBox", [&]() -> Status {
        occ::handle<TDocStd_Document> doc = newMillimetreDocument();
        const occ::handle<XCAFDoc_ShapeTool> shapes = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
        const occ::handle<XCAFDoc_ColorTool> colors = XCAFDoc_DocumentTool::ColorTool(doc->Main());
        const TDF_Label box = shapes->AddShape(boxShape(0, 0, 0, 40, 20, 10), false);
        TDataStd_Name::Set(box, "Block");
        colors->SetColor(box, Quantity_Color(0.8, 0.1, 0.1, Quantity_TOC_sRGB), XCAFDoc_ColorSurf);
        IGESControl_Controller::Init();
        occ::handle<XSControl_WorkSession> session = new XSControl_WorkSession();
        session->SelectNorm("IGES");
        IGESCAFControl_Writer writer(session, inches ? "IN" : "MM");
        writer.SetColorMode(true);
        writer.SetNameMode(true);
        if (!writer.Transfer(doc))
            return tl::unexpected(detail::makeError(ErrorCode::KernelFailure, "IGES transfer failed", "testing"));
        writer.ComputeModel();
        IGESData_GlobalSection global = writer.Model()->GlobalSection();
        global.SetAuthorName(new TCollection_HAsciiString(""));
        global.SetCompanyName(new TCollection_HAsciiString(""));
        global.SetFileName(new TCollection_HAsciiString("box.igs"));
        writer.Model()->SetGlobalSection(global);
        std::ofstream out;
        if (!openForWriting(path, out))
            return tl::unexpected(detail::makeError(ErrorCode::IoError, "cannot write", "testing"));
        if (!writer.Write(out))
            return tl::unexpected(detail::makeError(ErrorCode::IoError, "IGES write failed", "testing"));
        return std::monostate{};
    });
}

Result<bool> isValidShape(const Shape& shape)
{
    return detail::guarded("kernel::testing::isValidShape", [&]() -> Result<bool> {
        if (auto ok = detail::requireShape(shape, "kernel::testing::isValidShape"); !ok)
            return tl::unexpected(ok.error());
        return BRepCheck_Analyzer(detail::occt(shape)).IsValid();
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
