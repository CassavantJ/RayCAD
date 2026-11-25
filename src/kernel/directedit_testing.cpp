// The S9 corpus method for the direct-edit tests (task T-080). See directedit_testing.h.
#include "directedit_testing.h"

#include "directedit_internal.h"
#include "io_internal.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <NCollection_List.hxx>
#include <ShapeAnalysis_Shell.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace ray::kernel::testing {

namespace {

using detail::FaceModel;

constexpr double kPi = 3.14159265358979323846;

Vec3 vec(const gp_Pnt& p)
{
    return Vec3{p.X(), p.Y(), p.Z()};
}

int nPick(int n)
{
    return std::min(n, 5);
}
int pick(int n, int k)
{
    return n <= 5 ? k : static_cast<int>(static_cast<long long>(k) * (n - 1) / 4);
}

std::vector<SubId> toSubIds(const std::vector<int>& faces)
{
    std::vector<SubId> out;
    for (const int f : faces)
        out.push_back(SubId{SubKind::Face, static_cast<std::uint32_t>(f)});
    return out;
}

// Everything the spike's `analyse` found, in its order.
struct Targets {
    std::vector<std::vector<int>> bores, boreTargets, stacks, runs, pockets;
    std::vector<int> pocketTop, fillets, planars;
};

Targets analyse(FaceModel& m)
{
    Targets t;
    const int n = m.faceCount();
    std::set<int> seenBore;
    for (int i = 0; i < n; ++i) {
        const std::vector<int>* bore = m.bore(i);
        if (bore == nullptr || !seenBore.insert(bore->front()).second)
            continue;
        t.bores.push_back(*bore);
        t.boreTargets.push_back(m.withCaps(*bore));
        const std::vector<int> s = m.stack(*bore);
        if (s != t.boreTargets.back() && std::find(t.stacks.begin(), t.stacks.end(), s) == t.stacks.end())
            t.stacks.push_back(s);
    }
    std::set<int> inRun;
    for (int i = 0; i < n; ++i) {
        if (m.isFillet(i))
            t.fillets.push_back(i);
        if (m.isBlendFace(i) && inRun.count(i) == 0) {
            const std::vector<int> run = m.blendRun(i);
            inRun.insert(run.begin(), run.end());
            if (run.size() >= 2)
                t.runs.push_back(run);
        }
    }
    // Pockets: a closed depression below a planar face, entered through an inner wire of it.
    const detail::ShapeIndex& faces = m.shape().impl->subs[detail::kindSlot(SubKind::Face)];
    const detail::AncestorIndex& edgeFaces = m.shape().impl->ancestors(SubKind::Edge, SubKind::Face);
    for (int i = 0; i < n; ++i) {
        const detail::FaceFacts& top = m.facts(i);
        if (top.type != GeomAbs_Plane || !top.hasPoint)
            continue;
        const TopoDS_Face face = m.face(i);
        const TopoDS_Wire outer = BRepTools::OuterWire(face);
        detail::ShapeIndex outerEdges;
        TopExp::MapShapes(outer, TopAbs_EDGE, outerEdges);
        std::set<int> rim;
        for (int k = 1; k <= outerEdges.Extent(); ++k)
            if (const int key = edgeFaces.FindIndex(outerEdges.FindKey(k)); key != 0)
                for (const TopoDS_Shape& f : edgeFaces.FindFromIndex(key))
                    if (const int q = faces.FindIndex(f) - 1; q != i && q >= 0)
                        rim.insert(q);
        for (TopExp_Explorer wire(face, TopAbs_WIRE); wire.More(); wire.Next()) {
            if (wire.Current().IsSame(outer))
                continue;
            std::set<int> comp;
            std::vector<int> todo;
            for (TopExp_Explorer edge(wire.Current(), TopAbs_EDGE); edge.More(); edge.Next())
                if (const int key = edgeFaces.FindIndex(edge.Current()); key != 0)
                    for (const TopoDS_Shape& f : edgeFaces.FindFromIndex(key))
                        if (const int q = faces.FindIndex(f) - 1; q != i && q >= 0 && comp.insert(q).second)
                            todo.push_back(q);
            bool escaped = false;
            while (!todo.empty() && !escaped) {
                const int c = todo.back();
                todo.pop_back();
                if (rim.count(c) != 0) {
                    escaped = true;
                    break;
                }
                for (const detail::FaceLink& link : m.links(c))
                    if (link.other != i && comp.insert(link.other).second)
                        todo.push_back(link.other);
            }
            if (escaped || comp.empty())
                continue;
            bool below = true;
            bool hasWall = false;
            const double tol = 1e-5 * std::max(1.0, m.diagonal());
            for (const int c : comp) {
                for (TopExp_Explorer v(m.face(c), TopAbs_VERTEX); v.More(); v.Next()) {
                    const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
                    if (gp_Vec(top.point, q).Dot(gp_Vec(top.normal)) > tol)
                        below = false;
                }
                const detail::FaceFacts& y = m.facts(c);
                if (y.type == GeomAbs_Plane && y.hasPoint && std::abs(y.normal.Dot(top.normal)) < 0.1)
                    hasWall = true;
            }
            if (!below || !hasWall)
                continue;
            std::vector<int> v(comp.begin(), comp.end());
            if (std::find(t.pockets.begin(), t.pockets.end(), v) == t.pockets.end()) {
                t.pockets.push_back(v);
                t.pocketTop.push_back(i);
            }
        }
    }
    for (int i = 0; i < n; ++i)
        if (m.facts(i).type == GeomAbs_Plane && m.facts(i).hasPoint)
            t.planars.push_back(i);
    std::stable_sort(t.planars.begin(), t.planars.end(), [&](int a, int b) { return m.area(a) > m.area(b); });
    return t;
}

// The stack deleteFaces removes for a bore: that of the narrowest bore in the bore's stack (directedit.cpp).
std::vector<int> stackOfHole(FaceModel& m, const std::vector<int>& bore)
{
    const std::vector<int>* drilled = &bore;
    for (const int k : m.stack(bore))
        if (const std::vector<int>* b = m.bore(k);
            b != nullptr && m.facts(b->front()).radius < m.facts(drilled->front()).radius)
            drilled = b;
    return m.stack(*drilled);
}

void holeExpectation(FaceModel& m, DirectEditCase& c, const std::vector<int>& targets, double loFactor)
{
    gp_Ax1 axis;
    double widest = 0.0;
    for (const int k : targets)
        if (m.facts(k).type == GeomAbs_Cylinder && m.facts(k).radius > widest) {
            widest = m.facts(k).radius;
            axis = m.facts(k).axis;
        }
    double tMin = 1e300, tMax = -1e300, reach = 0.0;
    for (const int k : targets) {
        for (TopExp_Explorer v(m.face(k), TopAbs_VERTEX); v.More(); v.Next()) {
            const gp_Pnt q = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
            tMin = std::min(tMin, detail::axialParameter(axis, q));
            tMax = std::max(tMax, detail::axialParameter(axis, q));
            reach = std::max(reach, detail::radialDistance(axis, q));
        }
        if (m.facts(k).hasPoint)
            c.checks.push_back({vec(m.facts(k).point), PointWant::In});
    }
    const double radius = std::max(widest, reach);
    const double reference = kPi * radius * radius * (tMax - tMin);
    c.sign = 1;
    c.lo = loFactor * reference;
    c.hi = 1.05 * reference;
}

void blendExpectation(FaceModel& m, DirectEditCase& c, const std::vector<int>& targets, bool hasFillets)
{
    double size = 0.0;
    int convex = 0, concave = 0;
    for (const int k : targets) {
        const detail::FaceFacts& x = m.facts(k);
        double r = x.type == GeomAbs_Torus ? x.minorRadius : x.radius;
        const bool analytic = x.type == GeomAbs_Cylinder || x.type == GeomAbs_Torus || x.type == GeomAbs_Sphere;
        if (!analytic) {
            // A spline corner patch: the spike borrowed the radius of the first face of the run.
            const detail::FaceFacts& y = m.facts(hasFillets ? targets.front() : k);
            r = std::max(y.radius, y.minorRadius);
            size += m.area(k) * r;
            continue;
        }
        size += m.area(k) * r;
        (x.concave ? concave : convex)++;
        c.checks.push_back({vec(x.point), x.concave ? PointWant::Out : PointWant::In});
    }
    c.sign = concave == 0 ? 1 : (convex == 0 ? -1 : 0);
    c.lo = 1e-4 * size;
    c.hi = 1.3 * size;
}

double boxVolume(FaceModel& m, const std::vector<int>& faces)
{
    Bnd_Box box;
    for (const int k : faces)
        BRepBndLib::AddOptimal(m.face(k), box, false, false);
    double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    box.Get(x0, y0, z0, x1, y1, z1);
    return (x1 - x0) * (y1 - y0) * (z1 - z0);
}

// Asks OCCT to stop once a time limit has passed.
class Deadline final : public Message_ProgressIndicator {
public:
    explicit Deadline(std::chrono::seconds limit) : until_(std::chrono::steady_clock::now() + limit) {}
    bool UserBreak() override { return std::chrono::steady_clock::now() > until_; }
    void Show(const Message_ProgressScope&, const bool) override {}

private:
    std::chrono::steady_clock::time_point until_;
};

bool isDelete(DirectEdit edit)
{
    return edit == DirectEdit::DeleteFillet || edit == DirectEdit::DeleteBlendRun || edit == DirectEdit::DeleteHole ||
           edit == DirectEdit::DeleteHoleStack || edit == DirectEdit::DeletePocket;
}

Bnd_Box boxOf(FaceModel& m, const std::vector<SubId>& faces, const std::vector<PointCheck>& points)
{
    Bnd_Box box;
    for (const SubId& f : faces)
        BRepBndLib::Add(m.face(static_cast<int>(f.index)), box, false);
    for (const PointCheck& p : points)
        box.Add(gp_Pnt(p.point.x, p.point.y, p.point.z));
    return box;
}

Box3 toBox3(const Bnd_Box& box)
{
    double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    box.Get(x0, y0, z0, x1, y1, z1);
    return Box3{Vec3{x0, y0, z0}, Vec3{x1, y1, z1}};
}

// vol(shape inside box), by BRepAlgoAPI_Common on a copy (the oracle's own, not the kernel's helper). Empty on failure.
std::optional<double> volumeInside(const TopoDS_Shape& shape, const Box3& b)
{
    const TopoDS_Shape box =
        BRepPrimAPI_MakeBox(gp_Pnt(b.min.x, b.min.y, b.min.z), gp_Pnt(b.max.x, b.max.y, b.max.z)).Shape();
    BRepBuilderAPI_Copy copier(shape, false, false);
    BRepAlgoAPI_Common common(copier.Shape(), box);
    common.SetRunParallel(false);
    common.SetNonDestructive(true);
    common.Build();
    if (!common.IsDone() || common.HasErrors())
        return std::nullopt;
    return detail::solidVolume(common.Shape());
}

Result<std::vector<DirectEditCase>> casesOf(const Shape& solid)
{
    FaceModel m(solid);
    Targets t = analyse(m);
    std::vector<DirectEditCase> out;
    const auto add = [&](DirectEdit edit, int available, auto&& fill) {
        for (int k = 0; k < nPick(available); ++k) {
            DirectEditCase c;
            c.edit = edit;
            c.k = k;
            c.available = available;
            fill(c, pick(available, k));
            if (isDelete(edit)) {
                // A removal changes the body near the removed faces (a removed convex round leaves a corner outside
                // its own box): their box with the check points, enlarged by its diagonal.
                Bnd_Box box = boxOf(m, c.faces, c.checks);
                box.Enlarge(std::sqrt(box.SquareExtent()) + 1e-3);
                c.region = toBox3(box);
            }
            out.push_back(std::move(c));
        }
    };
    add(DirectEdit::DeleteFillet, static_cast<int>(t.fillets.size()), [&](DirectEditCase& c, int i) {
        const int f = t.fillets[static_cast<std::size_t>(i)];
        c.faces = toSubIds({f});
        bool inRun = false;
        for (const std::vector<int>& run : t.runs)
            if (std::find(run.begin(), run.end(), f) != run.end())
                inRun = true;
        c.note = inRun ? "chain_member" : "isolated";
        blendExpectation(m, c, {f}, true);
    });
    add(DirectEdit::DeleteBlendRun, static_cast<int>(t.runs.size()), [&](DirectEditCase& c, int i) {
        const std::vector<int>& run = t.runs[static_cast<std::size_t>(i)];
        c.faces = toSubIds(run);
        blendExpectation(m, c, run, !t.fillets.empty());
    });
    add(DirectEdit::DeleteHole, static_cast<int>(t.boreTargets.size()), [&](DirectEditCase& c, int i) {
        const std::vector<int>& target = t.boreTargets[static_cast<std::size_t>(i)];
        c.faces = toSubIds(target);
        const std::vector<int> stack = stackOfHole(m, t.bores[static_cast<std::size_t>(i)]);
        const bool inStack = stack != target;
        c.note = inStack ? "in_stack" : "plain";
        holeExpectation(m, c, inStack ? stack : target, inStack ? 0.1 : 0.3);
    });
    add(DirectEdit::DeleteHoleStack, static_cast<int>(t.stacks.size()), [&](DirectEditCase& c, int i) {
        const std::vector<int>& stack = t.stacks[static_cast<std::size_t>(i)];
        c.faces = toSubIds(stack);
        holeExpectation(m, c, stack, 0.1);
    });
    add(DirectEdit::DeletePocket, static_cast<int>(t.pockets.size()), [&](DirectEditCase& c, int i) {
        const std::vector<int>& pocket = t.pockets[static_cast<std::size_t>(i)];
        c.faces = toSubIds(pocket);
        const double box = boxVolume(m, pocket);
        c.sign = 1;
        c.lo = 0.15 * box;
        c.hi = 1.05 * box;
        const detail::FaceFacts& top = m.facts(t.pocketTop[static_cast<std::size_t>(i)]);
        for (const int k : pocket)
            if (m.facts(k).hasPoint) {
                // An island top level with the face around the pocket ends up on the boundary.
                const bool level = std::abs(gp_Vec(top.point, m.facts(k).point).Dot(gp_Vec(top.normal))) <
                                   1e-5 * std::max(1.0, m.diagonal());
                c.checks.push_back({vec(m.facts(k).point), level ? PointWant::On : PointWant::In});
            }
    });
    for (const bool grow : {true, false})
        add(grow ? DirectEdit::ResizeLarger : DirectEdit::ResizeSmaller, static_cast<int>(t.bores.size()),
            [&](DirectEditCase& c, int i) {
                const std::vector<int>& bore = t.bores[static_cast<std::size_t>(i)];
                c.faces = toSubIds({bore.front()});
                const detail::HoleGeometry h = m.holeGeometry(bore);
                const double r = h.radius * (grow ? 1.25 : 0.8);
                c.newDiameter = 2.0 * r;
                const double reference = kPi * std::abs(r * r - h.radius * h.radius) * (h.tMax - h.tMin);
                c.sign = grow ? -1 : 1;
                c.lo = 0.3 * reference;
                c.hi = 1.7 * reference;
                const gp_Pnt foot = detail::footOnAxis(h.axis, h.wall);
                const gp_Vec radial = gp_Vec(foot, h.wall).Normalized();
                c.checks.push_back({vec(h.wall), grow ? PointWant::Out : PointWant::In});
                c.checks.push_back({vec(foot.Translated(radial * r)), PointWant::On});
                // The new and old wall: the whole bore, out to the larger radius and past its ends.
                std::vector<SubId> all = toSubIds(bore);
                Bnd_Box box = boxOf(m, all, c.checks);
                box.Enlarge(std::max(r, h.radius) + 1e-3);
                c.region = toBox3(box);
            });
    for (const bool outward : {true, false})
        add(outward ? DirectEdit::PushOut : DirectEdit::PushIn, static_cast<int>(t.planars.size()),
            [&](DirectEditCase& c, int i) {
                const int f = t.planars[static_cast<std::size_t>(i)];
                const detail::FaceFacts& x = m.facts(f);
                c.faces = toSubIds({f});
                const double d = std::clamp(0.01 * m.diagonal(), 0.25, 2.0);
                c.distance = outward ? d : -d;
                const double reference = m.area(f) * d;
                c.sign = outward ? 1 : -1;
                c.lo = 0.5 * reference;
                c.hi = 1.6 * reference;
                const gp_Vec n(x.normal);
                c.checks.push_back({vec(x.point), outward ? PointWant::In : PointWant::Out});
                c.checks.push_back({vec(x.point.Translated(n * c.distance)), PointWant::On});
                // The face swept by the distance.
                Bnd_Box box = boxOf(m, c.faces, c.checks);
                box.Enlarge(d + 1e-3);
                c.region = toBox3(box);
                bool square = true;
                for (const detail::FaceLink& link : m.links(f))
                    if (link.smooth || std::abs(link.normalDot) > 0.0175)
                        square = false;
                c.note = square ? "square" : "angled";
            });
    return out;
}

const char* stateName(detail::PointState state)
{
    switch (state) {
    case detail::PointState::In:
        return "IN";
    case detail::PointState::Out:
        return "OUT";
    case detail::PointState::On:
        return "ON";
    case detail::PointState::Unknown:
        break;
    }
    return "?";
}

DirectEditVerdict judge(const Shape& input, const DirectEditCase& c, const Shape& result)
{
    const TopoDS_Shape& r = detail::occt(result);
    std::vector<TopoDS_Shape> solids = detail::solidsOf(r);
    if (solids.size() != 1)
        return {"invalid", "result has " + std::to_string(solids.size()) + " solids"};
    if (!detail::checkedValid(r))
        return {"invalid", "BRepCheck_Analyzer reports errors"};
    ShapeAnalysis_Shell shells;
    shells.LoadShells(r);
    shells.CheckOrientedShells(r, true);
    if (shells.HasFreeEdges() || shells.HasBadEdges())
        return {"invalid", shells.HasFreeEdges() ? "free edges (not closed)" : "bad edge orientation"};
    const double v0 = detail::solidVolume(detail::occt(input));
    const double v1 = detail::solidVolume(r);
    if (!(v1 > 0.0))
        return {"invalid", "non-positive volume"};
    double dv = v1 - v0;
    double noOpScale = std::abs(v0);
    if (std::abs(dv) < 1e-4 * std::abs(v0)) {
        // Small next to the body: measure inside the edit's region (whole-body integration noise), and require the
        // whole-body change to agree with it within that noise.
        const std::optional<double> a = volumeInside(detail::occt(input), c.region);
        const std::optional<double> b = a ? volumeInside(r, c.region) : std::nullopt;
        if (!a || !b)
            return {"wrong", "the volume inside the edit's region could not be measured"};
        if (std::abs(dv - (*b - *a)) > 2e-5 * std::abs(v0))
            return {"wrong", "the body changed away from the edit (whole " + std::to_string(dv) + ", near the edit " +
                                 std::to_string(*b - *a) + ")"};
        dv = *b - *a;
        const Box3& g = c.region;
        noOpScale = std::max(1.0, (g.max.x - g.min.x) * (g.max.y - g.min.y) * (g.max.z - g.min.z));
    }
    if (std::abs(dv) < 1e-9 * noOpScale)
        return {"no_op", "volume unchanged"};
    if (c.sign != 0 && dv * c.sign < 0.0)
        return {"wrong", "volume changed in the wrong direction"};
    if (std::abs(dv) < c.lo || std::abs(dv) > c.hi)
        return {"wrong", "volume change " + std::to_string(dv) + " outside " + std::to_string(c.lo) + ".." +
                             std::to_string(c.hi)};
    int bad = 0;
    std::string which;
    for (const PointCheck& check : c.checks) {
        const detail::PointState got =
            detail::classifyPoint(solids.front(), gp_Pnt(check.point.x, check.point.y, check.point.z), 1e-5);
        const detail::PointState want = check.want == PointWant::In    ? detail::PointState::In
                                        : check.want == PointWant::Out ? detail::PointState::Out
                                                                       : detail::PointState::On;
        if (got != want) {
            ++bad;
            if (which.size() < 60)
                which += std::string(" want ") + stateName(want) + " got " + stateName(got) + ";";
        }
    }
    if (bad > 0)
        return {"wrong", std::to_string(bad) + " of " + std::to_string(c.checks.size()) +
                             " target point checks failed:" + which};
    return {"ok", {}};
}

} // namespace

