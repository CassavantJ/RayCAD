// Tool shapes (task T-070). What a profile means, how it is cut into convex pieces and which tool becomes which shape:
// toolshape.h.
// Floating point: this module is built with /fp:strict on MSVC (ray_deterministic). Its hot kernels switch off only
// FENV_ACCESS and floating-point exception semantics, which leaves MSVC's /fp:precise: IEEE operations in source order,
// no reassociation and no fused multiply-add (x64 without /arch:AVX2 has none, and Visual Studio 2022 does not contract
// without /fp:contract), so results are the same bits as under /fp:strict; GCC and Clang build with
// -ffp-contract=off -fno-fast-math, which is the same model. Strict mode costs these loops about a third of their speed
// (S6's prototype: 3.8 s under /fp:precise, 5.6 s under /fp:strict, same stock hash). third_party/dmath does the same.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma fenv_access(off)
#pragma float_control(except, off)
#endif
#include "toolshape.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ray::sim {

namespace detail {

std::size_t Envelope::segmentOf(double x) const
{
    if (k.empty() || !(x > 0)) {
        return 0;
    }
    // The last segment whose start is at or below x: from the bucket's first segment, forward.
    const double b = x * bucketScale;
    std::size_t j = b >= kEnvelopeBuckets ? bucket.back() : bucket[static_cast<std::size_t>(b)];
    while (j + 1 < k.size() && d[j + 1] <= x) {
        ++j;
    }
    return j;
}

double ConvexPiece::radiusAt(double h) const
{
    if (endless || ph.size() < 2 || h <= ph.front()) {
        return endless ? pr.back() : pr.front();
    }
    if (h >= ph.back()) {
        return pr.back();
    }
    const auto it = std::upper_bound(ph.begin(), ph.end(), h);
    const std::size_t j = static_cast<std::size_t>(it - ph.begin()) - 1;
    const double t = (h - ph[j]) / (ph[j + 1] - ph[j]);
    return pr[j] + (pr[j + 1] - pr[j]) * t;
}

namespace {

struct Pt {
    double h, r;
};

void finishEnvelope(Envelope& e)
{
    e.k.clear();
    for (std::size_t j = 0; j + 1 < e.d.size(); ++j) {
        e.k.push_back((e.v[j + 1] - e.v[j]) / (e.d[j + 1] - e.d[j]));
    }
    e.bucket.assign(kEnvelopeBuckets, 0);
    e.bucketScale = e.d.back() > 0 ? kEnvelopeBuckets / e.d.back() : 0.0;
    std::size_t j = 0;
    for (int b = 0; b < kEnvelopeBuckets; ++b) {
        // The segment holding the bucket's start (the start of the bucket as segmentOf computes it, or below).
        const double x = e.bucketScale > 0 ? b / e.bucketScale : 0.0;
        while (j + 1 < e.k.size() && e.d[j + 1] <= x) {
            ++j;
        }
        // A bucket start that rounds above a breakpoint could skip it: start one segment lower to be safe.
        e.bucket[static_cast<std::size_t>(b)] = static_cast<std::uint16_t>(j > 0 ? j - 1 : 0);
    }
}

// Emits the piece made of the points `cur` (heights increasing, radius concave) into `out`.
void emitPiece(const std::vector<Pt>& cur, std::vector<ConvexPiece>& out)
{
    if (cur.size() < 2) {
        return;
    }
    ConvexPiece p;
    for (const Pt& q : cur) {
        p.ph.push_back(q.h);
        p.pr.push_back(q.r);
    }
    p.h0 = p.ph.front();
    p.h1 = p.ph.back();
    p.rmax = *std::max_element(p.pr.begin(), p.pr.end());
    if (!(p.rmax > 1e-12)) {
        return; // no volume
    }
    const std::size_t m = p.pr.size() - 1;
    std::size_t pk1 = 0;
    while (p.pr[pk1] < p.rmax) {
        ++pk1;
    }
    std::size_t pk2 = m;
    while (p.pr[pk2] < p.rmax) {
        --pk2;
    }
    p.flatBottom = p.pr.front() >= p.rmax;
    p.flatTop = p.pr.back() >= p.rmax;
    p.plateauLo = p.ph[pk1];
    p.plateauHi = p.ph[pk2];
    // Lower envelope: the rising branch, read by radius.
    p.low.d = {0.0};
    p.low.v = {p.h0};
    if (p.flatBottom) {
        p.low.d.push_back(p.rmax);
        p.low.v.push_back(p.h0);
    } else {
        if (p.pr[0] > 0) {
            p.low.d.push_back(p.pr[0]);
            p.low.v.push_back(p.h0);
        }
        for (std::size_t i = 1; i <= pk1; ++i) {
            if (p.pr[i] > p.low.d.back()) {
                p.low.d.push_back(p.pr[i]);
                p.low.v.push_back(p.ph[i]);
            }
        }
    }
    finishEnvelope(p.low);
    // Upper envelope, negated: the falling branch, read by radius from the top down.
    p.highNeg.d = {0.0};
    p.highNeg.v = {-p.h1};
    if (p.flatTop) {
        p.highNeg.d.push_back(p.rmax);
        p.highNeg.v.push_back(-p.h1);
    } else {
        if (p.pr[m] > 0) {
            p.highNeg.d.push_back(p.pr[m]);
            p.highNeg.v.push_back(-p.h1);
        }
        for (std::size_t i = m; i-- > pk2;) {
            if (p.pr[i] > p.highNeg.d.back()) {
                p.highNeg.d.push_back(p.pr[i]);
                p.highNeg.v.push_back(-p.ph[i]);
            }
        }
    }
    finishEnvelope(p.highNeg);
    out.push_back(std::move(p));
}

// Cuts one sanitised profile (heights not decreasing) into convex pieces: a new piece starts at a step in radius at
// one height (a shoulder, up or down) and where the radius bends outward (its slope against height increases).
void decompose(const std::vector<Pt>& pts, std::vector<ConvexPiece>& out)
{
    // Levels: runs of points at one height, with the radius the profile arrives at and the one it leaves with.
    struct Level {
        double h, arrive, leave;
    };
    std::vector<Level> levels;
    for (const Pt& q : pts) {
        if (!levels.empty() && levels.back().h == q.h) {
            levels.back().leave = q.r;
        } else {
            levels.push_back({q.h, q.r, q.r});
        }
    }
    std::vector<Pt> cur;
    double curSlope = 0;
    for (std::size_t l = 0; l + 1 < levels.size(); ++l) {
        const Pt a{levels[l].h, levels[l].leave};
        const Pt b{levels[l + 1].h, levels[l + 1].arrive};
        const double slope = (b.r - a.r) / (b.h - a.h);
        if (cur.empty()) {
            cur = {a, b};
            curSlope = slope;
            continue;
        }
        const bool step = levels[l].arrive != levels[l].leave;
        const double tol = 1e-9 * (1.0 + std::abs(curSlope));
        if (step || slope > curSlope + tol) {
            emitPiece(cur, out);
            cur = {a, b};
        } else if (std::abs(slope - curSlope) <= tol && cur.size() >= 2) {
            cur.back() = b; // collinear: extend the last span
        } else {
            cur.push_back(b);
        }
        curSlope = slope;
    }
    emitPiece(cur, out);
}

std::vector<Pt> sanitise(const std::vector<Vec2>& profile)
{
    std::vector<Pt> pts;
    pts.reserve(profile.size());
    for (const Vec2& v : profile) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y)) {
            continue;
        }
        Pt q{v.y, std::max(0.0, v.x)};
        if (!pts.empty()) {
            q.h = std::max(q.h, pts.back().h);
            if (q.h == pts.back().h && q.r == pts.back().r) {
                continue;
            }
        }
        pts.push_back(q);
    }
    return pts;
}

} // namespace

