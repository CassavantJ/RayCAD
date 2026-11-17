// Spike S3: synthetic STEP generator.
// Builds mould-base-like plates (rounded outline, grid of through holes, counterbores, countersinks,
// filleted blind pockets, optional B-spline cavity floor) with OCCT and writes them with
// STEPControl_Writer, either as one solid or as an assembly of distinct solids.
//
// Usage: s3gen <out.step> <spec> [<spec> ...]
//   spec = W,D,T,pitch,pockets,freeform,seed[,toolShare]
//          W,D,T     plate size in mm
//          pitch     hole grid pitch in mm
//          pockets   number of blind pockets
//          freeform  0/1: first pocket gets a B-spline floor
//          seed      RNG seed (changes hole sizes and pocket layout, so no two bodies are identical)
//          toolShare 0..1 share of holes that get a counterbore or countersink (default 0.5)
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_PointsToBSplineSurface.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Interface_Static.hxx>
#include <NCollection_Array2.hxx>
#include <NCollection_List.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

struct Spec {
    double W = 400, D = 300, T = 40, pitch = 16;
    int pockets = 4;
    int freeform = 1;
    unsigned seed = 1;
    double toolShare = 0.5;
};

struct Rect { double x0, y0, x1, y1; };

static TopoDS_Wire roundedRect(double x0, double y0, double x1, double y1, double r, double z)
{
    auto P = [z](double x, double y) { return gp_Pnt(x, y, z); };
    const double k = r * (1.0 - std::sqrt(0.5));
    BRepBuilderAPI_MakeWire mw;
    // bottom, right, top, left with corner arcs (counter-clockwise seen from +Z)
    mw.Add(BRepBuilderAPI_MakeEdge(P(x0 + r, y0), P(x1 - r, y0)));
    mw.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(P(x1 - r, y0), P(x1 - k, y0 + k), P(x1, y0 + r)).Value()));
    mw.Add(BRepBuilderAPI_MakeEdge(P(x1, y0 + r), P(x1, y1 - r)));
    mw.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(P(x1, y1 - r), P(x1 - k, y1 - k), P(x1 - r, y1)).Value()));
    mw.Add(BRepBuilderAPI_MakeEdge(P(x1 - r, y1), P(x0 + r, y1)));
    mw.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(P(x0 + r, y1), P(x0 + k, y1 - k), P(x0, y1 - r)).Value()));
    mw.Add(BRepBuilderAPI_MakeEdge(P(x0, y1 - r), P(x0, y0 + r)));
    mw.Add(BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(P(x0, y0 + r), P(x0 + k, y0 + k), P(x0 + r, y0)).Value()));
    return mw.Wire();
}

// Pocket tool: rounded-rectangle prism from zBottom to zTop, bottom edges filleted.
static TopoDS_Shape pocketTool(const Rect& rc, double zBottom, double zTop, double cornerR, double floorR)
{
    TopoDS_Face f = BRepBuilderAPI_MakeFace(roundedRect(rc.x0, rc.y0, rc.x1, rc.y1, cornerR, zBottom));
    TopoDS_Shape prism = BRepPrimAPI_MakePrism(f, gp_Vec(0, 0, zTop - zBottom)).Shape();
    try {
        BRepFilletAPI_MakeFillet mf(prism);
        int n = 0;
        for (TopExp_Explorer ex(prism, TopAbs_EDGE); ex.More(); ex.Next()) {
            TopoDS_Vertex a, b;
            TopExp::Vertices(TopoDS::Edge(ex.Current()), a, b);
            if (a.IsNull() || b.IsNull()) continue;
            if (std::abs(BRep_Tool::Pnt(a).Z() - zBottom) < 1e-6 && std::abs(BRep_Tool::Pnt(b).Z() - zBottom) < 1e-6) {
                mf.Add(floorR, TopoDS::Edge(ex.Current()));
                ++n;
            }
        }
        if (n > 0) {
            mf.Build();
            if (mf.IsDone()) return mf.Shape();
        }
    } catch (const Standard_Failure&) {
    }
    return prism;
}