const char* directEditName(DirectEdit edit)
{
    switch (edit) {
    case DirectEdit::DeleteFillet:
        return "del_fillet";
    case DirectEdit::DeleteBlendRun:
        return "del_fillet_chain";
    case DirectEdit::DeleteHole:
        return "del_hole";
    case DirectEdit::DeleteHoleStack:
        return "del_hole_stack";
    case DirectEdit::DeletePocket:
        return "del_pocket";
    case DirectEdit::ResizeLarger:
        return "rs_grow";
    case DirectEdit::ResizeSmaller:
        return "rs_shrink";
    case DirectEdit::PushOut:
        return "pp_out";
    case DirectEdit::PushIn:
        return "pp_in";
    }
    return "?";
}

Result<std::vector<DirectEditCase>> directEditCases(const Shape& solid)
{
    constexpr std::string_view where = "testing::directEditCases";
    if (solid.isNull() || solid.count(SubKind::Solid) != 1)
        return tl::unexpected(Error{ErrorCode::InvalidArgument, "one solid expected", {std::string(where)}});
    return detail::guarded(where, [&]() { return casesOf(solid); });
}

DirectEditVerdict judgeDirectEdit(const Shape& input, const DirectEditCase& edit, const Shape& result)
{
    if (result.isNull())
        return {"invalid", "null result"};
    auto verdict = detail::guarded("testing::judgeDirectEdit",
                                   [&]() -> Result<DirectEditVerdict> { return judge(input, edit, result); });
    if (!verdict)
        return {"invalid", verdict.error().message};
    return *verdict;
}