CutterShape compileShape(std::span<const std::vector<Vec2>* const> profiles, bool endlessTop)
{
    CutterShape s;
    bool haveTop = false;
    Pt top{0, 0};
    for (const std::vector<Vec2>* profile : profiles) {
        if (profile == nullptr) {
            continue;
        }
        const std::vector<Pt> pts = sanitise(*profile);
        if (pts.empty()) {
            continue;
        }
        decompose(pts, s.pieces);
        // The top: the last height and the largest radius the profile has there.
        top = pts.back();
        for (std::size_t i = pts.size(); i-- > 0 && pts[i].h == pts.back().h;) {
            top.r = std::max(top.r, pts[i].r);
        }
        haveTop = true;
    }
    if (endlessTop && haveTop && top.r > 1e-12) {
        ConvexPiece p;
        p.ph = {top.h, top.h + 1.0};
        p.pr = {top.r, top.r};
        p.h0 = top.h;
        p.h1 = kInfHeight;
        p.rmax = top.r;
        p.endless = true;
        p.flatBottom = true;
        p.flatTop = true;
        p.plateauLo = top.h;
        p.plateauHi = kInfHeight;
        p.low.d = {0.0, top.r};
        p.low.v = {top.h, top.h};
        p.highNeg.d = {0.0, top.r};
        p.highNeg.v = {-kInfHeight, -kInfHeight};
        finishEnvelope(p.low);
        finishEnvelope(p.highNeg);
        s.pieces.push_back(std::move(p));
    }
    if (!s.pieces.empty()) {
        s.hmin = s.pieces.front().h0;
        s.hmax = s.pieces.front().h1;
        for (const ConvexPiece& p : s.pieces) {
            s.rmax = std::max(s.rmax, p.rmax);
            s.hmin = std::min(s.hmin, p.h0);
            s.hmax = std::max(s.hmax, p.h1);
        }
    }
    return s;
}

