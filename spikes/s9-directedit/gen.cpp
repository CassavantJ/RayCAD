// S9 spike: synthetic job-shop style corpus, written to STEP so the solids are history-free.
#include "common.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeUpgrade_ShapeDivideClosed.hxx>
#include <cmath>
#include <cstdio>
#include <functional>

namespace
{
const double EPS = 1e-6;
const gp_Dir DX(1, 0, 0), DY(0, 1, 0), DZ(0, 0, 1);
std::vector<std::string> g_warn;

struct EI
{
  TopoDS_Edge e;
  gp_Pnt      a, b, m;
  bool        line;
};
using Pred = std::function<bool(const EI&)>;

std::vector<EI> edgesOf(const TopoDS_Shape& s)
{
  ShapeMap em;
  TopExp::MapShapes(s, TopAbs_EDGE, em);
  std::vector<EI> out;
  for (int i = 1; i <= em.Extent(); ++i)
  {
    TopoDS_Edge e = TopoDS::Edge(em(i));
    if (BRep_Tool::Degenerated(e))
      continue;
    BRepAdaptor_Curve c(e);
    EI                x;
    x.e    = e;
    x.a    = c.Value(c.FirstParameter());
    x.b    = c.Value(c.LastParameter());
    x.m    = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
    x.line = c.GetType() == GeomAbs_Line;
    out.push_back(x);
  }
  return out;
}

bool eq(double a, double b) { return std::abs(a - b) < 1e-4; }
bool vertical(const EI& e) { return e.line && eq(e.a.X(), e.b.X()) && eq(e.a.Y(), e.b.Y()); }
bool alongY(const EI& e) { return e.line && eq(e.a.X(), e.b.X()) && eq(e.a.Z(), e.b.Z()); }
bool alongX(const EI& e) { return e.line && eq(e.a.Y(), e.b.Y()) && eq(e.a.Z(), e.b.Z()); }
bool onZ(const EI& e, double z) { return eq(e.a.Z(), z) && eq(e.b.Z(), z) && eq(e.m.Z(), z); }
double dxy(const gp_Pnt& p, double x, double y) { return std::hypot(p.X() - x, p.Y() - y); }

TopoDS_Shape box(double x, double y, double z, double dx, double dy, double dz)
{
  return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Shape();
}
TopoDS_Shape cylZ(double x, double y, double z0, double r, double h)
{
  return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, z0), DZ), r, h).Shape();
}
TopoDS_Shape cylAx(const gp_Pnt& p, const gp_Dir& d, double r, double h)
{
  return BRepPrimAPI_MakeCylinder(gp_Ax2(p, d), r, h).Shape();
}
TopoDS_Shape cut(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
  BRepAlgoAPI_Cut op(a, b);
  if (!op.IsDone())
    throw Standard_Failure("gen cut failed");
  return op.Shape();
}
TopoDS_Shape fuse(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
  BRepAlgoAPI_Fuse op(a, b);
  if (!op.IsDone())
    throw Standard_Failure("gen fuse failed");
  return op.Shape();
}
TopoDS_Shape fillet(const TopoDS_Shape& s, double r, const Pred& pred, const char* what)
{
  try
  {
    BRepFilletAPI_MakeFillet mk(s);
    int                      n = 0;
    for (const EI& e : edgesOf(s))
      if (pred(e))
      {
        mk.Add(r, e.e);
        ++n;
      }
    if (n == 0)
    {
      g_warn.push_back(std::string("no edges for fillet: ") + what);
      return s;
    }
    mk.Build();
    if (mk.IsDone() && BRepCheck_Analyzer(mk.Shape()).IsValid())
      return mk.Shape();
  }
  catch (const Standard_Failure&)
  {
  }
  g_warn.push_back(std::string("fillet failed, skipped: ") + what);
  return s;
}
TopoDS_Shape chamfer(const TopoDS_Shape& s, double d, const Pred& pred, const char* what)
{
  try
  {
    BRepFilletAPI_MakeChamfer mk(s);
    int                       n = 0;
    for (const EI& e : edgesOf(s))
      if (pred(e))
      {
        mk.Add(d, e.e);
        ++n;
      }
    if (n == 0)
    {
      g_warn.push_back(std::string("no edges for chamfer: ") + what);
      return s;
    }
    mk.Build();
    if (mk.IsDone() && BRepCheck_Analyzer(mk.Shape()).IsValid())
      return mk.Shape();
  }
  catch (const Standard_Failure&)
  {
  }
  g_warn.push_back(std::string("chamfer failed, skipped: ") + what);
  return s;
}
// Box with its four vertical edges rounded: the cutter shape of an end mill pocket.
TopoDS_Shape roundedTool(double x0, double y0, double z0, double dx, double dy, double dz, double r)
{
  return fillet(box(x0, y0, z0, dx, dy, dz), r, vertical, "pocket tool corners");
}
TopoDS_Shape obroundTool(double x0, double x1, double y, double w, double z0, double h)
{
  TopoDS_Shape t = box(x0, y - w / 2, z0, x1 - x0, w, h);
  t              = fuse(t, cylZ(x0, y, z0, w / 2, h));
  t              = fuse(t, cylZ(x1, y, z0, w / 2, h));
  return t;
}
Pred outerTop(double L, double W, double T)
{
  return [=](const EI& e) {
    return e.line && onZ(e, T)
           && (eq(e.m.X(), 0) || eq(e.m.X(), L) || eq(e.m.Y(), 0) || eq(e.m.Y(), W));
  };
}