DirectEditFault::DirectEditFault(int count)
{
    detail::directEditFaultRequests().fetch_add(count, std::memory_order_relaxed);
}

DirectEditFault::~DirectEditFault()
{
    detail::directEditFaultRequests().store(0, std::memory_order_relaxed);
}

std::string infeasibleRequest(const Shape& input, const DirectEditCase& c)
{
    auto reason = detail::guarded("testing::infeasibleRequest", [&]() -> Result<std::string> {
        const TopoDS_Shape& body = detail::occt(input);
        const std::vector<TopoDS_Shape> solids = detail::solidsOf(body);
        if (solids.size() != 1)
            return std::string();
        const auto state = [&](const Vec3& p) {
            return detail::classifyPoint(solids.front(), gp_Pnt(p.x, p.y, p.z), 1e-5);
        };
        const auto fallsApart = [&](const TopoDS_Shape& tool) {
            BRepBuilderAPI_Copy copier(body, false, false);
            BRepAlgoAPI_Cut cut;
            NCollection_List<TopoDS_Shape> arguments, tools;
            arguments.Append(copier.Shape());
            tools.Append(tool);
            cut.SetArguments(arguments);
            cut.SetTools(tools);
            cut.SetRunParallel(false);
            cut.SetNonDestructive(true);
            // At most 30 s: the cut can be as slow as the edit (minutes on NIST CTC 02); undecided counts as feasible.
            occ::handle<Deadline> deadline = new Deadline(std::chrono::seconds(30));
            cut.Build(deadline->Start());
            return cut.IsDone() && !cut.HasErrors() && detail::solidsOf(cut.Shape()).size() > 1;
        };
        if (c.edit == DirectEdit::PushOut || c.edit == DirectEdit::PushIn) {
            // checks: [0] the face's sample point, [1] that point moved by the distance.
            const detail::PointState moved = state(c.checks[1].point);
            if (c.edit == DirectEdit::PushOut && moved == detail::PointState::In)
                return std::string("the moved face would lie inside the part's material");
            if (c.edit == DirectEdit::PushIn) {
                if (moved == detail::PointState::Out)
                    return std::string("the moved face would lie outside the part");
                const detail::ShapeIndex& faces = input.impl->subs[detail::kindSlot(SubKind::Face)];
                const TopoDS_Face face = TopoDS::Face(faces.FindKey(static_cast<int>(c.faces.front().index) + 1));
                const gp_Pnt p0(c.checks[0].point.x, c.checks[0].point.y, c.checks[0].point.z);
                const gp_Pnt p1(c.checks[1].point.x, c.checks[1].point.y, c.checks[1].point.z);
                BRepBuilderAPI_Copy copier(face, true, false);
                const TopoDS_Shape prism = BRepPrimAPI_MakePrism(copier.Shape(), gp_Vec(p0, p1)).Shape();
                if (fallsApart(prism))
                    return std::string("pulling the face in that far cuts the part in two");
            }
            return std::string();
        }
        if (c.edit == DirectEdit::ResizeLarger) {
            // checks: [0] the old wall point, [1] the new wall point beside it.
            FaceModel m(input);
            const std::vector<int>* bore = m.bore(static_cast<int>(c.faces.front().index));
            if (bore == nullptr)
                return std::string();
            const detail::HoleGeometry h = m.holeGeometry(*bore);
            const double r = 0.5 * c.newDiameter;
            const double t0 = h.tMin - (h.openMin ? r + 1.0 : 0.0);
            const double t1 = h.tMax + (h.openMax ? r + 1.0 : 0.0);
            const gp_Pnt base = h.axis.Location().Translated(gp_Vec(h.axis.Direction()) * t0);
            const TopoDS_Shape cylinder =
                BRepPrimAPI_MakeCylinder(gp_Ax2(base, h.axis.Direction()), r, t1 - t0).Shape();
            if (fallsApart(cylinder))
                return std::string("a hole that large cuts the part in two");
        }
        return std::string();
    });
    return reason ? *reason : std::string();
}

