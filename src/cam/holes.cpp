// Hole recognition and drilling order (cam.h, task T-034): recognizeHoles and orderHolesShortestPath.
//
// RECOGNITION reads the B-rep through the kernel queries only (surfaceInfo, faceSample, curveInfo, adjacent;
// makeCylinder and minDistance for the clearance check), in the setup frame: a point p of the model is R p + t in the
// WCS.
//   1. Pieces: the cylinder and cone faces with their axis along setup Z (|axis x Z| <= kAxisSin) that are concave:
//      faceSample's outward normal points towards the axis, so the material is outside (a hole wall, not a boss). The
//      edges of a piece are rings (circles coaxial with it, square to Z, of its radius at their height), lines along
//      the axis (seams, and the joints where an exporter split a cylinder) and other edges (where a cross passage cuts
//      it, a drill point's apex). Its height range comes from the rings and the ends of the other edges.
//   2. Segments: pieces of one surface, merged: adjacent pieces of one cylinder (radius within kPosTol) or one cone,
//      coaxial within kPosTol; and pieces of one cylinder on both sides of a cross passage (both adjacent to a
//      cylinder face whose axis is not along Z).
//   3. Tops: a segment whose top rings close a full turn (kTurnTol) and border only up-facing planes square to Z, none
//      of them the floor of a wider coaxial step. This is the angular coverage rule: a fillet (a quarter turn), the end
//      of a slot (half a turn), a cut-out on an edge or the corner of a pocket has no full ring at its top and is
//      never a hole. It also rejects a hole whose top is cut by anything (it is then not a v1 hole).
//   4. The walk down from each top, each segment used once. Below a segment's bottom ring:
//        - the next segment: a cone narrowing downwards, or a cylinder of the radius a cone ends at;
//        - a planar step: an up-facing annulus square to Z whose inner ring is the next segment's top (counterbore);
//        - a flat floor: an up-facing disc square to Z (bottom Flat);
//        - exit faces only (bottom Through).
//      A cone narrowing to its apex, or cut off by other faces before it gets there, is a drill point (bottom Conical;
//      its depth runs to the apex its geometry gives). A bottom ring that does not close a turn (a passage cuts it), or
//      a segment without one: Through when every face beyond it is an exit face. Exit faces: planes facing downwards
//      (the far side; the floor of a counterbore made from the far side), concave cylinders and cones whose axis is not
//      along Z (a cross passage), and coaxial cones that widen downwards (a countersink made from the far side).
//      Anything else (a torus at a pocket's floor, a sphere, a free-form face, a convex face) and the stack is not a v1
//      hole. A stack holds at least one cylinder.
//   5. Clearance: nothing may lie above the top inside the top circle. Faces whose box in the WCS (exact for a frame
//      that permutes the model axes, enclosing otherwise) overlaps the top circle's square and reaches more than
//      kAboveTol above the top are tested with minDistance against a cylinder of the top radius less kProbeInset, from
//      kProbeLift above the top to above the part; a plane square to Z is decided by its height alone.
//   6. Classification (cam.h RecognizedHole): the bore is the lowest cylinder. Counterbored: a cylinder above it is
//      wider. A cone at the entry (above the first cylinder) is a chamfer when its radial width is at most
//      max(kChamferMaxWidthMm, kChamferMaxFraction x its lower diameter), else a countersink: an edge break is small
//      next to the hole, a countersink is not (1 mm on a 12 mm hole is a chamfer, 9 on 4.5 mm a countersink). A cone
//      below a cylinder is a conical step (the point of the larger drill of a stepped hole) and is neither.
// Determinism: values come from the exact geometry in a fixed order; heights, positions and diameters are rounded to
// whole nanometres in the WCS (toNm, ties to even); holes are sorted by (x, y, zTop, axisSub).
//
// ORDERING (orderHolesShortestPath): an open path from `start`. Nearest neighbour first (O(n^2), ties to the lower
// index), then 2-opt moves found through 8 nearest neighbours per point (lists from an x-sorted sweep), first
// improvement, at most kMaxOptPasses passes over the path; each pass is O(n K) tests plus O(n) per reversal made. Every
// accepted move shortens the path, so the result is never longer than the nearest-neighbour path. Coordinates are
// integer nanometres held in doubles (exact below 2^53 nm), distances IEEE double with dmath::sqrt in a fixed order.
#include "state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <set>
#include <tuple>
#include <utility>