void DiscHull::build()
{
    bmin = std::min(b1 - r1, b2 - r2);
    bmax = std::max(b1 + r1, b2 + r2);
    const double da = a2 - a1, db = b2 - b1;
    const double len = std::sqrt(da * da + db * db);
    tangents = len > std::abs(r1 - r2) && len > 0.0;
    if (!tangents) {
        return;
    }
    const double ua = da / len, ub = db / len;
    const double delta = (r1 - r2) / len;
    const double s = std::sqrt(std::max(0.0, 1.0 - delta * delta));
    const double na = -ub, nb = ua;
    const double pa = delta * ua + s * na, pb = delta * ub + s * nb; // N+
    const double ma = delta * ua - s * na, mb = delta * ub - s * nb; // N-
    ta[0] = a1 + r1 * pa;
    tb[0] = b1 + r1 * pb;
    ta[1] = a2 + r2 * pa;
    tb[1] = b2 + r2 * pb;
    ta[2] = a1 + r1 * ma;
    tb[2] = b1 + r1 * mb;
    ta[3] = a2 + r2 * ma;
    tb[3] = b2 + r2 * mb;
}

namespace {

inline void widen(double a, double& lo, double& hi)
{
    lo = std::min(lo, a);
    hi = std::max(hi, a);
}

inline bool discCross(double a, double b, double r, double y, double& lo, double& hi)
{
    const double dy = y - b;
    const double q = r * r - dy * dy;
    if (q < 0) {
        return false;
    }
    const double s = std::sqrt(q);
    widen(a - s, lo, hi);
    widen(a + s, lo, hi);
    return true;
}

inline bool segCross(double a1, double b1, double a2, double b2, double y, double& lo, double& hi)
{
    if ((y < b1 && y < b2) || (y > b1 && y > b2)) {
        return false;
    }
    if (b1 == b2) {
        widen(a1, lo, hi);
        widen(a2, lo, hi);
        return true;
    }
    const double t = (y - b1) / (b2 - b1);
    widen(a1 + (a2 - a1) * t, lo, hi);
    return true;
}

} // namespace

bool DiscHull::cross(double y, double& lo, double& hi) const
{
    if (y < bmin || y > bmax) {
        return false;
    }
    double l = std::numeric_limits<double>::infinity(), h = -l;
    bool hit = discCross(a1, b1, r1, y, l, h);
    hit = discCross(a2, b2, r2, y, l, h) || hit;
    if (tangents) {
        hit = segCross(ta[0], tb[0], ta[1], tb[1], y, l, h) || hit;
        hit = segCross(ta[2], tb[2], ta[3], tb[3], y, l, h) || hit;
    }
    if (!hit) {
        return false;
    }
    lo = std::min(lo, l);
    hi = std::max(hi, h);
    return true;
}