std::vector<std::string> validityFaults(const Shape& shape, int limit)
{
    auto lines = detail::guarded("testing::validityFaults", [&]() -> Result<std::vector<std::string>> {
        std::vector<std::string> out;
        if (shape.isNull())
            return out;
        BRepCheck_Analyzer analyzer(detail::occt(shape));
        if (analyzer.IsValid())
            return out;
        static const char* const kNames[] = {"vertex", "edge", "face", "solid"};
        for (const SubKind kind : {SubKind::Solid, SubKind::Face, SubKind::Edge, SubKind::Vertex}) {
            const detail::ShapeIndex& index = shape.impl->subs[detail::kindSlot(kind)];
            for (int i = 1; i <= index.Extent() && static_cast<int>(out.size()) < limit; ++i) {
                const occ::handle<BRepCheck_Result> result = analyzer.Result(index.FindKey(i));
                if (result.IsNull())
                    continue;
                std::set<std::string> found;
                const auto collect = [&](const NCollection_List<BRepCheck_Status>& statuses) {
                    for (NCollection_List<BRepCheck_Status>::Iterator it(statuses); it.More(); it.Next())
                        if (it.Value() != BRepCheck_NoError) {
                            std::ostringstream text;
                            BRepCheck::Print(it.Value(), text);
                            std::string s = text.str();
                            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                                s.pop_back();
                            found.insert(s);
                        }
                };
                collect(result->Status());
                for (result->InitContextIterator(); result->MoreShapeInContext(); result->NextShapeInContext())
                    collect(result->StatusOnShape());
                if (found.empty())
                    continue;
                std::string line = std::string(kNames[detail::kindSlot(kind)]) + " " + std::to_string(i - 1) + ":";
                for (const std::string& s : found)
                    line += " " + s;
                out.push_back(line);
            }
        }
        return out;
    });
    return lines ? *lines : std::vector<std::string>{"(the check failed: " + lines.error().message + ")"};
}

RawDirectEditHistory::RawDirectEditHistory()
{
    detail::rawDirectEditHistory().fetch_add(1, std::memory_order_relaxed);
}

RawDirectEditHistory::~RawDirectEditHistory()
{
    detail::rawDirectEditHistory().fetch_sub(1, std::memory_order_relaxed);
}

void waitForAbandonedEdits()
{
    detail::waitForIoThreads();
}

int runningEditThreads()
{
    return detail::runningIoThreads();
}

EditEnd::EditEnd() : ticks_(std::make_shared<std::atomic<std::int64_t>>(detail::kEditRunning)) {}

std::optional<std::chrono::steady_clock::time_point> EditEnd::time() const
{
    const std::int64_t ticks = ticks_->load();
    if (ticks == detail::kEditRunning)
        return std::nullopt;
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ticks));
}

EditEndScope::EditEndScope(const EditEnd& end) : previous_(detail::editEndRecord())
{
    detail::editEndRecord() = end.ticks_;
}

EditEndScope::~EditEndScope()
{
    detail::editEndRecord() = previous_;
}

} // namespace ray::kernel::testing
