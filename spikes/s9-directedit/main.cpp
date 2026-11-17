// S9 spike: direct-edit success rate on history-free solids. Throwaway code.
//   s9 gen <outDir>               write the synthetic corpus as STEP
//   s9 targets <file.stp>         print one JSON line with target counts
//   s9 run <file.stp> <edit> <k>  attempt one edit on the k-th chosen target, print one JSON line
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "common.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepCheck.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <BRepTools.hxx>
#include <BRepTools_History.hxx>
#include <Bnd_Box.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_PrinterOStream.hxx>
#include <ShapeAnalysis_Shell.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <typeinfo>

namespace
{
const double PI = 3.14159265358979323846;

// ------------------------------------------------------------------ model analysis
struct Adj
{
  int  other;
  bool smooth, line, circle;
  double ndot = 2.0; // dot of the two outward normals at the edge midpoint (2 = unknown)
  gp_Pnt mid;
};
struct FaceInfo
{
  TopoDS_Face         f;
  GeomAbs_SurfaceType type = GeomAbs_OtherSurface;
  double              area = 0, radius = 0, minorR = 0;
  gp_Ax1              axis;
  double              u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  gp_Pnt              p;
  gp_Dir              n;
  bool                hasP = false, concave = false;
  std::vector<Adj>    adj;
};
struct Model
{
  TopoDS_Shape                  solid;
  ShapeMap                      faces;
  std::vector<FaceInfo>         fi; // 0-based, face i is faces(i+1)
  double                        vol = 0, diag = 0;
  int                           nSolidsInFile = 0;
  bool                          inputValid = false;
  std::vector<std::vector<int>> holes, holeTargets, stacks, chains, pockets;
  std::vector<int>              pocketTop;
  std::vector<int>              fillets, planars;
};

bool normalAt(const TopoDS_Face& f, double u, double v, gp_Pnt& p, gp_Dir& n)
{
  Handle(Geom_Surface) s = BRep_Tool::Surface(f);
  GeomLProp_SLProps    pr(s, u, v, 1, 1e-9);
  if (!pr.IsNormalDefined())
    return false;
  p = pr.Value();
  n = pr.Normal();
  if (f.Orientation() == TopAbs_REVERSED)
    n.Reverse();
  return true;
}

bool interiorPoint(const TopoDS_Face& f, gp_Pnt& p, gp_Dir& n)
{
  double u0, u1, v0, v1;
  BRepTools::UVBounds(f, u0, u1, v0, v1);
  static const double fr[] = {0.5, 0.3, 0.7, 0.2, 0.8, 0.4, 0.6, 0.1, 0.9};
  for (double a : fr)
    for (double b : fr)
    {
      gp_Pnt2d                uv(u0 + a * (u1 - u0), v0 + b * (v1 - v0));
      BRepClass_FaceClassifier c(f, uv, 1e-7);
      if (c.State() == TopAbs_IN && normalAt(f, uv.X(), uv.Y(), p, n))
        return true;
    }
  return false;
}

gp_Pnt footOnAxis(const gp_Ax1& ax, const gp_Pnt& p)
{
  double t = gp_Vec(ax.Location(), p).Dot(gp_Vec(ax.Direction()));
  return ax.Location().Translated(gp_Vec(ax.Direction()) * t);
}
double axisT(const gp_Ax1& ax, const gp_Pnt& p)
{
  return gp_Vec(ax.Location(), p).Dot(gp_Vec(ax.Direction()));
}
double radial(const gp_Ax1& ax, const gp_Pnt& p) { return footOnAxis(ax, p).Distance(p); }
bool   coaxial(const gp_Ax1& a, const gp_Ax1& b, double tol)
{
  return a.Direction().IsParallel(b.Direction(), 1e-6) && radial(a, b.Location()) < tol;
}

void analyse(Model& m)
{
  TopExp::MapShapes(m.solid, TopAbs_FACE, m.faces);
  const int nf = m.faces.Extent();
  m.fi.resize(nf);
  Bnd_Box bb;
  BRepBndLib::Add(m.solid, bb);
  m.diag       = std::sqrt(bb.SquareExtent());
  m.vol        = volumeOf(m.solid);
  m.inputValid = BRepCheck_Analyzer(m.solid).IsValid();
  const double ltol = 1e-6 * std::max(1.0, m.diag);

  for (int i = 0; i < nf; ++i)
  {
    FaceInfo& x = m.fi[i];
    x.f         = TopoDS::Face(m.faces(i + 1));
    BRepAdaptor_Surface as(x.f);
    x.type = as.GetType();
    GProp_GProps g;
    BRepGProp::SurfaceProperties(x.f, g);
    x.area = g.Mass();
    BRepTools::UVBounds(x.f, x.u0, x.u1, x.v0, x.v1);
    x.hasP = interiorPoint(x.f, x.p, x.n);
    if (x.type == GeomAbs_Cylinder)
    {
      x.axis   = as.Cylinder().Axis();
      x.radius = as.Cylinder().Radius();
    }
    else if (x.type == GeomAbs_Cone)
    {
      x.axis = as.Cone().Axis();
    }
    else if (x.type == GeomAbs_Torus)
    {
      x.axis   = as.Torus().Axis();
      x.radius = as.Torus().MajorRadius();
      x.minorR = as.Torus().MinorRadius();
    }
    else if (x.type == GeomAbs_Sphere)
    {
      x.axis   = gp_Ax1(as.Sphere().Location(), gp_Dir(0, 0, 1));
      x.radius = as.Sphere().Radius();
    }
    if (x.hasP)
    {
      if (x.type == GeomAbs_Cylinder || x.type == GeomAbs_Cone)
      {
        gp_Vec d(footOnAxis(x.axis, x.p), x.p);
        x.concave = d.Dot(gp_Vec(x.n)) < 0;
      }
      else if (x.type == GeomAbs_Torus)
      {
        gp_Pnt foot = footOnAxis(x.axis, x.p);
        gp_Vec rad(foot, x.p);
        // remove the axial part to get the direction to the minor circle centre
        gp_Vec axd(x.axis.Direction());
        gp_Vec q(x.axis.Location(), x.p);
        gp_Vec inPlane = q - axd * q.Dot(axd);
        if (inPlane.Magnitude() > 1e-12)
        {
          gp_Pnt c  = x.axis.Location().Translated(inPlane.Normalized() * x.radius);
          x.concave = gp_Vec(c, x.p).Dot(gp_Vec(x.n)) < 0;
        }
      }
      else if (x.type == GeomAbs_Sphere)
      {
        x.concave = gp_Vec(x.axis.Location(), x.p).Dot(gp_Vec(x.n)) < 0;
      }
    }
  }

  // adjacency with tangency flag
  AncMap e2f;
  TopExp::MapShapesAndAncestors(m.solid, TopAbs_EDGE, TopAbs_FACE, e2f);
  for (int i = 1; i <= e2f.Extent(); ++i)
  {
    TopoDS_Edge e = TopoDS::Edge(e2f.FindKey(i));
    if (BRep_Tool::Degenerated(e))
      continue;
    std::vector<int> fs;
    for (ShapeList::Iterator it(e2f(i)); it.More(); it.Next())
    {
      int k = m.faces.FindIndex(it.Value()) - 1;
      if (k >= 0 && std::find(fs.begin(), fs.end(), k) == fs.end())
        fs.push_back(k);
    }
    if (fs.size() != 2)
      continue;
    BRepAdaptor_Curve c(e);
    double            f, l;
    BRep_Tool::Range(e, f, l);
    double mid    = 0.5 * (f + l);
    bool   smooth = false;
    double ndot   = 2.0;
    try
    {
      double                 a0, a1, b0, b1;
      Handle(Geom2d_Curve) pa = BRep_Tool::CurveOnSurface(e, m.fi[fs[0]].f, a0, a1);
      Handle(Geom2d_Curve) pb = BRep_Tool::CurveOnSurface(e, m.fi[fs[1]].f, b0, b1);
      if (!pa.IsNull() && !pb.IsNull())
      {
        gp_Pnt2d ua = pa->Value(mid), ub = pb->Value(mid);
        gp_Pnt   p1, p2;
        gp_Dir   n1, n2;
        if (normalAt(m.fi[fs[0]].f, ua.X(), ua.Y(), p1, n1)
            && normalAt(m.fi[fs[1]].f, ub.X(), ub.Y(), p2, n2))
        {
          ndot   = n1.Dot(n2);
          smooth = ndot > std::cos(2.0 * PI / 180.0);
        }
      }
    }
    catch (const Standard_Failure&)
    {
    }
    Adj a;
    a.smooth = smooth;
    a.ndot   = ndot;
    a.line   = c.GetType() == GeomAbs_Line;
    a.circle = c.GetType() == GeomAbs_Circle;
    a.mid    = c.Value(mid);
    a.other  = fs[1];
    m.fi[fs[0]].adj.push_back(a);
    a.other = fs[0];
    m.fi[fs[1]].adj.push_back(a);
  }

  // ---- holes: concave cylinder faces on one cylinder that together span 360 degrees
  std::vector<int> holeOf(nf, -1);
  for (int i = 0; i < nf; ++i)
  {
    const FaceInfo& x = m.fi[i];
    if (x.type != GeomAbs_Cylinder || !x.concave || holeOf[i] >= 0)
      continue;
    std::vector<int> grp{i}, stack{i};
    std::set<int>    seen{i};
    while (!stack.empty())
    {
      int c = stack.back();
      stack.pop_back();
      for (const Adj& a : m.fi[c].adj)
      {
        const FaceInfo& y = m.fi[a.other];
        if (seen.count(a.other) || y.type != GeomAbs_Cylinder || !y.concave)
          continue;
        if (std::abs(y.radius - x.radius) > ltol || !coaxial(x.axis, y.axis, ltol))
          continue;
        seen.insert(a.other);
        grp.push_back(a.other);
        stack.push_back(a.other);
      }
    }
    double span = 0;
    for (int k : grp)
      span += m.fi[k].u1 - m.fi[k].u0;
    if (span < 2 * PI - 1e-4)
      continue;
    std::sort(grp.begin(), grp.end());
    for (int k : grp)
      holeOf[k] = (int)m.holes.size();
    m.holes.push_back(grp);
  }
  auto addCaps = [&](std::vector<int>& set) {
    bool grew = true;
    while (grew)
    {
      grew = false;
      std::set<int> in(set.begin(), set.end());
      std::set<int> cand;
      for (int k : set)
        for (const Adj& a : m.fi[k].adj)
          if (!in.count(a.other))
            cand.insert(a.other);
      for (int c : cand)
      {
        bool all = !m.fi[c].adj.empty();
        for (const Adj& a : m.fi[c].adj)
          if (!in.count(a.other))
            all = false;
        if (all)
        {
          set.push_back(c);
          grew = true;
        }
      }
    }
    std::sort(set.begin(), set.end());
  };
  for (const auto& g : m.holes)
  {
    std::vector<int> t = g;
    addCaps(t);
    m.holeTargets.push_back(t);
    // coaxial stack: counterbore, countersink, chamfer and drill point belonging to the same hole
    const gp_Ax1 ax = m.fi[g[0]].axis;
    auto         admissible = [&](int k) {
      const FaceInfo& y = m.fi[k];
      if (y.type == GeomAbs_Cylinder || y.type == GeomAbs_Cone)
        return y.concave && coaxial(ax, y.axis, ltol);
      if (y.type == GeomAbs_Plane)
      {
        if (!y.hasP || !y.n.IsParallel(ax.Direction(), 1e-6) || y.adj.empty())
          return false;
        for (const Adj& a : y.adj)
          if (!a.circle)
            return false;
        return true;
      }
      return false;
    };
    std::set<int>    in(g.begin(), g.end());
    std::vector<int> st(g.begin(), g.end());
    while (!st.empty())
    {
      int c = st.back();
      st.pop_back();
      for (const Adj& a : m.fi[c].adj)
        if (!in.count(a.other) && admissible(a.other))
        {
          in.insert(a.other);
          st.push_back(a.other);
        }
    }
    // drop planes that touch anything outside the set (for example the top of a boss)
    bool changed = true;
    while (changed)
    {
      changed = false;
      for (auto it = in.begin(); it != in.end();)
      {
        bool drop = false;
        if (m.fi[*it].type == GeomAbs_Plane)
          for (const Adj& a : m.fi[*it].adj)
            if (!in.count(a.other))
              drop = true;
        if (drop)
        {
          it      = in.erase(it);
          changed = true;
        }
        else
          ++it;
      }
    }
    // keep what is still connected to the seed
    std::set<int> keep(g.begin(), g.end());
    st.assign(g.begin(), g.end());
    while (!st.empty())
    {
      int c = st.back();
      st.pop_back();
      for (const Adj& a : m.fi[c].adj)
        if (in.count(a.other) && !keep.count(a.other))
        {
          keep.insert(a.other);
          st.push_back(a.other);
        }
    }
    std::vector<int> s(keep.begin(), keep.end());
    addCaps(s);
    if (s != t)
    {
      bool dup = false;
      for (const auto& o : m.stacks)
        if (o == s)
          dup = true;
      if (!dup)
        m.stacks.push_back(s);
    }
  }

  // ---- fillets: cylinder/torus faces tangent to two different neighbours
  std::vector<char> isFillet(nf, 0);
  for (int i = 0; i < nf; ++i)
  {
    const FaceInfo& x = m.fi[i];
    if (!x.hasP)
      continue;
    std::set<int> others;
    if (x.type == GeomAbs_Cylinder)
    {
      if (holeOf[i] >= 0 || (x.u1 - x.u0) > 175.0 * PI / 180.0)
        continue;
      for (const Adj& a : x.adj)
      {
        const FaceInfo& y = m.fi[a.other];
        bool sameCyl = y.type == GeomAbs_Cylinder && std::abs(y.radius - x.radius) < ltol
                       && coaxial(x.axis, y.axis, ltol);
        if (a.smooth && a.line && !sameCyl)
          others.insert(a.other);
      }
    }
    else if (x.type == GeomAbs_Torus)
    {
      if ((x.v1 - x.v0) > 175.0 * PI / 180.0)
        continue;
      for (const Adj& a : x.adj)
      {
        const FaceInfo& y = m.fi[a.other];
        bool sameTor = y.type == GeomAbs_Torus && std::abs(y.radius - x.radius) < ltol
                       && std::abs(y.minorR - x.minorR) < ltol && coaxial(x.axis, y.axis, ltol);
        if (a.smooth && !sameTor)
          others.insert(a.other);
      }
    }
    if (others.size() >= 2)
    {
      isFillet[i] = 1;
      m.fillets.push_back(i);
    }
  }
  // chains: connected fillet faces plus spherical corner patches
  {
    std::vector<char> node = isFillet;
    for (int i = 0; i < nf; ++i)
      if (!isFillet[i] && m.fi[i].type != GeomAbs_Plane && m.fi[i].type != GeomAbs_Cylinder
          && m.fi[i].type != GeomAbs_Cone)
      { // corner patch: sphere, torus or spline face whose every edge is tangent-continuous and
        // which touches at least two fillet faces
        std::set<int> fs;
        bool          allSmooth = !m.fi[i].adj.empty();
        for (const Adj& a : m.fi[i].adj)
        {
          if (!a.smooth)
            allSmooth = false;
          else if (isFillet[a.other])
            fs.insert(a.other);
        }
        if (allSmooth && fs.size() >= 2)
          node[i] = 1;
      }
    std::vector<char> done(nf, 0);
    for (int i = 0; i < nf; ++i)
    {
      if (!node[i] || done[i])
        continue;
      std::vector<int> comp{i}, st{i};
      done[i] = 1;
      while (!st.empty())
      {
        int c = st.back();
        st.pop_back();
        for (const Adj& a : m.fi[c].adj)
          if (node[a.other] && !done[a.other])
          {
            done[a.other] = 1;
            comp.push_back(a.other);
            st.push_back(a.other);
          }
      }
      std::sort(comp.begin(), comp.end());
      if (comp.size() >= 2)
        m.chains.push_back(comp);
    }
  }

  // ---- pockets: closed depression below a planar face, entered through an inner wire
  for (int i = 0; i < nf; ++i)
  {
    const FaceInfo& F = m.fi[i];
    if (F.type != GeomAbs_Plane || !F.hasP)
      continue;
    TopoDS_Wire outer = BRepTools::OuterWire(F.f);
    ShapeMap    outerEdges;
    TopExp::MapShapes(outer, TopAbs_EDGE, outerEdges);
    std::set<int> rim; // faces across the outer wire
    for (int k = 1; k <= outerEdges.Extent(); ++k)
    {
      for (ShapeList::Iterator it(e2f.FindFromKey(outerEdges(k))); it.More(); it.Next())
      {
        int q = m.faces.FindIndex(it.Value()) - 1;
        if (q != i)
          rim.insert(q);
      }
    }
    for (TopExp_Explorer wx(F.f, TopAbs_WIRE); wx.More(); wx.Next())
    {
      if (wx.Current().IsSame(outer))
        continue;
      std::set<int>    comp;
      std::vector<int> st;
      for (TopExp_Explorer ex(wx.Current(), TopAbs_EDGE); ex.More(); ex.Next())
        for (ShapeList::Iterator it(e2f.FindFromKey(ex.Current())); it.More(); it.Next())
        {
          int q = m.faces.FindIndex(it.Value()) - 1;
          if (q != i && comp.insert(q).second)
            st.push_back(q);
        }
      bool escaped = false;
      while (!st.empty() && !escaped)
      {
        int c = st.back();
        st.pop_back();
        if (rim.count(c))
        {
          escaped = true;
          break;
        }
        for (const Adj& a : m.fi[c].adj)
          if (a.other != i && comp.insert(a.other).second)
            st.push_back(a.other);
      }
      if (escaped || comp.empty())
        continue;
      bool   below = true, hasWall = false;
      double tol = 1e-5 * std::max(1.0, m.diag);
      for (int c : comp)
      {
        for (TopExp_Explorer vx(m.fi[c].f, TopAbs_VERTEX); vx.More(); vx.Next())
        {
          gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
          if (gp_Vec(F.p, q).Dot(gp_Vec(F.n)) > tol)
            below = false;
        }
        if (m.fi[c].type == GeomAbs_Plane && m.fi[c].hasP && std::abs(m.fi[c].n.Dot(F.n)) < 0.1)
          hasWall = true;
      }
      if (!below || !hasWall)
        continue;
      std::vector<int> v(comp.begin(), comp.end());
      if (std::find(m.pockets.begin(), m.pockets.end(), v) == m.pockets.end())
      {
        m.pockets.push_back(v);
        m.pocketTop.push_back(i);
      }
    }
  }

  // ---- planar faces, largest first
  for (int i = 0; i < nf; ++i)
    if (m.fi[i].type == GeomAbs_Plane && m.fi[i].hasP)
      m.planars.push_back(i);
  std::stable_sort(m.planars.begin(), m.planars.end(),
                   [&](int a, int b) { return m.fi[a].area > m.fi[b].area; });
}

// choose up to 5 of n, evenly spread over the detection order
int nPick(int n) { return std::min(n, 5); }
int pick(int n, int k) { return n <= 5 ? k : (int)((long long)k * (n - 1) / 4); }

// ------------------------------------------------------------------ edits
struct Out
{
  TopoDS_Shape              res;
  Handle(BRepTools_History) hist;
  ShapeList                 inputs; // solid first, then tool shapes
  ShapeList                 keyToolFaces; // tool faces that become the edited face
  std::string               apiErr, warn;
};

template <class T> std::string dumpWarn(T& o)
{
  std::ostringstream ss;
  if (o.HasWarnings())
    o.DumpWarnings(ss);
  std::string s = ss.str();
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '"' || c == '\\')
      c = ' ';
  return s;
}
template <class T> std::string dumpErr(T& o)
{
  std::ostringstream ss;
  o.DumpErrors(ss);
  std::string s = ss.str();
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '"' || c == '\\')
      c = ' ';
  return s.empty() ? std::string("not done") : s;
}