bool stadiumCross(double a1, double b1, double a2, double b2, double r, double y, double& lo, double& hi)
{
    double l = std::numeric_limits<double>::infinity(), h = -l;
    bool hit = discCross(a1, b1, r, y, l, h);
    hit = discCross(a2, b2, r, y, l, h) || hit;
    const double da = a2 - a1, db = b2 - b1;
    const double len2 = da * da + db * db;
    if (len2 > 0) {
        // The rectangle: |cross(w, dir)| <= r len and 0 <= dot(w, dir) <= len^2, with w = (x - a1, y - b1).
        const double len = std::sqrt(len2);
        const double wy = y - b1;
        double x0 = -std::numeric_limits<double>::infinity(), x1 = -x0;
        bool ok = true;
        if (db != 0) {
            // cross = wx db - wy da, within [-r len, r len].
            double c0 = (wy * da - r * len) / db, c1 = (wy * da + r * len) / db;
            if (c0 > c1) {
                std::swap(c0, c1);
            }
            x0 = c0;
            x1 = c1;
        } else {
            ok = std::abs(wy * da) <= r * len;
        }
        if (ok) {
            if (da != 0) {
                // dot = wx da + wy db, within [0, len2].
                double d0 = (0.0 - wy * db) / da, d1 = (len2 - wy * db) / da;
                if (d0 > d1) {
                    std::swap(d0, d1);
                }
                x0 = std::max(x0, d0);
                x1 = std::min(x1, d1);
            } else {
                const double dot = wy * db;
                ok = dot >= 0 && dot <= len2;
            }
        }
        if (ok && x0 <= x1) {
            widen(a1 + x0, l, h);
            widen(a1 + x1, l, h);
            hit = true;
        }
    }
    if (!hit) {
        return false;
    }
    lo = std::min(lo, l);
    hi = std::max(hi, h);
    return true;
}

void prepareSlopes(const Envelope& env, double m, std::vector<double>& c)
{
    const double am = std::abs(m);
    c.resize(env.k.size());
    for (std::size_t j = 0; j < env.k.size(); ++j) {
        const double k = env.k[j];
        c[j] = k > am ? k / std::sqrt(k * k - am * am) : -1.0;
    }
}

double minimiseAlong(const Envelope& env, const double* c, double m, double p0, double e, double pa, double pb)
{
    if (env.constant()) {
        return env.v[0] + std::min(m * pa, m * pb);
    }
    // In s = +-(p - p0), chosen so that the linear term rises with s: f = m p0 + |m| s + env(sqrt(e^2 + s^2)).
    const double am = std::abs(m);
    const double sa = m >= 0 ? pa - p0 : p0 - pb;
    const double sb = m >= 0 ? pb - p0 : p0 - pa;
    const double base = m * p0;
    const double dmax = env.dmax();
    if (sa >= 0) {
        return base + am * sa + env.eval(std::min(std::sqrt(e * e + sa * sa), dmax));
    }
    const double shi = std::min(sb, 0.0);
    const double dlo = std::min(std::sqrt(e * e + shi * shi), dmax);
    if (am == 0) {
        return base + env.eval(dlo);
    }
    const double dhi = std::min(std::sqrt(e * e + sa * sa), dmax);
    if (!(dhi > dlo)) {
        return base + am * shi + env.eval(dlo);
    }
    // On s <= 0, read by d = sqrt(e^2 + s^2): phi(d) = env(d) - |m| sqrt(d^2 - e^2) is convex. On segment j its slope
    // k_j - |m| d / sqrt(d^2 - e^2) turns non-negative at d = e c_j; find the first segment where that happens before
    // its end.
    std::size_t lo = env.segmentOf(dlo), hi = env.segmentOf(dhi) + 1; // search [lo, hi)
    const std::size_t end = hi;
    const auto pred = [&](std::size_t j) {
        return c[j] >= 0 && e * c[j] <= env.d[j + 1];
    };
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (pred(mid)) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (lo == end) {
        return base + am * sa + env.evalIn(end - 1, dhi);
    }
    const std::size_t j = lo;
    const double dstar = std::clamp(std::max(env.d[j], e * c[j]), dlo, dhi);
    const double s = -std::sqrt(std::max(0.0, dstar * dstar - e * e));
    return base + am * s + env.evalIn(j, dstar);
}

} // namespace detail

