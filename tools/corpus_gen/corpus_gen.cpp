// corpus_gen: writes the synthetic part of the RayCAD test corpus (tests/corpus/files/raycad-synthetic/).
//
// Prismatic job-shop parts built with OpenCASCADE: through and blind holes with drill points, counterbores,
// countersinks, closed and open pockets with corner radii, islands, slots, keyways, T-slots, fillets, chamfers,
// thin walls and ribs. Every part is checked with BRepCheck_Analyzer before it is written.
//
// The output is reproducible and carries no personal data: the STEP FILE_NAME entry, the AP203 person,
// organization and date entities, and the IGES global section carry fixed names and a fixed date instead of the
// user name, IP address and clock that OCCT would write; files are written by bare file name from inside the
// output folder, so no local path appears either.
//
// Usage: corpus_gen <output folder>
// Prints one tab-separated line per file, starting with "part": file, format, units, schema, faces, solids,
// volume, features. OpenCASCADE prints its own transfer statistics between them.
// Exit code 1 if any part fails to build, is invalid, or fails to write.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Tool.hxx>
#include <DESTEP_Parameters.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <IGESControl_Writer.hxx>
#include <IGESData_GlobalSection.hxx>
#include <IGESData_IGESModel.hxx>
#include <Interface_Static.hxx>
#include <NCollection_List.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

const double PI = 3.14159265358979323846;
const double INCH = 25.4;
const gp_Dir DX(1, 0, 0), DY(0, 1, 0), DZ(0, 0, 1);
const char* const TIMESTAMP = "2026-01-01T00:00:00";
const char* const IGES_DATE = "20260101.000000";
const char* const AUTHOR = "RayCAD corpus_gen";
const char* const ORGANIZATION = "RayCAD project";

using Shape = TopoDS_Shape;

struct BuildError {
    std::string what;
};

// ---------- primitives and booleans ----------

Shape box(double x, double y, double z, double dx, double dy, double dz)
{
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Shape();
}

Shape cyl(const gp_Pnt& p, const gp_Dir& d, double r, double h)
{
    return BRepPrimAPI_MakeCylinder(gp_Ax2(p, d), r, h).Shape();
}

Shape cylZ(double x, double y, double z0, double r, double h)
{
    return cyl(gp_Pnt(x, y, z0), DZ, r, h);
}

Shape cone(const gp_Pnt& p, const gp_Dir& d, double r1, double r2, double h)
{
    return BRepPrimAPI_MakeCone(gp_Ax2(p, d), r1, r2, h).Shape();
}

Shape cut(const Shape& a, const Shape& b)
{
    BRepAlgoAPI_Cut op(a, b);
    if (!op.IsDone())
        throw BuildError{"boolean cut failed"};
    return op.Shape();
}

Shape fuse(const Shape& a, const Shape& b)
{
    BRepAlgoAPI_Fuse op(a, b);
    if (!op.IsDone())
        throw BuildError{"boolean fuse failed"};
    return op.Shape();
}

Shape unify(const Shape& s)
{
    ShapeUpgrade_UnifySameDomain u(s, true, true, true);
    u.Build();
    return u.Shape();
}

// Rounded rectangle centred on (cx, cy), w along X, l along Y, corner radius r (0 = sharp), extruded along +Z
// from z0 by h. r = min(w, l) / 2 gives an obround slot.
Shape roundedRect(double cx, double cy, double w, double l, double r, double z0, double h)
{
    if (r <= 1e-9)
        return box(cx - w / 2, cy - l / 2, z0, w, l, h);
    const double x0 = cx - w / 2, x1 = cx + w / 2, y0 = cy - l / 2, y1 = cy + l / 2;
    BRepBuilderAPI_MakeWire wire;
    auto line = [&](double ax, double ay, double bx, double by) {
        if (std::hypot(bx - ax, by - ay) > 1e-9)
            wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(ax, ay, z0), gp_Pnt(bx, by, z0)).Edge());
    };
    auto arc = [&](double ccx, double ccy, double a0) {
        gp_Pnt p0(ccx + r * std::cos(a0), ccy + r * std::sin(a0), z0);
        gp_Pnt pm(ccx + r * std::cos(a0 + PI / 4), ccy + r * std::sin(a0 + PI / 4), z0);
        gp_Pnt p1(ccx + r * std::cos(a0 + PI / 2), ccy + r * std::sin(a0 + PI / 2), z0);
        wire.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(p0, pm, p1).Value()).Edge());
    };
    line(x0 + r, y0, x1 - r, y0);
    arc(x1 - r, y0 + r, -PI / 2);
    line(x1, y0 + r, x1, y1 - r);
    arc(x1 - r, y1 - r, 0);
    line(x1 - r, y1, x0 + r, y1);
    arc(x0 + r, y1 - r, PI / 2);
    line(x0, y1 - r, x0, y0 + r);
    arc(x0 + r, y0 + r, PI);
    if (!wire.IsDone())
        throw BuildError{"rounded rectangle wire failed"};
    TopoDS_Face face = BRepBuilderAPI_MakeFace(wire.Wire(), true).Face();
    return BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, h)).Shape();
}