namespace ray::cam {

namespace {

// ---------- Tolerances ----------
constexpr double kPi = 3.14159265358979323846;
constexpr double kAxisSin = 1e-4;           // an axis or normal is along setup Z when |d x Z| <= this (0.0057 deg)
constexpr double kPosTol = 1e-3;            // mm: coaxial axes, equal radii, equal heights
constexpr double kTurnTol = 1e-6;           // relative: rings close a turn when their angles add up to 2 pi (1 - this)
constexpr double kMinRadius = 1e-3;         // mm: a ring smaller than this is a cone's apex
constexpr double kConeSlopeTol = 1e-6;      // two cones are one surface when dr/dz agrees this closely (relative)
constexpr double kChamferMaxWidthMm = 0.5;  // a cone above the bore at most this wide (radially) is a chamfer ...
constexpr double kChamferMaxFraction = 0.2; // ... or at most this fraction of its lower diameter
constexpr double kProbeInset = 0.01;        // mm: the clearance probe is this much smaller in radius than the top
constexpr double kProbeLift = 0.01;         // mm: and starts this far above the top
constexpr double kAboveTol = 1e-3;          // mm: a face reaching less than this above the top does not block it
constexpr double kTouching = 1e-6;          // mm: a minDistance at most this means the face meets the probe
constexpr double kRigidTol = 1e-9;          // the frame's rows are orthonormal to this

using kernel::SubId;
using kernel::SubKind;

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

// ---------- Vectors (IEEE arithmetic only; ray_cam is built without floating-point contraction) ----------

Vec3 add(Vec3 a, Vec3 b)
{
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 sub(Vec3 a, Vec3 b)
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 mul(Vec3 a, double s)
{
    return Vec3{a.x * s, a.y * s, a.z * s};
}
double dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double length(Vec3 a)
{
    return dmath::sqrt(dot(a, a));
}
double planar(double x, double y)
{
    return dmath::sqrt(x * x + y * y);
}

// ---------- The frame ----------

struct Frame {
    Vec3 x, y, z; // rows of R: the WCS axes in model coordinates
    Vec3 t;       // the translation
};

Vec3 toWcs(const Frame& f, Vec3 p)
{
    return Vec3{dot(f.x, p) + f.t.x, dot(f.y, p) + f.t.y, dot(f.z, p) + f.t.z};
}
Vec3 dirToWcs(const Frame& f, Vec3 d)
{
    return Vec3{dot(f.x, d), dot(f.y, d), dot(f.z, d)};
}
Vec3 toModel(const Frame& f, Vec3 q) // p = R^T (q - t)
{
    const Vec3 u = sub(q, f.t);
    return add(add(mul(f.x, u.x), mul(f.y, u.y)), mul(f.z, u.z));
}

Result<Frame> frameOf(const Xform& xf)
{
    const double* m = xf.m;
    for (int i = 0; i < 12; ++i) {
        if (!std::isfinite(m[i])) {
            return failure(ErrorCode::InvalidArgument, "The setup frame for hole recognition is not finite");
        }
    }
    Frame f{Vec3{m[0], m[1], m[2]}, Vec3{m[4], m[5], m[6]}, Vec3{m[8], m[9], m[10]}, Vec3{m[3], m[7], m[11]}};
    const Vec3 rows[] = {f.x, f.y, f.z};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (std::fabs(dot(rows[i], rows[j]) - (i == j ? 1.0 : 0.0)) > kRigidTol) {
                return failure(ErrorCode::InvalidArgument,
                               "The setup frame for hole recognition is not a rigid move (it scales or skews)");
            }
        }
    }
    const double det = f.x.x * (f.y.y * f.z.z - f.y.z * f.z.y) - f.x.y * (f.y.x * f.z.z - f.y.z * f.z.x) +
                       f.x.z * (f.y.x * f.z.y - f.y.y * f.z.x);
    if (!(det > 0.0)) {
        return failure(ErrorCode::InvalidArgument, "The setup frame for hole recognition mirrors the part");
    }
    return f;
}

// A WCS direction along setup Z (either way).
bool alongZ(Vec3 d)
{
    const double len = length(d);
    return len > 0.0 && planar(d.x, d.y) <= kAxisSin * len;
}

bool near(double a, double b, double tol = kPosTol)
{
    return std::fabs(a - b) <= tol;
}

// Whole nanometres, then mm: what every reported length is rounded to.
double roundedMm(double mm)
{
    return toMm(toNm(mm));
}

// ---------- The shape, read lazily ----------

struct PlaneInfo {
    bool square = false; // square to setup Z
    bool up = false;     // outward normal has +Z
    bool down = false;   // outward normal has -Z (any slope)
    double z = 0;        // WCS height (square planes)
};

class Model {
public:
    Model(const kernel::Shape& shape, const Frame& frame)
        : shape_(shape), frame_(frame), faces_(shape.count(SubKind::Face)), edges_(shape.count(SubKind::Edge)),
          surfaces_(faces_), samples_(faces_), planes_(faces_), faceEdges_(faces_), curves_(edges_), edgeFaces_(edges_)
    {
    }

    const kernel::Shape& shape() const { return shape_; }
    const Frame& frame() const { return frame_; }
    std::uint32_t faces() const { return faces_; }

    Result<const kernel::SurfaceInfo*> surface(std::uint32_t f)
    {
        if (!surfaces_[f]) {
            auto s = kernel::surfaceInfo(shape_, SubId{SubKind::Face, f});
            if (!s) {
                return failure(ErrorCode::KernelFailure,
                               "Face " + std::to_string(f) + " could not be read: " + s.error().message);
            }
            surfaces_[f] = *s;
        }
        return &*surfaces_[f];
    }
    Result<const kernel::FaceSample*> sample(std::uint32_t f)
    {
        if (!samples_[f]) {
            auto s = kernel::faceSample(shape_, SubId{SubKind::Face, f});
            if (!s) {
                return failure(ErrorCode::KernelFailure,
                               "The normal of face " + std::to_string(f) + " could not be read: " + s.error().message);
            }
            samples_[f] = *s;
        }
        return &*samples_[f];
    }
    Result<const kernel::CurveInfo*> curve(std::uint32_t e)
    {
        if (!curves_[e]) {
            auto c = kernel::curveInfo(shape_, SubId{SubKind::Edge, e});
            if (!c) {
                return failure(ErrorCode::KernelFailure,
                               "Edge " + std::to_string(e) + " could not be read: " + c.error().message);
            }
            curves_[e] = *c;
        }
        return &*curves_[e];
    }
    const std::vector<std::uint32_t>& edgesOf(std::uint32_t f)
    {
        if (!faceEdges_[f]) {
            std::vector<std::uint32_t> out;
            for (const SubId& e : kernel::adjacent(shape_, SubId{SubKind::Face, f}, SubKind::Edge)) {
                out.push_back(e.index);
            }
            faceEdges_[f] = std::move(out);
        }
        return *faceEdges_[f];
    }
    const std::vector<std::uint32_t>& facesOf(std::uint32_t e)
    {
        if (!edgeFaces_[e]) {
            std::vector<std::uint32_t> out;
            for (const SubId& f : kernel::adjacent(shape_, SubId{SubKind::Edge, e}, SubKind::Face)) {
                out.push_back(f.index);
            }
            edgeFaces_[e] = std::move(out);
        }
        return *edgeFaces_[e];
    }
    // The plane facts of a planar face; nullopt for any other face.
    Result<std::optional<PlaneInfo>> plane(std::uint32_t f)
    {
        if (!planes_[f]) {
            auto s = surface(f);
            if (!s) {
                return tl::unexpected(s.error());
            }
            PlaneInfo p;
            if ((*s)->type == kernel::SurfaceType::Plane) {
                auto smp = sample(f);
                if (!smp) {
                    return tl::unexpected(smp.error());
                }
                const Vec3 n = dirToWcs(frame_, (*smp)->normal);
                p.square = alongZ(n);
                p.up = n.z > 0.0;
                p.down = n.z < -kAxisSin;
                p.z = toWcs(frame_, (*smp)->point).z;
                planes_[f].emplace(p);
            } else {
                planes_[f].emplace();
            }
        }
        return *planes_[f];
    }

private:
    const kernel::Shape& shape_;
    Frame frame_;
    std::uint32_t faces_, edges_;
    std::vector<std::optional<kernel::SurfaceInfo>> surfaces_;
    std::vector<std::optional<kernel::FaceSample>> samples_;
    std::vector<std::optional<std::optional<PlaneInfo>>> planes_;
    std::vector<std::optional<std::vector<std::uint32_t>>> faceEdges_;
    std::vector<std::optional<kernel::CurveInfo>> curves_;
    std::vector<std::optional<std::vector<std::uint32_t>>> edgeFaces_;
};

// ---------- Pieces and segments ----------

struct Ring {
    std::uint32_t edge;
    double z, radius, angle;
};

struct Piece {
    std::uint32_t face = 0;
    bool cone = false;
    Vec3 p, d;             // a point of the axis and its direction (d.z > 0), WCS
    double r0 = 0, z0 = 0; // radius r0 at height z0 ...
    double k = 0;          // ... changing by k per mm of height (0 for a cylinder; > 0: narrowing downwards)
    double semi = 0;       // cone: the half angle, radians
    double zlo = 0, zhi = 0;
    std::vector<Ring> rings;
};

double radiusAt(const Piece& p, double z)
{
    return p.r0 + p.k * (z - p.z0);
}
Vec2 axisAt(const Piece& p, double z)
{
    const double t = (z - p.p.z) / p.d.z;
    return Vec2{p.p.x + p.d.x * t, p.p.y + p.d.y * t};
}
bool coaxialAt(const Piece& a, const Piece& b, double z)
{
    const Vec2 u = axisAt(a, z);
    const Vec2 v = axisAt(b, z);
    return planar(u.x - v.x, u.y - v.y) <= kPosTol;
}

// One surface: cylinder pieces of one radius, or cone pieces of one cone, on one axis.
bool sameSurface(const Piece& a, const Piece& b)
{
    if (a.cone != b.cone) {
        return false;
    }
    const double z = (a.zlo + a.zhi + b.zlo + b.zhi) / 4.0;
    if (!coaxialAt(a, b, z) || !near(radiusAt(a, z), radiusAt(b, z))) {
        return false;
    }
    return !a.cone || std::fabs(a.k - b.k) <= kConeSlopeTol * std::max(1.0, std::fabs(a.k));
}

struct Segment {
    std::vector<int> pieces; // indices into the piece list, ascending (so by face)
    bool cone = false;
    double k = 0;
    double zlo = 0, zhi = 0;
    double rTop = 0, rBottom = 0;
    double topAngle = 0, bottomAngle = 0;
    std::vector<std::uint32_t> topNbrs, bottomNbrs, otherNbrs; // faces beyond the top rings, bottom rings, the rest
    bool hasBottomRing = false;
};

double fullTurn()
{
    return 2.0 * kPi * (1.0 - kTurnTol);
}

struct Recognizer {
    Model& m;
    JobContext& ctx;
    std::vector<Piece> pieces;
    std::vector<int> pieceOfFace; // -1 when the face is not a piece
    std::vector<int> segOfPiece;
    std::vector<Segment> segs;