// Pocket tool with a wavy B-spline floor: common of the prism and the solid above the surface.
static TopoDS_Shape freeformTool(const Rect& rc, double zLow, double zTop, double cornerR, double amp, unsigned seed)
{
    const int N = 24;
    const double m = 5.0;
    NCollection_Array2<gp_Pnt> pts(1, N, 1, N);
    std::mt19937 rng(seed * 7919u + 13u);
    std::uniform_real_distribution<double> ph(0.0, 6.28318);
    const double p1 = ph(rng), p2 = ph(rng), p3 = ph(rng);
    for (int i = 1; i <= N; ++i)
        for (int j = 1; j <= N; ++j) {
            const double u = double(i - 1) / (N - 1), v = double(j - 1) / (N - 1);
            const double x = rc.x0 - m + u * (rc.x1 - rc.x0 + 2 * m);
            const double y = rc.y0 - m + v * (rc.y1 - rc.y0 + 2 * m);
            const double z = zLow + amp * (1.0 + 0.5 * std::sin(6.0 * u + p1) * std::cos(5.0 * v + p2)
                                           + 0.3 * std::sin(11.0 * u * v + p3) + 0.2 * std::cos(9.0 * (u - v)));
            pts.SetValue(i, j, gp_Pnt(x, y, z));
        }
    Handle(Geom_BSplineSurface) surf = GeomAPI_PointsToBSplineSurface(pts, 3, 8, GeomAbs_C2, 1e-3).Surface();
    TopoDS_Face sf = BRepBuilderAPI_MakeFace(surf, 1e-6);
    TopoDS_Shape above = BRepPrimAPI_MakePrism(sf, gp_Vec(0, 0, (zTop - zLow) + 50.0)).Shape();
    TopoDS_Face f = BRepBuilderAPI_MakeFace(roundedRect(rc.x0, rc.y0, rc.x1, rc.y1, cornerR, zLow - 1.0));
    TopoDS_Shape prism = BRepPrimAPI_MakePrism(f, gp_Vec(0, 0, zTop - zLow + 1.0)).Shape();
    BRepAlgoAPI_Common common(prism, above);
    if (!common.IsDone() || common.HasErrors() || common.Shape().IsNull())
        return pocketTool(rc, zLow, zTop, cornerR, 3.0);
    return common.Shape();
}