// ---------------- families ----------------
TopoDS_Shape plateHoles(int v)
{
  double       L = 100 + 20 * v, W = 60 + 10 * v, T = 10 + 2 * v, d = 6 + v;
  TopoDS_Shape s  = fillet(box(0, 0, 0, L, W, T), 5 + v, vertical, "plate corners");
  int          nx = 3 + v, ny = 2;
  if (v == 2)
    s = chamfer(s, 1.0, [=](const EI& e) { return onZ(e, T); }, "plate top");
  for (int i = 0; i < nx; ++i)
    for (int j = 0; j < ny; ++j)
      s = cut(s, cylZ(L * (i + 1) / (nx + 1), W * (j + 1) / (ny + 1), -1, d / 2, T + 2));
  return s;
}

TopoDS_Shape cborePlate(int v)
{
  double       L = 120, W = 80, T = 16 + 2 * v;
  TopoDS_Shape s = chamfer(box(0, 0, 0, L, W, T), 0.5, outerTop(L, W, T), "cbore plate top");
  for (double x : {15.0, L - 15})
    for (double y : {15.0, W - 15})
    {
      s = cut(s, cylZ(x, y, -1, 3.3, T + 2));
      s = cut(s, cylZ(x, y, T - 6.5, 5.5, 7.5));
    }
  if (v == 1)
    for (double x : {L / 2 - 20, L / 2 + 20})
      s = cut(s, cylZ(x, W / 2, T - 10, 4, 11));
  if (v == 2)
    for (double x : {L / 2 - 20, L / 2 + 20})
    {
      s = cut(s, cylZ(x, W / 2, -1, 2.75, T + 2));
      s = cut(s, BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(x, W / 2, T - 2.5), DZ), 2.75, 5.75, 3.0).Shape());
    }
  return s;
}

TopoDS_Shape pocketBlock(int v)
{
  double       L = v == 2 ? 120 : 80, W = 60, T = 25;
  TopoDS_Shape s = box(0, 0, 0, L, W, T);
  if (v < 2)
  {
    s = cut(s, roundedTool(15, 15, T - 10, 50, 30, 11, 4 + v));
    if (v == 1)
      s = fillet(s, 2, [=](const EI& e) { return onZ(e, T - 10); }, "pocket floor");
  }
  else
  {
    s = cut(s, roundedTool(10, 12, T - 10, 40, 36, 11, 6));
    s = cut(s, roundedTool(65, 12, T - 6, 45, 36, 7, 6));
  }
  return s;
}

TopoDS_Shape lBracket(int v)
{
  double                    Wd = 40 + 10 * v;
  BRepBuilderAPI_MakePolygon poly;
  for (auto [x, z] : std::initializer_list<std::pair<double, double>>{
         {0, 0}, {60, 0}, {60, 8}, {8, 8}, {8, 50}, {0, 50}})
    poly.Add(gp_Pnt(x, 0, z));
  poly.Close();
  TopoDS_Shape s =
    BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(poly.Wire()).Face(), gp_Vec(0, Wd, 0)).Shape();
  s = fillet(s, 6, [](const EI& e) { return alongY(e) && eq(e.m.X(), 8) && eq(e.m.Z(), 8); },
             "L inner");
  if (v >= 1)
    s = fillet(s, 14, [](const EI& e) { return alongY(e) && eq(e.m.X(), 0) && eq(e.m.Z(), 0); },
               "L outer");
  if (v == 2)
    s = cut(s, obroundTool(30, 48, Wd / 2, 7, -1, 10));
  else
    for (double y : {Wd / 3, 2 * Wd / 3})
      s = cut(s, cylZ(40, y, -1, 3.25, 10));
  for (double y : {Wd / 3, 2 * Wd / 3})
    s = cut(s, cylAx(gp_Pnt(-1, y, 35), DX, 3.25, 10));
  return s;
}

