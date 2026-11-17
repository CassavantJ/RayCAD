// Minimal kernel layer over OCCT 8.0.1: operations that return a result plus a normalised history
// (the shape of kernel.h ShapeHistory), built only from Modified()/Generated()/IsDeleted().
#include "s4.h"

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
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <chrono>

namespace s4 {

NameId hashStr(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h ? h : 1;
}

// ---------------------------------------------------------------- ShapeIdx
std::shared_ptr<ShapeIdx> indexShape(const TopoDS_Shape& s) {
    auto p = std::make_shared<ShapeIdx>();
    p->shape = s;
    TopExp::MapShapes(s, TopAbs_FACE, p->m[KFace]);
    TopExp::MapShapes(s, TopAbs_EDGE, p->m[KEdge]);
    TopExp::MapShapes(s, TopAbs_VERTEX, p->m[KVertex]);
    TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, p->EF);
    TopExp::MapShapesAndAncestors(s, TopAbs_VERTEX, TopAbs_FACE, p->VF);
    TopExp::MapShapesAndAncestors(s, TopAbs_VERTEX, TopAbs_EDGE, p->VE);
    for (int k = 0; k < 3; ++k) p->fp[k].assign(p->m[k].Extent(), std::nullopt);
    return p;
}
SubId ShapeIdx::find(const TopoDS_Shape& s) const {
    if (s.IsNull()) return {};
    switch (s.ShapeType()) {
    case TopAbs_FACE: return {KFace, m[KFace].FindIndex(s)};
    case TopAbs_EDGE: return {KEdge, m[KEdge].FindIndex(s)};
    case TopAbs_VERTEX: return {KVertex, m[KVertex].FindIndex(s)};
    default: return {};
    }
}
static std::vector<int> uniq(std::vector<int> v) {
    std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end());
    v.erase(std::remove(v.begin(), v.end(), 0), v.end());
    return v;
}
std::vector<int> ShapeIdx::facesOfEdge(int e) const {
    std::vector<int> r;
    for (const auto& f : EF.FindFromKey(m[KEdge](e))) r.push_back(m[KFace].FindIndex(f));
    return uniq(r);
}
std::vector<int> ShapeIdx::facesOfVertex(int v) const {
    std::vector<int> r;
    for (const auto& f : VF.FindFromKey(m[KVertex](v))) r.push_back(m[KFace].FindIndex(f));
    return uniq(r);
}
std::vector<int> ShapeIdx::edgesOfVertex(int v) const {
    std::vector<int> r;
    for (const auto& f : VE.FindFromKey(m[KVertex](v))) r.push_back(m[KEdge].FindIndex(f));
    return uniq(r);
}
std::vector<int> ShapeIdx::edgesOfFace(int f) const {
    std::vector<int> r;
    for (TopExp_Explorer ex(m[KFace](f), TopAbs_EDGE); ex.More(); ex.Next()) r.push_back(m[KEdge].FindIndex(ex.Current()));
    return uniq(r);
}

std::vector<int> ShapeIdx::verticesOfEdge(int e) const {
    std::vector<int> r;
    for (TopExp_Explorer ex(m[KEdge](e), TopAbs_VERTEX); ex.More(); ex.Next()) r.push_back(m[KVertex].FindIndex(ex.Current()));
    return uniq(r);
}
std::vector<int> ShapeIdx::around(Kind k, int i) const {
    if (k == KEdge) return facesOfEdge(i);
    if (k == KVertex) return facesOfVertex(i);
    std::vector<int> nb;
    for (int e : edgesOfFace(i)) for (int x : facesOfEdge(e)) if (x != i) nb.push_back(x);
    return uniq(nb);
}