    // ---- step 1 ----
    Status findPieces()
    {
        const Frame& f = m.frame();
        pieceOfFace.assign(m.faces(), -1);
        for (std::uint32_t face = 0; face < m.faces(); ++face) {
            RAY_CAM_TRY(ctx.cancel.check());
            auto s = m.surface(face);
            if (!s) {
                return tl::unexpected(s.error());
            }
            const kernel::SurfaceInfo& info = **s;
            if (info.type != kernel::SurfaceType::Cylinder && info.type != kernel::SurfaceType::Cone) {
                continue;
            }
            const Vec3 aw = dirToWcs(f, info.axis);
            if (!alongZ(aw)) {
                continue;
            }
            Piece p;
            p.face = face;
            p.cone = info.type == kernel::SurfaceType::Cone;
            p.p = toWcs(f, info.origin);
            p.d = mul(aw, (aw.z < 0.0 ? -1.0 : 1.0) / length(aw));
            p.r0 = info.radius;
            p.z0 = p.p.z;
            if (p.cone) {
                // gp_Cone: radius RefRadius + h tan(semiAngle) at the distance h along its axis; h = (z - z0) / aw.z.
                p.k = dmath::tan(info.semiAngle) / (aw.z / length(aw));
                p.semi = std::fabs(info.semiAngle);
            }
            // Concave: the outward normal points towards the axis.
            auto smp = m.sample(face);
            if (!smp) {
                return tl::unexpected(smp.error());
            }
            const Vec3 q = toWcs(f, (*smp)->point);
            const Vec3 n = dirToWcs(f, (*smp)->normal);
            const Vec2 c = axisAt(p, q.z);
            if (!((q.x - c.x) * n.x + (q.y - c.y) * n.y < 0.0)) {
                continue;
            }
            // Edges: rings and the height range.
            bool first = true;
            const auto extend = [&](double z) {
                p.zlo = first ? z : std::min(p.zlo, z);
                p.zhi = first ? z : std::max(p.zhi, z);
                first = false;
            };
            for (const std::uint32_t e : m.edgesOf(face)) {
                auto ci = m.curve(e);
                if (!ci) {
                    return tl::unexpected(ci.error());
                }
                const kernel::CurveInfo& cv = **ci;
                if (cv.type == kernel::CurveType::Circle && alongZ(dirToWcs(f, cv.axis))) {
                    const Vec3 centre = toWcs(f, cv.center);
                    const Vec2 a = axisAt(p, centre.z);
                    if (planar(centre.x - a.x, centre.y - a.y) <= kPosTol && near(cv.radius, radiusAt(p, centre.z)) &&
                        cv.radius > 0.0) {
                        const double angle = cv.closed ? 2.0 * kPi : cv.length / cv.radius;
                        p.rings.push_back(Ring{e, centre.z, cv.radius, angle});
                        extend(centre.z);
                        continue;
                    }
                }
                extend(toWcs(f, cv.start).z);
                extend(toWcs(f, cv.end).z);
            }
            if (first) {
                continue; // a face without edges: nothing to go on
            }
            pieceOfFace[face] = static_cast<int>(pieces.size());
            pieces.push_back(std::move(p));
        }
        return {};
    }

