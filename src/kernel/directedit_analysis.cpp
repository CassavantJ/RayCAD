// Face analysis for the direct edits (task T-080). See directedit_internal.h for the rules.
#include "directedit_internal.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace ray::kernel::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;
// cos(2 degrees): normals closer than this are tangent (spike S9).
constexpr double kTangentCos = 0.99939082701909576;
// A fillet spans less than 175 degrees.
constexpr double kFilletMaxSpan = 175.0 * kPi / 180.0;
// Angular spans of a bore's faces add up to a whole turn, less this.
constexpr double kTurnSlack = 1e-4;
// Parallel axes: within this angle (rad).
constexpr double kParallelAngle = 1e-6;

const ShapeIndex& faceIndex(const Shape& shape)
{
    return shape.impl->subs[kindSlot(SubKind::Face)];
}

const ShapeIndex& edgeIndex(const Shape& shape)
{
    return shape.impl->subs[kindSlot(SubKind::Edge)];
}

bool normalAt(const TopoDS_Face& face, double u, double v, gp_Pnt& point, gp_Dir& normal)
{
    const occ::handle<Geom_Surface> surface = BRep_Tool::Surface(face);
    if (surface.IsNull())
        return false;
    GeomLProp_SLProps props(surface, u, v, 1, 1e-9);
    if (!props.IsNormalDefined())
        return false;
    point = props.Value();
    normal = props.Normal();
    if (face.Orientation() == TopAbs_REVERSED)
        normal.Reverse();
    return true;
}

} // namespace

bool facePointAt(const TopoDS_Face& face, double fu, double fv, gp_Pnt& point, gp_Dir& normal)
{
    double u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const gp_Pnt2d uv(u0 + fu * (u1 - u0), v0 + fv * (v1 - v0));
    BRepClass_FaceClassifier classifier(face, uv, 1e-7);
    return classifier.State() == TopAbs_IN && normalAt(face, uv.X(), uv.Y(), point, normal);
}

bool interiorPoint(const TopoDS_Face& face, gp_Pnt& point, gp_Dir& normal)
{
    static constexpr std::array<double, 9> kFractions{0.5, 0.3, 0.7, 0.2, 0.8, 0.4, 0.6, 0.1, 0.9};
    for (const double a : kFractions)
        for (const double b : kFractions)
            if (facePointAt(face, a, b, point, normal))
                return true;
    return false;
}

gp_Pnt footOnAxis(const gp_Ax1& axis, const gp_Pnt& point)
{
    return axis.Location().Translated(gp_Vec(axis.Direction()) * axialParameter(axis, point));
}

double axialParameter(const gp_Ax1& axis, const gp_Pnt& point)
{
    return gp_Vec(axis.Location(), point).Dot(gp_Vec(axis.Direction()));
}

double radialDistance(const gp_Ax1& axis, const gp_Pnt& point)
{
    return footOnAxis(axis, point).Distance(point);
}

PointState classifyPointFast(const TopoDS_Shape& solid, const gp_Pnt& point, double tolerance)
{
    BRepClass3d_SolidClassifier classifier(solid, point, tolerance);
    switch (classifier.State()) {
    case TopAbs_IN:
        return PointState::In;
    case TopAbs_OUT:
        return PointState::Out;
    case TopAbs_ON:
        return PointState::On;
    default:
        return PointState::Unknown;
    }
}

PointState classifyPoint(const TopoDS_Shape& solid, const gp_Pnt& point, double onTolerance)
{
    PointState state = classifyPointFast(solid, point, onTolerance);
    // Distance to the boundary (against the solid itself it would be 0 for any point inside).
    double distance = -1.0;
    for (TopExp_Explorer shell(solid, TopAbs_SHELL); shell.More(); shell.Next()) {
        BRepExtrema_DistShapeShape measure(BRepBuilderAPI_MakeVertex(point).Vertex(), shell.Current());
        if (measure.IsDone() && measure.NbSolution() > 0 && (distance < 0.0 || measure.Value() < distance))
            distance = measure.Value();
    }
    if (distance >= 0.0) {
        if (distance < onTolerance)
            return PointState::On;
        if (state == PointState::On)
            return PointState::Unknown;
    }
    return state;
}

