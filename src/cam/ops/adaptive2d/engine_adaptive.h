// SPDX-License-Identifier: LGPL-2.1-or-later

/**************************************************************************
 *   Copyright (c) 2018 Kresimir Tusek <kresimir.tusek@gmail.com>          *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

// MODIFIED FILE. This is FreeCAD's src/Mod/CAM/libarea/Adaptive.hpp from FreeCAD main at commit
// 56062b61c7085af5af12ead4c02ce5765d0107c4, changed for RayCAD by the RayCAD contributors in November 2026 (task
// T-060). The list of changes is at the top of engine_adaptive.cpp; the unmodified upstream text is this file's first
// version in the repository's history. Private to src/cam/ops/adaptive2d: the RayCAD interface is engine.h.
// Upstream's layout is kept (no clang-format) so that the file stays comparable with upstream line by line.
// clang-format off

#include <clipper2/clipper.h>
#include <ray/core.h>
#include <cstdint>
#include <functional>
#include <vector>
#include <list>
#include <optional>
#include <string>

#pragma once

#ifndef __DBL_MAX__
# define __DBL_MAX__ 1.7976931348623158e+308
#endif

#define NTOL 1.0e-7  // numeric tolerance

namespace ray::cam::adaptive2d::fork
{

enum MotionType
{
    mtCutting = 0,
    mtLinkClear = 1,
    mtLinkNotClear = 2,
    mtLinkClearAtPrevPass = 3  // unused
};

enum OperationType
{
    otClearingInside = 0,
    otClearingOutside = 1,
    otProfilingInside = 2,
    otProfilingOutside = 3
};

// RayCAD: what an output path is for (upstream only has the MotionType).
enum PathRole
{
    prPass = 0,     // a pass of the engagement regulator
    prLeadIn = 1,   // the lead-in before a pass (mtCutting)
    prLeadOut = 2,  // the lead-out after a pass (mtCutting)
    prLink = 3,     // a link between passes (mtLinkClear, mtLinkNotClear, or mtCutting when allowed to cut through)
    prFinish = 4    // a finishing pass along the boundary
};

typedef std::pair<double, double> DPoint;
typedef std::vector<DPoint> DPath;
typedef std::vector<DPath> DPaths;

// RayCAD: output paths stay in the engine's integer grid (upstream converted them to doubles in mm and back to
// integers by truncation), with the engine's own estimate of the radial depth of cut of each segment.
struct TPath
{
    int motion = mtCutting;  // MotionType
    int role = prPass;       // PathRole
    Clipper2Lib::Path64 points;
    // depth[i] (i >= 1): the engine's estimate of the radial depth of cut, in grid units, of the segment from
    // points[i - 1] to points[i] (the cut area per unit of travel against the engine's cleared area); depth[0] = -1.
    // -1 where the engine has no estimate.
    std::vector<double> depth;
    // RayCAD (T-063): a lead's stretch through cleared area, points airFrom..airTo (a prefix of a lead-in or a suffix
    // of a lead-out), checked clear of material with a margin (MarkAir); -1: none. The algorithm still uses the whole
    // lead as upstream did; only the output's motion of that stretch changes (engine.cpp).
    int64_t airFrom = -1, airTo = -1;
    // RayCAD (T-063): where EstimateDepth measured a segment in pieces whose estimates differ, pieces[i] holds each
    // piece's end point and estimate (the last ends at points[i]), so the output can keep them apart (a finishing
    // pass's long wall segment that meets a corner); empty otherwise.
    std::vector<std::vector<std::pair<Clipper2Lib::Point64, double>>> pieces;
};

class ClearedArea;

typedef std::vector<TPath> TPaths;

struct AdaptiveOutput
{
    // RayCAD: every field is initialised (upstream left the first four undefined when no start point was found).
    Clipper2Lib::Point64 HelixCenterPoint {};
    Clipper2Lib::Point64 StartPoint {};
    int64_t HelixRadius = 0;   // RayCAD: grid units; 0 when the region is entered from cleared area
    bool HelixEntry = false;   // RayCAD: entered by a helix about HelixCenterPoint
    int SeedEntry = -1;        // RayCAD (T-063): entered at Adaptive2d::entrySeeds[SeedEntry] (StartPoint); -1: not
    TPaths AdaptivePaths;
    int ReturnMotionType = mtLinkNotClear;  // MotionType, problem with serialization if enum is used
    double ClearedArea = 0;    // Total area cleared in this operation (mm^2)
    double clipperScale = 0;   // Clipper integer units per mm used in this operation
    Clipper2Lib::Paths64 ToolBoundPaths;  // RayCAD: where the tool centre may go in this region (grid units)
    Clipper2Lib::Paths64 ClearedPaths;    // RayCAD: the cleared area after this region (grid units)

    // Warning/error flags
    bool StartPointNotFound = false;
    bool LeadPathFailed = false;
    bool UnexpectedRotateIterations = false;
    bool TooManyFailedEngagements = false;
    bool UnclearedAreaRemains = false;
    bool FailedToSetUpFinishingPass = false;
    bool FinishingLeadInFailed = false;
    // RayCAD
    bool Stopped = false;           // cancelled, or a limit below was reached: the region is incomplete
    bool LimitReached = false;      // one of Limits' hard limits was reached
    std::string LimitName;          // which one
};

// RayCAD: limits on work, in place of upstream's wall-clock limits (clock()) and its unlimited loops. Every limit
// counts iterations or nodes, so a run gives the same output on any machine and under any load.
struct Limits
{
    // ResolveLinkPath: segments examined (upstream: 0.5 s of wall time); the link then retracts (mtLinkNotClear).
    int64_t linkSearchSteps = 64;
    // ResolveLinkPath: points tried sideways from one segment's midpoint (upstream: unlimited).
    int64_t linkSideSteps = 256;
    // Hard limits: reaching one stops the region and sets LimitReached (the result is incomplete).
    int64_t passes = 100000;            // upstream: LONG_MAX
    int64_t pointsPerPass = 1000000;    // upstream: LONG_MAX
    int64_t steps = 20000000;           // engagement steps over the whole run (each up to 30 cut-area evaluations)
    int64_t failedEngagements = 10000;  // upstream's limit on passes in a row that cut nothing
    int64_t leadSteps = 10000;          // upstream's limit on the steps of one lead-in or lead-out
};

// RayCAD: counters of one run, for tests and reports.
struct Stats
{
    int64_t steps = 0;               // engagement steps (iterateNextStep calls)
    int64_t cutAreaCalls = 0;        // CalcCutArea calls
    int64_t passes = 0;
    int64_t linkSearches = 0;        // ResolveLinkPath calls
    int64_t linkSearchSteps = 0;     // segments examined over all link searches
    int64_t linkSearchLimitHits = 0; // searches stopped by Limits::linkSearchSteps or linkSideSteps
    int64_t linkSideProbes = 0;      // sideways probes over all link searches
    int64_t engageSearches = 0;      // searches for the next engagement point
    int64_t regions = 0;             // connected areas of the tool centre (0: the tool fits nowhere)
    int64_t failedPassesInARow = 0;  // the longest run of passes that cut nothing (Limits::failedEngagements)
};

class Adaptive2d
{
public:
    Adaptive2d();
    double toolDiameter = 5;
    double helixRampTargetDiameter = 0;
    double helixRampMinDiameter = 0;
    double stepOverFactor = 0.2;
    double tolerance = 0.1;
    double stockToLeave = 0;
    bool forceInsideOut = true;
    bool finishingProfile = true;
    double keepToolDownDistRatio = 3.0;  // keep tool down distance ratio
    OperationType opType = OperationType::otClearingInside;
    // RayCAD: the grid (Clipper integer units per mm), chosen by the caller from the tolerance (engine.cpp), and the
    // limits, the cancel token polled at every step, and a progress sink (fraction of the regions done).
    int64_t scaleFactor = 100;
    Limits limits;
    const ray::CancelToken* cancel = nullptr;
    std::function<void(double)> progress;
    // RayCAD (T-063, entry.h): entries the caller made. entrySeeds: tool positions (grid units) and directions where
    // a region starts its first pass, as after a helix, instead of searching its cleared area for an engagement point
    // (the caller put the area the entry cleared, a ramp's or a pre-drilled hole's, into the cleared paths); the first
    // seed inside the region is used. helixEntryAllowed false: no helix (the region is not entered: StartPointNotFound).
    // onlyRegionsAt: when not empty, only the regions (connected areas of the tool centre) containing one of these
    // points are processed, the others left out of the output.
    std::vector<std::pair<Clipper2Lib::Point64, Clipper2Lib::PointD>> entrySeeds;
    bool helixEntryAllowed = true;
    std::vector<Clipper2Lib::Point64> onlyRegionsAt;
    // RayCAD (T-063): the stretches of leads through cleared area are marked (TPath::airFrom, airTo) when they stay
    // this many grid units clear of material.
    double airClearance = 4;

    // RayCAD: the inputs are in grid units (scaleFactor per mm), converted by the caller.
    std::list<AdaptiveOutput> Execute(
        const Clipper2Lib::Paths64& stockPaths,
        const Clipper2Lib::Paths64& paths,
        const Clipper2Lib::Paths64& clearedPaths
    );
    const Stats& GetStats() const
    {
        return stats;
    }
    int64_t GetToolRadiusScaled() const
    {
        return toolRadiusScaled;
    }

private:
    std::list<AdaptiveOutput> results;
    Clipper2Lib::Paths64 stockInputPaths;
    double stepOverScaled = 1;
    int64_t toolRadiusScaled = 10;
    int64_t finishPassOffsetScaled = 0;
    int64_t helixRampMaxRadiusScaled = 0;
    int64_t helixRampMinRadiusScaled = 0;
    double referenceCutArea = 0;
    double optimalCutAreaPD = 0;
    bool stopProcessing = false;
    int current_region = 0;
    int region_count = 0;   // RayCAD: for progress
    Stats stats;            // RayCAD

    bool StopRequested();   // RayCAD: polls the cancel token and the hard limits
    void HitLimit(AdaptiveOutput& output, const char* name);
    void ReportProgress(double regionFraction);
    // RayCAD: the engine's estimate of the radial depth of cut along a path, against the cleared area as it is.
    // RayCAD (T-063): with `pieces`, the estimate of each piece where they differ (TPath::pieces).
    std::vector<double> EstimateDepth(
        const Clipper2Lib::Path64& path,
        ClearedArea& cleared,
        std::vector<std::vector<std::pair<Clipper2Lib::Point64, double>>>* pieces = nullptr
    );
    // RayCAD (T-063): marks the stretch of a lead (with its depth estimates) that runs through cleared area with
    // airClearance to spare: a lead-in's prefix (from `before`, the point the tool comes from, when given) or a
    // lead-out's suffix. Reads the cleared area only.
    void MarkAir(TPath& lead, ClearedArea& cleared, bool leadIn, const Clipper2Lib::Point64* before);
    // The lead-in of a link and lead-in pair (FindLinkPath's result: the lead-in last, after the tool's previous point
    // or the link that ends where the tool comes from).
    void MarkLeadInAir(TPaths& linkAndLeadIn, ClearedArea& cleared);

    void ProcessPolyNode(
        Clipper2Lib::Paths64 boundPaths,
        Clipper2Lib::Paths64 toolBoundPaths,
        Clipper2Lib::Paths64 finishingPaths,
        const Clipper2Lib::Paths64& initialClearedPaths
    );
    bool FindEntryPoint(
        const Clipper2Lib::Paths64& toolBoundPaths,
        const Clipper2Lib::Paths64& boundPaths,
        ClearedArea& cleared /*output*/,
        Clipper2Lib::Point64& entryPoint /*output*/,
        Clipper2Lib::Point64& toolPos,
        Clipper2Lib::PointD& toolDir,
        int64_t& helixRadiusScaled,
        AdaptiveOutput& adaptiveOutput
    );
    std::pair<double, double> CalcCutArea(
        Clipper2Lib::Point64 c1,
        Clipper2Lib::Point64 c2,
        ClearedArea& clearedArea,
        bool useCache = true  // RayCAD
    );
    std::optional<TPaths> FindLinkPath(
        const std::optional<Clipper2Lib::Point64>& prevPoint,
        const Clipper2Lib::Point64& pathStart,
        const Clipper2Lib::PointD& pathDir,
        ClearedArea& cleared,
        const Clipper2Lib::Paths64& toolBoundPaths,
        AdaptiveOutput& adaptiveOutput
    );
    std::optional<std::pair<Clipper2Lib::Point64, Clipper2Lib::PointD>> AppendToolPath(
        AdaptiveOutput& output,
        const TPath& passToolPath,
        TPaths& linkPath,
        ClearedArea& cleared,
        const Clipper2Lib::Paths64& toolBoundPaths
    );
    bool IsClearPath(
        const Clipper2Lib::Path64& path,
        ClearedArea& clearedArea,
        double safetyDistanceScaled = 0
    );
    bool IsAllowedToCutTrough(
        const Clipper2Lib::Point64& p1,
        const Clipper2Lib::Point64& p2,
        ClearedArea& clearedArea,
        const Clipper2Lib::Paths64& toolBoundPaths,
        double areaFactor = 1.5,
        bool skipBoundsCheck = false
    );
    bool MakeLeadPath(
        bool leadIn,
        const Clipper2Lib::Point64& startPoint,
        const Clipper2Lib::PointD& startDir,
        Clipper2Lib::Point64 beaconPoint,
        ClearedArea& clearedArea,
        const Clipper2Lib::Paths64& toolBoundPaths,
        Clipper2Lib::Path64& output,
        AdaptiveOutput& adaptiveOutput
    );

    bool ResolveLinkPath(
        const Clipper2Lib::Point64& startPoint,
        const Clipper2Lib::Point64& endPoint,
        ClearedArea& clearedArea,
        Clipper2Lib::Path64& output
    );

    void ApplyStockToLeave(Clipper2Lib::Paths64& inputPaths);

