// Reference generators of the invariant checker's tests (reference_paths.h).
#include "reference_paths.h"

#include <algorithm>
#include <cmath>

namespace ray::cam::invariants {

PathBuilder::PathBuilder(Vec3 start) : pos_(start)
{
    path_.startX = toNm(start.x);
    path_.startY = toNm(start.y);
    path_.startZ = toNm(start.z);
}

PathBuilder& PathBuilder::rapid(Vec3 to, MoveRole role)
{
    Move m{};
    m.kind = MoveKind::Rapid;
    m.role = role;
    m.x = toNm(to.x);
    m.y = toNm(to.y);
    m.z = toNm(to.z);
    path_.moves.push_back(m);
    pos_ = to;
    return *this;
}

PathBuilder& PathBuilder::feed(Vec3 to, double mmPerMin, MoveRole role)
{
    Move m{};
    m.kind = MoveKind::Linear;
    m.role = role;
    m.feedMicronPerMin = static_cast<std::int32_t>(std::lround(mmPerMin * 1000));
    m.x = toNm(to.x);
    m.y = toNm(to.y);
    m.z = toNm(to.z);
    path_.moves.push_back(m);
    pos_ = to;
    return *this;
}

PathBuilder& PathBuilder::arc(Vec3 to, Vec2 centre, bool ccw, double mmPerMin, MoveRole role)
{
    Move m{};
    m.kind = ccw ? MoveKind::ArcCCW : MoveKind::ArcCW;
    m.role = role;
    m.plane = ArcPlane::XY;
    m.feedMicronPerMin = static_cast<std::int32_t>(std::lround(mmPerMin * 1000));
    m.x = toNm(to.x);
    m.y = toNm(to.y);
    m.z = toNm(to.z);
    m.i = toNm(centre.x);
    m.j = toNm(centre.y);
    path_.moves.push_back(m);
    pos_ = to;
    return *this;
}

Toolpath zigzagFace(const FaceRef& r)
{
    const Box3& s = r.stock;
    const double R = r.toolRadius;
    const double xs = s.min.x - R - r.overhang, xe = s.max.x + R + r.overhang;
    const double ya = s.min.y + R / 2, yb = s.max.y - R / 2;
    std::vector<double> ys;
    if (yb <= ya) {
        ys.push_back((s.min.y + s.max.y) / 2);
    } else {
        const int n = static_cast<int>(std::ceil((yb - ya) / r.stepover));
        for (int k = 0; k <= n; ++k) {
            ys.push_back(std::min(ya + k * r.stepover, yb));
        }
    }
    PathBuilder b(Vec3{xs, ys[0], r.clearance});
    b.rapid(Vec3{xs, ys[0], r.clearance}, MoveRole::Approach);
    b.rapid(Vec3{xs, ys[0], r.z + 1}, MoveRole::Approach);
    b.feed(Vec3{xs, ys[0], r.z}, r.feed, MoveRole::Plunge);
    double x = xs;
    for (std::size_t k = 0; k < ys.size(); ++k) {
        x = (k % 2 == 0) ? xe : xs;
        b.feed(Vec3{x, ys[k], r.z}, r.feed, MoveRole::Cut);
        if (k + 1 < ys.size()) {
            b.feed(Vec3{x, ys[k + 1], r.z}, r.feed, MoveRole::Link);
        }
    }
    b.rapid(Vec3{x, ys.back(), r.clearance}, MoveRole::Retract);
    return b.take();
}

Toolpath contourRect(const ContourRef& r)
{
    const double off = r.toolRadius + r.stockToLeave;
    const double lr = r.leadRadius;
    const double xm = (r.x0 + r.x1) / 2, yb = r.y0 - off;
    const Vec2 lc{xm, yb - lr}; // the lead arcs' centre, outside
    const double sx = r.climb ? xm + lr : xm - lr;
    PathBuilder b(Vec3{sx, lc.y, r.clearance});
    b.rapid(Vec3{sx, lc.y, r.clearance}, MoveRole::Approach);
    for (const double z : r.depths) {
        if (b.position().z != r.retract) {
            b.rapid(Vec3{sx, lc.y, r.retract}, MoveRole::Approach);
        }
        b.feed(Vec3{sx, lc.y, z}, r.feed, MoveRole::Plunge);
        const Vec3 p0{xm, yb, z};
        // Climb: clockwise round the part, the lead arcs counter-clockwise; conventional the mirror.
        b.arc(p0, lc, r.climb, r.feed, MoveRole::LeadIn);
        const bool ccw = !r.climb;
        if (r.climb) {
            b.feed(Vec3{r.x0, yb, z}, r.feed);
            b.arc(Vec3{r.x0 - off, r.y0, z}, Vec2{r.x0, r.y0}, ccw, r.feed);
            b.feed(Vec3{r.x0 - off, r.y1, z}, r.feed);
            b.arc(Vec3{r.x0, r.y1 + off, z}, Vec2{r.x0, r.y1}, ccw, r.feed);
            b.feed(Vec3{r.x1, r.y1 + off, z}, r.feed);
            b.arc(Vec3{r.x1 + off, r.y1, z}, Vec2{r.x1, r.y1}, ccw, r.feed);
            b.feed(Vec3{r.x1 + off, r.y0, z}, r.feed);
            b.arc(Vec3{r.x1, yb, z}, Vec2{r.x1, r.y0}, ccw, r.feed);
        } else {
            b.feed(Vec3{r.x1, yb, z}, r.feed);
            b.arc(Vec3{r.x1 + off, r.y0, z}, Vec2{r.x1, r.y0}, ccw, r.feed);
            b.feed(Vec3{r.x1 + off, r.y1, z}, r.feed);
            b.arc(Vec3{r.x1, r.y1 + off, z}, Vec2{r.x1, r.y1}, ccw, r.feed);
            b.feed(Vec3{r.x0, r.y1 + off, z}, r.feed);
            b.arc(Vec3{r.x0 - off, r.y1, z}, Vec2{r.x0, r.y1}, ccw, r.feed);
            b.feed(Vec3{r.x0 - off, r.y0, z}, r.feed);
            b.arc(Vec3{r.x0, yb, z}, Vec2{r.x0, r.y0}, ccw, r.feed);
        }
        b.feed(p0, r.feed);
        const double ex = r.climb ? xm - lr : xm + lr;
        b.arc(Vec3{ex, lc.y, z}, lc, r.climb, r.feed, MoveRole::LeadOut);
        b.rapid(Vec3{ex, lc.y, r.retract}, MoveRole::Retract);
        b.rapid(Vec3{sx, lc.y, r.retract}, MoveRole::Link);
    }
    b.rapid(Vec3{sx, lc.y, r.clearance}, MoveRole::Retract);
    return b.take();
}

namespace {

// One rounded-rectangle loop about (cx, cy), half sizes hw, hh, corner radius rc, from the middle of its -Y side.
void loop(PathBuilder& b, double cx, double cy, double hw, double hh, double rc, double z, bool ccw, double feed)
{
    rc = std::clamp(rc, 0.0, std::min(hw, hh));
    const auto corner = [&](Vec3 to, Vec2 c) {
        if (rc > 0) {
            b.arc(to, c, ccw, feed);
        }
    };
    const double l = cx - hw, r = cx + hw, bo = cy - hh, t = cy + hh;
    if (ccw) {
        b.feed(Vec3{r - rc, bo, z}, feed);
        corner(Vec3{r, bo + rc, z}, Vec2{r - rc, bo + rc});
        b.feed(Vec3{r, t - rc, z}, feed);
        corner(Vec3{r - rc, t, z}, Vec2{r - rc, t - rc});
        b.feed(Vec3{l + rc, t, z}, feed);
        corner(Vec3{l, t - rc, z}, Vec2{l + rc, t - rc});
        b.feed(Vec3{l, bo + rc, z}, feed);
        corner(Vec3{l + rc, bo, z}, Vec2{l + rc, bo + rc});
    } else {
        b.feed(Vec3{l + rc, bo, z}, feed);
        corner(Vec3{l, bo + rc, z}, Vec2{l + rc, bo + rc});
        b.feed(Vec3{l, t - rc, z}, feed);
        corner(Vec3{l + rc, t, z}, Vec2{l + rc, t - rc});
        b.feed(Vec3{r - rc, t, z}, feed);
        corner(Vec3{r, t - rc, z}, Vec2{r - rc, t - rc});
        b.feed(Vec3{r, bo + rc, z}, feed);
        corner(Vec3{r - rc, bo, z}, Vec2{r - rc, bo + rc});
    }
    b.feed(Vec3{cx, bo, z}, feed);
}

} // namespace

Toolpath pocketRect(const PocketRef& r)
{
    const double off = r.toolRadius + r.stockToLeave;
    const double cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2;
    const double hw0 = (r.x1 - r.x0) / 2 - off, hh0 = (r.y1 - r.y0) / 2 - off;
    std::vector<int> ks;
    for (int k = 0; std::min(hw0, hh0) - k * r.stepover > 1e-9; ++k) {
        ks.push_back(k);
    }
    const bool ccw = r.climb;
    const double zs = r.zTop + 0.5;
    PathBuilder b(Vec3{cx + r.helixRadius, cy, r.clearance});
    b.rapid(Vec3{cx + r.helixRadius, cy, r.clearance}, MoveRole::Approach);
    b.rapid(Vec3{cx + r.helixRadius, cy, r.retract}, MoveRole::Approach);
    b.feed(Vec3{cx + r.helixRadius, cy, zs}, r.feed, MoveRole::Plunge);
    const int turns = std::max(1, static_cast<int>(std::ceil((zs - r.z) / r.helixPitch)));
    for (int t = 1; t <= turns; ++t) {
        b.arc(Vec3{cx + r.helixRadius, cy, zs + (r.z - zs) * t / turns}, Vec2{cx, cy}, ccw, r.feed, MoveRole::Ramp);
    }
    b.arc(Vec3{cx + r.helixRadius, cy, r.z}, Vec2{cx, cy}, ccw, r.feed, MoveRole::Cut);
    if (ks.empty()) {
        b.rapid(Vec3{cx + r.helixRadius, cy, r.clearance}, MoveRole::Retract);
        return b.take();
    }
    // The centre line inside the innermost loop.
    const double a = hw0 - ks.back() * r.stepover, c = hh0 - ks.back() * r.stepover;
    if (a >= c) {
        b.feed(Vec3{cx - (a - c), cy, r.z}, r.feed);
        b.feed(Vec3{cx + (a - c), cy, r.z}, r.feed);
    } else {
        b.feed(Vec3{cx, cy - (c - a), r.z}, r.feed);
        b.feed(Vec3{cx, cy + (c - a), r.z}, r.feed);
    }
    for (auto it = ks.rbegin(); it != ks.rend(); ++it) {
        const double d = *it * r.stepover;
        const double hw = hw0 - d, hh = hh0 - d;
        b.feed(Vec3{cx, cy - hh, r.z}, r.feed);
        loop(b, cx, cy, hw, hh, r.cornerRadius - off - d, r.z, ccw, r.feed);
    }
    b.rapid(Vec3{cx, cy - hh0, r.clearance}, MoveRole::Retract);
    return b.take();
}

} // namespace ray::cam::invariants