// ---------- hole features (all drilled along -Z from ztop) ----------

// Through or blind hole. A blind hole gets a 118-degree drill point below its full-diameter depth.
Shape drill(const Shape& s, double x, double y, double ztop, double d, double depth, bool point)
{
    Shape tool = cylZ(x, y, ztop - depth, d / 2, depth + 1.0);
    if (point) {
        double tip = (d / 2) / std::tan(59.0 * PI / 180.0);
        tool = fuse(tool, cone(gp_Pnt(x, y, ztop - depth), -DZ, d / 2, 0.0, tip));
    }
    return cut(s, tool);
}

Shape counterbore(const Shape& s, double x, double y, double ztop, double d, double cd, double cdepth, double depth)
{
    Shape tool = fuse(cylZ(x, y, ztop - depth, d / 2, depth + 1.0), cylZ(x, y, ztop - cdepth, cd / 2, cdepth + 1.0));
    return cut(s, tool);
}

Shape countersink(const Shape& s, double x, double y, double ztop, double d, double cd, double depth)
{
    double h = (cd - d) / 2; // 90-degree countersink
    Shape tool = cylZ(x, y, ztop - depth, d / 2, depth + 1.0);
    tool = fuse(tool, cone(gp_Pnt(x, y, ztop - h), DZ, d / 2, cd / 2, h));
    tool = fuse(tool, cylZ(x, y, ztop, cd / 2, 1.0));
    return cut(s, tool);
}

// ---------- edge selection, fillets and chamfers ----------

struct EdgeInfo {
    TopoDS_Edge edge;
    gp_Pnt a, b, m;
    bool line;
    bool circle;
};

std::vector<EdgeInfo> edgesOf(const Shape& s)
{
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> map;
    TopExp::MapShapes(s, TopAbs_EDGE, map);
    std::vector<EdgeInfo> out;
    for (int i = 1; i <= map.Extent(); ++i) {
        TopoDS_Edge e = TopoDS::Edge(map(i));
        if (BRep_Tool::Degenerated(e))
            continue;
        BRepAdaptor_Curve c(e);
        EdgeInfo x;
        x.edge = e;
        x.a = c.Value(c.FirstParameter());
        x.b = c.Value(c.LastParameter());
        x.m = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
        x.line = c.GetType() == GeomAbs_Line;
        x.circle = c.GetType() == GeomAbs_Circle;
        out.push_back(x);
    }
    return out;
}

bool eq(double a, double b)
{
    return std::abs(a - b) < 1e-4;
}

bool vertical(const EdgeInfo& e)
{
    return e.line && eq(e.a.X(), e.b.X()) && eq(e.a.Y(), e.b.Y());
}

bool atZ(const EdgeInfo& e, double z)
{
    return eq(e.a.Z(), z) && eq(e.b.Z(), z) && eq(e.m.Z(), z);
}

// Vertical edge on the outside of the axis-aligned box [x0,x1]x[y0,y1].
std::function<bool(const EdgeInfo&)> outerCorner(double x0, double y0, double x1, double y1)
{
    return [=](const EdgeInfo& e) {
        return vertical(e) && (eq(e.a.X(), x0) || eq(e.a.X(), x1)) && (eq(e.a.Y(), y0) || eq(e.a.Y(), y1));
    };
}

// Edge lying in the plane z on the outline of the box [x0,x1]x[y0,y1].
std::function<bool(const EdgeInfo&)> outline(double x0, double y0, double x1, double y1, double z)
{
    return [=](const EdgeInfo& e) {
        if (!e.line || !atZ(e, z))
            return false;
        bool onX = eq(e.a.X(), e.b.X()) && (eq(e.a.X(), x0) || eq(e.a.X(), x1));
        bool onY = eq(e.a.Y(), e.b.Y()) && (eq(e.a.Y(), y0) || eq(e.a.Y(), y1));
        return onX || onY;
    };
}

Shape fillet(const Shape& s, double r, const std::function<bool(const EdgeInfo&)>& pick, const char* what)
{
    BRepFilletAPI_MakeFillet mk(s);
    int n = 0;
    for (const EdgeInfo& e : edgesOf(s))
        if (pick(e)) {
            mk.Add(r, e.edge);
            ++n;
        }
    if (n == 0)
        throw BuildError{std::string("no edges for fillet: ") + what};
    mk.Build();
    if (!mk.IsDone())
        throw BuildError{std::string("fillet failed: ") + what};
    return mk.Shape();
}

Shape chamfer(const Shape& s, double d, const std::function<bool(const EdgeInfo&)>& pick, const char* what)
{
    BRepFilletAPI_MakeChamfer mk(s);
    int n = 0;
    for (const EdgeInfo& e : edgesOf(s))
        if (pick(e)) {
            mk.Add(d, e.edge);
            ++n;
        }
    if (n == 0)
        throw BuildError{std::string("no edges for chamfer: ") + what};
    mk.Build();
    if (!mk.IsDone())
        throw BuildError{std::string("chamfer failed: ") + what};
    return mk.Shape();
}