FaceModel::FaceModel(const Shape& shape) : shape_(shape)
{
    const int count = faceIndex(shape).Extent();
    facts_.resize(static_cast<std::size_t>(count));
    links_.resize(static_cast<std::size_t>(count));
    area_.assign(static_cast<std::size_t>(count), -1.0);
    boreOf_.assign(static_cast<std::size_t>(count), -2);
    fillet_.assign(static_cast<std::size_t>(count), -1);
    corner_.assign(static_cast<std::size_t>(count), -1);
    Bnd_Box box;
    BRepBndLib::Add(occt(shape), box);
    if (!box.IsVoid())
        diagonal_ = std::sqrt(box.SquareExtent());
    lengthTolerance_ = 1e-6 * std::max(1.0, diagonal_);
}

TopoDS_Face FaceModel::face(int index) const
{
    return TopoDS::Face(faceIndex(shape_).FindKey(index + 1));
}

bool FaceModel::coaxial(const gp_Ax1& a, const gp_Ax1& b) const
{
    return a.Direction().IsParallel(b.Direction(), kParallelAngle) &&
           radialDistance(a, b.Location()) < lengthTolerance_;
}

const FaceFacts& FaceModel::facts(int index)
{
    std::optional<FaceFacts>& slot = facts_[static_cast<std::size_t>(index)];
    if (slot)
        return *slot;
    FaceFacts x;
    const TopoDS_Face f = face(index);
    const BRepAdaptor_Surface surface(f);
    x.type = surface.GetType();
    BRepTools::UVBounds(f, x.uMin, x.uMax, x.vMin, x.vMax);
    x.hasPoint = interiorPoint(f, x.point, x.normal);
    switch (x.type) {
    case GeomAbs_Cylinder:
        x.axis = surface.Cylinder().Axis();
        x.radius = surface.Cylinder().Radius();
        break;
    case GeomAbs_Cone:
        x.axis = surface.Cone().Axis();
        break;
    case GeomAbs_Torus:
        x.axis = surface.Torus().Axis();
        x.radius = surface.Torus().MajorRadius();
        x.minorRadius = surface.Torus().MinorRadius();
        break;
    case GeomAbs_Sphere:
        x.axis = gp_Ax1(surface.Sphere().Location(), gp_Dir(0, 0, 1));
        x.radius = surface.Sphere().Radius();
        break;
    default:
        break;
    }
    if (x.hasPoint) {
        if (x.type == GeomAbs_Cylinder || x.type == GeomAbs_Cone) {
            const gp_Vec out(footOnAxis(x.axis, x.point), x.point);
            x.concave = out.Dot(gp_Vec(x.normal)) < 0.0;
        } else if (x.type == GeomAbs_Torus) {
            // Direction from the small circle's centre to the point, against the normal.
            const gp_Vec along(x.axis.Direction());
            const gp_Vec q(x.axis.Location(), x.point);
            const gp_Vec inPlane = q - along * q.Dot(along);
            if (inPlane.Magnitude() > 1e-12) {
                const gp_Pnt centre = x.axis.Location().Translated(inPlane.Normalized() * x.radius);
                x.concave = gp_Vec(centre, x.point).Dot(gp_Vec(x.normal)) < 0.0;
            }
        } else if (x.type == GeomAbs_Sphere) {
            x.concave = gp_Vec(x.axis.Location(), x.point).Dot(gp_Vec(x.normal)) < 0.0;
        }
    }
    slot = x;
    return *slot;
}

double FaceModel::area(int index)
{
    double& value = area_[static_cast<std::size_t>(index)];
    if (value < 0.0) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(face(index), props);
        value = props.Mass();
    }
    return value;
}