TopoDS_Shape filletedBlock(int v)
{
  double       L = 60, W = 40, T = 20;
  TopoDS_Shape s = box(0, 0, 0, L, W, T);
  if (v == 0)
  {
    s = fillet(s, 5, vertical, "block vertical");
    s = fillet(s, 3, [=](const EI& e) { return onZ(e, T); }, "block top loop");
  }
  else if (v == 1)
  {
    s = fillet(s, 3, [](const EI&) { return true; }, "block all edges");
  }
  else
  {
    try
    {
      BRepFilletAPI_MakeFillet mk(s);
      for (const EI& e : edgesOf(s))
      {
        if (vertical(e) && eq(e.m.X(), 0) && eq(e.m.Y(), 0))
          mk.Add(2, e.e);
        else if (alongX(e) && eq(e.m.Y(), 0) && eq(e.m.Z(), T))
          mk.Add(3, e.e);
        else if (alongY(e) && eq(e.m.X(), 0) && eq(e.m.Z(), T))
          mk.Add(4, e.e);
      }
      mk.Build();
      if (mk.IsDone() && BRepCheck_Analyzer(mk.Shape()).IsValid())
        s = mk.Shape();
      else
        g_warn.push_back("3-radius corner failed, skipped");
    }
    catch (const Standard_Failure&)
    {
      g_warn.push_back("3-radius corner threw, skipped");
    }
    s = cut(s, cylZ(L / 2, W / 2, -1, 4, T + 2));
  }
  return s;
}

TopoDS_Shape manifold(int v)
{
  TopoDS_Shape s = box(0, 0, 0, 60, 60, 40);
  if (v == 2)
    s = chamfer(s, 1.0, outerTop(60, 60, 40), "manifold top");
  s = cut(s, cylAx(gp_Pnt(-1, 30, 20), DX, 5, 62));
  s = cut(s, cylAx(gp_Pnt(30 + (v == 1 ? 4 : 0), -1, 20), DY, 4, 62));
  auto blind = [&](double x, double y, double r, double depth) {
    double zb = 40 - depth;
    s         = cut(s, cylZ(x, y, zb, r, depth + 1));
    s = cut(s, BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(x, y, zb), gp_Dir(0, 0, -1)), r, 0.0,
                                    r / std::tan(59.0 * M_PI / 180.0))
                 .Shape());
  };
  blind(30, 30, 3, 18);
  if (v == 1)
    s = cut(s, cylAx(gp_Pnt(61, 15, 30), gp_Dir(-1, 0, 0), 4, 16));
  if (v == 2)
    for (double x : {10.0, 50.0})
      for (double y : {10.0, 50.0})
        blind(x, y, 2.5, 12);
  return s;
}

TopoDS_Shape slotPlate(int v)
{
  double       T = 12;
  TopoDS_Shape s = box(0, 0, 0, 120, 60, T);
  s              = cut(s, obroundTool(15, 35 + 10 * v, 30, 8, -1, T + 2));
  s              = cut(s, roundedTool(65, 18, T - 5, 40, 25, 6, 3));
  if (v >= 1)
    s = cut(s, cylZ(85, 30, -1, 2.5, T + 2));
  if (v == 2)
    s = cut(s, cylZ(65, 30, -1, 4, T + 2));
  return s;
}

TopoDS_Shape stepBlock(int v)
{
  TopoDS_Shape s = cut(box(0, 0, 0, 80, 50, 30), box(40, -1, 15, 41, 52, 16));
  s = fillet(s, 3, [](const EI& e) { return alongY(e) && eq(e.m.X(), 40) && eq(e.m.Z(), 15); },
             "step root");
  if (v == 2)
    s = fillet(s, 4, [](const EI& e) { return vertical(e) && eq(e.m.X(), 0); }, "step verticals");
  if (v >= 1)
    s = chamfer(s, 1.5,
                [](const EI& e) {
                  return alongX(e) && eq(e.m.Z(), 30) && (eq(e.m.Y(), 0) || eq(e.m.Y(), 50));
                },
                "step top chamfer");
  for (double y : {15.0, 35.0})
    s = cut(s, cylZ(60, y, -1, 3, 17));
  if (v == 2)
    s = cut(s, cylZ(20, 25, 20, 5, 11));
  return s;
}

