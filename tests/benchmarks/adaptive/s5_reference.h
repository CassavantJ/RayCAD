// The adaptive review pack's reference case (task T-062): spike S5's islands pocket (90 x 60 mm, two islands,
// tests/tools/sweepcheck/s5_cases.h "p3") at 10 % load with a 12.7 mm tool, measured exactly as the engine's own
// acceptance tests measure it, so the report's known issues carry today's values:
//   - p99 and its forward-half reading: tests/unit/cam/adaptive_acceptance_test.cpp (the engine run, leads counted as
//     cuts);
//   - the reduced-feed flag check: tests/unit/cam/adaptive_entry_test.cpp (the entry planner's run, its flags).
// Kept in a file of its own because the engine's test helpers (adaptive_test_util.h) and the operation's
// (adaptive2d_test_util.h) both declare short helper names in neighbouring namespaces.
#pragma once

namespace ray::bench::adaptive {

struct S5Reference {
    double targetDeg = 0;     // straight-cut engagement of the load
    double p99Deg = 0;        // 99th percentile by cut length, full reading
    double forwardP99Deg = 0; // the same on the tool's forward half
    double medianDeg = 0;
    double unflaggedFullPct = 0;    // cut length read above 1.25 x target on moves without the reduced-feed flag
    double unflaggedForwardPct = 0; // the same, forward-half reading
    double flaggedPct = 0;          // cut length on flagged moves
};

S5Reference measureS5Islands();

} // namespace ray::bench::adaptive