// ---------- makeToolShape ----------
namespace {

using detail::kArcChordTolMm;
using detail::kArcMinChords;

constexpr double kPi = 3.14159265358979323846;

// The radius at height h of a polyline whose heights do not decrease (the largest radius where h is a level step).
struct Poly {
    std::vector<Vec2> p; // (radius, height)
    void add(double r, double h)
    {
        if (!p.empty() && p.back().x == r && p.back().y == h) {
            return;
        }
        p.push_back({r, h});
    }
};

// The part of `src` between heights lo and hi (lo < hi), cut by interpolation at both ends.
std::vector<Vec2> clipHeights(const std::vector<Vec2>& src, double lo, double hi)
{
    std::vector<Vec2> out;
    if (!(hi > lo) || src.size() < 2) {
        return out;
    }
    const auto push = [&](Vec2 v) {
        if (out.empty() || out.back().x != v.x || out.back().y != v.y) {
            out.push_back(v);
        }
    };
    for (std::size_t i = 0; i + 1 < src.size(); ++i) {
        const Vec2 a = src[i], b = src[i + 1];
        if (b.y < lo || a.y > hi) {
            continue;
        }
        if (a.y == b.y) {
            push(a);
            push(b);
            continue;
        }
        const auto at = [&](double h) {
            const double t = (h - a.y) / (b.y - a.y);
            return Vec2{a.x + (b.x - a.x) * t, h};
        };
        push(a.y >= lo ? a : at(lo));
        push(b.y <= hi ? b : at(hi));
    }
    // A level step exactly at a cut belongs to the side above it: the lower part ends with the radius the step
    // arrives at, the upper part starts with the one it leaves with (the solid is the same either way; the joined
    // profiles then have the step once).
    if (lo > src.front().y) {
        std::size_t n = 0;
        while (n + 1 < out.size() && out[n + 1].y == lo && out[n].y == lo) {
            ++n;
        }
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
    }
    if (hi < src.back().y) {
        while (out.size() >= 2 && out[out.size() - 2].y == hi && out.back().y == hi) {
            out.pop_back();
        }
    }
    return out;
}

} // namespace

