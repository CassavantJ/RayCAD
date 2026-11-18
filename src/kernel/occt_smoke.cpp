// Calls into every OCCT toolkit ray_kernel links (task T-001). See occt_smoke.h.
#include "occt_smoke.h"

#include <BRepAlgo.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFeat_MakePrism.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Tool.hxx>
#include <DE_Wrapper.hxx>
#include <GC_MakeSegment.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Line.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Plane.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <HLRBRep_Algo.hxx>
#include <IGESControl_Writer.hxx>
#include <Interface_Static.hxx>
#include <OSD_Environment.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeFix_Shape.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <StlAPI_Writer.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <sstream>
#include <stdexcept>

#if defined(_MSC_VER)
#include <excpt.h>
#endif

namespace ray::kernel::detail {

namespace {

// The real work. Throws Standard_Failure or std::exception on failure; `done` lists finished toolkits.
void runSmoke(std::vector<std::string>& done)
{
    auto require = [](bool ok, const char* what) {
        if (!ok)
            throw std::runtime_error(what);
    };

    // TKernel
    OSD_Environment env("RAYCAD_OCCT_SMOKE");
    (void)env.Value();
    done.emplace_back("TKernel");

    // TKMath
    gp_Trsf rotation;
    rotation.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0.5);
    require(rotation.Form() == gp_Rotation, "TKMath: rotation not built");
    done.emplace_back("TKMath");

    // TKG2d
    Handle(Geom2d_Line) line2d = new Geom2d_Line(gp_Pnt2d(0, 0), gp_Dir2d(1, 0));
    require(!line2d.IsNull(), "TKG2d: no line");
    done.emplace_back("TKG2d");

    // TKG3d
    Handle(Geom_Plane) plane = new Geom_Plane(gp_Pln());
    done.emplace_back("TKG3d");

    // TKGeomBase
    GC_MakeSegment makeSegment(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0));
    require(makeSegment.IsDone(), "TKGeomBase: no segment");
    Handle(Geom_TrimmedCurve) segment = makeSegment.Value();
    done.emplace_back("TKGeomBase");

    // TKGeomAlgo
    GeomAPI_ProjectPointOnSurf projection(gp_Pnt(1, 2, 3), plane);
    require(projection.NbPoints() == 1, "TKGeomAlgo: projection failed");
    done.emplace_back("TKGeomAlgo");