const std::vector<FaceLink>& FaceModel::links(int index)
{
    std::optional<std::vector<FaceLink>>& slot = links_[static_cast<std::size_t>(index)];
    if (slot)
        return *slot;
    std::vector<FaceLink> out;
    const TopoDS_Face self = face(index);
    const ShapeIndex& faces = faceIndex(shape_);
    const ShapeIndex& edges = edgeIndex(shape_);
    const AncestorIndex& edgeFaces = shape_.impl->ancestors(SubKind::Edge, SubKind::Face);
    std::set<int> seenEdges;
    for (TopExp_Explorer it(self, TopAbs_EDGE); it.More(); it.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(it.Current());
        if (BRep_Tool::Degenerated(edge))
            continue;
        const int e = edges.FindIndex(edge) - 1;
        if (e < 0 || !seenEdges.insert(e).second)
            continue;
        const int key = edgeFaces.FindIndex(edge);
        if (key == 0)
            continue;
        std::vector<int> around;
        for (const TopoDS_Shape& f : edgeFaces.FindFromIndex(key)) {
            const int k = faces.FindIndex(f) - 1;
            if (k >= 0 && std::find(around.begin(), around.end(), k) == around.end())
                around.push_back(k);
        }
        if (around.size() != 2 || std::find(around.begin(), around.end(), index) == around.end())
            continue;
        FaceLink link;
        link.edge = e;
        link.other = around[0] == index ? around[1] : around[0];
        const BRepAdaptor_Curve curve(edge);
        link.line = curve.GetType() == GeomAbs_Line;
        link.circle = curve.GetType() == GeomAbs_Circle;
        double first = 0.0, last = 0.0;
        BRep_Tool::Range(edge, first, last);
        const double middle = 0.5 * (first + last);
        link.middle = curve.Value(middle);
        try {
            const TopoDS_Face other = face(link.other);
            double a0 = 0.0, a1 = 0.0, b0 = 0.0, b1 = 0.0;
            const occ::handle<Geom2d_Curve> pa = BRep_Tool::CurveOnSurface(edge, self, a0, a1);
            const occ::handle<Geom2d_Curve> pb = BRep_Tool::CurveOnSurface(edge, other, b0, b1);
            if (!pa.IsNull() && !pb.IsNull()) {
                const gp_Pnt2d ua = pa->Value(middle);
                const gp_Pnt2d ub = pb->Value(middle);
                gp_Pnt p1, p2;
                gp_Dir n1, n2;
                if (normalAt(self, ua.X(), ua.Y(), p1, n1) && normalAt(other, ub.X(), ub.Y(), p2, n2)) {
                    link.normalDot = n1.Dot(n2);
                    link.smooth = link.normalDot > kTangentCos;
                }
            }
        } catch (const Standard_Failure&) {
            // Not measurable: neither smooth nor of known angle.
        }
        out.push_back(link);
    }
    slot = std::move(out);
    return *slot;
}

const std::vector<int>* FaceModel::bore(int index)
{
    int& known = boreOf_[static_cast<std::size_t>(index)];
    if (known >= 0)
        return &bores_[static_cast<std::size_t>(known)];
    if (known == -1)
        return nullptr;
    known = -1;
    const FaceFacts seed = facts(index);
    if (seed.type != GeomAbs_Cylinder || !seed.concave)
        return nullptr;
    std::vector<int> group{index};
    std::vector<int> todo{index};
    std::set<int> seen{index};
    while (!todo.empty()) {
        const int c = todo.back();
        todo.pop_back();
        for (const FaceLink& link : links(c)) {
            if (seen.count(link.other) != 0)
                continue;
            const FaceFacts& y = facts(link.other);
            if (y.type != GeomAbs_Cylinder || !y.concave || std::abs(y.radius - seed.radius) > lengthTolerance_ ||
                !coaxial(seed.axis, y.axis))
                continue;
            seen.insert(link.other);
            group.push_back(link.other);
            todo.push_back(link.other);
        }
    }
    double span = 0.0;
    for (const int k : group)
        span += facts(k).uMax - facts(k).uMin;
    if (span < 2.0 * kPi - kTurnSlack) {
        for (const int k : group)
            boreOf_[static_cast<std::size_t>(k)] = -1;
        return nullptr;
    }
    std::sort(group.begin(), group.end());
    const int id = static_cast<int>(bores_.size());
    for (const int k : group)
        boreOf_[static_cast<std::size_t>(k)] = id;
    bores_.push_back(std::move(group));
    return &bores_.back();
}