// Progress indicator that asks the algorithm to stop after a deadline (cancel test).
class CancelAfter : public Message_ProgressIndicator
{
public:
  std::chrono::steady_clock::time_point deadline;
  bool UserBreak() override { return std::chrono::steady_clock::now() > deadline; }
  void Show(const Message_ProgressScope&, const bool) override {}
};

void defeature(const TopoDS_Shape& solid, const Model& m, const std::vector<int>& faces, Out& o)
{
  BRepAlgoAPI_Defeaturing df;
  df.SetShape(solid);
  for (int k : faces)
    df.AddFaceToRemove(m.fi[k].f);
  df.SetRunParallel(false);
  df.SetToFillHistory(true);
  if (const char* cm = std::getenv("S9_CANCEL_MS"))
  {
    Handle(CancelAfter) pi = new CancelAfter;
    pi->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::atoi(cm));
    df.Build(pi->Start());
  }
  else
    df.Build();
  o.inputs.Append(solid);
  if (!df.IsDone() || df.HasErrors())
  {
    o.apiErr = "defeaturing: " + dumpErr(df);
    return;
  }
  o.warn = dumpWarn(df);
  o.res  = df.Shape();
  o.hist = df.History();
}

void boolean(bool isCut, const TopoDS_Shape& a, const TopoDS_Shape& tool, Out& o, bool unify)
{
  ShapeList args, tools;
  args.Append(a);
  tools.Append(tool);
  Handle(BRepTools_History) h;
  TopoDS_Shape              r;
  auto run = [&](auto& op) {
    op.SetArguments(args);
    op.SetTools(tools);
    op.SetRunParallel(false);
    op.SetToFillHistory(true);
    op.Build();
    if (!op.IsDone() || op.HasErrors())
    {
      o.apiErr = "boolean: " + dumpErr(op);
      return;
    }
    o.warn += dumpWarn(op);
    r = op.Shape();
    h = op.History();
  };
  if (isCut)
  {
    BRepAlgoAPI_Cut op;
    run(op);
  }
  else
  {
    BRepAlgoAPI_Fuse op;
    run(op);
  }
  o.inputs.Append(tool);
  if (!o.apiErr.empty())
    return;
  const char* um = std::getenv("S9_UNIFY");
  if (unify && !(um && um[0] == '0'))
  {
    ShapeUpgrade_UnifySameDomain u(r, !(um && um[0] == '2'), true, false);
    if (um && um[0] == '3')
      u.AllowInternalEdges(true);
    u.Build();
    if (!u.History().IsNull())
      h->Merge(u.History());
    r = u.Shape();
  }
  if (o.hist.IsNull())
    o.hist = h;
  else
    o.hist->Merge(h);
  o.res = r;
}

