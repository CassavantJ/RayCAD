// Helpers of the adaptive engine's tests (task T-060): the S5 pockets as engine input, and the engine's output as the
// sweep checker's input. The checker (tests/tools/sweepcheck) shares no code with the engine; these helpers only
// convert between the two sets of plain types.
#pragma once

#include "cam/geom2d.h"
#include "cam/ops/adaptive2d/engine.h"
#include "s5_cases.h"
#include "sweepcheck.h"

#include <ray/core.h>

#include <string>
#include <vector>

namespace ray::cam::adaptive2d::test {

namespace sw = ray::test::sweep;

inline constexpr double kToolMm = sw::s5::kToolDia;

inline geom2d::Path loopPath(const sw::Loop& l)
{
    geom2d::Path p;
    p.closed = true;
    p.start = geom2d::toPt({l[0].first, l[0].second});
    for (std::size_t i = 1; i <= l.size(); i++) {
        const sw::Pt& q = l[i % l.size()];
        p.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, geom2d::toPt({q.first, q.second})});
    }
    return geom2d::canonicalize(std::move(p));
}

inline geom2d::Regions regionsOf(const sw::Loops& loops)
{
    std::vector<geom2d::Path> paths;
    for (const sw::Loop& l : loops) {
        paths.push_back(loopPath(l));
    }
    JobContext ctx;
    auto nesting = geom2d::nest(paths, ctx);
    return nesting ? geom2d::regionsByNesting(paths, *nesting) : geom2d::Regions{};
}

// S5's parameters (spikes/s5-adaptive/src/gen.cpp): 12.7 mm tool, tolerance 0.1 mm, finishing pass, keep-tool-down
// ratio 3, helix 75% of the tool (minimum: upstream's default, tool / 8), allowed outside the stock.
inline Input inputFor(const sw::s5::Case& c, double stepover)
{
    Input in;
    in.regions = regionsOf(c.pocket);
    in.stock = regionsOf(c.stock);
    in.toolDiameter = toNm(kToolMm);
    in.stepover = stepover;
    in.tolerance = kDefaultTolerance;
    in.finishingPass = true;
    in.keepToolDownRatio = 3.0;
    in.helixDiameter = toNm(0.75 * kToolMm);
    in.enterFromOutside = true;
    return in;
}

inline sw::Pt mmPt(Pt p)
{
    return {toMm(p.x), toMm(p.y)};
}

// The engine's output as the checker's input: helix entries as helices, LinkClear links as tool-down links,
// LinkRetract links as retracts, everything else as cuts.
inline sw::Input checkInputFor(const sw::s5::Case& c, const Output& out, double stepover)
{
    sw::Input in;
    in.pocket = c.pocket;
    in.stock = c.stock;
    in.toolDiameter = kToolMm;
    in.targetDeg = angleForStepover(stepover);
    for (const Region& r : out.regions) {
        if (r.entry.kind == EntryMode::None) {
            continue;
        }
        sw::Region rg;
        rg.helixCentre = mmPt(r.entry.centre);
        rg.helixRadius = r.entry.kind == EntryMode::Helix ? toMm(r.entry.radius) : 0.0;
        rg.start = mmPt(r.entry.start);
        for (const Path& p : r.paths) {
            sw::Path sp;
            sp.kind = p.motion == Motion::Cut         ? sw::Kind::Cut
                      : p.motion == Motion::LinkClear ? sw::Kind::LinkDown
                                                      : sw::Kind::Retract;
            for (const Pt& q : p.points) {
                sp.points.push_back(mmPt(q));
            }
            rg.paths.push_back(std::move(sp));
        }
        in.regions.push_back(std::move(rg));
    }
    return in;
}

inline Result<Output> runCase(const std::string& id, double stepover)
{
    static const std::vector<sw::s5::Case> cases = sw::s5::makeCases();
    JobContext ctx;
    return runEngine(inputFor(sw::s5::caseById(cases, id), stepover), ctx);
}

inline std::string hashText(const std::string& text)
{
    Hasher h;
    h.str(text);
    return h.finish().hex();
}

} // namespace ray::cam::adaptive2d::test
