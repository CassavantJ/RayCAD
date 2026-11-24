// Gouge detection of the simulator (task T-072; simulator.h, GOUGES, says what is measured and why). Per sub-motion,
// exact against the part's triangles: a parallel pass flags the sub-motions whose cutting profile, shrunk by the
// threshold, still meets the part; then each run of flagged moves gets its depth (bisection on the shrink) and the tip
// where it first goes beyond the tolerance (bisection along its first sub-motion). Every result is a function of one
// sub-motion and the part, merged in program order: the worker count changes nothing.
#include "simulator.h"

#include <algorithm>
#include <cmath>

namespace ray::sim::detail {

namespace {

// The shrink a sub-motion's flag test uses: the threshold less the arc's sagitta for a chord (the arc lies up to that
// outside its chord, so the chord's tool is grown back by it).
double chordAdjust(bool chord)
{
    return chord ? kVerifyArcSagittaMm : 0.0;
}

// The profile shrunk by s as shrunkProfile (verify_geom.h) shrinks it (every radius less s, both ends pulled in by s:
// it holds the tool eroded by a ball of radius s), except that no radius goes below the smaller of its own and
// kGougeNeedleMm: past the tool's radius a needle along the axis is left, which goes on measuring how far the tool
// went in along its axis (a tool wholly inside the part).
std::vector<Vec2> shrunk(const std::vector<Vec2>& profile, double s)
{
    std::vector<Vec2> pts;
    for (const Vec2& v : profile) {
        if (std::isfinite(v.x) && std::isfinite(v.y)) {
            Vec2 q{std::max(0.0, v.x), v.y};
            if (!pts.empty()) {
                q.y = std::max(q.y, pts.back().y);
            }
            pts.push_back(q);
        }
    }
    if (pts.size() < 2 || !(s > 0)) {
        return pts;
    }
    const double lo = pts.front().y + s, hi = pts.back().y - s;
    if (!(hi > lo)) {
        return {};
    }
    std::vector<Vec2> out;
    for (const Vec2& v : pts) {
        out.push_back({std::max(v.x - s, std::min(v.x, kGougeNeedleMm)), std::clamp(v.y, lo, hi)});
    }
    return out;
}

CutterShape shrunkZone(const std::vector<Vec2>& profile, double s)
{
    return compileProfile(shrunk(profile, s), false);
}

double maxRadius(const std::vector<Vec2>& p)
{
    double r = 0;
    for (const Vec2& v : p) {
        r = std::max(r, v.x);
    }
    return r;
}

struct GougeTool {
    std::vector<Vec2> profile; // empty: nothing to test
    CutterShape flag[2];       // shrunk by the threshold: [0] lines, [1] chords of arcs
    double span = 0;           // the profile's height range; shrunk by span / 2 nothing is left
    double radius = 0;         // its largest radius
};

Vec3 lerp(const Vec3& a, const Vec3& b, double t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

} // namespace

std::vector<Vec2> gougeProfile(const ToolShape& shape, const lib::Tool* tool)
{
    std::vector<Vec2> p = shape.cuttingProfile;
    if (tool != nullptr && (tool->type == lib::ToolType::Tap || tool->type == lib::ToolType::ThreadMill) &&
        tool->threadPitch > 0) {
        const double d = kThreadDepthPerPitch * tool->threadPitch;
        for (Vec2& v : p) {
            v.x = std::max(0.0, v.x - d);
        }
    }
    return p;
}

Result<std::vector<GougeEvent>> findGouges(const VerifyInput& in, const std::vector<Motion>& motions, double tolMm,
                                           IJobSystem& jobs, JobContext& ctx, GougeStats* statsOut)
{
    std::vector<GougeEvent> out;
    GougeStats stats;
    Progress flagProgress = ctx.progress.sub(0.8);
    Progress depthProgress = ctx.progress.sub(0.2);
    if (in.part == nullptr || in.part->indices.empty() || motions.empty()) {
        ctx.progress.set(1.0, "Checking for gouges");
        if (statsOut != nullptr) {
            *statsOut = stats;
        }
        return out;
    }
    const double tol = std::max(tolMm, kGougeMinTolMm);
    const double threshold = tol - kGougePartChordMm - kArcChordTolMm;
    const SoupIndex part(*in.part);

    std::vector<GougeTool> tools(in.ops.size());
    for (std::size_t k = 0; k < in.ops.size(); ++k) {
        const VerifyOperation& op = in.ops[k];
        if (op.shape == nullptr || op.shape->cuttingProfile.size() < 2) {
            continue;
        }
        GougeTool& t = tools[k];
        t.profile = gougeProfile(*op.shape, op.tool);
        t.span = t.profile.back().y - t.profile.front().y;
        t.radius = maxRadius(t.profile);
        for (int v = 0; v < 2; ++v) {
            t.flag[v] = shrunkZone(t.profile, threshold - chordAdjust(v == 1));
        }
    }

    // Pass 1: flag every sub-motion whose shrunk tool still meets the part.
    std::vector<std::uint8_t> flagged(motions.size(), 0);
    constexpr std::size_t kGrain = 512;
    JobContext flagCtx{ctx.cancel, flagProgress};
    const Status st = jobs.parallelChunks(motions.size(), kGrain, flagCtx,
                                          [&](std::size_t, std::size_t begin, std::size_t end) -> Status {
                                              for (std::size_t i = begin; i < end; ++i) {
                                                  const Motion& m = motions[i];
                                                  const GougeTool& t = tools[m.op];
                                                  if (t.profile.empty()) {
                                                      continue;
                                                  }
                                                  const CutterShape& z = t.flag[m.chord ? 1 : 0];
                                                  flagged[i] = sweepHitsSoup(z, m.a, m.b, part) ? 1 : 0;
                                              }
                                              return Status{};
                                          });
    if (!st) {
        return tl::unexpected(st.error());
    }
    for (std::size_t i = 0; i < motions.size(); ++i) {
        if (!tools[motions[i].op].profile.empty()) {
            ++stats.tested;
        }
        stats.flagged += flagged[i];
    }

    // Pass 2: runs of consecutive IR moves of one operation with a flagged sub-motion, in program order.
    struct Run {
        std::uint32_t op = 0, firstMove = 0, lastMove = 0, moves = 0;
        std::uint32_t firstMotion = 0;
        std::vector<std::uint32_t> flaggedMotions;
    };
    std::vector<Run> runs;
    for (std::uint32_t i = 0; i < motions.size(); ++i) {
        if (flagged[i] == 0) {
            continue;
        }
        const Motion& m = motions[i];
        if (!runs.empty()) {
            Run& r = runs.back();
            if (r.op == m.op && m.move <= r.lastMove + 1) {
                if (m.move != r.lastMove) {
                    ++r.moves;
                    r.lastMove = m.move;
                }
                r.flaggedMotions.push_back(i);
                continue;
            }
        }
        Run r;
        r.op = m.op;
        r.firstMove = r.lastMove = m.move;
        r.moves = 1;
        r.firstMotion = i;
        r.flaggedMotions.push_back(i);
        runs.push_back(std::move(r));
    }

    std::size_t done = 0;
    for (const Run& r : runs) {
        if (Status s = ctx.cancel.check(); !s) {
            return tl::unexpected(s.error());
        }
        depthProgress.set(static_cast<double>(done) / static_cast<double>(runs.size()), "Measuring gouges");
        ++done;
        const GougeTool& t = tools[r.op];
        // The depth of a sub-motion is at least s when the profile shrunk by s (less the chord's adjustment) meets the
        // part. Shrunk by half the profile's span nothing is left.
        const auto hitsAt = [&](const Motion& m, double s) {
            const double e = s - chordAdjust(m.chord);
            if (e * 2.0 >= t.span) {
                return false;
            }
            return sweepHitsSoup(shrunkZone(t.profile, e), m.a, m.b, part);
        };
        double deepest = threshold;
        std::uint32_t deepestMotion = r.firstMotion;
        bool whole = false; // the needle went in over the whole cutting length: the depth is a lower bound
        for (const std::uint32_t i : r.flaggedMotions) {
            // Every few sub-motions: a long run of gouging moves would otherwise hold off a cancel.
            if ((i & 15u) == 0) {
                if (Status s = ctx.cancel.check(); !s) {
                    return tl::unexpected(s.error());
                }
            }
            const Motion& m = motions[i];
            // Only a sub-motion deeper than the deepest so far needs a search.
            const double probe = deepest + kGougeDepthStepMm;
            if (!hitsAt(m, probe)) {
                continue;
            }
            double lo = probe;
            double hi = t.span * 0.5 + chordAdjust(m.chord);
            while (hi - lo > kGougeDepthStepMm) {
                const double mid = 0.5 * (lo + hi);
                if (hitsAt(m, mid)) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            deepest = lo;
            deepestMotion = i;
            whole = hi >= t.span * 0.5 + chordAdjust(m.chord) - 2.0 * kGougeDepthStepMm;
        }
        const bool wholly = deepest >= t.radius; // past the radius: the needle, depth along the axis
        // The tip where the run's first sub-motion first goes beyond the tolerance.
        const Motion& f = motions[r.firstMotion];
        const CutterShape& z = t.flag[f.chord ? 1 : 0];
        Vec3 at = f.a;
        if (!sweepHitsSoup(z, f.a, f.a, part)) {
            double t0 = 0, t1 = 1;
            for (int it = 0; it < 40; ++it) {
                const double mid = 0.5 * (t0 + t1);
                if (sweepHitsSoup(z, f.a, lerp(f.a, f.b, mid), part)) {
                    t1 = mid;
                } else {
                    t0 = mid;
                }
            }
            at = lerp(f.a, f.b, t1);
        }
        const double depth = deepest + kGougePartChordMm + kArcChordTolMm;
        GougeEvent g;
        g.op = r.op;
        SimEvent& e = g.event;
        e.kind = EventKind::Gouge;
        e.severity = Severity::Error;
        e.operation = in.ops[r.op].id;
        e.moveIndex = r.firstMove;
        e.position = at;
        e.amountMm = depth;
        std::string msg = "The tool cuts into the part, up to " + mmText(depth) + " mm deep";
        if (whole) {
            msg = "The tool goes wholly into the part, at least " + mmText(depth) +
                  " mm deep along its axis (its whole cutting length)";
        } else if (wholly) {
            msg = "The tool goes wholly into the part, up to " + mmText(depth) + " mm deep along its axis";
        }
        msg += ", beyond the gouge tolerance of " + mmText(tol) + " mm";
        if (tol > tolMm) {
            msg += " (raised from " + mmText(tolMm) + " mm, below what the part's triangles can tell)";
        }
        const std::uint32_t deepestMove = motions[deepestMotion].move;
        if (r.moves > 1) {
            msg += "; this move and the " + std::to_string(r.moves - 1) + " after it, deepest at move " +
                   std::to_string(deepestMove);
        }
        msg += ". Measured exactly against the part's triangles (to within " +
               mmText(kGougePartChordMm + kArcChordTolMm) + " mm), not on the stock cells: the same at every tier.";
        e.message = std::move(msg);
        out.push_back(std::move(g));
    }
    ctx.progress.set(1.0, "Checking for gouges");
    if (statsOut != nullptr) {
        *statsOut = stats;
    }
    return out;
}

} // namespace ray::sim::detail