// per-face offset; every other face keeps offset 0
void offsetFaces(const TopoDS_Shape& solid, const Model& m, const std::vector<int>& faces,
                 double off, const std::string& cfg, Out& o)
{
  GeomAbs_JoinType join  = cfg[0] == 'a' ? GeomAbs_Arc : GeomAbs_Intersection;
  bool             inter = cfg[1] == 'c';
  BRepOffset_MakeOffset mo;
  mo.Initialize(solid, 0.0, 1e-6, BRepOffset_Skin, inter, false, join, false, false);
  for (int k : faces)
    mo.SetOffsetOnFace(m.fi[k].f, off);
  mo.MakeOffsetShape();
  o.inputs.Append(solid);
  if (!mo.IsDone())
  {
    o.apiErr = "BRepOffset_MakeOffset error " + std::to_string((int)mo.Error());
    return;
  }
  TopoDS_Shape r = mo.Shape();
  // MakeOffsetShape may return a shell; make it a solid so the checks are uniform
  int nSolid = 0, nShell = 0;
  for (TopExp_Explorer ex(r, TopAbs_SOLID); ex.More(); ex.Next())
    ++nSolid;
  if (nSolid == 0)
  {
    BRepBuilderAPI_MakeSolid ms;
    for (TopExp_Explorer ex(r, TopAbs_SHELL); ex.More(); ex.Next())
    {
      ms.Add(TopoDS::Shell(ex.Current()));
      ++nShell;
    }
    if (nShell > 0 && ms.IsDone())
    {
      r = ms.Solid();
      if (volumeOf(r) < 0)
        r.Reverse();
      o.warn += "offset returned shell, wrapped in solid; ";
    }
  }
  o.res = r;
  ShapeList args;
  args.Append(solid);
  o.hist = new BRepTools_History(args, mo);
}

