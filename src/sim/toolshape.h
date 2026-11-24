// Tool shapes (task T-070): makeToolShape (include/ray/sim.h) and the form the stock model sweeps them in.
//
// A ToolShape profile is a (radius, height) polyline, heights not decreasing, read as the solid of revolution about +Z
// that it bounds (closed along the axis at its lowest and highest points; a level step is a shoulder). The stock model
// does not sweep that solid directly: compileProfile cuts it into CONVEX pieces, each a solid of revolution whose
// radius r(h) is concave on its height range [h0, h1] (a cylinder, cone, frustum, the chords of a ball or a corner
// radius, a single-profile thread mill's V; with discs closing it at h0 and h1). A convex piece swept along a straight
// move is convex, so every ray of a dexel grid meets it in ONE interval, found in closed form:
//   - rays along the tool axis (Z): the lowest point over the ray is min over the move of z(t) + E(d(t)), with d(t) the
//     ray's distance from the tool axis and E(d) the piece's lower envelope (the lowest height at which the radius is
//     at least d: convex and piecewise linear in d); the highest point is the same with the upper envelope. On the
//     move's own parameter this is a convex one-dimensional minimum, found by a binary search over E's segments and
//     solved in closed form on the segment that holds it (Envelope, minimiseAlong).
//   - rays across the tool axis (X, Y): at a fixed height the piece's slice at time t is a disc whose radius is linear
//   in
//     t between the profile's breakpoints and whose centre is linear in t, so the slice swept over such a span is the
//     convex hull of its two end discs (DiscHull), which a ray crosses in one interval.
// A shape whose radius is not concave (a stepped tool, a neck, teeth, a holder) is the union of several pieces, and the
// union of their intervals is the cut. Pieces meet only on their boundary discs.
//
// makeToolShape follows the toolpath invariant checker's tool model (tests/unit/cam/invariants/tool_profile.h, which
// agrees with the tool library's drawing), with the zones as ToolShape's three profiles:
//   - cutting: the tip shape up to fluteLength (0: overallLength, else 4 diameters), or up to the holder's nose if that
//     is lower. FlatEndMill, FaceMill, BoringBar, Reamer, Tap, ThreadMill: a flat end of radius R. BallEndMill: a
//     quarter circle of radius R. BullEndMill: a quarter circle of radius cornerRadius at the corner. ChamferMill,
//     SpotDrill, Drill, EngraveCutter: a cone of included angle tipAngleDeg from tipDiameter / 2 at the tip to R (an
//     angle outside (0, 180): flat). Taps and thread mills are swept as their major diameter (no thread form), as the
//     checker does; a profile with teeth works too, as several pieces, if a caller builds one.
//   - shank: above the flutes the neck the tool states (else min(cutting radius, shank radius); a thread mill without a
//     stated neck R - 0.6134 pitch), to the shoulder, a 45 degree cone to the shank radius, the shank, up to the
//     holder's nose (stickOut; 0: overallLength, else 6 diameters), or without a holder up to the overall length.
//   - holder: segments from the gauge line down, the stack's top on the gauge line max(gaugeLength, stack height) above
//     the nose; a stack that ends above the nose carries its lowest diameter down to it; never narrower than the shank.
// Arcs become chords with their ends on the arc and a sagitta of at most kArcChordTolMm, so the swept tool is at most
// that much smaller than the real one.
#pragma once

#include <ray/sim.h>

#include <cstdint>
#include <span>
#include <vector>