TopoDS_Shape fixturePlate(int v)
{
  double       L = 152.4, W = 101.6, T = 15.875, dp = 9.525;
  TopoDS_Shape s = chamfer(box(0, 0, 0, L, W, T), 0.5, outerTop(L, W, T), "fixture top");
  if (v == 1)
  {
    s = cut(s, roundedTool(20, 25, T - dp, 40, 51, dp + 1, 6.35));
    s = cut(s, roundedTool(75, 30, T - dp, 60, 40, dp + 1, 6.35));
  }
  else
  {
    s = cut(s, roundedTool(20, 25, T - dp, 50, 51, dp + 1, 6.35));
    s = cut(s, roundedTool(82, 25, T - dp, 50, 51, dp + 1, 6.35));
  }
  if (v == 2)
    s = fillet(s, 1.0, [=](const EI& e) { return onZ(e, T - dp); }, "fixture pocket floors");
  for (auto [x, y] : std::initializer_list<std::pair<double, double>>{
         {76.2, 12}, {76.2, 89.6}, {10, 50.8}, {142.4, 50.8}})
    s = cut(s, cylZ(x, y, -1, 2.55, T + 2));
  for (double x : {10.0, L - 10})
    for (double y : {10.0, W - 10})
    {
      s = cut(s, cylZ(x, y, -1, 3.38, T + 2));
      s = cut(s, cylZ(x, y, T - 6.6, 5.6, 7.6));
    }
  return s;
}

TopoDS_Shape bossBracket(int v)
{
  TopoDS_Shape s = box(0, 0, 0, 80, 50, 10);
  s              = fuse(s, cylZ(40, 25, 10, 15, 20));
  if (v == 2)
    s = fuse(s, roundedTool(5, 15, 10, 15, 20, 8, 3));
  s = fillet(s, 3,
             [](const EI& e) { return !e.line && onZ(e, 10) && eq(dxy(e.m, 40, 25), 15); },
             "boss root");
  if (v >= 1)
    s = fillet(s, 1.5,
               [](const EI& e) { return !e.line && onZ(e, 30) && eq(dxy(e.m, 40, 25), 15); },
               "boss top");
  s = cut(s, cylZ(40, 25, -1, 6, 32));
  for (double x : {8.0, 72.0})
    for (double y : {8.0, 42.0})
      s = cut(s, cylZ(x, y, -1, 3, 12));
  return s;
}

TopoDS_Shape uChannel(int v)
{
  BRepBuilderAPI_MakePolygon poly;
  for (auto [x, z] : std::initializer_list<std::pair<double, double>>{
         {0, 0}, {60, 0}, {60, 40}, {54, 40}, {54, 6}, {6, 6}, {6, 40}, {0, 40}})
    poly.Add(gp_Pnt(x, 0, z));
  poly.Close();
  TopoDS_Shape s =
    BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(poly.Wire()).Face(), gp_Vec(0, 80, 0)).Shape();
  s = fillet(s, 4,
             [](const EI& e) {
               return alongY(e) && eq(e.m.Z(), 6) && (eq(e.m.X(), 6) || eq(e.m.X(), 54));
             },
             "U inner");
  if (v >= 1)
    s = fillet(s, 8,
               [](const EI& e) {
                 return alongY(e) && eq(e.m.Z(), 0) && (eq(e.m.X(), 0) || eq(e.m.X(), 60));
               },
               "U outer");
  for (double y : {20.0, 60.0})
    s = cut(s, cylAx(gp_Pnt(-1, y, 28), DX, 3.5, 62));
  if (v == 2)
    s = cut(s, obroundTool(22, 38, 40, 8, -1, 8));
  else
    s = cut(s, cylZ(30, 40, -1, 4, 8));
  return s;
}

TopoDS_Shape islandPocket(int v)
{
  double       T    = 25;
  TopoDS_Shape s    = box(0, 0, 0, 100, 70, T);
  TopoDS_Shape tool = roundedTool(15, 12, 13, 70, 46, 13, 5);
  if (v == 1)
    tool = cut(tool, cylZ(50, 35, 13, 8, 7));
  else
    tool = cut(tool, box(40, 29, 12, 20, 12, 15));
  s = cut(s, tool);
  if (v == 2)
  {
    s = fillet(s, 2, [](const EI& e) { return onZ(e, 13); }, "island pocket floor");
    s = cut(s, cylZ(50, 35, 17, 3, 9));
  }
  return s;
}

