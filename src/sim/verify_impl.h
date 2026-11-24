// The safety checks' internals shared by their source files (task T-071). What they do: verify.h.
#pragma once

#include "verify.h"
#include "verify_geom.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ray::sim::detail {

// One straight sub-motion of the tool tip, setup WCS, mm.
struct Motion {
    Vec3 a, b;
    std::uint32_t op = 0;   // index into VerifyInput::ops
    std::uint32_t move = 0; // the IR move it belongs to
    bool rapid = false;
    bool chord = false; // a chord of an arc (the tool is inflated by kVerifyArcSagittaMm along it)
};

// The motions of every operation in program order (verify_motion.cpp): first the tool standing at the toolpath's start
// (a zero-length rapid of move 0), then arcs as chords within kVerifyArcSagittaMm, canned cycles as buildBackplot
// expands them (with the machine's peck clearance), other zero-length motions left out.
std::vector<Motion> expandMotions(const VerifyInput& in);

// The zones of one operation's tool, compiled (verify.cpp). [0] for straight motions, [1] for chords of arcs.
struct ToolZones {
    CutterShape cutting[2], shank[2], holder[2]; // grown by kVerifyGrowMm (+ the sagitta for chords)
    CutterShape cuttingSkin[2], shankSkin[2];    // shrunk by the part skin (rapids against the part)
    CutterShape dead[2];                         // the dead zone, grown; empty for a centre-cutting tool
    bool monotone = false;                       // the radius never narrows going up
    double cuttingRadius = 0;
    double deadRadius = 0; // 0: centre-cutting
    double fluteTop = 0;   // the height the cutting profile ends at
    double shankTop = 0;   // the holder's nose (top of the shank zone)
    double shankTopRadius = 0;
    // What more stick-out would bring out of the holder: flute up to toolFluteTop (it cuts: no collision), then the
    // body at bodyRadius.
    double toolFluteTop = 0;
    double bodyRadius = 0;
    double gaugeHeight = 0; // the top of the tool above its tip (travel in Z)
    bool hasHolder = false;
    // Exact-profile ToolShapes for the cut-stock model's queries (the endless top cut off with a zero-radius stub
    // where a zone must stay bounded).
    ToolShape full, shankOnly, holderOnly, deadOnly;
};

// Kinds of contact the analytic layer hands to the cut-stock layer (bits).
enum DeferBits : std::uint8_t { kDeferRapid = 1, kDeferShank = 2, kDeferHolder = 4, kDeferPlunge = 8 };

struct Deferred {
    std::uint32_t motion = 0;
    std::uint8_t bits = 0;
};

// Where an obstacle hit came from.
enum class Layer : std::uint8_t { Analytic, CutStock };
// Obstacle ids: kStock, kPart, then kFixture0 + fixture index; kNone for events without one.
inline constexpr std::int32_t kNone = -1;
inline constexpr std::int32_t kStock = 0;
inline constexpr std::int32_t kPart = 1;
inline constexpr std::int32_t kFixture0 = 2;

// One hit of one sub-motion, before runs are joined.
struct RawHit {
    std::uint32_t op = 0, move = 0, motion = 0;
    EventKind kind = EventKind::RapidIntoStock;
    std::int32_t obstacle = kNone;
    Layer layer = Layer::Analytic;
    Vec3 position{}; // filled when the event is made (analytic) or at the hit (cut stock)
    bool havePosition = false;
    double amount = 0;
};

// A feed motion of an operation whose holder or shank met something: what the ToolTooShort suggestion needs.
struct LengthHit {
    std::uint32_t motion = 0;
    bool holder = false;   // else the shank
    double need = 0;       // holder: the extra stick-out that clears; shank: the flute length that clears
    bool clearable = true; // holder: false when no stick-out clears
};

// The cut-stock layer (verify_cutstock.cpp): runs the program into a model at the fixed cell up to the last deferred
// motion and turns the deferred contacts that meet material into hits.
struct CutStockResult {
    std::vector<RawHit> hits;
    std::vector<LengthHit> lengths;
    double cellMm = 0;
};
Result<CutStockResult> runCutStock(const VerifyInput& in, const std::vector<Motion>& motions,
                                   const std::vector<ToolZones>& zones, const std::vector<Deferred>& deferred,
                                   IJobSystem& jobs, JobContext& ctx, Progress progress);

// Lengths for ToolTooShort (verify.cpp): the lifted holder (holder raised by s, the shank lengthened by s) and the
// shank kept above height `from` as ToolShapes / zones.
ToolShape liftedHolderShape(const ToolZones& z, double s);
ToolShape shankAboveShape(const ToolZones& z, double from);
// The smallest s in [0, cap] for which hits(s) is false, to 1e-4 mm; cap + 1 when hits(cap). hits must be false
// somewhere above a value once false there (lifting only helps).
template <class F>
double smallestClear(double cap, F&& hits)
{
    if (!hits(0.0)) {
        return 0.0;
    }
    if (hits(cap)) {
        return cap + 1.0;
    }
    double lo = 0.0, hi = cap;
    while (hi - lo > 1e-4) {
        const double mid = 0.5 * (lo + hi);
        if (hits(mid)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return hi;
}
inline constexpr double kLiftCapMm = 1000.0; // the largest extra stick-out or flute length searched

// Text of a length in mm to 0.001, rounded half to even, without trailing zeros ("12.7", "-3").
std::string mmText(double mm);

} // namespace ray::sim::detail