struct HoleGeom
{
  gp_Ax1 ax;
  double r = 0, tmin = 0, tmax = 0;
  bool   openMin = true, openMax = true;
  gp_Pnt p; // point on the wall
};
HoleGeom holeGeom(const Model& m, const std::vector<int>& grp)
{
  HoleGeom h;
  h.ax   = m.fi[grp[0]].axis;
  h.r    = m.fi[grp[0]].radius;
  h.p    = m.fi[grp[0]].p;
  h.tmin = 1e300;
  h.tmax = -1e300;
  std::set<int> in(grp.begin(), grp.end());
  for (int k : grp)
    for (TopExp_Explorer ex(m.fi[k].f, TopAbs_EDGE); ex.More(); ex.Next())
    {
      if (BRep_Tool::Degenerated(TopoDS::Edge(ex.Current())))
        continue;
      BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
      for (int i = 0; i <= 32; ++i)
      {
        double t = axisT(h.ax, c.Value(c.FirstParameter()
                                       + (c.LastParameter() - c.FirstParameter()) * i / 32.0));
        h.tmin = std::min(h.tmin, t);
        h.tmax = std::max(h.tmax, t);
      }
    }
  double len = h.tmax - h.tmin;
  for (int k : grp)
    for (const Adj& a : m.fi[k].adj)
    {
      if (in.count(a.other) || !m.fi[a.other].hasP)
        continue;
      double t      = axisT(h.ax, a.mid);
      double rmaxN  = 0;
      for (TopExp_Explorer ex(m.fi[a.other].f, TopAbs_EDGE); ex.More(); ex.Next())
      {
        if (BRep_Tool::Degenerated(TopoDS::Edge(ex.Current())))
          continue;
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        for (int i = 0; i <= 16; ++i)
          rmaxN = std::max(rmaxN, radial(h.ax, c.Value(c.FirstParameter()
                                                       + (c.LastParameter() - c.FirstParameter()) * i / 16.0)));
      }
      bool closed = rmaxN < h.r + 1e-5 * std::max(1.0, h.r);
      if (!closed)
        continue;
      if (t - h.tmin < 0.25 * len)
        h.openMin = false;
      else if (h.tmax - t < 0.25 * len)
        h.openMax = false;
    }
  return h;
}
void addCylFaces(const TopoDS_Shape& tool, ShapeList& l)
{
  for (TopExp_Explorer ex(tool, TopAbs_FACE); ex.More(); ex.Next())
    if (BRepAdaptor_Surface(TopoDS::Face(ex.Current())).GetType() == GeomAbs_Cylinder)
      l.Append(ex.Current());
}
TopoDS_Shape holeTool(const HoleGeom& h, double rNew)
{
  double ext = rNew + 0.1; // past open ends, enough to clear a crossing bore of larger radius
  double t0 = h.tmin - (h.openMin ? ext : 0.0), t1 = h.tmax + (h.openMax ? ext : 0.0);
  gp_Pnt base = h.ax.Location().Translated(gp_Vec(h.ax.Direction()) * t0);
  return BRepPrimAPI_MakeCylinder(gp_Ax2(base, h.ax.Direction()), rNew, t1 - t0).Shape();
}

// ------------------------------------------------------------------ attempt state
struct Attempt
{
  std::string      file, edit;
  int              k = 0, nAvail = 0;
  Model            m;
  std::vector<int> targets;
  Out              out;
  // expectations
  int    sign = 0;           // expected sign of volume change, 0 = any non-zero
  double loV = 0, hiV = 0;   // plausible |dV| range
  struct PtCheck
  {
    gp_Pnt       p;
    TopAbs_State want;
  };
  std::vector<PtCheck> checks;
  std::string          note;
  int                  nonPerp = -1, tangent = -1;
  // results
  std::string category = "unknown", detail;
  double      tMs = -1, v1 = 0;
  int         faces1 = 0;
  int         hUntouched = 0, hMapped = 0, hDeletedFlag = 0, hLost = 0, hOrphans = 0;
  int         hTargetsRemoved = 0, hTargetsModified = 0, hMappedGen = 0;
  int         hInconsistent = 0, hKey = 0, hKeyFound = 0;
  bool        hAvail = false;
};
Attempt g;
std::atomic<int> g_phase{0}; // 0 load, 1 op running, 2 op finished

std::string jesc(const std::string& s)
{
  std::string o;
  for (char c : s)
  {
    if (c == '"' || c == '\\')
      o += ' ';
    else if ((unsigned char)c < 32)
      o += ' ';
    else
      o += c;
  }
  if (o.size() > 300)
    o.resize(300);
  return o;
}