static std::int64_t q4(double x) { return std::llround(x * 1e4); }
static void canon(gp_Dir& d) {
    double c[3] = {d.X(), d.Y(), d.Z()};
    for (double v : c) { if (std::abs(v) > 1e-9) { if (v < 0) d.Reverse(); break; } }
}
static void setAxis(Fingerprint& f, const gp_Dir& d) {
    f.qAxisX = std::llround(d.X() * 1e6); f.qAxisY = std::llround(d.Y() * 1e6); f.qAxisZ = std::llround(d.Z() * 1e6);
}
const Fingerprint& ShapeIdx::fingerprint(SubId s) const {
    auto& slot = fp[s.kind][s.index - 1];
    if (slot) return *slot;
    Fingerprint f;
    const TopoDS_Shape& sh = get(s);
    if (s.kind == KFace) {
        TopoDS_Face face = TopoDS::Face(sh);
        BRepAdaptor_Surface sf(face);
        f.type = (int)sf.GetType();
        GProp_GProps g; BRepGProp::SurfaceProperties(face, g);
        f.qArea = q4(g.Mass());
        gp_Pnt c = g.CentreOfMass(); f.qCx = q4(c.X()); f.qCy = q4(c.Y()); f.qCz = q4(c.Z());
        if (sf.GetType() == GeomAbs_Plane) {
            gp_Dir n = sf.Plane().Axis().Direction();
            if (!sf.Plane().Direct()) n.Reverse();
            if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
            setAxis(f, n);
        } else if (sf.GetType() == GeomAbs_Cylinder) {
            gp_Dir a = sf.Cylinder().Axis().Direction(); canon(a); setAxis(f, a); f.qRadius = q4(sf.Cylinder().Radius());
        } else if (sf.GetType() == GeomAbs_Cone) {
            gp_Dir a = sf.Cone().Axis().Direction(); canon(a); setAxis(f, a); f.qRadius = q4(sf.Cone().RefRadius());
        } else if (sf.GetType() == GeomAbs_Torus) {
            gp_Dir a = sf.Torus().Axis().Direction(); canon(a); setAxis(f, a); f.qRadius = q4(sf.Torus().MajorRadius());
        } else if (sf.GetType() == GeomAbs_Sphere) {
            f.qRadius = q4(sf.Sphere().Radius());
        }
        std::vector<int> nb;
        for (int e : edgesOfFace(s.index)) for (int x : facesOfEdge(e)) if (x != s.index) nb.push_back(x);
        f.neighbourCount = (std::uint16_t)uniq(nb).size();
    } else if (s.kind == KEdge) {
        TopoDS_Edge e = TopoDS::Edge(sh);
        BRepAdaptor_Curve c(e);
        f.type = (int)c.GetType();
        GProp_GProps g; BRepGProp::LinearProperties(e, g);
        f.qArea = q4(g.Mass());
        gp_Pnt cm = g.CentreOfMass(); f.qCx = q4(cm.X()); f.qCy = q4(cm.Y()); f.qCz = q4(cm.Z());
        if (c.GetType() == GeomAbs_Line) { gp_Dir d = c.Line().Direction(); canon(d); setAxis(f, d); }
        else if (c.GetType() == GeomAbs_Circle) { gp_Dir d = c.Circle().Axis().Direction(); canon(d); setAxis(f, d); f.qRadius = q4(c.Circle().Radius()); }
        f.neighbourCount = (std::uint16_t)facesOfEdge(s.index).size();
    } else {
        f.type = -1;
        gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(sh));
        f.qCx = q4(p.X()); f.qCy = q4(p.Y()); f.qCz = q4(p.Z());
        f.neighbourCount = (std::uint16_t)edgesOfVertex(s.index).size();
    }
    slot = f;
    return *slot;
}

// ---------------------------------------------------------------- history normalisation
static void pushU(std::vector<SubId>& v, SubId s) { if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s); }