static TopoDS_Shape makePlate(const Spec& s, bool parallelBool, int& holesOut, int& toolsOut)
{
    std::mt19937 rng(s.seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const double margin = 12.0;

    // Pocket rectangles: a row of pockets across the plate centre.
    std::vector<Rect> pockets;
    if (s.pockets > 0) {
        const double bandY0 = s.D * 0.30, bandY1 = s.D * 0.70;
        const double cell = (s.W - 2 * margin) / s.pockets;
        for (int i = 0; i < s.pockets; ++i) {
            const double cx0 = margin + i * cell + 6.0 + 4.0 * uni(rng);
            const double cx1 = margin + (i + 1) * cell - 6.0 - 4.0 * uni(rng);
            pockets.push_back({cx0, bandY0 + 6.0 * uni(rng), cx1, bandY1 - 6.0 * uni(rng)});
        }
    }
    auto inPocket = [&](double x, double y, double clear) {
        for (const Rect& r : pockets)
            if (x > r.x0 - clear && x < r.x1 + clear && y > r.y0 - clear && y < r.y1 + clear) return true;
        return false;
    };

    // Outline and through holes as one face, then one prism.
    BRepBuilderAPI_MakeFace mf(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), roundedRect(0, 0, s.W, s.D, 10.0, 0.0));
    struct Hole { double x, y, r; };
    std::vector<Hole> holes;
    const double radii[5] = {2.5, 3.0, 3.4, 4.25, 5.0};
    for (double y = margin; y <= s.D - margin + 1e-9; y += s.pitch)
        for (double x = margin; x <= s.W - margin + 1e-9; x += s.pitch) {
            if (inPocket(x, y, 9.0)) continue;
            Hole h{x, y, radii[rng() % 5]};
            holes.push_back(h);
            TopoDS_Edge e = BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(x, y, 0), gp_Dir(0, 0, 1)), h.r));
            TopoDS_Wire w = BRepBuilderAPI_MakeWire(e);
            mf.Add(TopoDS::Wire(w.Reversed()));
        }
    TopoDS_Shape plate = BRepPrimAPI_MakePrism(mf.Face(), gp_Vec(0, 0, s.T)).Shape();
    holesOut = int(holes.size());

    // Tools: counterbores, countersinks, pockets.
    NCollection_List<TopoDS_Shape> tools;
    for (const Hole& h : holes) {
        const double u = uni(rng);
        if (u < s.toolShare * 0.65) {
            const double depth = std::min(s.T * 0.4, h.r * 1.6 + 2.0);
            tools.Append(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(h.x, h.y, s.T - depth), gp_Dir(0, 0, 1)),
                                                  h.r + 2.25, depth + 1.0).Shape());
        } else if (u < s.toolShare) {
            const double d = 1.5; // 45 degree countersink, depth d, continued 1 mm above the top face
            tools.Append(BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(h.x, h.y, s.T - d), gp_Dir(0, 0, 1)),
                                              h.r, h.r + d + 1.0, d + 1.0).Shape());
        }
    }
    for (size_t i = 0; i < pockets.size(); ++i) {
        const double depth = s.T * (0.35 + 0.3 * uni(rng));
        if (i == 0 && s.freeform)
            tools.Append(freeformTool(pockets[i], s.T - depth, s.T + 1.0, 8.0, depth * 0.25, s.seed));
        else
            tools.Append(pocketTool(pockets[i], s.T - depth, s.T + 1.0, 8.0, 3.0));
    }
    toolsOut = int(tools.Size());
    if (tools.IsEmpty()) return plate;

    NCollection_List<TopoDS_Shape> args;
    args.Append(plate);
    BRepAlgoAPI_Cut cut;
    cut.SetArguments(args);
    cut.SetTools(tools);
    cut.SetRunParallel(parallelBool);
    cut.Build();
    if (!cut.IsDone() || cut.HasErrors()) {
        std::fprintf(stderr, "boolean cut failed for seed %u\n", s.seed);
        return plate;
    }
    return cut.Shape();
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: s3gen <out.step> <W,D,T,pitch,pockets,freeform,seed[,toolShare]> ...\n");
        return 2;
    }
    std::vector<Spec> specs;
    for (int i = 2; i < argc; ++i) {
        Spec s;
        int n = std::sscanf(argv[i], "%lf,%lf,%lf,%lf,%d,%d,%u,%lf", &s.W, &s.D, &s.T, &s.pitch, &s.pockets,
                            &s.freeform, &s.seed, &s.toolShare);
        if (n < 7) { std::fprintf(stderr, "bad spec %s\n", argv[i]); return 2; }
        specs.push_back(s);
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<TopoDS_Shape> bodies(specs.size());
    std::vector<int> nh(specs.size()), nt(specs.size());
    std::atomic<size_t> next{0};
    const bool single = specs.size() == 1;
    auto work = [&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= specs.size()) return;
            try {
                bodies[i] = makePlate(specs[i], single, nh[i], nt[i]);
            } catch (const Standard_Failure& e) {
                std::fprintf(stderr, "body %zu failed: %s\n", i, e.what());
            }
        }
    };
    std::vector<std::thread> th;
    const unsigned nth = single ? 1u : std::min<unsigned>(16u, unsigned(specs.size()));
    for (unsigned i = 0; i < nth; ++i) th.emplace_back(work);
    for (auto& t : th) t.join();

    TopoDS_Shape out;
    int totalHoles = 0, totalTools = 0, valid = 0;
    if (single) {
        out = bodies[0];
        totalHoles = nh[0]; totalTools = nt[0];
        valid = BRepCheck_Analyzer(out).IsValid() ? 1 : 0;
    } else {
        TopoDS_Compound c;
        BRep_Builder bb;
        bb.MakeCompound(c);
        double z = 0.0;
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].IsNull()) continue;
            gp_Trsf tr;
            tr.SetTranslation(gp_Vec(25.0 * double(i % 3), 15.0 * double(i % 2), z));
            z += specs[i].T + 5.0;
            bb.Add(c, bodies[i].Moved(TopLoc_Location(tr)));
            totalHoles += nh[i]; totalTools += nt[i];
            valid += BRepCheck_Analyzer(bodies[i]).IsValid() ? 1 : 0;
        }
        out = c;
    }
    const auto t1 = std::chrono::steady_clock::now();

    Interface_Static::SetCVal("write.step.unit", "MM");
    Interface_Static::SetIVal("write.step.assembly", single ? 0 : 1);
    STEPControl_Writer writer;
    if (writer.Transfer(out, STEPControl_AsIs) != IFSelect_RetDone) { std::fprintf(stderr, "transfer failed\n"); return 1; }
    if (writer.Write(argv[1]) != IFSelect_RetDone) { std::fprintf(stderr, "write failed\n"); return 1; }
    const auto t2 = std::chrono::steady_clock::now();
    int faces = 0;
    for (TopExp_Explorer ex(out, TopAbs_FACE); ex.More(); ex.Next()) ++faces;
    std::printf("%s bodies=%zu valid=%d holes=%d tools=%d faces=%d build=%.1fs write=%.1fs\n", argv[1], specs.size(),
                valid, totalHoles, totalTools, faces, std::chrono::duration<double>(t1 - t0).count(),
                std::chrono::duration<double>(t2 - t1).count());
    return 0;
}