void emit()
{
  std::printf(
    "{\"file\":\"%s\",\"edit\":\"%s\",\"k\":%d,\"navail\":%d,\"ntarget_faces\":%d,"
    "\"category\":\"%s\",\"detail\":\"%s\",\"t_ms\":%.2f,\"v0\":%.6g,\"v1\":%.6g,\"dv\":%.6g,"
    "\"sign\":%d,\"lo\":%.6g,\"hi\":%.6g,\"faces0\":%d,\"faces1\":%d,\"input_valid\":%d,"
    "\"hist_avail\":%d,\"h_untouched\":%d,\"h_mapped\":%d,\"h_deleted\":%d,\"h_lost\":%d,"
    "\"h_inconsistent\":%d,\"h_key\":%d,\"h_key_found\":%d,\"nonperp\":%d,\"tangent\":%d,\"h_orphans\":%d,\"h_mapped_gen\":%d,\"h_t_removed\":%d,\"h_t_modified\":%d,\"warn\":\"%s\",\"note\":\"%s\"}\n",
    jesc(g.file).c_str(), g.edit.c_str(), g.k, g.nAvail, (int)g.targets.size(), g.category.c_str(),
    jesc(g.detail).c_str(), g.tMs, g.m.vol, g.v1, g.v1 - g.m.vol, g.sign, g.loV, g.hiV,
    g.m.faces.Extent(), g.faces1, g.m.inputValid ? 1 : 0, g.hAvail ? 1 : 0, g.hUntouched, g.hMapped,
    g.hDeletedFlag, g.hLost, g.hInconsistent, g.hKey, g.hKeyFound, g.nonPerp, g.tangent, g.hOrphans, g.hMappedGen, g.hTargetsRemoved, g.hTargetsModified,
    jesc(g.out.warn).c_str(), jesc(g.note).c_str());
  std::fflush(stdout);
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool endsWith(const std::string& s, const char* p)
{
  std::string q(p);
  return s.size() >= q.size() && s.compare(s.size() - q.size(), q.size(), q) == 0;
}

// Selects targets and sets expectations. Returns false when the k-th target does not exist.
bool prepare()
{
  Model&             m = g.m;
  const std::string& e = g.edit;
  auto bboxVol = [&](const std::vector<int>& fs) {
    Bnd_Box b;
    for (int k : fs)
      BRepBndLib::AddOptimal(m.fi[k].f, b, false, false);
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    return (x1 - x0) * (y1 - y0) * (z1 - z0);
  };
  if (e == "del_fillet" || e == "del_fillet_chain")
  {
    if (e == "del_fillet")
    {
      g.nAvail = (int)m.fillets.size();
      if (g.k >= nPick(g.nAvail))
        return false;
      g.targets = {m.fillets[pick(g.nAvail, g.k)]};
      // is this face one piece of a longer blend (touches another blend face)?
      bool inChain = false;
      for (const auto& ch : m.chains)
        if (std::find(ch.begin(), ch.end(), g.targets[0]) != ch.end())
          inChain = true;
      const FaceInfo& x = m.fi[g.targets[0]];
      g.note = std::string(inChain ? "chain_member" : "isolated") + (x.concave ? " concave" : " convex")
               + (x.type == GeomAbs_Torus ? " torus" : " cylinder");
    }
    else
    {
      g.nAvail = (int)m.chains.size();
      if (g.k >= nPick(g.nAvail))
        return false;
      g.targets = m.chains[pick(g.nAvail, g.k)];
    }
    double S = 0;
    int    nConvex = 0, nConcave = 0;
    for (int k : g.targets)
    {
      const FaceInfo& x = m.fi[k];
      double          r = x.type == GeomAbs_Torus ? x.minorR : x.radius;
      bool analytic = x.type == GeomAbs_Cylinder || x.type == GeomAbs_Torus || x.type == GeomAbs_Sphere;
      if (!analytic)
      { // spline corner patch: borrow radius and convexity from the first fillet in the chain
        const FaceInfo& y = m.fi[m.fillets.empty() ? k : g.targets[0]];
        r = std::max(y.radius, y.minorR);
        S += x.area * r;
        continue;
      }
      S += x.area * r;
      (x.concave ? nConcave : nConvex)++;
      // convex blend removed -> material added -> old surface point ends up inside
      g.checks.push_back({x.p, x.concave ? TopAbs_OUT : TopAbs_IN});
    }
    g.sign = nConcave == 0 ? 1 : (nConvex == 0 ? -1 : 0);
    g.loV  = 1e-4 * S;
    g.hiV  = 1.3 * S;
    return true;
  }
  if (e == "del_hole" || e == "del_hole_stack")
  {
    const auto& lists = e == "del_hole" ? m.holeTargets : m.stacks;
    g.nAvail          = (int)lists.size();
    if (g.k >= nPick(g.nAvail))
      return false;
    g.targets = lists[pick(g.nAvail, g.k)];
    if (e == "del_hole")
    { // does this bore belong to a counterbore / countersink / chamfer stack?
      bool inStack = false;
      for (const auto& st : m.stacks)
        if (std::includes(st.begin(), st.end(), g.targets.begin(), g.targets.end()))
          inStack = true;
      g.note = inStack ? "in_stack" : "plain";
    }
    gp_Ax1 ax;
    double rmax = 0, rext = 0, tmin = 1e300, tmax = -1e300;
    for (int k : g.targets)
      if (m.fi[k].type == GeomAbs_Cylinder && m.fi[k].radius > rmax)
      {
        rmax = m.fi[k].radius;
        ax   = m.fi[k].axis;
      }
    for (int k : g.targets)
    {
      for (TopExp_Explorer vx(m.fi[k].f, TopAbs_VERTEX); vx.More(); vx.Next())
      {
        gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(vx.Current()));
        double t = axisT(ax, q);
        tmin     = std::min(tmin, t);
        tmax     = std::max(tmax, t);
        rext     = std::max(rext, radial(ax, q));
      }
      if (m.fi[k].hasP)
        g.checks.push_back({m.fi[k].p, TopAbs_IN});
    }
    rmax       = std::max(rmax, rext); // countersinks and chamfers are wider than the bore
    double ref = PI * rmax * rmax * (tmax - tmin);
    g.sign     = 1;
    g.loV      = (e == "del_hole" ? 0.3 : 0.1) * ref;
    g.hiV      = 1.05 * ref;
    return true;
  }
  if (e == "del_pocket")
  {
    g.nAvail = (int)m.pockets.size();
    if (g.k >= nPick(g.nAvail))
      return false;
    g.targets = m.pockets[pick(g.nAvail, g.k)];
    double B  = bboxVol(g.targets);
    g.sign    = 1;
    g.loV     = 0.15 * B;
    g.hiV     = 1.05 * B;
    const FaceInfo& top = m.fi[m.pocketTop[pick(g.nAvail, g.k)]];
    for (int k : g.targets)
      if (m.fi[k].hasP)
      { // an island top that is level with the surrounding face ends up on the boundary
        bool level =
          std::abs(gp_Vec(top.p, m.fi[k].p).Dot(gp_Vec(top.n))) < 1e-5 * std::max(1.0, m.diag);
        g.checks.push_back({m.fi[k].p, level ? TopAbs_ON : TopAbs_IN});
      }
    return true;
  }
  if (startsWith(e, "rs_"))
  {
    g.nAvail = (int)m.holes.size();
    if (g.k >= nPick(g.nAvail))
      return false;
    g.targets  = m.holes[pick(g.nAvail, g.k)];
    HoleGeom h = holeGeom(m, g.targets);
    bool     grow = endsWith(e, "grow");
    double   rNew = h.r * (grow ? 1.25 : 0.8);
    double   ref  = PI * std::abs(rNew * rNew - h.r * h.r) * (h.tmax - h.tmin);
    g.sign        = grow ? -1 : 1;
    g.loV         = 0.3 * ref;
    g.hiV         = 1.7 * ref;
    gp_Pnt foot   = footOnAxis(h.ax, h.p);
    gp_Vec rad    = gp_Vec(foot, h.p).Normalized();
    g.checks.push_back({h.p, grow ? TopAbs_OUT : TopAbs_IN});
    g.checks.push_back({foot.Translated(rad * rNew), TopAbs_ON});
    return true;
  }
  if (startsWith(e, "pp_"))
  {
    g.nAvail = (int)m.planars.size();
    if (g.k >= nPick(g.nAvail))
      return false;
    int             fidx = m.planars[pick(g.nAvail, g.k)];
    const FaceInfo& x    = m.fi[fidx];
    g.targets            = {fidx};
    bool   outw          = endsWith(e, "out");
    double d             = std::clamp(0.01 * m.diag, 0.25, 2.0);
    double ref           = x.area * d;
    g.sign               = outw ? 1 : -1;
    g.loV                = 0.5 * ref;
    g.hiV                = 1.6 * ref;
    gp_Vec n(x.n);
    g.checks.push_back({x.p, outw ? TopAbs_IN : TopAbs_OUT});
    g.checks.push_back({x.p.Translated(n * (outw ? d : -d)), TopAbs_ON});
    int nonPerp = 0, tangent = 0;
    for (const Adj& a : x.adj)
    {
      if (a.smooth)
        ++tangent;
      else if (std::abs(a.ndot) > 0.0175)
        ++nonPerp;
    }
    g.nonPerp = nonPerp;
    g.tangent = tangent;
    return true;
  }
  return false;
}