static void extract(BRepBuilderAPI_MakeShape& mk, const std::vector<const ShapeIdx*>& ins, const ShapeIdx& res, ShapeHistory& h) {
    for (int i = 0; i < (int)ins.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            for (int j = 1; j <= ins[i]->m[k].Extent(); ++j) {
                const TopoDS_Shape& s = ins[i]->m[k](j);
                SubId src{(Kind)k, j};
                std::vector<SubId> mod, gen;
                for (const auto& x : mk.Modified(s)) { SubId r = res.find(x); if (r.index) pushU(mod, r); }
                for (const auto& x : mk.Generated(s)) { SubId r = res.find(x); if (r.index) pushU(gen, r); }
                bool del = mk.IsDeleted(s);
                SubId self = res.find(s);
                h.nIn[k]++;
                if (!mod.empty()) {
                    std::sort(mod.begin(), mod.end());
                    h.entries.push_back({HistoryEntry::Modified, i, src, mod, false});
                } else if (self.index) {
                    h.entries.push_back({HistoryEntry::Modified, i, src, {self}, true});
                } else {
                    if (del) h.deletedOk[k]++; else h.vanished[k]++;
                    h.entries.push_back({HistoryEntry::Deleted, i, src, {}, false});
                }
                if (!gen.empty()) {
                    std::sort(gen.begin(), gen.end());
                    h.entries.push_back({HistoryEntry::Generated, i, src, gen, false});
                }
            }
        }
    }
}

// ---------------------------------------------------------------- sweep
static TopoDS_Edge makeEnt(const Frame& f, const Ent& e) {
    if (e.k == 'L') return BRepBuilderAPI_MakeEdge(f.at(e.x0, e.y0), f.at(e.x1, e.y1)).Edge();
    gp_Circ c(gp_Ax2(f.at(e.cx, e.cy), f.n, f.xd), e.r);
    if (e.k == 'C') return BRepBuilderAPI_MakeEdge(c).Edge();
    return BRepBuilderAPI_MakeEdge(c, f.at(e.x0, e.y0), f.at(e.x1, e.y1)).Edge();
}
static bool onEnt(const Ent& e, double u, double v) {
    const double tol = 1e-6;
    if (e.k == 'L') {
        double dx = e.x1 - e.x0, dy = e.y1 - e.y0, len = std::hypot(dx, dy);
        double t = ((u - e.x0) * dx + (v - e.y0) * dy) / len;
        double d = std::abs((u - e.x0) * dy - (v - e.y0) * dx) / len;
        return d < tol && t > -tol && t < len + tol;
    }
    if (std::abs(std::hypot(u - e.cx, v - e.cy) - e.r) > tol) return false;
    if (e.k == 'C') return true;
    double a0 = std::atan2(e.y0 - e.cy, e.x0 - e.cx), a1 = std::atan2(e.y1 - e.cy, e.x1 - e.cx), a = std::atan2(v - e.cy, u - e.cx);
    auto norm = [](double x) { while (x < 0) x += 2 * M_PI; while (x >= 2 * M_PI) x -= 2 * M_PI; return x; };
    return norm(a - a0) <= norm(a1 - a0) + 1e-9;
}

OpOut opSweep(const Feature& f) {
    OpOut o;
    auto t0 = std::chrono::steady_clock::now();
    try {
        gp_Dir d = f.dir();
        std::vector<TopoDS_Face> faces;
        for (const Loop& l : f.loops) {
            BRepBuilderAPI_MakeWire mw;
            for (const Ent& e : l.ents) mw.Add(makeEnt(f.frame, e));
            if (!mw.IsDone()) { o.err = "wire failed"; return o; }
            gp_Pln pl(gp_Ax3(f.frame.o, d, f.frame.xd));
            BRepBuilderAPI_MakeFace mf(pl, mw.Wire(), true);
            if (!mf.IsDone()) { o.err = "face failed"; return o; }
            faces.push_back(mf.Face());
        }
        TopoDS_Shape prof;
        if (faces.size() == 1) prof = faces[0];
        else { BRep_Builder B; TopoDS_Compound c; B.MakeCompound(c); for (auto& x : faces) B.Add(c, x); prof = c; }
        BRepPrimAPI_MakePrism mk(prof, gp_Vec(d) * f.dist);
        if (!mk.IsDone()) { o.err = "prism failed"; return o; }
        GProp_GProps g; BRepGProp::VolumeProperties(mk.Shape(), g);
        if (g.Mass() <= 0) { o.err = "prism has non-positive volume"; return o; }
        o.result = indexShape(mk.Shape());
        for (size_t li = 0; li < f.loops.size(); ++li) {
            const Loop& l = f.loops[li];
            for (TopExp_Explorer ex(faces[li], TopAbs_EDGE); ex.More(); ex.Next()) {
                TopoDS_Edge e = TopoDS::Edge(ex.Current());
                BRepAdaptor_Curve c(e);
                gp_Pnt mid = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
                gp_Vec dv(f.frame.o, mid);
                double u = dv.Dot(gp_Vec(f.frame.xd)), v = dv.Dot(gp_Vec(f.frame.yd()));
                const Ent* hit = nullptr;
                for (const Ent& en : l.ents) if (onEnt(en, u, v)) { hit = &en; break; }
                if (!hit) { o.err = "profile edge not matched to a sketch entity"; return o; }
                for (const auto& gsh : mk.Generated(e)) {
                    SubId r = o.result->find(gsh);
                    if (r.index && r.kind == KFace)
                        o.origins.push_back({r, hashStr(f.id + "/ent/" + std::to_string(hit->tag)), ".side"});
                }
            }
            SubId a = o.result->find(mk.FirstShape(faces[li])), b = o.result->find(mk.LastShape(faces[li]));
            NameId lp = hashStr(f.id + "/loop/" + std::to_string(l.id));
            if (a.index) o.origins.push_back({a, lp, ".startcap"});
            if (b.index) o.origins.push_back({b, lp, ".endcap"});
        }
        o.ok = true;
    } catch (const Standard_Failure& e) {
        o.err = std::string("OCCT: ") + e.what();
    }
    o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return o;
}