    // ---- step 2 ----
    int findRoot(std::vector<int>& parent, int i)
    {
        while (parent[static_cast<std::size_t>(i)] != i) {
            parent[static_cast<std::size_t>(i)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(i)])];
            i = parent[static_cast<std::size_t>(i)];
        }
        return i;
    }
    void unite(std::vector<int>& parent, int a, int b)
    {
        a = findRoot(parent, a);
        b = findRoot(parent, b);
        if (a != b) {
            parent[static_cast<std::size_t>(std::max(a, b))] = std::min(a, b);
        }
    }

    Status buildSegments()
    {
        const Frame& f = m.frame();
        std::vector<int> parent(pieces.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::set<std::uint32_t> passages; // cylinder faces off Z next to a piece
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            for (const std::uint32_t e : m.edgesOf(pieces[i].face)) {
                for (const std::uint32_t other : m.facesOf(e)) {
                    const int j = pieceOfFace[other];
                    if (j >= 0) {
                        if (static_cast<std::size_t>(j) != i &&
                            sameSurface(pieces[i], pieces[static_cast<std::size_t>(j)])) {
                            unite(parent, static_cast<int>(i), j);
                        }
                        continue;
                    }
                    auto s = m.surface(other);
                    if (!s) {
                        return tl::unexpected(s.error());
                    }
                    if ((*s)->type == kernel::SurfaceType::Cylinder && !alongZ(dirToWcs(f, (*s)->axis))) {
                        passages.insert(other);
                    }
                }
            }
        }
        // Pieces of one cylinder on both sides of a cross passage.
        for (const std::uint32_t x : passages) {
            std::vector<int> touching;
            for (const std::uint32_t e : m.edgesOf(x)) {
                for (const std::uint32_t other : m.facesOf(e)) {
                    if (pieceOfFace[other] >= 0) {
                        touching.push_back(pieceOfFace[other]);
                    }
                }
            }
            std::sort(touching.begin(), touching.end());
            touching.erase(std::unique(touching.begin(), touching.end()), touching.end());
            for (std::size_t a = 0; a < touching.size(); ++a) {
                for (std::size_t b = a + 1; b < touching.size(); ++b) {
                    const Piece& pa = pieces[static_cast<std::size_t>(touching[a])];
                    const Piece& pb = pieces[static_cast<std::size_t>(touching[b])];
                    if (!pa.cone && sameSurface(pa, pb)) {
                        unite(parent, touching[a], touching[b]);
                    }
                }
            }
        }
        // Segments in the order of their first piece.
        segOfPiece.assign(pieces.size(), -1);
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const int root = findRoot(parent, static_cast<int>(i));
            if (segOfPiece[static_cast<std::size_t>(root)] < 0) {
                segOfPiece[static_cast<std::size_t>(root)] = static_cast<int>(segs.size());
                segs.emplace_back();
            }
            segOfPiece[i] = segOfPiece[static_cast<std::size_t>(root)];
            segs[static_cast<std::size_t>(segOfPiece[i])].pieces.push_back(static_cast<int>(i));
        }
        for (Segment& s : segs) {
            const Piece& p0 = pieces[static_cast<std::size_t>(s.pieces.front())];
            s.cone = p0.cone;
            s.k = p0.k;
            s.zlo = p0.zlo;
            s.zhi = p0.zhi;
            for (const int i : s.pieces) {
                s.zlo = std::min(s.zlo, pieces[static_cast<std::size_t>(i)].zlo);
                s.zhi = std::max(s.zhi, pieces[static_cast<std::size_t>(i)].zhi);
            }
            s.rTop = radiusAt(p0, s.zhi);
            s.rBottom = radiusAt(p0, s.zlo);
            std::set<std::uint32_t> own;
            for (const int i : s.pieces) {
                own.insert(pieces[static_cast<std::size_t>(i)].face);
            }
            std::set<std::uint32_t> top, bottom, other, ringEdges;
            for (const int i : s.pieces) {
                const Piece& p = pieces[static_cast<std::size_t>(i)];
                for (const Ring& r : p.rings) {
                    std::set<std::uint32_t>* into = nullptr;
                    if (near(r.z, s.zhi)) {
                        s.topAngle += r.angle;
                        into = &top;
                    } else if (near(r.z, s.zlo)) {
                        s.bottomAngle += r.angle;
                        s.hasBottomRing = true;
                        into = &bottom;
                    } else {
                        continue; // a joint between pieces at mid height
                    }
                    ringEdges.insert(r.edge);
                    for (const std::uint32_t face : m.facesOf(r.edge)) {
                        if (own.count(face) == 0) {
                            into->insert(face);
                        }
                    }
                }
            }
            for (const int i : s.pieces) {
                for (const std::uint32_t e : m.edgesOf(pieces[static_cast<std::size_t>(i)].face)) {
                    if (ringEdges.count(e) != 0) {
                        continue;
                    }
                    for (const std::uint32_t face : m.facesOf(e)) {
                        if (own.count(face) == 0) {
                            other.insert(face);
                        }
                    }
                }
            }
            s.topNbrs.assign(top.begin(), top.end());
            s.bottomNbrs.assign(bottom.begin(), bottom.end());
            s.otherNbrs.assign(other.begin(), other.end());
        }
        return {};
    }

    const Piece& firstPiece(const Segment& s) const { return pieces[static_cast<std::size_t>(s.pieces.front())]; }
    int segmentOfFace(std::uint32_t face) const
    {
        const int p = pieceOfFace[face];
        return p < 0 ? -1 : segOfPiece[static_cast<std::size_t>(p)];
    }

    // The rings of planar face `face` about the axis of `s` at the plane's height; nullopt when one of its edges is not
    // such a ring (the face is then not a step or floor of this hole). Pairs of (edge, radius).
    Result<std::optional<std::vector<std::pair<std::uint32_t, double>>>> ringsAbout(std::uint32_t face,
                                                                                    const Segment& s, double z)
    {
        const Frame& f = m.frame();
        std::vector<std::pair<std::uint32_t, double>> out;
        const Piece& p = firstPiece(s);
        for (const std::uint32_t e : m.edgesOf(face)) {
            auto ci = m.curve(e);
            if (!ci) {
                return tl::unexpected(ci.error());
            }
            const kernel::CurveInfo& cv = **ci;
            if (cv.type != kernel::CurveType::Circle || !alongZ(dirToWcs(f, cv.axis))) {
                return std::optional<std::vector<std::pair<std::uint32_t, double>>>();
            }
            const Vec3 c = toWcs(f, cv.center);
            const Vec2 a = axisAt(p, c.z);
            if (!near(c.z, z) || planar(c.x - a.x, c.y - a.y) > kPosTol) {
                return std::optional<std::vector<std::pair<std::uint32_t, double>>>();
            }
            out.emplace_back(e, cv.radius);
        }
        if (out.empty()) {
            return std::optional<std::vector<std::pair<std::uint32_t, double>>>();
        }
        return std::optional<std::vector<std::pair<std::uint32_t, double>>>(std::move(out));
    }

    // Whether `face` is a face a hole may break out into (step 4).
    Result<bool> isExit(std::uint32_t face)
    {
        if (const int sg = segmentOfFace(face); sg >= 0) {
            const Segment& s = segs[static_cast<std::size_t>(sg)];
            return s.cone && s.k < 0.0; // a countersink made from the far side
        }
        auto pl = m.plane(face);
        if (!pl) {
            return tl::unexpected(pl.error());
        }
        if (*pl) {
            return (*pl)->down;
        }
        auto s = m.surface(face);
        if (!s) {
            return tl::unexpected(s.error());
        }
        const kernel::SurfaceInfo& info = **s;
        if (info.type != kernel::SurfaceType::Cylinder && info.type != kernel::SurfaceType::Cone) {
            return false;
        }
        if (alongZ(dirToWcs(m.frame(), info.axis))) {
            return false; // a convex face along Z: a boss
        }
        auto smp = m.sample(face);
        if (!smp) {
            return tl::unexpected(smp.error());
        }
        const Vec3 w = sub((*smp)->point, info.origin);
        const double al = length(info.axis);
        const Vec3 a = al > 0.0 ? mul(info.axis, 1.0 / al) : info.axis;
        const Vec3 radial = sub(w, mul(a, dot(w, a)));
        return dot((*smp)->normal, radial) < 0.0; // a concave passage
    }

    Result<bool> allExit(const std::vector<std::uint32_t>& faces)
    {
        if (faces.empty()) {
            return false;
        }
        for (const std::uint32_t face : faces) {
            auto e = isExit(face);
            if (!e) {
                return tl::unexpected(e.error());
            }
            if (!*e) {
                return false;
            }
        }
        return true;
    }

    // ---- step 3 ----
    Result<bool> isTop(const Segment& s)
    {
        if (s.cone && s.k < 0.0) {
            return false; // widens downwards: an undercut from this side
        }
        if (s.topNbrs.empty() || s.topAngle < fullTurn()) {
            return false;
        }
        for (const std::uint32_t face : s.topNbrs) {
            if (pieceOfFace[face] >= 0) {
                return false;
            }
            auto pl = m.plane(face);
            if (!pl) {
                return tl::unexpected(pl.error());
            }
            if (!*pl || !(*pl)->square || !(*pl)->up || !near((*pl)->z, s.zhi)) {
                return false;
            }
            // The floor of a wider coaxial step: a ring of it, wider than this segment, borders a piece.
            auto rings = ringsAbout(face, s, (*pl)->z);
            if (!rings) {
                return tl::unexpected(rings.error());
            }
            if (*rings) {
                for (const auto& [edge, radius] : **rings) {
                    if (radius <= s.rTop + kPosTol) {
                        continue;
                    }
                    for (const std::uint32_t other : m.facesOf(edge)) {
                        if (other != face && pieceOfFace[other] >= 0) {
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }

    // ---- step 4 ----
    struct Chain {
        std::vector<int> segs;
        std::vector<std::uint32_t> extra; // steps and the floor
        HoleBottom bottom = HoleBottom::Through;
    };

    // The single segment the faces all belong to, or -1.
    int onlySegment(const std::vector<std::uint32_t>& faces) const
    {
        int found = -1;
        for (const std::uint32_t face : faces) {
            const int s = segmentOfFace(face);
            if (s < 0 || (found >= 0 && s != found)) {
                return -1;
            }
            found = s;
        }
        return found;
    }

    // Whether segment `next` continues the hole below `cur` at height z with radius r.
    bool continues(const Segment& cur, int next, double z, double r, const std::vector<char>& used) const
    {
        if (next < 0 || used[static_cast<std::size_t>(next)] != 0) {
            return false;
        }
        const Segment& n = segs[static_cast<std::size_t>(next)];
        return &n != &cur && near(n.zhi, z) && n.rTop <= r + kPosTol && !(n.cone && n.k < 0.0) &&
               coaxialAt(firstPiece(n), firstPiece(cur), z);
    }

    Result<std::optional<Chain>> walk(int top, std::vector<char>& used)
    {
        Chain chain;
        int cur = top;
        while (true) {
            used[static_cast<std::size_t>(cur)] = 1;
            chain.segs.push_back(cur);
            const Segment& s = segs[static_cast<std::size_t>(cur)];
            if (s.cone && (!s.hasBottomRing || s.rBottom <= kMinRadius)) {
                chain.bottom = HoleBottom::Conical; // to the apex, or cut off on the way
                break;
            }
            if (!s.hasBottomRing || s.bottomAngle < fullTurn()) {
                // The bottom ring is cut (a passage) or missing: through if everything beyond it is an exit.
                std::vector<std::uint32_t> beyond = s.bottomNbrs;
                beyond.insert(beyond.end(), s.otherNbrs.begin(), s.otherNbrs.end());
                std::sort(beyond.begin(), beyond.end());
                beyond.erase(std::unique(beyond.begin(), beyond.end()), beyond.end());
                auto exit = allExit(beyond);
                if (!exit) {
                    return tl::unexpected(exit.error());
                }
                if (!*exit) {
                    return std::optional<Chain>();
                }
                chain.bottom = HoleBottom::Through;
                break;
            }
            const std::vector<std::uint32_t>& below = s.bottomNbrs;
            // The next segment, directly.
            const int next = onlySegment(below);
            if (continues(s, next, s.zlo, s.rBottom, used)) {
                cur = next;
                continue;
            }
            // A floor or a step.
            if (below.size() == 1 && segmentOfFace(below[0]) < 0) {
                const std::uint32_t face = below[0];
                auto pl = m.plane(face);
                if (!pl) {
                    return tl::unexpected(pl.error());
                }
                if (*pl && (*pl)->square && (*pl)->up && near((*pl)->z, s.zlo)) {
                    auto rings = ringsAbout(face, s, (*pl)->z);
                    if (!rings) {
                        return tl::unexpected(rings.error());
                    }
                    if (!*rings) {
                        return std::optional<Chain>(); // something else on that face: not a v1 hole
                    }
                    std::vector<std::uint32_t> inner;
                    double innerRadius = 0;
                    for (const auto& [edge, radius] : **rings) {
                        if (near(radius, s.rBottom)) {
                            continue;
                        }
                        if (radius > s.rBottom) {
                            return std::optional<Chain>();
                        }
                        innerRadius = radius;
                        for (const std::uint32_t other : m.facesOf(edge)) {
                            if (other != face) {
                                inner.push_back(other);
                            }
                        }
                    }
                    chain.extra.push_back(face);
                    if (inner.empty()) {
                        chain.bottom = HoleBottom::Flat;
                        break;
                    }
                    std::sort(inner.begin(), inner.end());
                    inner.erase(std::unique(inner.begin(), inner.end()), inner.end());
                    const int step = onlySegment(inner);
                    if (!continues(s, step, s.zlo, innerRadius, used) ||
                        !near(segs[static_cast<std::size_t>(step)].rTop, innerRadius)) {
                        return std::optional<Chain>();
                    }
                    cur = step;
                    continue;
                }
            }
            // Breaking out.
            auto exit = allExit(below);
            if (!exit) {
                return tl::unexpected(exit.error());
            }
            if (!*exit) {
                return std::optional<Chain>();
            }
            chain.bottom = HoleBottom::Through;
            break;
        }
        const bool hasCylinder = std::any_of(chain.segs.begin(), chain.segs.end(),
                                             [&](int i) { return !segs[static_cast<std::size_t>(i)].cone; });
        if (!hasCylinder) {
            return std::optional<Chain>();
        }
        return std::optional<Chain>(std::move(chain));
    }

    // ---- steps 5 and 6 ----
    RecognizedHole describe(const Chain& chain)
    {
        RecognizedHole h{};
        h.through = chain.bottom == HoleBottom::Through;
        h.bottom = chain.bottom;
        std::set<std::uint32_t> faces(chain.extra.begin(), chain.extra.end());
        for (std::size_t i = 0; i < chain.segs.size(); ++i) {
            const Segment& s = segs[static_cast<std::size_t>(chain.segs[i])];
            for (const int p : s.pieces) {
                faces.insert(pieces[static_cast<std::size_t>(p)].face);
            }
            HoleSegment out{};
            out.conical = s.cone;
            out.diameter = roundedMm(2.0 * s.rTop);
            out.zTop = toNm(s.zhi);
            const bool point = s.cone && i + 1 == chain.segs.size() && chain.bottom == HoleBottom::Conical;
            if (point) {
                const Piece& p = firstPiece(s);
                out.zBottom = toNm(p.z0 - p.r0 / p.k); // the apex
                out.bottomDiameter = 0;
            } else {
                out.zBottom = toNm(s.zlo);
                out.bottomDiameter = roundedMm(2.0 * s.rBottom);
            }
            out.depth = toMm(out.zTop - out.zBottom);
            out.coneAngleDeg = s.cone ? std::nearbyint(firstPiece(s).semi * 360.0 / kPi * 1e9) / 1e9 : 0.0;
            h.segments.push_back(out);
            if (!s.cone) {
                h.bore = i;
            }
        }
        const Segment& bore = segs[static_cast<std::size_t>(chain.segs[h.bore])];
        const Piece& bp = firstPiece(bore);
        h.zTop = h.segments.front().zTop;
        h.zBottom = h.segments.back().zBottom;
        const Vec2 c = axisAt(bp, segs[static_cast<std::size_t>(chain.segs.front())].zhi);
        h.x = toNm(c.x);
        h.y = toNm(c.y);
        h.top = Vec3{toMm(h.x), toMm(h.y), toMm(h.zTop)};
        const kernel::SurfaceInfo& info = **m.surface(bp.face); // read in findPieces
        const double al = length(info.axis);
        h.axis = mul(info.axis, (dot(info.axis, m.frame().z) < 0.0 ? -1.0 : 1.0) / al);
        h.axisSub = SubId{SubKind::Face, bp.face};
        h.faces.reserve(faces.size());
        for (const std::uint32_t face : faces) {
            h.faces.push_back(SubId{SubKind::Face, face});
        }
        const double boreDiameter = h.segments[h.bore].diameter;
        bool entry = true; // the cones before the first cylinder: the entry
        for (std::size_t i = 0; i < h.bore; ++i) {
            const HoleSegment& s = h.segments[i];
            if (!s.conical) {
                h.counterbored = h.counterbored || s.diameter > boreDiameter + 2.0 * kPosTol;
                entry = false;
                continue;
            }
            if (!entry) {
                continue; // a cone below a cylinder: a conical step (a drill point's floor), not an entry
            }
            const double width = (s.diameter - s.bottomDiameter) / 2.0;
            if (width <= std::max(kChamferMaxWidthMm, kChamferMaxFraction * s.bottomDiameter)) {
                h.chamfered = true;
            } else {
                h.countersunk = true;
            }
        }
        return h;
    }

    // Whether anything lies above the top of `h` inside its top circle.
    Result<bool> blocked(const RecognizedHole& h, double topRadius, const std::vector<Box3>& boxes, double partTop)
    {
        const double x = toMm(h.x);
        const double y = toMm(h.y);
        const double z = toMm(h.zTop);
        const double probeRadius = topRadius - kProbeInset;
        std::optional<kernel::Shape> probe;
        for (std::uint32_t face = 0; face < m.faces(); ++face) {
            const Box3& b = boxes[face];
            if (b.max.z <= z + kAboveTol || b.min.x >= x + topRadius || b.max.x <= x - topRadius ||
                b.min.y >= y + topRadius || b.max.y <= y - topRadius) {
                continue;
            }
            if (std::binary_search(h.faces.begin(), h.faces.end(), SubId{SubKind::Face, face})) {
                continue;
            }
            auto pl = m.plane(face);
            if (!pl) {
                return tl::unexpected(pl.error());
            }
            if (*pl && (*pl)->square && (*pl)->z <= z + kAboveTol) {
                continue;
            }
            if (!(probeRadius > 0.0)) {
                continue;
            }
            if (!probe) {
                const double height = partTop - z + 1.0;
                auto made = kernel::makeCylinder(toModel(m.frame(), Vec3{x, y, z + kProbeLift}), m.frame().z,
                                                 probeRadius, height);
                if (!made) {
                    return failure(ErrorCode::KernelFailure,
                                   "The clearance above a hole could not be checked: " + made.error().message);
                }
                probe = *made;
            }
            auto d = kernel::minDistance(m.shape(), SubId{SubKind::Face, face}, *probe, SubId{SubKind::Solid, 0});
            if (!d) {
                return failure(ErrorCode::KernelFailure,
                               "The clearance above a hole could not be checked: " + d.error().message);
            }
            if (d->value <= kTouching) {
                return true;
            }
        }
        return false;
    }
};

// The box of every face in the WCS: exact when the frame permutes the axes, enclosing otherwise.
std::vector<Box3> wcsBoxes(Model& m, double& partTop)
{
    std::vector<Box3> out(m.faces());
    partTop = -std::numeric_limits<double>::infinity();
    for (std::uint32_t f = 0; f < m.faces(); ++f) {
        const Box3 b = (**m.surface(f)).bounds; // every face was read in findPieces
        Box3 w{Vec3{1e300, 1e300, 1e300}, Vec3{-1e300, -1e300, -1e300}};
        for (int i = 0; i < 8; ++i) {
            const Vec3 corner{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
            const Vec3 q = toWcs(m.frame(), corner);
            w.min = Vec3{std::min(w.min.x, q.x), std::min(w.min.y, q.y), std::min(w.min.z, q.z)};
            w.max = Vec3{std::max(w.max.x, q.x), std::max(w.max.y, q.y), std::max(w.max.z, q.z)};
        }
        out[f] = w;
        partTop = std::max(partTop, w.max.z);
    }
    return out;
}

// The thread of the hole feature that made the bore: the first name of role "hole.*" on the bore's faces.
void pickThread(RecognizedHole& h, const std::vector<std::uint32_t>& boreFaces, const HoleOptions& options)
{
    if (options.names == nullptr || !options.threadOf) {
        return;
    }
    for (const std::uint32_t face : boreFaces) {
        for (const Hash256& name : options.names->namesOf(SubId{SubKind::Face, face})) {
            const naming::TopoName* record = options.names->record(name);
            if (record == nullptr || record->role.rfind("hole.", 0) != 0) {
                continue;
            }
            if (auto thread = options.threadOf(record->origin); thread && !thread->spec.empty()) {
                h.threadSpec = thread->spec;
                h.threadPitch = thread->pitch;
                h.threadNominalDiameter = thread->nominalDiameter; // T-035
                h.threadDepth = thread->depth;
            }
            return;
        }
    }
}

Result<std::vector<RecognizedHole>> recognize(const kernel::Shape& shape, const Xform& wcsFromModel, JobContext& ctx,
                                              const HoleOptions& options)
{
    if (shape.isNull()) {
        return failure(ErrorCode::InvalidArgument, "Hole recognition needs a shape");
    }
    auto frame = frameOf(wcsFromModel);
    if (!frame) {
        return tl::unexpected(frame.error());
    }
    Model model(shape, *frame);
    Recognizer r{model, ctx, {}, {}, {}, {}};
    RAY_CAM_TRY(r.findPieces());
    ctx.progress.set(0.4);
    RAY_CAM_TRY(r.buildSegments());
    RAY_CAM_TRY(ctx.cancel.check());
    ctx.progress.set(0.6);

    double partTop = 0;
    const std::vector<Box3> boxes = wcsBoxes(model, partTop);
    std::vector<char> used(r.segs.size(), 0);
    std::vector<RecognizedHole> holes;
    for (std::size_t i = 0; i < r.segs.size(); ++i) {
        RAY_CAM_TRY(ctx.cancel.check());
        auto top = r.isTop(r.segs[i]);
        if (!top) {
            return tl::unexpected(top.error());
        }
        if (!*top || used[i] != 0) {
            continue;
        }
        auto chain = r.walk(static_cast<int>(i), used);
        if (!chain) {
            return tl::unexpected(chain.error());
        }
        if (!*chain) {
            continue;
        }
        RecognizedHole h = r.describe(**chain);
        if (options.maxDiameter > 0.0 && h.segments[h.bore].diameter > options.maxDiameter + 1e-9) {
            continue;
        }
        auto isBlocked = r.blocked(h, r.segs[i].rTop, boxes, partTop);
        if (!isBlocked) {
            return tl::unexpected(isBlocked.error());
        }
        if (*isBlocked) {
            continue;
        }
        std::vector<std::uint32_t> boreFaces;
        for (const int p : r.segs[static_cast<std::size_t>((*chain)->segs[h.bore])].pieces) {
            boreFaces.push_back(r.pieces[static_cast<std::size_t>(p)].face);
        }
        pickThread(h, boreFaces, options);
        holes.push_back(std::move(h));
    }
    std::sort(holes.begin(), holes.end(), [](const RecognizedHole& a, const RecognizedHole& b) {
        return std::tie(a.x, a.y, a.zTop, a.axisSub) < std::tie(b.x, b.y, b.zTop, b.axisSub);
    });
    ctx.progress.set(1.0);
    return holes;
}

// ---------- Ordering ----------

constexpr std::size_t kNeighbours = 8;
constexpr int kMaxOptPasses = 50;
constexpr double kMinGain = 0.5; // nm: a move must shorten the path by at least this

struct Pt {
    double x, y;
};

double dist2(const Pt& a, const Pt& b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return dx * dx + dy * dy;
}
double dist(const Pt& a, const Pt& b)
{
    return dmath::sqrt(dist2(a, b));
}

Pt toPt(Vec2 v)
{
    const auto nm = [](double mm) {
        return std::isfinite(mm) ? static_cast<double>(toNm(mm)) : 0.0;
    };
    return Pt{nm(v.x), nm(v.y)};
}

// The kNeighbours nearest holes of every point in `pts` (the holes are 0..holes-1; pts may hold more points after
// them, such as the start), by (squared distance, index), from a sweep in x order.
std::vector<std::vector<std::uint32_t>> neighbourLists(const std::vector<Pt>& pts, std::size_t holes)
{
    std::vector<std::uint32_t> byX(holes);
    std::iota(byX.begin(), byX.end(), 0u);
    std::sort(byX.begin(), byX.end(), [&](std::uint32_t a, std::uint32_t b) {
        return std::tie(pts[a].x, pts[a].y, a) < std::tie(pts[b].x, pts[b].y, b);
    });
    std::vector<std::vector<std::uint32_t>> out(pts.size());
    std::vector<std::pair<double, std::uint32_t>> best;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        best.clear();
        const Pt& p = pts[i];
        const auto worst = [&]() {
            return best.size() < kNeighbours ? std::numeric_limits<double>::infinity() : best.back().first;
        };
        const auto offer = [&](std::uint32_t j) {
            if (j == i) {
                return;
            }
            const std::pair<double, std::uint32_t> c{dist2(p, pts[j]), j};
            if (best.size() == kNeighbours && !(c < best.back())) {
                return;
            }
            best.insert(std::upper_bound(best.begin(), best.end(), c), c);
            if (best.size() > kNeighbours) {
                best.pop_back();
            }
        };
        // Start the sweep where p would sit in x order.
        const auto at =
            std::lower_bound(byX.begin(), byX.end(), p.x, [&](std::uint32_t a, double x) { return pts[a].x < x; });
        for (auto it = at; it != byX.end(); ++it) {
            const double dx = pts[*it].x - p.x;
            if (dx * dx > worst()) {
                break;
            }
            offer(*it);
        }
        for (auto it = at; it != byX.begin();) {
            --it;
            const double dx = p.x - pts[*it].x;
            if (dx * dx > worst()) {
                break;
            }
            offer(*it);
        }
        for (const auto& [d, j] : best) {
            out[i].push_back(j);
        }
    }
    return out;
}

} // namespace

// ---------- recognizeHoles ----------

Result<std::vector<RecognizedHole>> recognizeHoles(const kernel::Shape& shape, const Xform& wcsFromModel,
                                                   JobContext& ctx, const HoleOptions& options)
{
    try {
        return recognize(shape, wcsFromModel, ctx, options);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while recognizing holes");
    }
}

Result<std::vector<RecognizedHole>> recognizeHoles(const kernel::Shape& shape, const naming::IResolver& resolver,
                                                   BodyId body, const Xform& wcsFromModel, JobContext& ctx,
                                                   const HoleOptions& options)
{
    try {
        auto holes = recognize(shape, wcsFromModel, ctx, options);
        if (!holes) {
            return holes;
        }
        for (RecognizedHole& h : *holes) {
            RAY_CAM_TRY(ctx.cancel.check());
            h.axisFace = resolver.makeRef(body, h.axisSub);
        }
        return holes;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while recognizing holes");
    }
}

// The thread pickup of one part of a ResolvedGeometry (cam.h, added by T-035): its element map and the document's
// hole threads, both read from `geom`.
HoleOptions holeOptionsFor(const ResolvedGeometry& geom, std::size_t part)
{
    HoleOptions options;
    if (part >= geom.partNames.size() || geom.holeThreads.empty()) {
        return options;
    }
    options.names = &geom.partNames[part];
    const std::map<FeatureId, HoleThread>* threads = &geom.holeThreads;
    options.threadOf = [threads](const FeatureId& id) -> std::optional<HoleThread> {
        const auto it = threads->find(id);
        if (it == threads->end()) {
            return std::nullopt;
        }
        return it->second;
    };
    return options;
}

// ---------- orderHolesShortestPath ----------

std::vector<std::size_t> orderHolesShortestPath(std::span<const Vec2> xy, Vec2 start)
{
    const std::size_t n = xy.size();
    if (n == 0) {
        return {};
    }
    // Points 0..n-1 are the holes, point n the start.
    std::vector<Pt> pts;
    pts.reserve(n + 1);
    for (const Vec2& v : xy) {
        pts.push_back(toPt(v));
    }
    pts.push_back(toPt(start));
    const auto d = [&](std::uint32_t a, std::uint32_t b) {
        return dist(pts[a], pts[b]);
    };

    // 1. Nearest neighbour from the start.
    std::vector<std::uint32_t> path; // path[0] is the start, path[1..n] the holes
    path.reserve(n + 1);
    path.push_back(static_cast<std::uint32_t>(n));
    std::vector<char> visited(n, 0);
    for (std::size_t step = 0; step < n; ++step) {
        const Pt& from = pts[path.back()];
        std::size_t best = n;
        double bestD = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < n; ++i) {
            if (visited[i] == 0) {
                const double dd = dist2(from, pts[i]);
                if (best == n || dd < bestD) {
                    best = i;
                    bestD = dd;
                }
            }
        }
        visited[best] = 1;
        path.push_back(static_cast<std::uint32_t>(best));
    }
    if (n < 3) {
        return std::vector<std::size_t>(path.begin() + 1, path.end());
    }

    // 2. 2-opt through neighbour lists. Reversing path[i..j] (1 <= i < j <= n) replaces the edges (path[i-1], path[i])
    // and (path[j], path[j+1]) by (path[i-1], path[j]) and (path[i], path[j+1]); with j = n there is no second edge.
    const std::vector<std::vector<std::uint32_t>> nbrs = neighbourLists(pts, n);
    std::vector<std::size_t> pos(n + 1);
    for (std::size_t i = 0; i <= n; ++i) {
        pos[path[i]] = i;
    }
    const auto gainOf = [&](std::size_t i, std::size_t j) {
        const std::uint32_t a = path[i - 1];
        const std::uint32_t b = path[i];
        const std::uint32_t c = path[j];
        double before = d(a, b);
        double after = d(a, c);
        if (j < n) {
            const std::uint32_t e = path[j + 1];
            before += d(c, e);
            after += d(b, e);
        }
        return before - after;
    };
    const auto reverse = [&](std::size_t i, std::size_t j) {
        std::reverse(path.begin() + static_cast<std::ptrdiff_t>(i), path.begin() + static_cast<std::ptrdiff_t>(j) + 1);
        for (std::size_t q = i; q <= j; ++q) {
            pos[path[q]] = q;
        }
    };
    for (int pass = 0; pass < kMaxOptPasses; ++pass) {
        bool improved = false;
        for (std::size_t i = 1; i <= n; ++i) {
            bool moved = true;
            while (moved) {
                moved = false;
                const std::uint32_t a = path[i - 1];
                const std::uint32_t b = path[i];
                const double ab = d(a, b);
                // New edge (a, c): c among a's neighbours after b, or the last point.
                for (const std::uint32_t c : nbrs[a]) {
                    if (d(a, c) >= ab) {
                        break;
                    }
                    const std::size_t j = pos[c];
                    if (j > i && gainOf(i, j) > kMinGain) {
                        reverse(i, j);
                        moved = improved = true;
                        break;
                    }
                }
                if (!moved && i < n && gainOf(i, n) > kMinGain) {
                    reverse(i, n);
                    moved = improved = true;
                }
                if (!moved && i + 1 <= n) {
                    // New edge (b', e) for the edge (c, e) = (path[i], path[i+1]) read as the second edge: b' among
                    // e's neighbours before c, the move reversing path[pos[b']..i].
                    const std::uint32_t c = path[i];
                    const std::uint32_t e = path[i + 1];
                    const double ce = d(c, e);
                    for (const std::uint32_t bb : nbrs[e]) {
                        if (d(bb, e) >= ce) {
                            break;
                        }
                        const std::size_t k = pos[bb];
                        if (k >= 1 && k < i && gainOf(k, i) > kMinGain) {
                            reverse(k, i);
                            moved = improved = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!improved) {
            break;
        }
    }
    return std::vector<std::size_t>(path.begin() + 1, path.end());
}

} // namespace ray::cam