namespace ray::sim::detail {

inline constexpr double kArcChordTolMm = 0.001; // sagitta of the chords of a ball or corner radius
inline constexpr int kArcMinChords = 2;         // per quarter circle, however large the tolerance
inline constexpr double kInfHeight = 1e30;      // the height of a piece that goes on upward without end

inline constexpr int kEnvelopeBuckets = 1024;

// A convex, piecewise linear, nondecreasing function of the radial distance d on [0, d.back()].
struct Envelope {
    std::vector<double> d; // breakpoints, d[0] = 0, increasing
    std::vector<double> v; // values at the breakpoints
    std::vector<double> k; // slope of segment j (between d[j] and d[j + 1]); one fewer than d
    // The segment at the start of each of kEnvelopeBuckets equal parts of [0, dmax] (segmentOf starts there).
    std::vector<std::uint16_t> bucket;
    double bucketScale = 0; // kEnvelopeBuckets / dmax
    bool constant() const { return k.empty() || k.back() == 0.0; }
    double dmax() const { return d.back(); }
    std::size_t segmentOf(double x) const; // the segment holding x (clamped to the ends)
    double evalIn(std::size_t seg, double x) const { return v[seg] + k[seg] * (x - d[seg]); }
    double eval(double x) const { return k.empty() ? v[0] : evalIn(segmentOf(x), x); }
};

// One convex piece of a shape (see the top of this file).
struct ConvexPiece {
    std::vector<double> ph, pr; // profile breakpoints: heights increasing, radii concave in height
    double h0 = 0, h1 = 0;      // ph.front(), ph.back(); h1 = kInfHeight for an endless piece
    double rmax = 0;
    bool endless = false;                 // goes on upward without end at radius pr.back() (a cylinder)
    Envelope low;                         // E(d): the lowest height whose radius is at least d
    Envelope highNeg;                     // -G(d), G the highest such height (unused when flatTop)
    bool flatBottom = false;              // E is h0 everywhere (the radius is largest at the bottom: a cylinder)
    bool flatTop = false;                 // G is h1 everywhere (largest at the top), or the piece is endless
    double plateauLo = 0, plateauHi = -1; // the heights over which the radius is rmax (lo > hi: a single height)
    double radiusAt(double h) const;      // h in [h0, h1]
};

struct CutterShape {
    std::vector<ConvexPiece> pieces;
    double rmax = 0;           // largest radius of any piece
    double hmin = 0, hmax = 0; // height range of the union (hmax = kInfHeight when endless)
    bool empty() const { return pieces.empty(); }
};

// The convex pieces of one or more profiles joined upward (each profile read on its own; they are united). With
// `endlessTop` the top of the last non-empty profile goes on upward without end at its last radius. Points with a
// negative radius count as 0; a height lower than the one before it is raised to it.
CutterShape compileShape(std::span<const std::vector<Vec2>* const> profiles, bool endlessTop);
inline CutterShape compileProfile(const std::vector<Vec2>& profile, bool endlessTop = false)
{
    const std::vector<Vec2>* one[] = {&profile};
    return compileShape(one, endlessTop);
}

// The convex hull of two discs in a plane (centres (a1, b1), (a2, b2), radii r1, r2), crossed by lines b = const.
struct DiscHull {
    double a1, b1, r1, a2, b2, r2;
    double bmin, bmax;     // the hull's extent in b
    bool tangents = false; // false: one disc holds the other
    double ta[4], tb[4];   // tangent points: T1+, T2+, T1-, T2-
    void build();
    // The hull's interval on the line b = y, widening [lo, hi]; false (and no change) when the line misses it.
    bool cross(double y, double& lo, double& hi) const;
};

// The interval of a stadium (the segment (a1, b1)-(a2, b2) thickened by r) on the line b = y.
bool stadiumCross(double a1, double b1, double a2, double b2, double r, double y, double& lo, double& hi);

// min over p in [pa, pb] of  m p + env(sqrt(e^2 + (p - p0)^2))  for a convex nondecreasing env (sqrt(e^2 + (p - p0)^2)
// <= env.dmax() on that range). `c` holds, per segment j, k_j / sqrt(k_j^2 - m^2) when k_j > |m|, else -1
// (prepareSlopes); the minimum is then a binary search and a closed form.
void prepareSlopes(const Envelope& env, double m, std::vector<double>& c);
double minimiseAlong(const Envelope& env, const double* c, double m, double p0, double e, double pa, double pb);

} // namespace ray::sim::detail