    // TKPrim
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 20.0).Shape();
    const TopoDS_Shape cylinder =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(50, 30, -5), gp_Dir(0, 0, 1)), 8.0, 40.0).Shape();
    done.emplace_back("TKPrim");

    // TKBRep
    int faceCount = 0;
    TopoDS_Face firstFace;
    for (TopExp_Explorer it(box, TopAbs_FACE); it.More(); it.Next()) {
        if (faceCount == 0)
            firstFace = TopoDS::Face(it.Current());
        ++faceCount;
    }
    require(faceCount == 6, "TKBRep: a box must have 6 faces");
    require(!BRep_Tool::Surface(firstFace).IsNull(), "TKBRep: face without surface");
    done.emplace_back("TKBRep");

    // TKTopAlgo
    const TopoDS_Edge edge = BRepBuilderAPI_MakeEdge(segment).Edge();
    require(!edge.IsNull(), "TKTopAlgo: no edge");
    GProp_GProps boxProps;
    BRepGProp::VolumeProperties(box, boxProps);
    require(boxProps.Mass() > 119999.0 && boxProps.Mass() < 120001.0, "TKTopAlgo: wrong box volume");
    require(BRepCheck_Analyzer(box).IsValid(), "TKTopAlgo: box not valid");
    done.emplace_back("TKTopAlgo");

    // TKBO
    BRepAlgoAPI_Cut cut(box, cylinder);
    require(cut.IsDone() && !cut.HasErrors(), "TKBO: cut failed");
    const TopoDS_Shape drilled = cut.Shape();
    GProp_GProps drilledProps;
    BRepGProp::VolumeProperties(drilled, drilledProps);
    require(drilledProps.Mass() < boxProps.Mass(), "TKBO: cut removed nothing");
    done.emplace_back("TKBO");

    // TKBool
    require(BRepAlgo::IsValid(drilled), "TKBool: cut result not valid");
    done.emplace_back("TKBool");

    // TKShHealing
    ShapeFix_Shape fix(drilled);
    fix.Perform();
    const TopoDS_Shape healed = fix.Shape();
    require(!healed.IsNull(), "TKShHealing: no result");
    done.emplace_back("TKShHealing");

    // TKFillet
    BRepFilletAPI_MakeFillet fillet(box);
    for (TopExp_Explorer it(box, TopAbs_EDGE); it.More(); it.Next())
        fillet.Add(2.0, TopoDS::Edge(it.Current()));
    fillet.Build();
    require(fillet.IsDone(), "TKFillet: fillet failed");
    done.emplace_back("TKFillet");

    // TKOffset (construction only: the kernel does not use BRepOffset_MakeOffset, spike S9)
    BRepOffsetAPI_MakeThickSolid thickSolid;
    require(!thickSolid.IsDone(), "TKOffset: unexpected state");
    done.emplace_back("TKOffset");

    // TKFeat (set-up call only; nothing is built)
    BRepFeat_MakePrism featPrism;
    featPrism.Init(box, firstFace, firstFace, gp_Dir(0, 0, 1), 1, true);
    require(!featPrism.IsDone(), "TKFeat: unexpected state");
    done.emplace_back("TKFeat");

    // TKMesh
    BRepMesh_IncrementalMesh mesher(healed, 0.2, false, 0.35, false);
    require(mesher.IsDone(), "TKMesh: meshing failed");
    done.emplace_back("TKMesh");

    // TKHLR
    Handle(HLRBRep_Algo) hlr = new HLRBRep_Algo();
    hlr->Add(box);
    require(hlr->NbShapes() == 1, "TKHLR: shape not added");
    done.emplace_back("TKHLR");

    // TKXSBase
    (void)Interface_Static::IsPresent("write.precision.mode");
    done.emplace_back("TKXSBase");

    // TKDE
    Handle(DE_Wrapper) wrapper = DE_Wrapper::GlobalWrapper();
    require(!wrapper.IsNull(), "TKDE: no global wrapper");
    done.emplace_back("TKDE");

    // TKDESTEP: write the drilled box to memory and read it back
    std::stringstream stepText;
    {
        STEPControl_Writer writer;
        require(writer.Transfer(drilled, STEPControl_AsIs) == IFSelect_RetDone, "TKDESTEP: transfer failed");
        require(writer.WriteStream(stepText) == IFSelect_RetDone, "TKDESTEP: write failed");
        STEPControl_Reader reader;
        require(reader.ReadStream("smoke.step", stepText) == IFSelect_RetDone, "TKDESTEP: read failed");
        require(reader.TransferRoots() > 0, "TKDESTEP: nothing transferred");
        GProp_GProps readProps;
        BRepGProp::VolumeProperties(reader.OneShape(), readProps);
        const double relative = (readProps.Mass() - drilledProps.Mass()) / drilledProps.Mass();
        require(relative > -1e-6 && relative < 1e-6, "TKDESTEP: volume changed in the round trip");
    }
    done.emplace_back("TKDESTEP");

    // TKDEIGES
    IGESControl_Writer igesWriter;
    require(igesWriter.AddShape(box), "TKDEIGES: shape not added");
    done.emplace_back("TKDEIGES");

    // TKDESTL (construction only)
    StlAPI_Writer stlWriter;
    stlWriter.ASCIIMode() = false;
    done.emplace_back("TKDESTL");

    // TKLCAF, then TKXCAF
    Handle(TDocStd_Document) document = new TDocStd_Document("BinXCAF");
    require(!document->Main().IsNull(), "TKLCAF: document has no main label");
    done.emplace_back("TKLCAF");
    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
    require(!shapeTool.IsNull(), "TKXCAF: no shape tool");
    require(!shapeTool->AddShape(box).IsNull(), "TKXCAF: shape not added");
    done.emplace_back("TKXCAF");
}

// Runs the smoke and turns C++ exceptions into an error message. Returns true on success.
bool runCatching(std::vector<std::string>& done, std::string& message)
{
    try {
        runSmoke(done);
        return true;
    } catch (const Standard_Failure& failure) {
        message = std::string("OCCT failure: ") + failure.what();
    } catch (const std::exception& failure) {
        message = failure.what();
    } catch (...) {
        message = "unknown exception";
    }
    return false;
}

#if defined(_MSC_VER)
// A delay-loaded DLL that is missing raises a structured exception at the first call into it. This frame
// has no C++ objects to unwind, which __try requires.
bool runGuarded(std::vector<std::string>& done, std::string& message, unsigned long& sehCode)
{
    __try {
        return runCatching(done, message);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sehCode = _exception_code();
        return false;
    }
}
#endif

} // namespace

std::vector<std::string> occtToolkits()
{
    std::vector<std::string> names;
    std::stringstream list(RAY_OCCT_TOOLKITS);
    for (std::string name; std::getline(list, name, ',');)
        names.push_back(name);
    return names;
}

std::string occtVersion()
{
    return OCC_VERSION_COMPLETE;
}

Result<std::vector<std::string>> occtSmoke()
{
    std::vector<std::string> done;
    std::string message;
#if defined(_MSC_VER)
    unsigned long sehCode = 0;
    const bool ok = runGuarded(done, message, sehCode);
    if (!ok && sehCode != 0) {
        std::ostringstream text;
        text << "structured exception 0x" << std::hex << sehCode;
        if (sehCode == 0xC06D007Eul || sehCode == 0xC06D007Ful)
            text << " (an OpenCASCADE DLL or one of its functions could not be loaded)";
        message = text.str();
    }
#else
    const bool ok = runCatching(done, message);
#endif
    if (!ok) {
        const std::string after = done.empty() ? std::string("none") : done.back();
        return tl::unexpected(Error{
            ErrorCode::KernelFailure, message + "; last toolkit that passed: " + after, {"kernel::detail::occtSmoke"}});
    }
    return done;
}

} // namespace ray::kernel::detail