std::vector<int> FaceModel::withCaps(std::vector<int> set)
{
    for (bool grew = true; grew;) {
        grew = false;
        const std::set<int> in(set.begin(), set.end());
        std::set<int> candidates;
        for (const int k : set)
            for (const FaceLink& link : links(k))
                if (in.count(link.other) == 0)
                    candidates.insert(link.other);
        for (const int c : candidates) {
            const std::vector<FaceLink>& around = links(c);
            bool all = !around.empty();
            for (const FaceLink& link : around)
                if (in.count(link.other) == 0)
                    all = false;
            if (all) {
                set.push_back(c);
                grew = true;
            }
        }
    }
    std::sort(set.begin(), set.end());
    set.erase(std::unique(set.begin(), set.end()), set.end());
    return set;
}

std::vector<int> FaceModel::stack(const std::vector<int>& bore)
{
    if (bore.empty())
        return {};
    const gp_Ax1 axis = facts(bore.front()).axis;
    const auto admissible = [&](int k) {
        const FaceFacts& y = facts(k);
        if (y.type == GeomAbs_Cylinder || y.type == GeomAbs_Cone)
            return y.concave && coaxial(axis, y.axis);
        if (y.type == GeomAbs_Plane) {
            const std::vector<FaceLink>& around = links(k);
            if (!y.hasPoint || !y.normal.IsParallel(axis.Direction(), kParallelAngle) || around.empty())
                return false;
            return std::all_of(around.begin(), around.end(), [](const FaceLink& l) { return l.circle; });
        }
        return false;
    };
    std::set<int> in(bore.begin(), bore.end());
    std::vector<int> todo(bore.begin(), bore.end());
    while (!todo.empty()) {
        const int c = todo.back();
        todo.pop_back();
        for (const FaceLink& link : links(c))
            if (in.count(link.other) == 0 && admissible(link.other)) {
                in.insert(link.other);
                todo.push_back(link.other);
            }
    }
    // Planes that touch anything outside the set are not part of the hole (the top of a boss).
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = in.begin(); it != in.end();) {
            bool drop = false;
            if (facts(*it).type == GeomAbs_Plane)
                for (const FaceLink& link : links(*it))
                    if (in.count(link.other) == 0)
                        drop = true;
            if (drop) {
                it = in.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
    // What is still connected to the bore.
    std::set<int> keep(bore.begin(), bore.end());
    todo.assign(bore.begin(), bore.end());
    while (!todo.empty()) {
        const int c = todo.back();
        todo.pop_back();
        for (const FaceLink& link : links(c))
            if (in.count(link.other) != 0 && keep.count(link.other) == 0) {
                keep.insert(link.other);
                todo.push_back(link.other);
            }
    }
    return withCaps(std::vector<int>(keep.begin(), keep.end()));
}

HoleGeometry FaceModel::holeGeometry(const std::vector<int>& bore)
{
    HoleGeometry h;
    const FaceFacts& first = facts(bore.front());
    h.axis = first.axis;
    h.radius = first.radius;
    h.wall = first.point;
    h.tMin = 1e300;
    h.tMax = -1e300;
    constexpr int kSamples = 32;
    const auto sampleEdges = [](const TopoDS_Face& f, int samples, auto&& visit) {
        for (TopExp_Explorer it(f, TopAbs_EDGE); it.More(); it.Next()) {
            const TopoDS_Edge edge = TopoDS::Edge(it.Current());
            if (BRep_Tool::Degenerated(edge))
                continue;
            const BRepAdaptor_Curve curve(edge);
            const double a = curve.FirstParameter();
            const double b = curve.LastParameter();
            for (int i = 0; i <= samples; ++i)
                visit(curve.Value(a + (b - a) * i / samples));
        }
    };
    for (const int k : bore)
        sampleEdges(face(k), kSamples, [&](const gp_Pnt& p) {
            const double t = axialParameter(h.axis, p);
            h.tMin = std::min(h.tMin, t);
            h.tMax = std::max(h.tMax, t);
        });
    const double length = h.tMax - h.tMin;
    const std::set<int> in(bore.begin(), bore.end());
    for (const int k : bore)
        for (const FaceLink& link : links(k)) {
            if (in.count(link.other) != 0 || !facts(link.other).hasPoint)
                continue;
            const double t = axialParameter(h.axis, link.middle);
            double widest = 0.0;
            sampleEdges(face(link.other), kSamples / 2,
                        [&](const gp_Pnt& p) { widest = std::max(widest, radialDistance(h.axis, p)); });
            const bool closed = widest < h.radius + 1e-5 * std::max(1.0, h.radius);
            if (!closed)
                continue;
            if (t - h.tMin < 0.25 * length)
                h.openMin = false;
            else if (h.tMax - t < 0.25 * length)
                h.openMax = false;
        }
    return h;
}

bool FaceModel::endsOnPlainCircles(const std::vector<int>& bore)
{
    const std::set<int> in(bore.begin(), bore.end());
    for (const int k : bore)
        for (const FaceLink& link : links(k))
            if (in.count(link.other) == 0 && !link.circle)
                return false;
    return true;
}

bool FaceModel::isFillet(int index)
{
    std::int8_t& known = fillet_[static_cast<std::size_t>(index)];
    if (known >= 0)
        return known == 1;
    known = 0;
    const FaceFacts x = facts(index);
    if (!x.hasPoint)
        return false;
    std::set<int> others;
    if (x.type == GeomAbs_Cylinder) {
        if (x.uMax - x.uMin > kFilletMaxSpan || bore(index) != nullptr)
            return false;
        for (const FaceLink& link : links(index)) {
            const FaceFacts& y = facts(link.other);
            const bool sameCylinder = y.type == GeomAbs_Cylinder && std::abs(y.radius - x.radius) < lengthTolerance_ &&
                                      coaxial(x.axis, y.axis);
            if (link.smooth && link.line && !sameCylinder)
                others.insert(link.other);
        }
    } else if (x.type == GeomAbs_Torus) {
        if (x.vMax - x.vMin > kFilletMaxSpan)
            return false;
        for (const FaceLink& link : links(index)) {
            const FaceFacts& y = facts(link.other);
            const bool sameTorus = y.type == GeomAbs_Torus && std::abs(y.radius - x.radius) < lengthTolerance_ &&
                                   std::abs(y.minorRadius - x.minorRadius) < lengthTolerance_ &&
                                   coaxial(x.axis, y.axis);
            if (link.smooth && !sameTorus)
                others.insert(link.other);
        }
    }
    known = others.size() >= 2 ? 1 : 0;
    return known == 1;
}

bool FaceModel::isCornerPatch(int index)
{
    std::int8_t& known = corner_[static_cast<std::size_t>(index)];
    if (known >= 0)
        return known == 1;
    known = 0;
    const GeomAbs_SurfaceType type = facts(index).type;
    if (type == GeomAbs_Plane || type == GeomAbs_Cylinder || type == GeomAbs_Cone || isFillet(index))
        return false;
    const std::vector<FaceLink> around = links(index);
    if (around.empty())
        return false;
    std::set<int> fillets;
    for (const FaceLink& link : around) {
        if (!link.smooth)
            return false;
        if (isFillet(link.other))
            fillets.insert(link.other);
    }
    known = fillets.size() >= 2 ? 1 : 0;
    return known == 1;
}

std::vector<int> FaceModel::blendRun(int index)
{
    if (!isBlendFace(index))
        return {};
    std::vector<int> run{index};
    std::vector<int> todo{index};
    std::set<int> seen{index};
    while (!todo.empty()) {
        const int c = todo.back();
        todo.pop_back();
        const std::vector<FaceLink> around = links(c);
        for (const FaceLink& link : around)
            if (seen.insert(link.other).second && isBlendFace(link.other)) {
                run.push_back(link.other);
                todo.push_back(link.other);
            }
    }
    std::sort(run.begin(), run.end());
    return run;
}

} // namespace ray::kernel::detail