void doOp()
{
  Model&             m = g.m;
  const std::string& e = g.edit;
  if (startsWith(e, "del_"))
  {
    defeature(m.solid, m, g.targets, g.out);
    return;
  }
  if (startsWith(e, "rs_"))
  {
    HoleGeom h    = holeGeom(m, g.targets);
    bool     grow = endsWith(e, "grow");
    double   rNew = h.r * (grow ? 1.25 : 0.8);
    if (std::getenv("S9_DEBUG"))
      std::fprintf(stderr, "hole r=%.4f ax=(%.3f %.3f %.3f)+(%.3f %.3f %.3f) t=[%.4f,%.4f] open=%d,%d p=(%.4f %.4f %.4f)\n",
                   h.r, h.ax.Location().X(), h.ax.Location().Y(), h.ax.Location().Z(),
                   h.ax.Direction().X(), h.ax.Direction().Y(), h.ax.Direction().Z(), h.tmin, h.tmax,
                   (int)h.openMin, (int)h.openMax, h.p.X(), h.p.Y(), h.p.Z());
    if (startsWith(e, "rs_cut_"))
    {
      g.out.inputs.Append(m.solid);
      TopoDS_Shape tool = holeTool(h, rNew);
      addCylFaces(tool, g.out.keyToolFaces);
      boolean(true, m.solid, tool, g.out, false);
    }
    else if (startsWith(e, "rs_plug_"))
    {
      // shrink without defeaturing: fuse a sleeve (old radius outside, new radius inside) into the
      // bore. Only for bores whose ends are plain circles, so the sleeve ends flush.
      std::set<int> in(g.targets.begin(), g.targets.end());
      bool          flush = true;
      for (int k : g.targets)
        for (const Adj& a : m.fi[k].adj)
          if (!in.count(a.other) && !a.circle)
            flush = false;
      g.out.inputs.Append(m.solid);
      if (!flush)
      {
        g.out.apiErr = "not applicable: bore does not end in plain circles";
        return;
      }
      gp_Pnt       base = h.ax.Location().Translated(gp_Vec(h.ax.Direction()) * h.tmin);
      gp_Ax2       ax2(base, h.ax.Direction());
      double       len    = h.tmax - h.tmin;
      TopoDS_Shape sleeve = BRepAlgoAPI_Cut(BRepPrimAPI_MakeCylinder(ax2, h.r, len).Shape(),
                                            BRepPrimAPI_MakeCylinder(ax2, rNew, len).Shape())
                              .Shape();
      for (TopExp_Explorer ex(sleeve, TopAbs_FACE); ex.More(); ex.Next())
      {
        BRepAdaptor_Surface as(TopoDS::Face(ex.Current()));
        if (as.GetType() == GeomAbs_Cylinder && std::abs(as.Cylinder().Radius() - rNew) < 1e-9)
          g.out.keyToolFaces.Append(ex.Current());
      }
      boolean(false, m.solid, sleeve, g.out, false);
    }
    else if (startsWith(e, "rs_defeat_"))
    {
      // remove the hole (wall and blind bottom), then cut it again at the new radius
      std::vector<int> t = g.targets;
      for (const auto& ht : m.holeTargets)
        if (std::includes(ht.begin(), ht.end(), g.targets.begin(), g.targets.end()))
          t = ht;
      defeature(m.solid, m, t, g.out);
      if (!g.out.apiErr.empty())
        return;
      if (std::abs(volumeOf(g.out.res) - m.vol) < 1e-9 * std::abs(m.vol))
      {
        g.out.apiErr = "defeaturing step left the hole in place: " + g.out.warn;
        return;
      }
      TopoDS_Shape mid  = g.out.res;
      TopoDS_Shape tool = holeTool(h, rNew);
      addCylFaces(tool, g.out.keyToolFaces);
      boolean(true, mid, tool, g.out, false);
    }
    else
    {
      std::string cfg = e.substr(7, 2); // rs_off_XX_...
      offsetFaces(m.solid, m, g.targets, h.r - rNew, cfg, g.out);
    }
    return;
  }
  if (startsWith(e, "pp_"))
  {
    const FaceInfo& x    = m.fi[g.targets[0]];
    bool            outw = endsWith(e, "out");
    double          d    = std::clamp(0.01 * m.diag, 0.25, 2.0);
    if (startsWith(e, "pp_prism"))
    {
      gp_Vec                v = gp_Vec(x.n) * (outw ? d : -d);
      BRepPrimAPI_MakePrism pm(x.f, v);
      g.out.inputs.Append(m.solid);
      for (TopExp_Explorer ex(pm.LastShape(), TopAbs_FACE); ex.More(); ex.Next())
        g.out.keyToolFaces.Append(ex.Current());
      boolean(!outw, m.solid, pm.Shape(), g.out, startsWith(e, "pp_prismu_"));
    }
    else
    {
      std::string cfg = e.substr(7, 2); // pp_off_XX_...
      offsetFaces(m.solid, m, g.targets, outw ? d : -d, cfg, g.out);
    }
  }
}

// True when the two faces lie on the same analytic surface (same plane, same cylinder, ...).
bool sameSurface(const TopoDS_Face& a, const TopoDS_Face& b, double tol)
{
  BRepAdaptor_Surface sa(a), sb(b);
  if (sa.GetType() != sb.GetType())
    return false;
  switch (sa.GetType())
  {
    case GeomAbs_Plane: {
      gp_Pln pa = sa.Plane(), pb = sb.Plane();
      return pa.Axis().Direction().IsParallel(pb.Axis().Direction(), 1e-6)
             && pa.Distance(pb.Location()) < tol;
    }
    case GeomAbs_Cylinder:
      return std::abs(sa.Cylinder().Radius() - sb.Cylinder().Radius()) < tol
             && coaxial(sa.Cylinder().Axis(), sb.Cylinder().Axis(), tol);
    case GeomAbs_Cone:
      return coaxial(sa.Cone().Axis(), sb.Cone().Axis(), tol)
             && std::abs(sa.Cone().SemiAngle() - sb.Cone().SemiAngle()) < 1e-6;
    case GeomAbs_Sphere:
      return sa.Sphere().Location().Distance(sb.Sphere().Location()) < tol
             && std::abs(sa.Sphere().Radius() - sb.Sphere().Radius()) < tol;
    case GeomAbs_Torus:
      return coaxial(sa.Torus().Axis(), sb.Torus().Axis(), tol)
             && std::abs(sa.Torus().MajorRadius() - sb.Torus().MajorRadius()) < tol
             && std::abs(sa.Torus().MinorRadius() - sb.Torus().MinorRadius()) < tol;
    default:
      return true; // spline surfaces: type match only
  }
}