TopoDS_Shape flange(int v)
{
  TopoDS_Shape s = fuse(cylZ(0, 0, 0, 50, 12), cylZ(0, 0, 12, 20, 13));
  s = fillet(s, 3, [](const EI& e) { return !e.line && onZ(e, 12) && eq(dxy(e.m, 0, 0), 20); },
             "hub root");
  s = chamfer(s, 1.0, [](const EI& e) { return !e.line && onZ(e, 12) && eq(dxy(e.m, 0, 0), 50); },
              "flange rim");
  s = cut(s, cylZ(0, 0, -1, 10, 27));
  for (int i = 0; i < 6; ++i)
  {
    double a = (30 + 60 * i) * M_PI / 180.0, x = 35 * std::cos(a), y = 35 * std::sin(a);
    s = cut(s, cylZ(x, y, -1, 4.5, 14));
    if (v == 1)
      s = cut(s, cylZ(x, y, 7, 7.5, 6));
  }
  if (v == 2)
  {
    s = cut(s, box(44, -60, -1, 20, 120, 14));
    s = cut(s, box(-64, -60, -1, 20, 120, 14));
    s = cut(s, box(-2.5, 0, -1, 5, 12.5, 27));
  }
  return s;
}
} // namespace

bool writeStep(const TopoDS_Shape& s, const std::string& path)
{
  STEPControl_Writer w;
  if (w.Transfer(s, STEPControl_AsIs) != IFSelect_RetDone)
    return false;
  return w.Write(path.c_str()) == IFSelect_RetDone;
}

TopoDS_Shape readStepLargestSolid(const std::string& path, int& nSolids)
{
  nSolids = 0;
  STEPControl_Reader r;
  if (r.ReadFile(path.c_str()) != IFSelect_RetDone)
    return TopoDS_Shape();
  r.TransferRoots();
  TopoDS_Shape all = r.OneShape();
  TopoDS_Shape best;
  double       bestV = -1;
  for (TopExp_Explorer ex(all, TopAbs_SOLID); ex.More(); ex.Next())
  {
    ++nSolids;
    double v = std::abs(volumeOf(ex.Current()));
    if (v > bestV)
    {
      bestV = v;
      best  = ex.Current();
    }
  }
  return best;
}

int generateCorpus(const std::string& outDir)
{
  struct Fam
  {
    const char* name;
    TopoDS_Shape (*fn)(int);
  };
  const Fam fams[] = {{"plate_holes", plateHoles},     {"cbore_plate", cborePlate},
                      {"pocket_block", pocketBlock},   {"l_bracket", lBracket},
                      {"filleted_block", filletedBlock}, {"manifold", manifold},
                      {"slot_plate", slotPlate},       {"step_block", stepBlock},
                      {"fixture_plate", fixturePlate}, {"boss_bracket", bossBracket},
                      {"u_channel", uChannel},         {"island_pocket", islandPocket},
                      {"flange", flange}};
  int n = 0, bad = 0;
  for (const Fam& f : fams)
    for (int v = 0; v < 3; ++v)
    {
      ++n;
      char name[128];
      std::snprintf(name, sizeof name, "syn_%02d_%s_%d", n, f.name, v);
      g_warn.clear();
      try
      {
        TopoDS_Shape s = f.fn(v);
        bool         split = false;
        if (v == 1)
        { // mimic exporters that split closed faces (cylinders become two halves)
          ShapeUpgrade_ShapeDivideClosed dc(s);
          dc.SetNbSplitPoints(1);
          if (dc.Perform())
          {
            s     = dc.Result();
            split = true;
          }
        }
        std::string path = outDir + "/" + name + ".stp";
        if (!writeStep(s, path))
          throw Standard_Failure("STEP write failed");
        int          ns = 0;
        TopoDS_Shape back = readStepLargestSolid(path, ns);
        bool         valid = !back.IsNull() && BRepCheck_Analyzer(back).IsValid();
        ShapeMap     fm;
        if (!back.IsNull())
          TopExp::MapShapes(back, TopAbs_FACE, fm);
        std::printf("%s solids=%d faces=%d valid=%d split=%d", name, ns, fm.Extent(), valid ? 1 : 0,
                    split ? 1 : 0);
        for (auto& w : g_warn)
          std::printf(" [WARN %s]", w.c_str());
        std::printf("\n");
        if (!valid || ns != 1)
          ++bad;
      }
      catch (const Standard_Failure& e)
      {
        std::printf("%s FAILED %s\n", name, e.what());
        ++bad;
      }
    }
  std::printf("generated=%d bad=%d\n", n, bad);
  return bad;
}
