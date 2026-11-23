// The 2D cutter-sweep checker of spike S5 (spikes/s5-adaptive/src/check.cpp), promoted to a test library by task
// T-060. It measures a 2D toolpath of a flat end mill against a pocket at one depth: the engagement angle along every
// cut, material removed by tool-down links, area left uncut that the tool could reach, and gouge into the walls.
//
// INDEPENDENT. It shares no code with any toolpath generator: no src/cam (geom2d, link, the adaptive engine), no
// Clipper2, nothing but the C++ standard library. Its inputs are plain millimetre polylines; the tests that use it
// convert the engine's output themselves. Keep it that way: a checker that shares code with what it checks can share
// its mistakes.
//
// MODEL (as S5). A raster of the stock at this depth, cells of `cell` mm (0.02 by default). Cells are air (outside the
// stock), pocket material (to remove: inside the pocket loops, even-odd), keep-out material (stock outside the pocket:
// walls, islands), or removed. Input::cleared cells start removed. The tool disc (radius D / 2) is swept along every
// cut and every tool-down link in steps of at most one cell; retract links are not swept (their XY length is counted).
// A helix entry sweeps the circle of the tool centre about its centre once and is not in the statistics. A path that
// does not start where the previous one ended implies a straight move to its first point, swept with that path's kind.
// ENGAGEMENT at each step: kSamples points on a circle of radius r - kInset about the tool centre count when they lie
// in material the sweep has not removed up to kLag mm of path behind the tool (kInset = kLag^2 / (2 r), so the sample
// circle meets the lagged disc at 90 degrees to the motion and a wall the tool only touches is not counted). Weighted
// by step length. Error (S5 self-test, repeated in sweepcheck_test.cpp): about 1 degree on straight passes, +1.2
// degrees with 2 degrees of scatter on arcs: differences under about 3 degrees between a target and a measurement are
// inside the checker's error. UNCUT: pocket cells left that a disc of the tool radius can reach (a morphological
// opening of the area away from keep-out material, from two exact Euclidean distance transforms), and the largest
// thickness left. GOUGE: analytic, the tool radius minus the smallest distance from any tool centre (every swept step)
// to the pocket's edges that do not lie on the stock outline (an open side is no wall); also the keep-out area the
// raster sweep removed and its depth.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ray::test::sweep {

using Pt = std::pair<double, double>; // mm
using Loop = std::vector<Pt>;         // closed (last point not repeated)
using Loops = std::vector<Loop>;

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDefaultCell = 0.02; // mm
inline constexpr double kLag = 0.45;         // mm of path by which removal lags the engagement sample
inline constexpr int kSamples = 1440;        // 0.25 degree

enum class Kind {
    Cut,      // cutting: swept, measured
    LinkDown, // a tool-down link: swept; what it removes is reported, its engagement is not in the cut statistics
    Retract   // a link that leaves the material: not swept, only its XY length counted
};
struct Path {
    Kind kind = Kind::Cut;
    std::vector<Pt> points;
    bool measured = true; // a Cut whose engagement is left out of the statistics (it still removes material)
    // Added by T-063: per point, a flag of the segment that ends there (flags[0] unused; empty: none flagged). The
    // generator's reduced-feed flag, checked against this checker's own engagement (Input::flagCheckDeg).
    std::vector<unsigned char> flags;
};
struct Region {
    // Entry: with helixRadius > 0 the tool centre circles helixCentre once at that radius (swept, not measured), and
    // the region's first path starts at `start`; with 0 the tool starts at `start`.
    Pt helixCentre{0, 0};
    double helixRadius = 0;
    Pt start{0, 0};
    std::vector<Path> paths;
    // Added by T-063: a ramp entry. When it has points, the tool centre sweeps this polyline (from `start`) before the
    // paths, at once and not measured, as a helix is (a ramp leaves its whole line cut at depth before the first pass);
    // its length is added to rampLength.
    std::vector<Pt> ramp;
};
struct Input {
    Loops pocket;  // the area to clear (even-odd)
    Loops stock;   // the stock outline at this depth (even-odd)
    Loops cleared; // area already removed (even-odd), may be empty
    double toolDiameter = 0;
    double targetDeg = 0; // the target engagement, for the fractions above it
    std::vector<Region> regions;
    double cell = kDefaultCell; // engagement readings need the default: the sample inset is under half a 0.02 cell's
                                // diagonal; a coarser cell suits area and gouge checks only
    // Added by T-063. flagCheckDeg > 0: the report gives the measured cut length read above this angle on segments
    // whose Path::flags is 0. recordAboveDeg: every measured sample above it goes into Report::hot (diagnosis).
    double flagCheckDeg = 0;
    double recordAboveDeg = 1e9;
};
// A measured sample (Input::recordAboveDeg): where, the angle read, and which region, path and segment (the index of
// the point the segment ends at, in the input path; 0 for the implied move to a path's first point).
struct Hot {
    Pt at{0, 0};
    double deg = 0, length = 0;
    int region = 0, path = 0, segment = 0;
    bool flagged = false;
    double forwardDeg = 0; // the forward-half reading (Report::Forward)
};

