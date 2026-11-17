// References at least one symbol from every OpenCASCADE toolkit the product will use,
// and does a small amount of real work with each so the DLLs are loaded and initialised.
// Returns the number of triangles of the meshed result (a real value, so nothing is optimised away).
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom2d_Line.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Plane.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <IGESControl_Writer.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Version.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

int occtTouch()
{
    // TKernel, TKMath, TKG2d, TKG3d, TKGeomBase
    Handle(Geom_Plane) plane = new Geom_Plane(gp_Pln());
    Handle(Geom2d_Line) line2d = new Geom2d_Line(gp_Pnt2d(0, 0), gp_Dir2d(1, 0));
    GeomAPI_ProjectPointOnSurf proj(gp_Pnt(1, 2, 3), plane);
    Handle(Geom_TrimmedCurve) arc = GC_MakeArcOfCircle(gp_Pnt(0, 0, 0), gp_Pnt(5, 5, 0), gp_Pnt(10, 0, 0)).Value();

    // TKBRep, TKTopAlgo, TKPrim
    TopoDS_Edge e1 = BRepBuilderAPI_MakeEdge(arc);
    TopoDS_Edge e2 = BRepBuilderAPI_MakeEdge(gp_Pnt(10, 0, 0), gp_Pnt(0, 0, 0));
    TopoDS_Wire wire = BRepBuilderAPI_MakeWire(e1, e2);
    TopoDS_Face face = BRepBuilderAPI_MakeFace(wire);
    TopoDS_Shape prism = BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, 30)).Shape();
    TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 20.0).Shape();
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(50, 30, -5), gp_Dir(0, 0, 1)), 8.0, 40.0).Shape();

    // TKBO, TKBool
    TopoDS_Shape fused = BRepAlgoAPI_Fuse(box, prism).Shape();
    TopoDS_Shape cut = BRepAlgoAPI_Cut(fused, cyl).Shape();

    // TKFillet
    BRepFilletAPI_MakeFillet fillet(box);
    for (TopExp_Explorer ex(box, TopAbs_EDGE); ex.More(); ex.Next())
        fillet.Add(2.0, TopoDS::Edge(ex.Current()));
    TopoDS_Shape filleted = fillet.Shape();

    // TKOffset
    BRepOffsetAPI_MakeOffsetShape offset;
    offset.PerformByJoin(box, 1.5, 1e-6);
    TopoDS_Shape offsetShape = offset.Shape();

    // TKShHealing
    ShapeFix_Shape fix(cut);
    fix.Perform();
    TopoDS_Shape healed = fix.Shape();

    // TKMesh
    BRepMesh_IncrementalMesh mesh(healed, 0.1, false, 0.3, true);
    BRepMesh_IncrementalMesh mesh2(filleted, 0.1, false, 0.3, true);
    int triangles = 0;
    for (const TopoDS_Shape* s : {&healed, &filleted}) {
        for (TopExp_Explorer ex(*s, TopAbs_FACE); ex.More(); ex.Next()) {
            TopLoc_Location loc;
            Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
            if (!tri.IsNull())
                triangles += tri->NbTriangles();
        }
    }
    Bnd_Box bb;
    BRepBndLib::Add(offsetShape, bb);

    // TKLCAF, TKCAF, TKXCAF
    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", doc);
    XCAFDoc_DocumentTool::ShapeTool(doc->Main())->AddShape(healed);

    // TKXSBase, TKDE, TKDESTEP, TKDEIGES
    STEPCAFControl_Reader stepReader;
    IGESCAFControl_Reader igesReader;
    STEPControl_Writer stepWriter;
    stepWriter.Transfer(healed, STEPControl_AsIs);
    IGESControl_Writer igesWriter;
    igesWriter.AddShape(filleted);

    return triangles + (bb.IsVoid() ? 0 : 1) + (proj.NbPoints() > 0 ? 0 : 0);
}