// ---------------------------------------------------------------- boolean
OpOut opBoolean(BOp op, const ShapeIdx& target, const ShapeIdx& tool, bool unify) {
    OpOut o;
    auto t0 = std::chrono::steady_clock::now();
    try {
        std::unique_ptr<BRepAlgoAPI_BooleanOperation> mk;
        if (op == BOp::Cut) mk = std::make_unique<BRepAlgoAPI_Cut>(target.shape, tool.shape);
        else mk = std::make_unique<BRepAlgoAPI_Fuse>(target.shape, tool.shape);
        if (!mk->IsDone() || mk->HasErrors()) { o.err = "boolean failed"; return o; }
        if (unify) mk->SimplifyResult();
        o.result = indexShape(mk->Shape());
        if (o.result->count(KFace) == 0) { o.err = "boolean result empty"; return o; }
        extract(*mk, {&target, &tool}, *o.result, o.hist);
        o.valid = BRepCheck_Analyzer(o.result->shape).IsValid();
        o.ok = true;
    } catch (const Standard_Failure& e) {
        o.err = std::string("OCCT: ") + e.what();
    }
    o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return o;
}

// ---------------------------------------------------------------- fillet / chamfer
OpOut opBlend(bool fillet, const ShapeIdx& target, const std::vector<int>& edges, double size) {
    OpOut o;
    auto t0 = std::chrono::steady_clock::now();
    try {
        std::unique_ptr<BRepFilletAPI_LocalOperation> mk;
        if (fillet) {
            auto m = std::make_unique<BRepFilletAPI_MakeFillet>(target.shape);
            for (int e : edges) m->Add(size, TopoDS::Edge(target.m[KEdge](e)));
            mk = std::move(m);
        } else {
            auto m = std::make_unique<BRepFilletAPI_MakeChamfer>(target.shape);
            for (int e : edges) m->Add(size, TopoDS::Edge(target.m[KEdge](e)));
            mk = std::move(m);
        }
        mk->Build();
        if (!mk->IsDone()) { o.err = "blend not done"; return o; }
        o.result = indexShape(mk->Shape());
        if (o.result->count(KFace) == 0) { o.err = "blend result empty"; return o; }
        extract(*mk, {&target}, *o.result, o.hist);
        o.valid = BRepCheck_Analyzer(o.result->shape).IsValid();
        o.ok = true;
    } catch (const Standard_Failure& e) {
        o.err = std::string("OCCT: ") + e.what();
    }
    o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return o;
}

