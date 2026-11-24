// See s5_reference.h.
#include "s5_reference.h"

#include "adaptive_test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ray::bench::adaptive {

S5Reference measureS5Islands()
{
    using namespace ray::cam::adaptive2d;
    using namespace ray::cam::adaptive2d::test;
    static const std::vector<sw::s5::Case> cases = sw::s5::makeCases();
    const sw::s5::Case& c = sw::s5::caseById(cases, "p3");
    constexpr double load = 0.1;
    S5Reference ref;
    ref.targetDeg = angleForStepover(load);
    {
        // adaptive_acceptance_test.cpp accept(): the engine's passes, leads counted as cuts
        auto out = runCase("p3", load);
        REQUIRE(out);
        const sw::Report r = sw::check(checkInputFor(c, *out, load, {}, true));
        ref.p99Deg = r.p99;
        ref.forwardP99Deg = r.forward.p99;
        ref.medianDeg = r.median;
    }
    {
        // adaptive_entry_test.cpp allRows(): the entry planner's output with its reduced-feed flags
        auto out = planCase("p3", load);
        REQUIRE(out);
        const sw::Report r = sw::check(checkInputFor(c, *out, load));
        ref.unflaggedFullPct = r.pctAboveFlagCheckUnflagged;
        ref.unflaggedForwardPct = r.forward.pctAboveFlagCheckUnflagged;
        ref.flaggedPct = r.pctFlagged;
    }
    return ref;
}

} // namespace ray::bench::adaptive