ToolShape makeToolShape(const lib::Tool& tool, const lib::Holder* holder, double stickOut)
{
    using T = lib::ToolType;
    ToolShape out;
    const double R = std::isfinite(tool.diameter) ? std::max(tool.diameter, 0.0) / 2 : 0.0;
    if (!(R > 0)) {
        return out;
    }
    const double rs = tool.shankDiameter > 0 ? tool.shankDiameter / 2 : R;
    const double L = tool.overallLength > 0 ? tool.overallLength : 0;
    const double lf = tool.fluteLength > 0 ? tool.fluteLength : (L > 0 ? L : 8 * R);
    const double nose = stickOut > 0 ? stickOut : (L > 0 ? L : 12 * R);
    const bool withHolder = holder != nullptr && !holder->segments.empty();

    // The tool from the tip up (its solid's outline), as the invariant checker builds it.
    Poly s;
    double h = 0;
    switch (tool.type) {
    case T::BallEndMill:
    case T::BullEndMill: {
        const double rc = tool.type == T::BallEndMill ? R : std::clamp(tool.cornerRadius, 0.0, R);
        s.add(0, 0);
        if (rc > 0) {
            s.add(R - rc, 0);
            // Chords with their ends on the arc and a sagitta of at most kArcChordTolMm.
            const double step = 2.0 * dmath::acos(std::max(-1.0, 1.0 - kArcChordTolMm / rc));
            int n = static_cast<int>(std::ceil((kPi / 2) / std::max(step, 1e-9)));
            n = std::clamp(n, kArcMinChords, 4096);
            for (int i = 1; i <= n; ++i) {
                const double a = (kPi / 2) * i / n;
                const double r = i == n ? R : (R - rc) + rc * dmath::sin(a);
                const double hh = i == n ? rc : rc - rc * dmath::cos(a);
                s.add(r, hh);
            }
            h = rc;
        } else {
            s.add(R, 0);
        }
        break;
    }
    case T::ChamferMill:
    case T::SpotDrill:
    case T::Drill:
    case T::EngraveCutter: {
        const double a = tool.tipAngleDeg;
        const double rt = std::clamp(tool.tipDiameter / 2, 0.0, R);
        s.add(0, 0);
        if (a > 0 && a < 180 && rt < R) {
            const double hc = (R - rt) / dmath::tan(a * kPi / 360.0);
            s.add(rt, 0);
            s.add(R, hc);
            h = hc;
        } else {
            s.add(R, 0);
        }
        break;
    }
    default:
        s.add(0, 0);
        s.add(R, 0);
        break;
    }
    if (lf > h) {
        s.add(R, lf);
        h = lf;
    }
    const double rTop = s.p.back().x;
    double neck = std::min(rTop, rs);
    const auto widen = [&](double from, double to) {
        if (to > from + 1e-12) {
            h += to - from;
            s.add(to, h);
        }
    };
    if (tool.neckDiameter > 0) {
        const double rn = tool.neckDiameter / 2;
        const double neckTop = tool.neckLength > 0 ? tool.neckLength : tool.shoulderLength;
        if (rn < rTop) {
            s.add(rn, h); // a step in to the neck
        } else {
            widen(rTop, rn);
        }
        if (neckTop > h) {
            s.add(rn, neckTop);
            h = neckTop;
        }
        neck = rn;
        if (tool.shoulderLength > h) {
            const double body = std::min(rTop, rs);
            widen(rn, body);
            neck = std::max(rn, body);
            if (tool.shoulderLength > h) {
                s.add(neck, tool.shoulderLength);
                h = tool.shoulderLength;
            }
        }
    } else {
        if (tool.type == T::ThreadMill && tool.threadPitch > 0) {
            neck = std::min(neck, std::max(R - 0.6134 * tool.threadPitch, 0.1 * R));
        }
        if (neck < rTop) {
            s.add(neck, h);
        }
        if (tool.shoulderLength > h) {
            s.add(neck, tool.shoulderLength);
            h = tool.shoulderLength;
        }
    }
    if (std::abs(rs - neck) > 1e-12) {
        if (rs < neck) {
            // The checker's 45 degree cone also when the shank is narrower than the neck.
            h += neck - rs;
            s.add(rs, h);
        } else {
            widen(neck, rs);
        }
    }
    const double toolTop = withHolder ? nose : std::max({L > 0 ? L : nose, h, lf});
    if (toolTop > h) {
        s.add(rs, toolTop);
    }
    const double cutTop = std::min(lf, toolTop);
    out.cuttingProfile = clipHeights(s.p, 0, cutTop);
    out.shankProfile = clipHeights(s.p, cutTop, toolTop);
    if (!withHolder) {
        return out;
    }
    double stack = 0;
    for (const lib::HolderSegment& seg : holder->segments) {
        stack += std::max(0.0, seg.height);
    }
    const double gauge = std::max(holder->gaugeLength, stack);
    struct Band {
        double y0, y1, r0, r1;
    };
    std::vector<Band> topDown;
    double y = nose + gauge;
    for (const lib::HolderSegment& seg : holder->segments) {
        const double height = std::max(0.0, seg.height);
        if (!(height > 0)) {
            continue;
        }
        topDown.push_back({y - height, y, std::max(seg.lowerDiameter / 2, rs), std::max(seg.upperDiameter / 2, rs)});
        y -= height;
    }
    Poly hp;
    if (!topDown.empty() && y > nose) {
        hp.add(topDown.back().r0, nose);
    }
    for (auto it = topDown.rbegin(); it != topDown.rend(); ++it) {
        hp.add(it->r0, it->y0);
        hp.add(it->r1, it->y1);
    }
    out.holderProfile = std::move(hp.p);
    return out;
}

} // namespace ray::sim