// Hollows the solid, removing its topmost planar face(s) at z, leaving walls of thickness t.
Shape shellOpenTop(const Shape& s, double z, double t)
{
    NCollection_List<TopoDS_Shape> remove;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface surf(f);
        if (surf.GetType() != GeomAbs_Plane)
            continue;
        GProp_GProps g;
        BRepGProp::SurfaceProperties(f, g);
        if (eq(g.CentreOfMass().Z(), z) && std::abs(surf.Plane().Axis().Direction().Z()) > 0.999)
            remove.Append(f);
    }
    if (remove.IsEmpty())
        throw BuildError{"no top face to remove for shell"};
    BRepOffsetAPI_MakeThickSolid mk;
    mk.MakeThickSolidByJoin(s, remove, -t, 1e-4);
    if (!mk.IsDone())
        throw BuildError{"thick solid failed"};
    return mk.Shape();
}

// ---------- output ----------

struct Part {
    std::string name;
    Shape shape;
    bool inch = false;
    std::string features;
};

int countOf(const Shape& s, TopAbs_ShapeEnum type)
{
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher> map;
    TopExp::MapShapes(s, type, map);
    return map.Extent();
}

double volumeOf(const Shape& s)
{
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

std::string readAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeAll(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Replaces the FILE_NAME entry of a STEP header with fixed values, so reruns give identical bytes, and scrubs
// what OCCT's AP203 writer takes from the machine: the user name and IP address (PERSON, ORGANIZATION), the
// current date and time, and the time zone.
bool scrubStep(const std::string& file)
{
    std::string text = readAll(file);
    std::size_t begin = text.find("FILE_NAME(");
    std::size_t end = begin == std::string::npos ? begin : text.find(");", begin);
    if (end == std::string::npos)
        return false;
    std::string entry = "FILE_NAME('" + file + "','" + TIMESTAMP + "',('" + AUTHOR + "'),('" + ORGANIZATION +
                        "'),\n  'Open CASCADE STEP processor 8.0','RayCAD corpus_gen','')";
    text.replace(begin, end + 1 - begin, entry);
    const std::pair<const char*, const char*> rules[] = {
        {R"(PERSON\('[^']*','[^']*','[^']*',)", "PERSON('corpus_gen','','RayCAD',"},
        {R"(= ORGANIZATION\('[^']*','[^']*','[^']*'\))", "= ORGANIZATION('RayCAD','RayCAD project','')"},
        {R"(CALENDAR_DATE\(\d+,\d+,\d+\))", "CALENDAR_DATE(2026,1,1)"},
        {R"(LOCAL_TIME\(\d+,\d+,)", "LOCAL_TIME(0,0,"},
        {R"(COORDINATED_UNIVERSAL_TIME_OFFSET\([^)]*\))", "COORDINATED_UNIVERSAL_TIME_OFFSET(0,$,.AHEAD.)"},
    };
    for (const auto& [pattern, replacement] : rules)
        text = std::regex_replace(text, std::regex(pattern), replacement);
    writeAll(file, text);
    return true;
}

bool writeStep(const Part& p, const std::string& file, DESTEP_Parameters::WriteMode_StepSchema schema)
{
    DESTEP_Parameters params;
    params.InitFromStatic();
    params.WriteSchema = schema;
    params.WriteUnit = p.inch ? UnitsMethods_LengthUnit_Inch : UnitsMethods_LengthUnit_Millimeter;
    STEPControl_Writer w;
    if (w.Transfer(p.shape, STEPControl_AsIs, params) != IFSelect_RetDone)
        return false;
    if (w.Write(file.c_str()) != IFSelect_RetDone)
        return false;
    return scrubStep(file);
}

bool writeIges(const Part& p, const std::string& file, bool brepMode)
{
    Interface_Static::SetCVal("write.iges.header.author", AUTHOR);
    Interface_Static::SetCVal("write.iges.header.company", ORGANIZATION);
    Interface_Static::SetCVal("write.iges.header.product", "RayCAD synthetic corpus part");
    Interface_Static::SetCVal("write.iges.header.receiver", "");
    IGESControl_Writer w(p.inch ? "IN" : "MM", brepMode ? 1 : 0);
    if (!w.AddShape(p.shape))
        return false;
    w.ComputeModel();
    IGESData_GlobalSection gs = w.Model()->GlobalSection();
    gs.SetFileName(new TCollection_HAsciiString(file.c_str()));
    gs.SetDate(new TCollection_HAsciiString(IGES_DATE));
    gs.SetLastChangeDate(new TCollection_HAsciiString(IGES_DATE));
    gs.SetAuthorName(new TCollection_HAsciiString(AUTHOR));
    gs.SetCompanyName(new TCollection_HAsciiString(ORGANIZATION));
    w.Model()->SetGlobalSection(gs);
    return w.Write(file.c_str());
}

// ---------- part families ----------
// Each family takes a variant index and returns one part. Dimensions are in millimetres; inch parts use
// inch-round dimensions multiplied by INCH and are written with inch units.

Part plateThruHoles(int v)
{
    const double L[] = {120, 150, 80, 6 * INCH, 200, 4 * INCH};
    const double W[] = {80, 100, 60, 4 * INCH, 60, 3 * INCH};
    const double T[] = {10, 12, 8, 0.5 * INCH, 6, 0.375 * INCH};
    const int NX[] = {3, 5, 2, 4, 8, 3};
    const int NY[] = {2, 3, 2, 3, 2, 2};
    const double D[] = {6.6, 9, 5.5, 0.266 * INCH, 4.5, 0.201 * INCH};
    Part p{"plate_thru_holes_" + std::to_string(v + 1), {}, v == 3 || v == 5, "through holes grid"};
    Shape s = box(0, 0, 0, L[v], W[v], T[v]);
    double mx = L[v] / (NX[v] + 1), my = W[v] / (NY[v] + 1);
    for (int i = 1; i <= NX[v]; ++i)
        for (int j = 1; j <= NY[v]; ++j)
            s = drill(s, i * mx, j * my, T[v], D[v], T[v], false);
    if (v % 2 == 1) {
        s = chamfer(s, T[v] / 10, outline(0, 0, L[v], W[v], T[v]), "top outline");
        p.features += "; top edge chamfer";
    }
    p.shape = s;
    return p;
}

Part plateCounterbores(int v)
{
    // Socket head cap screw counterbores: hole, counterbore diameter, counterbore depth.
    const double HD[] = {6.6, 9.0, 11.0, 0.266 * INCH, 5.5, 0.397 * INCH};
    const double CD[] = {11.0, 15.0, 18.0, 0.44 * INCH, 10.0, 0.625 * INCH};
    const double CH[] = {6.5, 8.6, 10.8, 0.26 * INCH, 5.5, 0.385 * INCH};
    const double T[] = {15, 20, 25, 0.75 * INCH, 12, 0.75 * INCH};
    const int N[] = {4, 4, 6, 4, 8, 2};
    Part p{"plate_counterbore_" + std::to_string(v + 1), {}, v == 3 || v == 5, "counterbored through holes"};
    double L = 30 + N[v] / 2 * CD[v] * 2.5, W = CD[v] * 5;
    Shape s = box(0, 0, 0, L, W, T[v]);
    int cols = N[v] / 2;
    for (int i = 0; i < cols; ++i)
        for (int j = 0; j < 2; ++j)
            s = counterbore(s, 15 + CD[v] * 1.25 + i * CD[v] * 2.5, W * (j == 0 ? 0.3 : 0.7), T[v], HD[v], CD[v], CH[v],
                            T[v]);
    if (v == 2 || v == 4) {
        s = fillet(s, 3.0, outerCorner(0, 0, L, W), "outer corners");
        p.features += "; corner fillets";
    }
    p.shape = s;
    return p;
}

Part plateCountersinks(int v)
{
    const double HD[] = {4.5, 6.6, 9.0, 0.266 * INCH};
    const double CD[] = {9.0, 13.0, 18.0, 0.52 * INCH};
    Part p{"plate_countersink_" + std::to_string(v + 1), {}, v == 3, "90-degree countersunk through holes"};
    double T = v == 3 ? 0.375 * INCH : 4 + 2 * v;
    double L = 100 + 20 * v, W = 50;
    Shape s = box(0, 0, 0, L, W, T);
    for (int i = 0; i < 3 + v; ++i)
        s = countersink(s, 15 + i * (L - 30) / (2 + v), W / 2, T, HD[v], CD[v], T);
    p.shape = s;
    return p;
}

Part blockBlindHoles(int v)
{
    // Tap drill sizes: M6 (5.0), M8 (6.8), M10 (8.5), 1/4-20 (#7, 0.201 in).
    const double D[] = {5.0, 6.8, 8.5, 0.201 * INCH};
    const double DEPTH[] = {15, 20, 25, 0.6 * INCH};
    Part p{"block_blind_holes_" + std::to_string(v + 1), {}, v == 3, "blind tap-drill holes with 118-degree points"};
    double L = 80, W = 60, H = 40;
    Shape s = box(0, 0, 0, L, W, H);
    for (int i = 0; i < 2 + v; ++i)
        s = drill(s, 15 + i * 50.0 / (1 + v), 20, H, D[v], DEPTH[v], true);
    s = drill(s, 40, 45, H, D[v] * 1.6, DEPTH[v] * 0.6, true);
    p.shape = s;
    return p;
}

Part closedPocket(int v)
{
    const double PW[] = {60, 80, 40, 2.5 * INCH, 50, 100};
    const double PL[] = {40, 50, 40, 1.5 * INCH, 30, 60};
    const double PR[] = {6, 8, 20, 0.25 * INCH, 3, 10};
    const double PD[] = {10, 15, 8, 0.375 * INCH, 12, 20};
    Part p{"closed_pocket_" + std::to_string(v + 1), {}, v == 3, "closed rectangular pocket with corner radii"};
    double L = PW[v] + 40, W = PL[v] + 40, H = PD[v] + 10;
    Shape s = box(0, 0, 0, L, W, H);
    s = cut(s, roundedRect(L / 2, W / 2, PW[v], PL[v], PR[v], H - PD[v], PD[v] + 1));
    if (v == 1 || v == 5) {
        double rf = v == 1 ? 2.0 : 3.0;
        s = fillet(
            s, rf,
            [&](const EdgeInfo& e) {
                return atZ(e, H - PD[v]) && e.m.X() > 20 - 1e-6 && e.m.X() < L - 20 + 1e-6 && e.m.Y() > 20 - 1e-6 &&
                       e.m.Y() < W - 20 + 1e-6;
            },
            "pocket floor");
        p.features += "; floor fillet";
    }
    if (v == 4 || v == 5) {
        s = fuse(s, cylZ(L / 2, W / 2, H - PD[v] - 0.5, PL[v] / 6, PD[v] - 2 + 0.5));
        s = unify(s);
        p.features += "; round island";
    }
    p.shape = s;
    return p;
}

Part openPocketStep(int v)
{
    Part p{"open_pocket_step_" + std::to_string(v + 1), {}, v == 2, "open-sided step pocket"};
    double L = 100, W = 60, H = 30;
    if (p.inch) {
        L = 3 * INCH;
        W = 3 * INCH;
        H = 2 * INCH;
    }
    Shape s = box(0, 0, 0, L, W, H);
    double sw = W * (0.3 + 0.1 * v), sd = H * 0.4;
    s = cut(s, box(-1, -1, H - sd, L + 2, sw + 1, sd + 1));
    if (v >= 1) {
        s = fillet(
            s, 2.0 + v,
            [&](const EdgeInfo& e) { return e.line && eq(e.a.Y(), sw) && eq(e.b.Y(), sw) && atZ(e, H - sd); },
            "step inner corner");
        p.features += "; inner corner fillet";
    }
    if (v == 3) {
        s = cut(s, roundedRect(L / 2, -5 + 15, 30, 30, 5, H - sd - 8, 9));
        p.features += "; open pocket into step floor";
    }
    p.shape = s;
    return p;
}

Part throughSlots(int v)
{
    Part p{"slot_through_" + std::to_string(v + 1), {}, v == 3, "through obround slots"};
    double T = v == 3 ? 0.5 * INCH : 8 + 2 * v;
    double sw = v == 3 ? 0.375 * INCH : 6 + 2 * v;
    double L = 140, W = 70;
    Shape s = box(0, 0, 0, L, W, T);
    int n = 2 + v;
    for (int i = 0; i < n; ++i)
        s = cut(s, roundedRect(20 + i * (L - 40) / (n - 1), W / 2, sw, 40, sw / 2, -1, T + 2));
    if (v == 2) {
        s = cut(s, roundedRect(L / 2, 8, 80, sw, sw / 2, -1, T + 2));
        p.features += "; slot along X";
    }
    p.shape = s;
    return p;
}

Part keywayTSlot(int v)
{
    Part p{"keyway_tslot_" + std::to_string(v + 1), {}, v == 3, ""};
    if (v % 2 == 0) {
        // Shaft with a blind end-milled keyway.
        double d = v == 0 ? 25 : 40, len = v == 0 ? 80 : 120, kw = v == 0 ? 8 : 12, kd = v == 0 ? 4 : 5;
        Shape s = cyl(gp_Pnt(0, 0, 0), DX, d / 2, len);
        s = cut(s, roundedRect(len / 2, 0, len * 0.4, kw, kw / 2, d / 2 - kd, kd + 1));
        s = chamfer(
            s, 1.0, [&](const EdgeInfo& e) { return e.circle && (eq(e.m.X(), 0) || eq(e.m.X(), len)); }, "shaft ends");
        p.shape = s;
        p.features = "shaft with blind keyway; end chamfers";
    } else {
        // Block with a T-slot running along X.
        double scale = v == 3 ? INCH / 25.0 : 1.0;
        double L = 120 * scale, W = 80 * scale, H = 40 * scale;
        double neck = 12 * scale, head = 22 * scale, headH = 9 * scale, neckH = 14 * scale;
        Shape s = box(0, 0, 0, L, W, H);
        s = cut(s, box(-1, W / 2 - neck / 2, H - neckH, L + 2, neck, neckH + 1));
        s = cut(s, box(-1, W / 2 - head / 2, H - neckH - headH, L + 2, head, headH));
        p.shape = s;
        p.features = "T-slot through along X";
    }
    return p;
}

Part filletBlock(int v)
{
    Part p{"fillet_block_" + std::to_string(v + 1), {}, v == 2, "vertical corner fillets"};
    double L = 80 + 10 * v, W = 50, H = 25;
    if (p.inch) {
        L = 4 * INCH;
        W = 2 * INCH;
        H = 1 * INCH;
    }
    double r = v == 2 ? 0.25 * INCH : 4 + 3 * v;
    Shape s = box(0, 0, 0, L, W, H);
    s = fillet(s, r, outerCorner(0, 0, L, W), "vertical corners");
    if (v >= 1) {
        s = fillet(s, 2.0, [&](const EdgeInfo& e) { return atZ(e, H); }, "top edges");
        p.features += "; top edge fillets (tangent chain)";
    }
    if (v == 3) {
        s = cut(s, roundedRect(L / 2, W / 2, L - 30, W - 24, 5, H - 8, 9));
        p.features += "; pocket";
    }
    p.shape = s;
    return p;
}

Part chamferBlock(int v)
{
    Part p{"chamfer_block_" + std::to_string(v + 1), {}, v == 1, "chamfered edges"};
    double L = 70, W = 45, H = 30;
    if (p.inch) {
        L = 3 * INCH;
        W = 2 * INCH;
        H = 1.25 * INCH;
    }
    double c = v == 1 ? 0.03 * INCH : 1.0 + v;
    Shape s = box(0, 0, 0, L, W, H);
    if (v >= 2)
        s = drill(s, L / 2, W / 2, H, 12, H, false);
    s = chamfer(s, c, outline(0, 0, L, W, H), "top outline");
    if (v == 2) {
        s = chamfer(
            s, 1.0,
            [&](const EdgeInfo& e) {
                return e.circle && atZ(e, H) && std::hypot(e.m.X() - L / 2, e.m.Y() - W / 2) < 8;
            },
            "hole edge");
        p.features += "; through hole with edge break";
    }
    if (v == 3) {
        s = chamfer(s, c, outline(0, 0, L, W, 0), "bottom outline");
        p.features += "; bottom chamfers; through hole";
    }
    p.shape = s;
    return p;
}

Part thinWallBox(int v)
{
    const double T[] = {2.0, 1.5, 1.0, 0.06 * INCH, 0.8};
    Part p{"thin_wall_box_" + std::to_string(v + 1), {}, v == 3, "shelled open box, thin walls"};
    double L = 80 + 10 * v, W = 50 + 5 * v, H = 30;
    Shape s = box(0, 0, 0, L, W, H);
    if (v == 1 || v == 4) {
        s = fillet(s, 6.0, outerCorner(0, 0, L, W), "outer corners");
        p.features += "; corner fillets before shell";
    }
    s = shellOpenTop(s, H, T[v]);
    if (v == 2) {
        s = drill(s, L / 2, W / 2, 1.0, 10, 1.0, false);
        p.features += "; floor hole";
    }
    p.shape = s;
    return p;
}

Part ribPlate(int v)
{
    const double RT[] = {1.5, 1.2, 2.0};
    Part p{"rib_plate_" + std::to_string(v + 1), {}, false, "base plate with thin ribs and bosses"};
    double L = 120, W = 80, T = 4, RH = 15 + 5 * v;
    Shape s = box(0, 0, 0, L, W, T);
    for (int i = 1; i <= 3 + v; ++i)
        s = fuse(s, box(i * L / (4 + v) - RT[v] / 2, 5, T - 0.01, RT[v], W - 10, RH));
    s = fuse(s, box(5, W / 2 - RT[v] / 2, T - 0.01, L - 10, RT[v], RH * 0.6));
    s = fuse(s, cylZ(12, 12, T - 0.01, 6, RH));
    s = fuse(s, cylZ(L - 12, W - 12, T - 0.01, 6, RH));
    s = drill(s, 12, 12, T + RH - 0.01, 3.3, RH, true);
    s = drill(s, L - 12, W - 12, T + RH - 0.01, 3.3, RH, true);
    p.shape = unify(s);
    return p;
}

Part flangeBoltCircle(int v)
{
    Part p{"flange_bolt_circle_" + std::to_string(v + 1), {}, v == 3, "disc flange, bolt circle, hub"};
    double D = v == 3 ? 5 * INCH : 100 + 20 * v, T = v == 3 ? 0.5 * INCH : 12 + 2 * v;
    int n = 4 + 2 * v;
    Shape s = cylZ(0, 0, 0, D / 2, T);
    s = fuse(s, cylZ(0, 0, T - 0.01, D / 4, T));
    s = unify(s);
    s = drill(s, 0, 0, 2 * T, D / 6, 2 * T, false);
    double bc = D * 0.38, hd = D / 12;
    for (int i = 0; i < n; ++i)
        s = drill(s, bc * std::cos(2 * PI * i / n), bc * std::sin(2 * PI * i / n), T, hd, T, false);
    if (v >= 1) {
        s = fillet(
            s, 2.0, [&](const EdgeInfo& e) { return e.circle && atZ(e, T) && eq(std::hypot(e.m.X(), e.m.Y()), D / 4); },
            "hub root");
        p.features += "; hub root fillet";
    }
    p.shape = s;
    return p;
}

Part lBracket(int v)
{
    Part p{"l_bracket_" + std::to_string(v + 1), {}, v == 1, "L bracket"};
    double L = 80, W = 50, T = 8, H = 60;
    if (p.inch) {
        L = 3 * INCH;
        W = 2 * INCH;
        T = 0.25 * INCH;
        H = 2.5 * INCH;
    }
    Shape s = fuse(box(0, 0, 0, L, W, T), box(0, 0, 0, T, W, H));
    s = unify(s);
    s = fillet(
        s, T * 0.75, [&](const EdgeInfo& e) { return e.line && eq(e.a.X(), T) && eq(e.b.X(), T) && atZ(e, T); },
        "inner corner");
    s = drill(s, L * 0.65, W * 0.3, T, 6.6, T, false);
    s = drill(s, L * 0.65, W * 0.7, T, 6.6, T, false);
    // Vertical obround slots through the upright, built along X from two cylinders and a box.
    for (int j = 0; j < 2; ++j) {
        double y = W * (j == 0 ? 0.3 : 0.7);
        Shape tool = fuse(cyl(gp_Pnt(-1, y, H * 0.5), DX, 3.3, T + 2), cyl(gp_Pnt(-1, y, H * 0.8), DX, 3.3, T + 2));
        tool = fuse(tool, box(-1, y - 3.3, H * 0.5, T + 2, 6.6, H * 0.3));
        s = cut(s, tool);
    }
    p.features += "; inner fillet; holes; slots in upright";
    if (v >= 2) {
        s = chamfer(s, 1.0, outerCorner(0, 0, L, W), "outer vertical corners");
        p.features += "; corner chamfers";
    }
    if (v == 3) {
        s = cut(s, roundedRect(L * 0.65, W / 2, 20, 12, 6, -1, T + 2));
        p.features += "; centre slot";
    }
    p.shape = unify(s);
    return p;
}

Part manifoldBlock(int v)
{
    Part p{"manifold_block_" + std::to_string(v + 1), {}, v == 2, "crossing drilled passages, ported faces"};
    double S = v == 2 ? 3 * INCH : 60 + 10 * v;
    Shape s = box(0, 0, 0, S, S, S * 0.8);
    double H = S * 0.8, d = S / 8;
    // Vertical passage from the top, blind with drill point.
    s = drill(s, S / 2, S / 2, H, d, H * 0.6, true);
    // Horizontal passages from the sides, meeting the vertical one.
    Shape px = cyl(gp_Pnt(-1, S / 2, H * 0.3), DX, d / 2, S / 2 + 1);
    Shape py = cyl(gp_Pnt(S / 2, -1, H * 0.3), DY, d / 2, S / 2 + 1);
    s = cut(cut(s, px), py);
    // Counterbored ports on the side faces.
    s = cut(s, cyl(gp_Pnt(-1, S / 2, H * 0.3), DX, d, 1 + d));
    s = cut(s, cyl(gp_Pnt(S / 2, -1, H * 0.3), DY, d, 1 + d));
    if (v >= 1) {
        for (int i = 0; i < 4; ++i)
            s = drill(s, S * (i % 2 == 0 ? 0.15 : 0.85), S * (i < 2 ? 0.15 : 0.85), H, 5.0, 12, true);
        p.features += "; four blind mounting holes";
    }
    if (v == 3) {
        s = cut(s, cyl(gp_Pnt(S * 0.25, S * 0.75, -1), DZ, d / 2, H + 2));
        p.features += "; through passage";
    }
    p.shape = s;
    return p;
}

// Benchmark-part-1-like fixture plate (PLAN.md 12.3): pockets, counterbores, tap drills, chamfers, inch.
Part fixturePlate(int v)
{
    Part p{"fixture_plate_" + std::to_string(v + 1),
           {},
           v != 2,
           "fixture plate: pockets with corner radii, counterbores, tap drills"};
    double L = 6 * INCH, W = 4 * INCH, T = 0.75 * INCH;
    if (v == 2) {
        L = 160;
        W = 100;
        T = 20;
    }
    Shape s = box(0, 0, 0, L, W, T);
    double pd = 0.375 * INCH * (v == 2 ? 0.8 : 1.0);
    s = cut(s, roundedRect(L * 0.3, W * 0.5, L * 0.25, W * 0.45, 0.25 * INCH, T - pd, pd + 1));
    s = cut(s, roundedRect(L * 0.7, W * 0.5, L * 0.2, W * 0.35, 0.25 * INCH, T - pd, pd + 1));
    double cx[] = {0.08, 0.92, 0.08, 0.92}, cy[] = {0.12, 0.12, 0.88, 0.88};
    for (int i = 0; i < 4; ++i)
        s = counterbore(s, L * cx[i], W * cy[i], T, 0.266 * INCH, 0.44 * INCH, 0.26 * INCH, T);
    for (int i = 0; i < 4; ++i)
        s = drill(s, L * (0.5 + (i % 2 == 0 ? -0.04 : 0.04)), W * (i < 2 ? 0.1 : 0.9), T, 0.201 * INCH,
                  v == 1 ? 0.5 * INCH : T, v == 1);
    if (v != 3) {
        s = chamfer(s, 0.02 * INCH, outline(0, 0, L, W, T), "top outline");
        p.features += "; top edge chamfer";
    }
    p.shape = s;
    return p;
}

Part circularPocketBoss(int v)
{
    Part p{"circular_pocket_" + std::to_string(v + 1), {}, v == 1, "circular pocket"};
    double S = v == 1 ? 3 * INCH : 80, H = 40;
    Shape s = box(0, 0, 0, S, S, H);
    double pd = v == 1 ? 1.5 * INCH : 50;
    s = cut(s, cylZ(S / 2, S / 2, H - 19, pd / 2, 20));
    if (v == 0) {
        s = fuse(s, cylZ(S / 2, S / 2, H - 19.5, 8, 15));
        s = unify(s);
        s = drill(s, S / 2, S / 2, H - 4.5, 6, 10, true);
        p.features += "; central boss with blind hole";
    } else {
        s = fillet(s, 3.0, [&](const EdgeInfo& e) { return e.circle && atZ(e, H - 19); }, "pocket floor");
        p.features += "; floor fillet";
    }
    p.shape = s;
    return p;
}

struct Family {
    std::function<Part(int)> make;
    int variants;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: corpus_gen <output folder>\n");
        return 2;
    }
    std::filesystem::create_directories(argv[1]);
    std::filesystem::current_path(argv[1]); // write by bare file name: no absolute path in any header

    const std::vector<Family> families = {
        {plateThruHoles, 6}, {plateCounterbores, 6},  {plateCountersinks, 4}, {blockBlindHoles, 4}, {closedPocket, 6},
        {openPocketStep, 4}, {throughSlots, 4},       {keywayTSlot, 4},       {filletBlock, 4},     {chamferBlock, 4},
        {thinWallBox, 5},    {ribPlate, 3},           {flangeBoltCircle, 4},  {lBracket, 4},        {manifoldBlock, 4},
        {fixturePlate, 4},   {circularPocketBoss, 2},
    };
    const DESTEP_Parameters::WriteMode_StepSchema schemas[] = {DESTEP_Parameters::WriteMode_StepSchema_AP214IS,
                                                               DESTEP_Parameters::WriteMode_StepSchema_AP203,
                                                               DESTEP_Parameters::WriteMode_StepSchema_AP242DIS};
    const char* schemaNames[] = {"AP214", "AP203", "AP242"};
    // IGES copies: one part from each of these families (index into `families`, variant), alternating BRep mode
    // (type 186 solids) and trimmed-surface mode (type 144).
    const int igesPick[][2] = {{0, 1}, {1, 2}, {4, 0}, {5, 1}, {6, 2}, {8, 1}, {9, 3}, {12, 1}, {14, 0}, {15, 0}};

    int failures = 0, count = 0;
    std::vector<Part> built;
    std::vector<int> familyOf;
    for (std::size_t f = 0; f < families.size(); ++f) {
        for (int v = 0; v < families[f].variants; ++v) {
            Part part;
            try {
                part = families[f].make(v);
            } catch (const BuildError& e) {
                std::fprintf(stderr, "family %zu variant %d: %s\n", f, v, e.what.c_str());
                ++failures;
                continue;
            } catch (const Standard_Failure& e) {
                std::fprintf(stderr, "family %zu variant %d: OCCT exception %s\n", f, v, e.what());
                ++failures;
                continue;
            }
            if (part.shape.IsNull() || !BRepCheck_Analyzer(part.shape).IsValid() ||
                countOf(part.shape, TopAbs_SOLID) != 1) {
                std::fprintf(stderr, "%s: result is not one valid solid\n", part.name.c_str());
                ++failures;
                continue;
            }
            int schema = count % 3;
            std::string file = part.name + ".stp";
            if (!writeStep(part, file, schemas[schema])) {
                std::fprintf(stderr, "%s: STEP write failed\n", file.c_str());
                ++failures;
                continue;
            }
            std::printf("part\t%s\tSTEP\t%s\t%s\t%d\t%d\t%.6f\t%s\n", file.c_str(), part.inch ? "inch" : "mm",
                        schemaNames[schema], countOf(part.shape, TopAbs_FACE), countOf(part.shape, TopAbs_SOLID),
                        volumeOf(part.shape), part.features.c_str());
            ++count;
            built.push_back(part);
            familyOf.push_back(static_cast<int>(f) * 100 + v);
        }
    }
    int k = 0;
    for (const auto& pick : igesPick) {
        for (std::size_t i = 0; i < built.size(); ++i) {
            if (familyOf[i] != pick[0] * 100 + pick[1])
                continue;
            bool brep = k % 2 == 0;
            std::string file = built[i].name + (brep ? "_brep.igs" : "_surf.igs");
            if (!writeIges(built[i], file, brep)) {
                std::fprintf(stderr, "%s: IGES write failed\n", file.c_str());
                ++failures;
            } else {
                std::printf("part\t%s\tIGES\t%s\t%s\t%d\t%d\t%.6f\t%s\n", file.c_str(), built[i].inch ? "inch" : "mm",
                            brep ? "IGES 5.3 BRep (186)" : "IGES 5.3 trimmed surfaces (144)",
                            countOf(built[i].shape, TopAbs_FACE), countOf(built[i].shape, TopAbs_SOLID),
                            volumeOf(built[i].shape), built[i].features.c_str());
            }
            ++k;
        }
    }
    std::fprintf(stderr, "corpus_gen: %d STEP, %d IGES, %d failure(s)\n", count, k, failures);
    return failures == 0 ? 0 : 1;
}
