// Time budgets of the UI tests: the longest UI-thread gap (PLAN.md 7: the UI thread never blocks for long), the
// warm start to the first frame (PLAN.md 9), how soon Esc ends a job. They are budgets for an optimised build. A
// Debug build runs Qt's debug libraries, OpenCASCADE's and our own code unoptimised, and every step on the UI
// thread takes about two to three times as long (a 105 ms gap where Release has 40 ms; a first frame at 2.75 s
// where Release has under 1 s). There the budgets are scaled by kDebugBudgetScale, so a Debug run still catches
// what they are for (a solve or a mesh on the UI thread stalls it for seconds) without failing on the build's speed.
#pragma once

namespace ray::test {

#ifdef NDEBUG
inline constexpr double kDebugBudgetScale = 1.0;
#else
inline constexpr double kDebugBudgetScale = 4.0;
#endif

// A budget given for an optimised build, in ms, for this build.
constexpr double uiBudgetMs(double optimisedMs)
{
    return optimisedMs * kDebugBudgetScale;
}

} // namespace ray::test