// ---------------------------------------------------------------- sampling for the ground-truth oracle
static void subdiv(const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c, double h, ElemSamples& es, int depth) {
    double ab = a.Distance(b), bc = b.Distance(c), ca = c.Distance(a);
    double mx = std::max({ab, bc, ca});
    if (mx <= h || depth > 12) {
        gp_Vec n = gp_Vec(a, b).Crossed(gp_Vec(a, c));
        double area = 0.5 * n.Magnitude();
        if (area <= 0) return;
        es.pts.emplace_back((a.XYZ() + b.XYZ() + c.XYZ()) / 3.0);
        es.w.push_back(area);
        return;
    }
    if (mx == ab) { gp_Pnt m((a.XYZ() + b.XYZ()) / 2); subdiv(a, m, c, h, es, depth + 1); subdiv(m, b, c, h, es, depth + 1); }
    else if (mx == bc) { gp_Pnt m((b.XYZ() + c.XYZ()) / 2); subdiv(a, b, m, h, es, depth + 1); subdiv(a, m, c, h, es, depth + 1); }
    else { gp_Pnt m((c.XYZ() + a.XYZ()) / 2); subdiv(a, b, m, h, es, depth + 1); subdiv(m, b, c, h, es, depth + 1); }
}

std::shared_ptr<Samples> sampleShape(const ShapeIdx& idx, bool withFaces) {
    auto S = std::make_shared<Samples>();
    if (withFaces) {
        BRepMesh_IncrementalMesh mesh(idx.shape, 0.05, false, 0.2, false);
        S->e[KFace].resize(idx.count(KFace));
        for (int i = 1; i <= idx.count(KFace); ++i) {
            ElemSamples& es = S->e[KFace][i - 1];
            TopoDS_Face f = TopoDS::Face(idx.m[KFace](i));
            {
                BRepAdaptor_Surface sf(f);
                es.type = (int)sf.GetType();
                bool rev = f.Orientation() == TopAbs_REVERSED;
                if (sf.GetType() == GeomAbs_Plane) {
                    gp_Dir n = sf.Plane().Axis().Direction();
                    if (!sf.Plane().Direct()) n.Reverse();
                    if (rev) n.Reverse();
                    es.nrm = n;
                } else if (sf.GetType() == GeomAbs_Cylinder) {
                    es.sigma = (sf.Cylinder().Direct() ? 1 : -1) * (rev ? -1 : 1);
                }
            }
            TopLoc_Location loc;
            auto tri = BRep_Tool::Triangulation(f, loc);
            if (tri.IsNull()) continue;
            gp_Trsf tr = loc.Transformation();
            std::vector<gp_Pnt> nd(tri->NbNodes() + 1);
            for (int n = 1; n <= tri->NbNodes(); ++n) { nd[n] = tri->Node(n).Transformed(tr); es.nodes.push_back(nd[n]); }
            for (int t = 1; t <= tri->NbTriangles(); ++t) {
                int a, b, c; tri->Triangle(t).Get(a, b, c);
                subdiv(nd[a], nd[b], nd[c], 2.5, es, 0);
            }
        }
    }
    S->e[KEdge].resize(idx.count(KEdge));
    for (int i = 1; i <= idx.count(KEdge); ++i) {
        ElemSamples& es = S->e[KEdge][i - 1];
        TopoDS_Edge e = TopoDS::Edge(idx.m[KEdge](i));
        if (BRep_Tool::Degenerated(e)) continue;
        BRepAdaptor_Curve c(e);
        es.type = (int)c.GetType();
        double f = c.FirstParameter(), l = c.LastParameter();
        const int N = 16;
        for (int j = 0; j < N; ++j) { es.pts.push_back(c.Value(f + (j + 0.5) / N * (l - f))); es.w.push_back(1.0); }
        es.nodes = es.pts;
        es.nodes.push_back(c.Value(f)); es.nodes.push_back(c.Value(l));
    }
    S->e[KVertex].resize(idx.count(KVertex));
    for (int i = 1; i <= idx.count(KVertex); ++i) {
        ElemSamples& es = S->e[KVertex][i - 1];
        gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(idx.m[KVertex](i)));
        es.pts = {p}; es.w = {1.0}; es.nodes = {p};
    }
    return S;
}

} // namespace s4