void evalHistory()
{
  if (g.out.hist.IsNull())
    return;
  g.hAvail = true;
  Handle(BRepTools_History) h = g.out.hist;
  ShapeMap                  resFaces;
  TopExp::MapShapes(g.out.res, TopAbs_FACE, resFaces);
  std::set<int> tset(g.targets.begin(), g.targets.end());
  std::set<int> accounted;
  auto markImages = [&](const TopoDS_Shape& s) {
    if (s.ShapeType() == TopAbs_FACE)
    {
      int k = resFaces.FindIndex(s);
      if (k > 0)
        accounted.insert(k);
    }
    for (ShapeList::Iterator it(h->Modified(s)); it.More(); it.Next())
    {
      int k = resFaces.FindIndex(it.Value());
      if (k > 0)
        accounted.insert(k);
    }
    for (ShapeList::Iterator it(h->Generated(s)); it.More(); it.Next())
    {
      int k = resFaces.FindIndex(it.Value());
      if (k > 0)
        accounted.insert(k);
    }
  };
  for (ShapeList::Iterator in(g.out.inputs); in.More(); in.Next())
    for (TopAbs_ShapeEnum t : {TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX})
    {
      ShapeMap sm;
      TopExp::MapShapes(in.Value(), t, sm);
      for (int i = 1; i <= sm.Extent(); ++i)
        markImages(sm(i));
    }
  g.hOrphans = resFaces.Extent() - (int)accounted.size();
  for (ShapeList::Iterator kt(g.out.keyToolFaces); kt.More(); kt.Next())
  { // can the edited face be found again as the image of the tool face that replaced it?
    ++g.hKey;
    bool found = resFaces.Contains(kt.Value());
    for (ShapeList::Iterator it(h->Modified(kt.Value())); it.More(); it.Next())
      if (resFaces.Contains(it.Value()))
        found = true;
    if (found)
      ++g.hKeyFound;
  }
  for (int i = 0; i < (int)g.m.fi.size(); ++i)
  {
    const TopoDS_Face& f       = g.m.fi[i].f;
    bool               present = resFaces.Contains(f);
    int                nMod = 0, nModIn = 0;
    bool               consistent = true;
    const double       stol = 1e-5 * std::max(1.0, g.m.diag);
    for (ShapeList::Iterator it(h->Modified(f)); it.More(); it.Next())
    {
      ++nMod;
      if (resFaces.Contains(it.Value()))
        ++nModIn;
      if (it.Value().ShapeType() != TopAbs_FACE || !sameSurface(f, TopoDS::Face(it.Value()), stol))
        consistent = false;
    }
    bool removed = h->IsRemoved(f);
    int  nGenIn  = 0;
    for (ShapeList::Iterator it(h->Generated(f)); it.More(); it.Next())
      if (it.Value().ShapeType() == TopAbs_FACE && resFaces.Contains(it.Value()))
      {
        ++nGenIn;
        if (nMod == 0 && !present && !sameSurface(f, TopoDS::Face(it.Value()), stol))
          consistent = false;
      }
    if (tset.count(i))
    {
      if (removed)
        ++g.hTargetsRemoved;
      else if (nMod > 0)
        ++g.hTargetsModified;
      continue;
    }
    ++g.hUntouched;
    if (!consistent)
      ++g.hInconsistent; // history maps an untouched face to a face on a different surface
    if (present || (nMod > 0 && nModIn == nMod))
      ++g.hMapped;
    else if (nGenIn > 0)
      ++g.hMappedGen; // image known only through Generated(), not Modified()
    else if (removed)
      ++g.hDeletedFlag;
    else
      ++g.hLost;
  }
}

void verify()
{
  if (!g.out.apiErr.empty())
  {
    g.category = "api_error";
    g.detail   = g.out.apiErr;
    return;
  }
  const TopoDS_Shape& r = g.out.res;
  if (r.IsNull())
  {
    g.category = "api_error";
    g.detail   = "null result";
    return;
  }
  ShapeMap rf;
  TopExp::MapShapes(r, TopAbs_FACE, rf);
  g.faces1   = rf.Extent();
  int nSolid = 0;
  TopoDS_Shape solid;
  for (TopExp_Explorer ex(r, TopAbs_SOLID); ex.More(); ex.Next())
  {
    ++nSolid;
    solid = ex.Current();
  }
  evalHistory();
  if (nSolid != 1)
  {
    g.category = "invalid";
    g.detail   = "result has " + std::to_string(nSolid) + " solids";
    return;
  }
  g.v1 = volumeOf(r);
  BRepCheck_Analyzer ana(r);
  if (!ana.IsValid())
  {
    g.category = "invalid";
    g.detail   = "BRepCheck_Analyzer reports errors:";
    std::set<std::string> seen;
    for (TopAbs_ShapeEnum t :
         {TopAbs_SOLID, TopAbs_SHELL, TopAbs_FACE, TopAbs_WIRE, TopAbs_EDGE, TopAbs_VERTEX})
    {
      ShapeMap sm;
      TopExp::MapShapes(r, t, sm);
      for (int i = 1; i <= sm.Extent(); ++i)
      {
        Handle(BRepCheck_Result) res = ana.Result(sm(i));
        if (res.IsNull())
          continue;
        auto add = [&](const NCollection_List<BRepCheck_Status>& l) {
          for (NCollection_List<BRepCheck_Status>::Iterator it(l); it.More(); it.Next())
            if (it.Value() != BRepCheck_NoError)
            {
              std::ostringstream ss;
              BRepCheck::Print(it.Value(), ss);
              std::string x = ss.str();
              while (!x.empty() && (unsigned char)x.back() < 32)
                x.pop_back();
              seen.insert(x);
            }
        };
        add(res->Status());
        for (res->InitContextIterator(); res->MoreShapeInContext(); res->NextShapeInContext())
          add(res->StatusOnShape());
      }
    }
    for (const auto& x : seen)
      g.detail += " " + x;
    return;
  }
  ShapeAnalysis_Shell sas;
  sas.LoadShells(r);
  sas.CheckOrientedShells(r, true);
  if (sas.HasFreeEdges() || sas.HasBadEdges())
  {
    g.category = "invalid";
    g.detail   = sas.HasFreeEdges() ? "free edges (not closed)" : "bad edge orientation";
    return;
  }
  if (!(g.v1 > 0))
  {
    g.category = "invalid";
    g.detail   = "non-positive volume";
    return;
  }
  double dv = g.v1 - g.m.vol;
  if (std::abs(dv) < 1e-9 * std::abs(g.m.vol))
  {
    g.category = "no_op";
    g.detail   = "volume unchanged; " + g.out.warn;
    return;
  }
  if (g.sign != 0 && dv * g.sign < 0)
  {
    g.category = "wrong";
    g.detail   = "volume changed in the wrong direction";
    return;
  }
  if (std::abs(dv) < g.loV || std::abs(dv) > g.hiV)
  {
    g.category = "wrong";
    g.detail   = "volume change outside plausible range";
    return;
  }
  int          bad = 0;
  std::string  which;
  TopoDS_Shape boundary = TopExp_Explorer(solid, TopAbs_SHELL).Current();
  auto        nm = [](TopAbs_State st) {
    return st == TopAbs_IN ? "IN" : st == TopAbs_OUT ? "OUT" : st == TopAbs_ON ? "ON" : "?";
  };
  for (const auto& c : g.checks)
  {
    // distance to the boundary shell (against a solid the distance is 0 for interior points)
    BRepExtrema_DistShapeShape  ds(BRepBuilderAPI_MakeVertex(c.p).Vertex(), boundary);
    BRepClass3d_SolidClassifier cl(solid, c.p, 1e-5);
    TopAbs_State                got = cl.State();
    if (ds.IsDone() && ds.NbSolution() > 0)
    { // BRepClass3d_SolidClassifier was seen to answer IN for a point lying on a cylinder face
      if (ds.Value() < 1e-5)
        got = TopAbs_ON;
      else if (got == TopAbs_ON)
        got = TopAbs_UNKNOWN;
    }
    if (got != c.want)
    {
      ++bad;
      if (std::getenv("S9_DEBUG"))
      {
        std::fprintf(stderr, "check p=(%.4f %.4f %.4f) want %s got %s dist=%.6g\n", c.p.X(), c.p.Y(),
                     c.p.Z(), nm(c.want), nm(cl.State()), ds.IsDone() ? ds.Value() : -1.0);
      }
      if (which.size() < 60)
        which += std::string(" want ") + nm(c.want) + " got " + nm(got) + ";";
    }
  }
  if (bad)
  {
    g.category = "wrong";
    g.detail = std::to_string(bad) + " of " + std::to_string(g.checks.size())
               + " target point checks failed (target not removed or not moved as intended):" + which;
    return;
  }
  g.category = "ok";
}