struct Report {
    // Lengths, mm.
    double cutLength = 0, linkDownLength = 0, retractLength = 0, helixLength = 0, rampLength = 0;
    int implicitJoins = 0;
    double joinLength = 0, joinMax = 0;
    std::size_t points = 0;
    // Engagement of measured cuts, degrees, weighted by length.
    double median = 0, p95 = 0, p99 = 0, max = 0;
    double pctAboveTarget = 0, pctAbove1_1x = 0, pctAbove1_25x = 0, pctAbove1_5x = 0, pctAboveTargetPlus5 = 0,
           pctAboveTargetPlus10 = 0, pctAboveTargetPlus20 = 0, pctAbove90 = 0, pctAir = 0; // air: under 2 degrees
    std::array<double, 18> histogram{}; // percent of measured cut length per 10 degree bin
    // Tool-down links.
    double linkMaxDeg = 0, linkPctEngaged = 0, linkRemovedMm2 = 0;
    // Where the largest engagement of a measured cut and of a tool-down link were measured (tool centre, mm).
    Pt maxAt{0, 0}, linkMaxAt{0, 0};
    // Area, mm^2.
    double pocketArea = 0, reachableArea = 0, uncutTotal = 0, uncutReachable = 0, uncutMaxThickness = 0;
    // Gouge, mm.
    double gougeAnalytic = 0, gougeArea = 0, gougeDepthRaster = 0;
    long long centreInKeepoutSteps = 0;
    // Added by T-063 (Input::flagCheckDeg, recordAboveDeg): percent of measured cut length read above flagCheckDeg on
    // unflagged segments, the largest angle read on an unflagged segment and where; percent of measured cut length on
    // flagged segments; the recorded samples.
    double pctAboveFlagCheckUnflagged = 0, unflaggedMax = 0, pctFlagged = 0;
    Pt unflaggedMaxAt{0, 0};
    std::vector<Hot> hot;
    // Added by T-063: the same engagement read on the forward half of the tool only (sample points ahead of the line
    // through its centre perpendicular to the motion). An edge point behind that line moves into the tool's own last
    // positions and cannot cut; the full reading counts material there in tight turns, where the disc lagging kLag
    // behind on a curve does not cover the outer side (S5's "+1.2 degrees on arcs", much more in a turn of a few
    // millimetres' radius). Straight cuts read the same both ways. The S5 statistics above are unchanged.
    struct Forward {
        double median = 0, p99 = 0, max = 0, pctAbove1_25x = 0, pctAbove1_5x = 0, pctAboveTargetPlus10 = 0;
        double pctAboveFlagCheckUnflagged = 0, unflaggedMax = 0;
        Pt unflaggedMaxAt{0, 0};
    } forward;
};

Report check(const Input&);
std::string summary(const Report&);

// Engagement of a straight cut of radial depth ae with a tool of radius r: acos(1 - ae / r), degrees.
double straightEngagementDeg(double ae, double r);

} // namespace ray::test::sweep
