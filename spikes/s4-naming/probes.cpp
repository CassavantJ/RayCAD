// Minimal history probes: one OCCT operation each, on a box. They show exactly what Modified(),
// Generated() and IsDeleted() report in OCCT 8.0.1, independent of the 40-feature part.
#include "s4.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Tool.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <gp_Ax2.hxx>

namespace s4 {

namespace {
struct Counts { int in = 0, kept = 0, mod1 = 0, split = 0, del = 0, vanished = 0, genSrc = 0; int res = 0, orphan = 0, merged = 0; };

template <class ModF, class GenF, class DelF>
void summarize(const char* name, const std::vector<const ShapeIdx*>& ins, const ShapeIdx& res, ModF Mod, GenF Gen, DelF Del) {
    Counts c[3];
    std::vector<int> anc[3];
    for (int k = 0; k < 3; ++k) { anc[k].assign(res.count((Kind)k) + 1, 0); c[k].res = res.count((Kind)k); }
    for (const ShapeIdx* in : ins)
        for (int k = 0; k < 3; ++k)
            for (int j = 1; j <= in->count((Kind)k); ++j) {
                const TopoDS_Shape& s = in->m[k](j);
                c[k].in++;
                std::vector<SubId> mod, gen;
                for (const auto& x : Mod(s)) { SubId r = res.find(x); if (r.index && std::find(mod.begin(), mod.end(), r) == mod.end()) mod.push_back(r); }
                for (const auto& x : Gen(s)) { SubId r = res.find(x); if (r.index && std::find(gen.begin(), gen.end(), r) == gen.end()) gen.push_back(r); }
                SubId self = res.find(s);
                if (!mod.empty()) { (mod.size() > 1 ? c[k].split : c[k].mod1)++; for (SubId r : mod) anc[r.kind][r.index]++; }
                else if (self.index) { c[k].kept++; anc[k][self.index]++; }
                else if (Del(s)) c[k].del++;
                else c[k].vanished++;
                if (!gen.empty()) { c[k].genSrc++; for (SubId r : gen) anc[r.kind][r.index] += 100; }
            }
    for (int k = 0; k < 3; ++k)
        for (int j = 1; j <= res.count((Kind)k); ++j) { if (anc[k][j] == 0) c[k].orphan++; if (anc[k][j] % 100 > 1) c[k].merged++; }
    std::printf("| %s |", name);
    for (int k = 0; k < 3; ++k)
        std::printf(" %d: kept %d, Modified 1:1 %d, split %d, deleted %d, vanished %d, Generated-from %d |", c[k].in, c[k].kept, c[k].mod1, c[k].split, c[k].del, c[k].vanished, c[k].genSrc);
    std::printf(" F %d (%d no history, %d merged) E %d (%d, %d) V %d (%d, %d) |\n", c[0].res, c[0].orphan, c[0].merged, c[1].res, c[1].orphan, c[1].merged, c[2].res, c[2].orphan, c[2].merged);
}
void viaMaker(const char* name, BRepBuilderAPI_MakeShape& mk, const std::vector<const ShapeIdx*>& ins) {
    auto res = indexShape(mk.Shape());
    summarize(name, ins, *res, [&](const TopoDS_Shape& s) { return mk.Modified(s); }, [&](const TopoDS_Shape& s) { return mk.Generated(s); },
              [&](const TopoDS_Shape& s) { return mk.IsDeleted(s); });
}
// the vertical edge of a box at (x, y)
int vEdge(const ShapeIdx& ix, double x, double y) {
    for (int e = 1; e <= ix.count(KEdge); ++e) {
        const Fingerprint& f = ix.fingerprint({KEdge, e});
        if (std::llabs(f.qCx - std::llround(x * 1e4)) < 5 && std::llabs(f.qCy - std::llround(y * 1e4)) < 5 && std::abs(f.qAxisZ) > 900000) return e;
    }
    return 0;
}
int edgeAt(const ShapeIdx& ix, double x, double y, double z) {
    for (int e = 1; e <= ix.count(KEdge); ++e) {
        const Fingerprint& f = ix.fingerprint({KEdge, e});
        if (std::llabs(f.qCx - std::llround(x * 1e4)) < 5 && std::llabs(f.qCy - std::llround(y * 1e4)) < 5 && std::llabs(f.qCz - std::llround(z * 1e4)) < 5) return e;
    }
    return 0;
}
} // namespace

void runHistoryProbes() {
    std::printf("\n| probe | input faces | input edges | input vertices | result (elements with no Modified/Generated ancestor, elements with 2+ Modified ancestors) |\n|-|-|-|-|-|\n");
    try {
        TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(40, 30, 20)).Shape();
        auto bi = indexShape(box);
        { // fillet one edge
            BRepFilletAPI_MakeFillet mk(box); mk.Add(5.0, TopoDS::Edge(bi->m[KEdge](vEdge(*bi, 0, 0)))); mk.Build();
            viaMaker("fillet, one edge of a box, r=5", mk, {bi.get()});
        }
        { // chamfer one edge
            BRepFilletAPI_MakeChamfer mk(box); mk.Add(3.0, TopoDS::Edge(bi->m[KEdge](vEdge(*bi, 0, 0)))); mk.Build();
            viaMaker("chamfer, one edge of a box, d=3", mk, {bi.get()});
        }
        { // fillet three edges at one corner
            BRepFilletAPI_MakeFillet mk(box);
            mk.Add(4.0, TopoDS::Edge(bi->m[KEdge](vEdge(*bi, 0, 0))));
            mk.Add(4.0, TopoDS::Edge(bi->m[KEdge](edgeAt(*bi, 20, 0, 0))));
            mk.Add(4.0, TopoDS::Edge(bi->m[KEdge](edgeAt(*bi, 0, 15, 0))));
            mk.Build();
            if (mk.IsDone()) viaMaker("fillet, three edges meeting at a box corner, r=4", mk, {bi.get()});
            else std::printf("| fillet, three edges meeting at a box corner | not done | | | |\n");
        }
        { // through hole
            TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 15, -5), gp_Dir(0, 0, 1)), 4, 30).Shape();
            auto ci = indexShape(cyl);
            BRepAlgoAPI_Cut mk(box, cyl);
            viaMaker("cut, through hole in a box (no SimplifyResult)", mk, {bi.get(), ci.get()});
            BRepAlgoAPI_Cut mk2(box, cyl); mk2.SimplifyResult();
            viaMaker("cut, through hole in a box + SimplifyResult", mk2, {bi.get(), ci.get()});
        }
        { // notch that splits a side face
            TopoDS_Shape tool = BRepPrimAPI_MakeBox(gp_Pnt(15, -5, -5), gp_Pnt(25, 10, 25)).Shape();
            auto ti = indexShape(tool);
            BRepAlgoAPI_Cut mk(box, tool); mk.SimplifyResult();
            viaMaker("cut, through notch that splits one side face + SimplifyResult", mk, {bi.get(), ti.get()});
        }
        { // fuse with coplanar faces
            TopoDS_Shape b2 = BRepPrimAPI_MakeBox(gp_Pnt(40, 0, 0), gp_Pnt(60, 30, 20)).Shape();
            auto b2i = indexShape(b2);
            BRepAlgoAPI_Fuse mk(box, b2);
            viaMaker("fuse, two boxes side by side, 4 coplanar face pairs (no SimplifyResult)", mk, {bi.get(), b2i.get()});
            BRepAlgoAPI_Fuse mk2(box, b2); mk2.SimplifyResult();
            viaMaker("fuse, same + SimplifyResult", mk2, {bi.get(), b2i.get()});
            // standalone unifier on the un-simplified fuse result
            BRepAlgoAPI_Fuse mk3(box, b2);
            auto fi = indexShape(mk3.Shape());
            ShapeUpgrade_UnifySameDomain u(mk3.Shape(), true, true, false);
            u.Build();
            auto ui = indexShape(u.Shape());
            auto h = u.History();
            summarize("ShapeUpgrade_UnifySameDomain on that fuse result (BRepTools_History)", {fi.get()}, *ui,
                      [&](const TopoDS_Shape& s) { return h->Modified(s); }, [&](const TopoDS_Shape& s) { return h->Generated(s); },
                      [&](const TopoDS_Shape& s) { return h->IsRemoved(s); });
        }
        { // prism
            BRepBuilderAPI_MakePolygon poly(gp_Pnt(0, 0, 0), gp_Pnt(40, 0, 0), gp_Pnt(40, 30, 0), gp_Pnt(0, 30, 0), true);
            TopoDS_Face f = BRepBuilderAPI_MakeFace(poly.Wire(), true).Face();
            auto fi = indexShape(f);
            BRepPrimAPI_MakePrism mk(f, gp_Vec(0, 0, 20));
            viaMaker("prism, rectangle face (Generated only; FirstShape/LastShape not counted)", mk, {fi.get()});
        }
    } catch (const Standard_Failure& e) {
        std::printf("probe failed: %s\n", e.what());
    }
}

} // namespace s4