void body(void*)
{
  try
  {
    auto t0 = std::chrono::steady_clock::now();
    g_phase = 1;
    try
    {
      doOp();
    }
    catch (...)
    {
      g.tMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      g_phase = 2;
      throw;
    }
    g.tMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_phase = 2;
    verify();
  }
  catch (const Standard_Failure& e)
  {
    g.category = g_phase == 2 && g.category == "unknown" && !g.out.res.IsNull() ? "invalid" : "exception";
    g.detail = std::string("Standard_Failure ") + typeid(e).name() + ": " + e.what();
  }
  catch (const std::exception& e)
  {
    g.category = "exception";
    g.detail   = std::string("std::exception: ") + e.what();
  }
  catch (...)
  {
    g.category = "exception";
    g.detail   = "unknown C++ exception";
  }
}

// Last resort for faults that are not raised on the guarded call stack (another thread, or a
// fault while unwinding): report where it happened, then die.
DWORD g_mainThread = 0;
LONG WINAPI lastResort(EXCEPTION_POINTERS* ep)
{
  char    mod[MAX_PATH] = "?";
  HMODULE hm            = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &hm))
    GetModuleFileNameA(hm, mod, MAX_PATH);
  const char* base = std::strrchr(mod, '\\');
  std::printf("{\"file\":\"%s\",\"edit\":\"%s\",\"k\":%d,\"navail\":%d,\"category\":\"hard_crash\","
              "\"detail\":\"unhandled structured exception 0x%08lX in %s, %s thread, phase %d\","
              "\"t_ms\":-1,\"faces0\":%d,\"input_valid\":%d}\n",
              g.file.c_str(), g.edit.c_str(), g.k, g.nAvail, ep->ExceptionRecord->ExceptionCode,
              base ? base + 1 : mod, GetCurrentThreadId() == g_mainThread ? "main" : "other",
              g_phase.load(), g.m.faces.Extent(), g.m.inputValid ? 1 : 0);
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
  return EXCEPTION_EXECUTE_HANDLER;
}

// Structured-exception guard: access violations and stack overflows end up here.
int sehGuard(void (*fn)(void*), unsigned long* code)
{
  __try
  {
    fn(nullptr);
    return 0;
  }
  __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
  {
    return 1;
  }
}

void printList(const char* name, const std::vector<std::vector<int>>& v)
{
  std::printf(",\"%s\":%d", name, (int)v.size());
}
} // namespace

int main(int argc, char** argv)
{
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  g_mainThread = GetCurrentThreadId();
  SetUnhandledExceptionFilter(lastResort);
  Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
  if (argc >= 3 && std::string(argv[1]) == "gen")
    return generateCorpus(argv[2]);
  if (argc < 3)
  {
    std::fprintf(stderr, "usage: s9 gen <dir> | targets <stp> | run <stp> <edit> <k>\n");
    return 2;
  }
  std::string mode = argv[1];
  g.file           = argv[2];
  {
    size_t p = g.file.find_last_of("/\\");
    std::string full = g.file;
    if (p != std::string::npos)
      g.file = g.file.substr(p + 1);
    try
    {
      g.m.solid = readStepLargestSolid(full, g.m.nSolidsInFile);
      if (g.m.solid.IsNull())
      {
        std::printf("{\"file\":\"%s\",\"error\":\"no solid\"}\n", g.file.c_str());
        return 3;
      }
      analyse(g.m);
    }
    catch (const Standard_Failure& e)
    {
      std::printf("{\"file\":\"%s\",\"error\":\"load/analyse threw %s\"}\n", g.file.c_str(),
                  jesc(e.what()).c_str());
      return 3;
    }
  }
  if (mode == "targets")
  {
    const Model& m = g.m;
    std::map<int, int> types;
    for (const auto& x : m.fi)
      types[(int)x.type]++;
    std::printf("{\"file\":\"%s\",\"solids_in_file\":%d,\"faces\":%d,\"volume\":%.6g,\"diag\":%.6g,"
                "\"input_valid\":%d,\"fillets\":%d,\"planars\":%d,\"plane_faces\":%d,"
                "\"cyl_faces\":%d,\"cone_faces\":%d,\"sphere_faces\":%d,\"torus_faces\":%d,"
                "\"other_faces\":%d",
                g.file.c_str(), m.nSolidsInFile, m.faces.Extent(), m.vol, m.diag,
                m.inputValid ? 1 : 0, (int)m.fillets.size(), (int)m.planars.size(),
                types[GeomAbs_Plane], types[GeomAbs_Cylinder], types[GeomAbs_Cone],
                types[GeomAbs_Sphere], types[GeomAbs_Torus],
                m.faces.Extent() - types[GeomAbs_Plane] - types[GeomAbs_Cylinder]
                  - types[GeomAbs_Cone] - types[GeomAbs_Sphere] - types[GeomAbs_Torus]);
    printList("holes", m.holes);
    printList("stacks", m.stacks);
    printList("chains", m.chains);
    printList("pockets", m.pockets);
    std::printf("}\n");
    return 0;
  }
  if (mode == "crashtest")
  { // proves that the guard used for every attempt contains an access violation
    unsigned long code = 0;
    int caught = sehGuard([](void*) { volatile int* p = nullptr; *p = 1; }, &code);
    std::printf("{\"crashtest\":\"access violation %s\",\"code\":\"0x%08lX\"}\n",
                caught ? "contained by __except" : "not raised", code);
    return 0;
  }
  if (mode != "run" || argc < 5)
    return 2;
  g.edit = argv[3];
  g.k    = std::atoi(argv[4]);
  if (!prepare())
  {
    g.category = "no_target";
    emit();
    return 0;
  }
  // watchdog: an attempt whose operation runs longer than 10 s is a timeout
  std::thread([] {
    auto t0 = std::chrono::steady_clock::now();
    while (g_phase != 1)
      Sleep(1);
    t0 = std::chrono::steady_clock::now();
    while (g_phase == 1)
    {
      Sleep(20);
      double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (g_phase == 1 && ms > (std::getenv("S9_TIMEOUT_MS") ? std::atof(std::getenv("S9_TIMEOUT_MS")) : 10000.0))
      {
        std::printf("{\"file\":\"%s\",\"edit\":\"%s\",\"k\":%d,\"navail\":%d,\"category\":\"timeout\","
                    "\"detail\":\"operation exceeded 10 s\",\"t_ms\":%.1f,\"faces0\":%d,"
                    "\"input_valid\":%d}\n",
                    jesc(g.file).c_str(), g.edit.c_str(), g.k, g.nAvail, ms, g.m.faces.Extent(),
                    g.m.inputValid ? 1 : 0);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 0);
      }
    }
  }).detach();
  if (const char* hp = std::getenv("S9_HEAP"))
  { // experiment: dirty the heap so that reads of uninitialised memory show up as changed results
    int               fill = std::atoi(hp);
    std::vector<void*> blocks;
    for (int i = 0; i < 200000; ++i)
    {
      size_t sz = 16 + (i * 37) % 2048;
      void*  b  = std::malloc(sz);
      std::memset(b, fill, sz);
      blocks.push_back(b);
    }
    for (void* b : blocks)
      std::free(b);
  }
  unsigned long code = 0;
  if (sehGuard(body, &code))
  {
    char buf[96];
    std::snprintf(buf, sizeof buf, "structured exception 0x%08lX caught by __except in-process", code);
    g.category = "crash";
    g.detail   = buf;
  }
  emit();
  // skip static destructors: after a contained fault the heap may be damaged
  TerminateProcess(GetCurrentProcess(), 0);
  return 0;
}