private:
    // Derivation for MIN_STEP_CLIPPPER (MSC for short in this derivation):
    // Diagram:
    // - circle C1 from previous pass, radius R
    // - circle C2 from current pass MSC away, horizontal
    // - line Lprev from previous pass, step over x
    // - line Llong from tool position through C1/Lprev intersection to C2
    // - line L1 from previous tool position to C1/Lprev intersection
    //
    // Length of Llong = R + y, where y is the longest protrusion into the cut area
    // When selecting MIN_STEP_CLIPPER, we need to ensure that the computed
    // value for y > 1 when using stepover x equal to the size of the finishing
    // pass. Finishing pass stepover is
    // x = stepover/10
    // x = 2 * R * stepoverFactor / 10 (Eq1).
    //
    // Construct right triangle with (R-x) of vertical radius from C1 and
    // L1. Third length (horizontal) = a
    // (R-x)^2 + a^2 = R^2
    // a^2 = 2*R*x - x^2  (Eq2)
    // a ~= sqrt(2*R*x)  (Eq3; x<<R)
    //
    // Construct right triangle with (R-x) of vertical radius from C2 and (R-y)
    // of Llong. Third length is MSC - a (horizontal).
    // (MSC-a)^2 + (R-x)^2 = (R-y)^2
    // MSC^2 - 2*a*MSC + a^2 + R^2 - 2*R*x + x^2 = R^2 - 2*R*y + y^2
    // MSC^2 - 2*a*MSC + a^2 - 2*R*x + x^2 = -2*R*y + y^2
    // MSC^2 - 2*MSC*sqrt(2*R*x) + (2*R*x - x^2) - 2*R*x + x^2 = - 2*R*y + y^2 (substitute Eq2, Eq3)
    // MSC^2 - 2*MSC*sqrt(2*R*x) = - 2*R*y + y^2
    // MSC^2 - 2*MSC*sqrt(2*R*(2*R*stepoverFactor/10)) = -2*R*y + y^2 (substitute Eq1)
    // MSC^2 - 2*R*MSC*sqrt(2*stepoverFactor/5) = -2*R*y + y^2
    // -2*R*MSC*sqrt(2*stepoverFactor/5) = -2*R*y (MSC << R, y << R)
    // MSC*sqrt(2*stepoverFactor/5) = y
    // MSC = y/sqrt(2*stepoverFactor/5)   (Eq4)
    //
    // To ensure we don't evaluate a positive cut area as zero, we need y to
    // measure > 1. The endpoints of y may be perturbed by up to sqrt(2)/2 each
    // due to integer rounding, so the true value of y must be at least 1+sqrt(2) ~= 2.4.
    // StepoverFactor may be as small as 1% = 0.01. Evaluating Eq4 with these values:
    //
    // MSC > 2.4/sqrt(2*.01/5)
    // MSC > 38.
    //
    // Historically we have used MSC = 16. It might be convenient that MSC is
    // many-times divisible by 2, so I have chosen 16*3 (>38) for its new value.

public:
    static constexpr double MIN_STEP_CLIPPER = 16.0 * 3;  // RayCAD: public and static (engine.cpp sizes the grid)
private:
    const int MAX_ITERATIONS = 30;
    const double AREA_ERROR_FACTOR = 0.05;     /* how precise to match the cut area to optimal,
                                                  reasonable value: 0.05 = 5%*/
    const size_t ANGLE_HISTORY_POINTS = 3;     // used for angle prediction
    const int DIRECTION_SMOOTHING_BUFLEN = 3;  // gyro points - used for angle smoothing


    const double CLEAN_PATH_TOLERANCE = 1.415;            // should be >sqrt(2)
    const double FINISHING_CLEAN_PATH_TOLERANCE = 1.415;  // should be >sqrt(2)
    const double FINISHING_THICKNESS_SCALE = 1 / 10.;     // finish at this fraction of stepover
};
}  // namespace ray::cam::adaptive2d::fork
// clang-format on
