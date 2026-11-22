// Feeds and speeds (cam.h computeFeeds, task T-048): spindle speed and feed from a tool preset's surface speed and
// chip load, with radial chip thinning and the diameter in contact of round and conical tools, held inside the
// machine's rpm, feed, power and torque limits.
//
// FORMULAS (standard machining formulas; sources are public references, cited by title, nothing copied from tables)
//   Spindle speed      n = 1000 vc / (pi Deff)                          rpm; vc m/min, Deff mm
//                      Machinery's Handbook (Industrial Press), "Speeds and Feeds"; Sandvik Coromant, "Milling
//                      formulas and definitions" and "Drilling formulas and definitions".
//   Table feed         vf = n fz z                                       mm/min; fz mm per flute, z flutes
//                      (same sources). Drilling: feed per revolution f = fz z.
//   Tapping feed       vf = P n                                          mm/min; P pitch, mm (any tapping guide;
//                      Machinery's Handbook, "Tapping").
//   Radial chip        for ae < Deff/2: hex = fz sin(arccos(1 - 2 ae / Deff)) = fz 2 sqrt(ae (Deff - ae)) / Deff,
//   thinning           so fz = hex Deff / (2 sqrt(ae (Deff - ae))); for ae >= Deff/2 the chip reaches fz: factor 1.
//                      Sandvik Coromant, "Milling formulas and definitions" (maximum chip thickness hex); Harvey Tool,
//                      "Combating Chip Thinning" (radial chip thinning factor).
//   Lead (entering)    hex = fz sin(kr), kr the angle between the cutting edge and the feed direction where the chip is
//   angle thinning     thickest. Ball nose at depth ap < R: sin(kr) = Deff / D. Corner radius r at ap < r:
//                      sin(kr) = sqrt(ap (2r - ap)) / r (the round-insert formula). Chamfer and engraving tools of
//                      included angle 2a: kr = 90 deg - a, sin(kr) = cos(a). Sandvik Coromant, "Milling formulas and
//                      definitions" (entering angle, round inserts); Harvey Tool, "Ball Nose Milling Without a
//                      Compromise" (effective diameter and ball-nose chip thinning).
//   Effective          ball, ap < R: Deff = 2 sqrt(ap (D - ap)); corner radius, ap < r: Deff = D - 2r + 2 sqrt(ap (2r -
//   ap)); diameter           chamfer, engraving and spot tools: Deff = tip diameter + 2 ap tan(a), at most D. Other
//   tools: D.
//                      (geometry of the tool; same sources)
//   Removal rate       milling Q = ae ap vf / 1000 cm3/min; drilling Q = pi/4 (D^2 - d0^2) vf / 1000, d0 = 0 for solid
//                      drilling (Sandvik Coromant formulas). For ball and corner-radius tools at shallow depth ae x ap
//                      is larger than the real chip section, so Q, power and torque are upper bounds there.
//   Cutting power      Pc = Q kc W / 60000 kW (Q cm3/min, kc N/mm2). Sandvik's milling net power Pc = ae ap vf kc /
//   60e6
//                      and drilling Pc = vc f D kc / 240000 are this formula for milling and for solid drilling.
//                      W is the tool wear factor (Machinery's Handbook's power formula has such a factor W).
//   Motor power        P = Pc / eta, eta the spindle (drive) efficiency (Machinery's Handbook, "Machining Power").
//   Spindle torque     T = Pc 60000 / (2 pi n) N m (power = torque x angular speed).
//   Tapping torque     T = kc W V / (2 pi) per revolution, V = 7/16 x pi/4 (D^2 - D1^2) P the thread groove volume cut
//                      per revolution, D1 = D - 2 x 5H/8 = D - (5 sqrt(3) / 8) P the basic minor diameter and 7/16 the
//                      part of the band D1..D the internal thread's groove fills (trapezoid of widths P/8 and 3P/4 over
//                      the band height 5H/8, ISO 68-1 / ASME B1.1 basic profile). It assumes a full-depth thread from
//                      a basic-minor-diameter hole: an upper bound. Power: P = T 2 pi n / 60000 / eta.
//
// KC MODEL. Specific cutting force depends on chip thickness: kc = kc1.1 hm^-mc, kc1.1 the force for a 1 mm x 1 mm
// chip, hm the mean chip thickness in mm, mc the material's exponent (Sandvik Coromant, "Milling formulas and
// definitions" and "Drilling formulas and definitions"; the kc1 and mc of ISO 513 / VDI 3323 material groups). With a
// material's mc (lib::Material::mc > 0), milling and drilling use it (PowerModel *KcMc):
//   Milling            hm = fz sin(kr) (1 - cos phi) / phi = fz sin(kr) 2 ae / (Deff phi), phi = arccos(1 - 2 ae /
//   Deff)
//                      the engagement angle (radians; ae >= Deff: phi = pi); the mean of fz sin(kr) sin(theta) over the
//                      arc, Sandvik's hm = sin(kr) 360 ae fz / (pi Deff arccos(1 - 2 ae / Deff)). Face mills (a cut
//                      centred on the cutter, as above): hm = fz (ae / D) / arcsin(ae / D). sin(kr) = 1 / leadThinning.
//   Drilling           hm = fz sin(kr), kr half the point angle (Tool::tipAngleDeg; 90 deg when it is 0).
//   Power and torque then grow as vf^(1 - mc) at a fixed rpm, so step 7 below repeats its proportional reduction until
//   the limits hold (each repeat leaves an excess ratio r at r^mc).
// mc is used as k / 256, k = mc x 256 rounded (at most 255): h^(k/256) is a product of repeated square roots of h,
// which dmath::sqrt computes exactly (IEEE), so kc is identical on every platform without a pow. A published
// two-decimal mc is at most 0.002 off, under 1 % of kc for chips down to 0.01 mm. Without mc, kc1.1 is used flat
// (PowerModel *Kc11): with hm well below 1 mm the true kc is higher (for mc 0.25 and hm 0.05 mm about 2.1 x kc1.1),
// so power and torque read low. Taps always use kc1.1 flat: a tap's chip thickness depends on its chamfer length,
// which lib::Tool does not hold.
//
// ORDER OF LIMITS (deterministic; every step is recorded in FeedsResult::limits)
//   Milling and drilling (only rpm and feed change; engagement is the operation's):
//     1. n from vc at Deff.  2. Maximum rpm: n lowered to it.  3. Minimum rpm: n raised to it.  4. n rounded to a
//     whole rpm.  5. vf = n fz z: the chip load is kept at the rpm the machine can run.  6. Maximum feed of the axes
//     that move (milling X and Y, drilling Z): vf lowered, so the chip load drops (flagged).  7. Power and torque,
//     together: both are proportional to vf at a fixed n, so vf is scaled by the smaller of Pmax/P and Tmax/T; each
//     one that was over is flagged Applied.  8. vf rounded down to a whole um/min (then lowered by 1 um/min while
//     any known limit would be exceeded by rounding).  9. Every reported value is recomputed from the rounded n and vf.
//   Tapping (vf = P n is fixed, so only n changes): 1. to 4. as above, the rounding also to the rpm step of an exact
//     feed (below).  5. Maximum Z feed: n lowered to the largest step with P n <= Fmax.  6. Power: n lowered in
//     proportion.  7. Torque per revolution does not depend on n: if over, it is Exceeded (no rpm fixes it).
//     A reduction that would go below the minimum rpm stops there and that limit is Exceeded.
//
// ROUNDING AND DETERMINISM. rpm is a whole number, rounded down (never above the surface speed or the maximum), or up
// to the machine minimum. Feeds are whole micrometres per minute (the toolpath unit), rounded down, so a rounded feed
// is never above a limit or the chip load. Taps: feed um/min = pitch nm x rpm / 1000; when 1000 / gcd(pitch nm, 1000)
// is at most kMaxTapRpmStep the rpm is rounded down to a multiple of it, so the feed is exact (metric pitches and 20,
// 16 and 32 TPI); otherwise the feed is rounded to the nearest um/min, ties to even (tapFeedExact false). Only +, -,
// x, /, sqrt (IEEE exact), floor and ray::dmath reach the output, in a module built without contraction or fast math,
// so results are identical on every platform.
//
// ASSUMPTIONS are the named constants below.
#include <ray/cam.h>

#include <algorithm>
#include <cmath> // std::floor, std::ceil, std::isfinite only; trigonometry goes through ray::dmath (PLAN.md section 8)
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <utility>

namespace ray::cam {
namespace {

constexpr double kPi = 3.14159265358979323846;
// Spindle drive efficiency: cutting power over motor power. 0.8 is the usual figure for a geared or belt-driven
// machining-centre spindle (Machinery's Handbook, "Machining Power", gives efficiencies of this order); direct-drive
// spindles are better, so this errs on the safe side.
constexpr double kSpindleEfficiency = 0.8;
// Tool wear factor W: a worn edge needs more force than a sharp one. 1.2 is an allowance chosen here for a tool
// partway through its life, of the order of Machinery's Handbook's W values; it raises power and torque, never feed.
constexpr double kToolWearFactor = 1.2;
// The largest chip thinning compensation (radial x lead). Below about ae = 0.01 D, or a ball nose cutting with its
// tip, the formulas ask for feeds that rub instead of cut; tool makers' guides stop compensating well before that.
// The effective diameter is also not taken below D / kMaxChipThinningFactor, so a shallow ball cut does not ask for
// unbounded rpm.
constexpr double kMaxChipThinningFactor = 5.0;
// Taps: the largest rpm step accepted to make pitch x rpm a whole number of um/min (32 TPI needs 4).
constexpr std::int64_t kMaxTapRpmStep = 10;
// ISO 68-1 basic profile: D1 = D - 2 x 5H/8 with H = sqrt(3)/2 P, so D - D1 = 5 sqrt(3) / 8 P.
constexpr double kIsoMinorDiameterPerPitch = 1.0825317547305482;
// ISO 68-1 basic profile: the internal thread's groove fills 7/16 of the band between D1 and D.
constexpr double kTapGrooveFraction = 7.0 / 16.0;
// mc is used in steps of 1 / kMcSteps (KC MODEL): 2^8, the number of repeated square roots.
constexpr int kMcSteps = 256;
// Step 7 with mc: the most repeats of the proportional power and torque reduction (each takes the excess ratio r to
// r^mc, mc < 1; 64 leave less than 1 um/min, which step 8 then removes).
constexpr int kMaxLimitRepeats = 64;
// Slack for rounding down values that are whole numbers in exact arithmetic but may land one ulp below.
constexpr double kRpmSlack = 1e-9;
constexpr double kFeedSlackUm = 1e-6;

Error invalid(std::string message)
{
    return Error{ErrorCode::InvalidArgument, std::move(message), {"computeFeeds"}};
}

bool finite(double v)
{
    return std::isfinite(v);
}

// k of mc = k / kMcSteps: mc x kMcSteps rounded half up, 0 for mc not above 0 (or not finite), at most kMcSteps - 1.
int mcSteps(double mc)
{
    if (!std::isfinite(mc) || !(mc > 0)) {
        return 0;
    }
    const double k = std::floor(mc * kMcSteps + 0.5);
    return k >= kMcSteps ? kMcSteps - 1 : static_cast<int>(k);
}

// h^(k / kMcSteps) for h > 0, 0 <= k < kMcSteps: h^(1/2), h^(1/4), ... by repeated dmath::sqrt, multiplied in over the
// bits of k from the highest (bit 128 is h^(128/256) = h^(1/2)).
double powSteps(double h, int k)
{
    double out = 1;
    double root = h;
    for (int bit = kMcSteps / 2; bit >= 1; bit /= 2) {
        root = dmath::sqrt(root);
        if ((k & bit) != 0) {
            out *= root;
        }
    }
    return out;
}

// A machine value: positive and finite is a known limit; 0 (or anything else) is not known.
bool known(double v)
{
    return std::isfinite(v) && v > 0;
}

FeedsMode modeOf(lib::ToolType type, std::string_view opType)
{
    using T = lib::ToolType;
    if (type == T::Tap)
        return FeedsMode::Tapping;
    if (type == T::Drill || type == T::SpotDrill || type == T::Reamer || type == T::BoringBar || opType == "drill")
        return FeedsMode::Drilling;
    return FeedsMode::Milling;
}

std::string_view defaultOpClass(lib::ToolType type, std::string_view opType, double ae, double d)
{
    using T = lib::ToolType;
    switch (type) {
    case T::Tap:
        return "tap";
    case T::Drill:
        return "drill";
    case T::SpotDrill:
        return "spot";
    case T::Reamer:
        return "ream";
    case T::BoringBar:
        return "bore";
    case T::ChamferMill:
        return "chamfer";
    case T::FaceMill:
        return "face";
    case T::ThreadMill:
        return "thread";
    case T::EngraveCutter:
        return "engrave";
    case T::FlatEndMill:
    case T::BallEndMill:
    case T::BullEndMill:
        break;
    }
    // End mills: a slot (by type, plunging, or a cut as wide as the tool) uses the slot data, anything else rough.
    // Finishing is chosen by the operation through FeedsInput::opClass.
    if (opType == "slot" || opType == "drill" || ae >= d)
        return "slot";
    return "rough";
}

const lib::ToolPreset* findPreset(const lib::Tool& tool, const MaterialId& material, std::string_view opClass)
{
    for (const auto& p : tool.presets)
        if (p.material == material && p.opClass == opClass)
            return &p;
    return nullptr;
}

std::string presetClassesFor(const lib::Tool& tool, const MaterialId& material)
{
    std::vector<std::string> classes;
    for (const auto& p : tool.presets)
        if (p.material == material)
            classes.push_back(p.opClass);
    std::sort(classes.begin(), classes.end());
    classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
    std::string out;
    for (const auto& c : classes)
        out += (out.empty() ? "" : ", ") + c;
    return out;
}

// Smallest known maxFeed of the given axes; 0 when none is known.
double axisFeedLimit(const lib::MachineDef& machine, FeedsMode mode)
{
    double limit = 0;
    for (const auto& axis : machine.axes) {
        bool moves = mode == FeedsMode::Milling ? (axis.letter == 'X' || axis.letter == 'Y') : axis.letter == 'Z';
        if (moves && known(axis.maxFeedMmMin))
            limit = limit > 0 ? std::min(limit, axis.maxFeedMmMin) : axis.maxFeedMmMin;
    }
    return limit;
}

// Diameter in contact and the lead-angle thinning factor (milling only) of the tool at depth ap from its tip.
// Milling: ball, corner-radius, chamfer and engraving tools. Drilling: spot drills when a depth is given (ap 0: the
// full diameter); a ball or corner-radius end mill plunging cuts with its whole diameter.
struct Contact {
    double deff = 0;
    double lead = 1;
};

Contact contactOf(const lib::Tool& tool, FeedsMode mode, double ap, std::vector<std::string>& notes)
{
    using T = lib::ToolType;
    const double d = tool.diameter;
    const bool milling = mode == FeedsMode::Milling;
    Contact c{d, 1};
    switch (tool.type) {
    case T::BallEndMill:
        if (milling && ap < d / 2) {
            c.deff = 2 * dmath::sqrt(ap * (d - ap));
            c.lead = d / c.deff;
        }
        break;
    case T::BullEndMill: {
        const double r = std::clamp(tool.cornerRadius, 0.0, d / 2);
        if (milling && r > 0 && ap < r) {
            const double s = dmath::sqrt(ap * (2 * r - ap));
            c.deff = d - 2 * r + 2 * s;
            c.lead = r / s;
        }
        break;
    }
    case T::ChamferMill:
    case T::EngraveCutter:
    case T::SpotDrill:
        if (ap <= 0)
            break;
        if (tool.tipAngleDeg > 0 && tool.tipAngleDeg < 180) {
            const double half = tool.tipAngleDeg / 2 * kPi / 180;
            c.deff = std::min(d, std::max(0.0, tool.tipDiameter) + 2 * ap * dmath::tan(half));
            if (milling)
                c.lead = 1 / dmath::cos(half);
        } else {
            notes.emplace_back("The tool has no tip angle: its full diameter is used.");
        }
        break;
    default:
        break;
    }
    const double floor = d / kMaxChipThinningFactor;
    if (c.deff < floor) {
        c.deff = floor;
        notes.emplace_back(
            "The diameter in contact at this depth is very small: a fifth of the tool diameter is used.");
    }
    return c;
}

// Rounds an rpm to a whole number: down, but not below the minimum or above the maximum (when known).
double roundRpm(double n, double minRpm, double maxRpm)
{
    double whole = std::floor(n + kRpmSlack);
    if (known(minRpm) && whole < minRpm)
        whole = std::ceil(minRpm - kRpmSlack);
    if (known(maxRpm) && whole > maxRpm)
        whole = std::floor(maxRpm + kRpmSlack);
    return whole;
}

void setLimit(FeedsLimit& limit, double machine, double requested, bool over)
{
    limit.machine = known(machine) ? machine : 0;
    limit.requested = requested;
    limit.state = !known(machine) ? LimitState::Unknown : (over ? LimitState::Applied : LimitState::NotReached);
}

void noteUnknownLimits(const FeedsResult& r, std::vector<std::string>& notes)
{
    if (r.limits.maxRpm.state == LimitState::Unknown)
        notes.emplace_back("The machine's maximum spindle speed is not known: rpm is not checked against it.");
    if (r.limits.minRpm.state == LimitState::Unknown)
        notes.emplace_back("The machine's minimum spindle speed is not known: rpm is not checked against it.");
    if (r.limits.maxFeed.state == LimitState::Unknown)
        notes.emplace_back("The machine's maximum cutting feed is not known: feed is not checked against it.");
    if (r.powerModel != PowerModel::Unknown) {
        if (r.limits.power.state == LimitState::Unknown)
            notes.emplace_back("The machine's spindle power is not known: power is not checked against it.");
        if (r.limits.torque.state == LimitState::Unknown)
            notes.emplace_back("The machine's spindle torque is not known: torque is not checked against it.");
    }
}

// Common checks of the input. Returns the error message, empty if the input is usable.
std::string checkInput(const FeedsInput& in, FeedsMode mode)
{
    const auto& tool = in.tool;
    if (!finite(tool.diameter) || tool.diameter <= 0)
        return "The tool's diameter must be greater than 0.";
    if (mode != FeedsMode::Tapping && tool.flutes <= 0)
        return "The tool must have at least one flute.";
    if (mode == FeedsMode::Tapping && (!finite(tool.threadPitch) || tool.threadPitch <= 0))
        return "The tap's thread pitch must be greater than 0.";
    if (!finite(in.radialEngagementMm) || in.radialEngagementMm < 0)
        return "The width of cut must be a number of at least 0.";
    if (!finite(in.axialDepthMm) || in.axialDepthMm < 0)
        return "The depth of cut must be a number of at least 0.";
    if (known(in.machine.minRpm) && known(in.machine.maxRpm) && in.machine.minRpm > in.machine.maxRpm)
        return "The machine's minimum spindle speed is above its maximum.";
    if (in.surfaceSpeedMMin && (!finite(*in.surfaceSpeedMMin) || *in.surfaceSpeedMMin <= 0))
        return "The surface speed must be greater than 0.";
    if (in.chipLoadMm && mode != FeedsMode::Tapping && (!finite(*in.chipLoadMm) || *in.chipLoadMm <= 0))
        return "The chip load must be greater than 0.";
    return {};
}

bool allFinite(const FeedsResult& r)
{
    for (double v : {r.rpm,
                     r.feedMmMin,
                     r.chipLoadMm,
                     r.effectiveChipLoadMm,
                     r.mrrCm3Min,
                     r.powerKw,
                     r.torqueNm,
                     r.surfaceSpeedMMin,
                     r.targetChipLoadMm,
                     r.effectiveDiameterMm,
                     r.radialEngagementMm,
                     r.axialDepthMm,
                     r.radialThinningFactor,
                     r.leadThinningFactor,
                     r.chipThinningFactor,
                     r.actualSurfaceSpeedMMin,
                     r.feedPerRevMm,
                     r.cuttingPowerKw,
                     r.limits.maxRpm.requested,
                     r.limits.minRpm.requested,
                     r.limits.maxFeed.requested,
                     r.limits.power.requested,
                     r.limits.torque.requested})
        if (!finite(v))
            return false;
    return true;
}

// ---------- Milling and drilling ----------

Result<FeedsResult> millOrDrill(const FeedsInput& in, FeedsResult r)
{
    const auto& tool = in.tool;
    const auto& machine = in.machine;
    const double d = tool.diameter;
    const double z = tool.flutes;
    auto& notes = r.notes;

    // Engagement.
    double ae = r.radialEngagementMm;
    double ap = r.axialDepthMm;
    bool solid = true; // drilling: no hole before this cut
    if (r.mode == FeedsMode::Milling) {
        if (ae > d) {
            ae = d;
            notes.emplace_back("The width of cut is wider than the tool: computed as a full-width slot.");
        }
        if (ae <= 0)
            return tl::unexpected(invalid("The width of cut must be greater than 0."));
        if (ap <= 0)
            return tl::unexpected(invalid("The depth of cut must be greater than 0."));
    } else {
        solid = ae <= 0 || ae >= d / 2;
        if (solid && ae <= 0 && (tool.type == lib::ToolType::Reamer || tool.type == lib::ToolType::BoringBar))
            notes.emplace_back("The hole before this cut is not given: removal rate and power are for solid drilling "
                               "(an upper bound).");
        ae = solid ? d / 2 : ae;
    }
    r.radialEngagementMm = ae;
    r.axialDepthMm = ap;
    if (r.mode == FeedsMode::Milling && tool.fluteLength > 0 && ap > tool.fluteLength)
        notes.emplace_back("The depth of cut is deeper than the tool's flute length.");

    // Diameter in contact and chip thinning (milling only).
    const Contact contact = contactOf(tool, r.mode, ap, notes);
    const double deff = contact.deff;
    r.effectiveDiameterMm = deff;
    if (r.mode == FeedsMode::Milling) {
        if (tool.type == lib::ToolType::FaceMill) {
            r.radialThinningFactor = 1;
            notes.emplace_back("Face mill: a cut centred on the cutter is assumed, so the chip is as thick as the chip "
                               "load (no radial thinning), and the tool has no lead angle (no lead-angle thinning).");
        } else if (ae < deff / 2) {
            r.radialThinningFactor = deff / (2 * dmath::sqrt(ae * (deff - ae)));
        }
        r.leadThinningFactor = contact.lead;
        const double total = r.radialThinningFactor * r.leadThinningFactor;
        r.thinningCapped = total > kMaxChipThinningFactor;
        r.chipThinningFactor = r.thinningCapped ? kMaxChipThinningFactor : total;
        if (r.thinningCapped)
            notes.emplace_back("The chip thinning compensation is capped: at this engagement the edge rubs more than "
                               "it cuts; consider a wider or deeper cut.");
    }
    const double fzTarget = r.targetChipLoadMm * r.chipThinningFactor;

    // 1-4. Spindle speed.
    const double nRequested = 1000 * r.surfaceSpeedMMin / (kPi * deff);
    double n = nRequested;
    setLimit(r.limits.maxRpm, machine.maxRpm, nRequested, known(machine.maxRpm) && n > machine.maxRpm);
    if (r.limits.maxRpm.state == LimitState::Applied)
        n = machine.maxRpm;
    setLimit(r.limits.minRpm, machine.minRpm, nRequested, known(machine.minRpm) && n < machine.minRpm);
    if (r.limits.minRpm.state == LimitState::Applied)
        n = machine.minRpm;
    n = roundRpm(n, machine.minRpm, machine.maxRpm);
    if (n < 1)
        return tl::unexpected(invalid("These cutting data give a spindle speed below 1 rpm."));

    // 5-6. Feed.
    const double feedLimit = axisFeedLimit(machine, r.mode);
    double vf = n * fzTarget * z;
    setLimit(r.limits.maxFeed, feedLimit, vf, known(feedLimit) && vf > feedLimit);
    if (r.limits.maxFeed.state == LimitState::Applied)
        vf = feedLimit;

    // 7. Power and torque at a feed, at this rpm (KC MODEL: kc1.1 flat, or kc1.1 hm^-mc).
    const double kc11 = in.material.specificCuttingForceNmm2;
    const double section = r.mode == FeedsMode::Milling
                               ? ae * ap
                               : (solid ? kPi / 4 * deff * deff : kPi / 4 * (d * d - (d - 2 * ae) * (d - 2 * ae)));
    const double mrrPerFeed = section / 1000; // cm3/min per mm/min
    const bool kcKnown = known(kc11);
    const int mcK = kcKnown ? mcSteps(in.material.mc) : 0;
    const bool chipModel = mcK > 0;
    const bool milling = r.mode == FeedsMode::Milling;
    r.powerModel = !kcKnown  ? PowerModel::Unknown
                   : milling ? (chipModel ? PowerModel::MillingKcMc : PowerModel::MillingKc11)
                             : (chipModel ? PowerModel::DrillingKcMc : PowerModel::DrillingKc11);
    // Mean chip thickness per mm of programmed chip load.
    double hmPerFz = 1;
    if (milling && tool.type == lib::ToolType::FaceMill) {
        const double s = std::min(1.0, ae / deff);
        hmPerFz = s / dmath::asin(s);
    } else if (milling) {
        const double e = std::min(ae, deff);
        const double phi = dmath::acos(1 - 2 * e / deff);
        hmPerFz = (2 * e / deff) / phi / r.leadThinningFactor;
    } else if (tool.tipAngleDeg > 0 && tool.tipAngleDeg < 180) {
        hmPerFz = dmath::sin(tool.tipAngleDeg / 2 * kPi / 180);
    }
    auto kcAt = [&](double feed) {
        if (!chipModel) {
            return kc11;
        }
        const double h = hmPerFz * feed / (n * z);
        return h > 0 ? kc11 / powSteps(h, mcK) : kc11;
    };
    auto cuttingPower = [&](double feed) {                                             // kW
        return kcKnown ? mrrPerFeed * kcAt(feed) * kToolWearFactor / 60000 * feed : 0; // as kc1.1 flat always was
    };
    auto motorPower = [&](double feed) {
        return cuttingPower(feed) / kSpindleEfficiency;
    };
    auto torque = [&](double feed) {
        return cuttingPower(feed) * 60000 / (2 * kPi * n);
    };
    // The factor that brings the power and torque at `feed` to the known limits (1 when neither is over).
    auto limitScale = [&](double feed) {
        double scale = 1;
        if (known(machine.spindlePowerKw) && motorPower(feed) > machine.spindlePowerKw)
            scale = std::min(scale, machine.spindlePowerKw / motorPower(feed));
        if (known(machine.spindleTorqueNm) && torque(feed) > machine.spindleTorqueNm)
            scale = std::min(scale, machine.spindleTorqueNm / torque(feed));
        return scale;
    };
    if (kcKnown) {
        const double p = motorPower(vf);
        const double t = torque(vf);
        setLimit(r.limits.power, machine.spindlePowerKw, p,
                 known(machine.spindlePowerKw) && p > machine.spindlePowerKw);
        setLimit(r.limits.torque, machine.spindleTorqueNm, t,
                 known(machine.spindleTorqueNm) && t > machine.spindleTorqueNm);
        // Proportional to vf with kc1.1 flat: one step. With mc they grow as vf^(1 - mc): repeat (KC MODEL).
        for (int i = 0; i < (chipModel ? kMaxLimitRepeats : 1); ++i) {
            const double scale = limitScale(vf);
            if (scale >= 1)
                break;
            vf *= scale;
        }
    } else { // not checked: Unknown, with the machine's values for the panel
        r.limits.power.machine = known(machine.spindlePowerKw) ? machine.spindlePowerKw : 0;
        r.limits.torque.machine = known(machine.spindleTorqueNm) ? machine.spindleTorqueNm : 0;
    }

    // 8. Whole um/min, rounded down, and never over a known limit after rounding.
    const double feedUmExact = vf * 1000;
    if (!(feedUmExact < static_cast<double>(std::numeric_limits<std::int32_t>::max())))
        return tl::unexpected(invalid("These cutting data give a feed above the largest toolpath feed."));
    auto feedUm = static_cast<std::int64_t>(std::floor(feedUmExact + kFeedSlackUm));
    auto over = [&](std::int64_t um) {
        const double feed = static_cast<double>(um) / 1000;
        if (known(feedLimit) && feed > feedLimit)
            return true;
        if (kcKnown && known(machine.spindlePowerKw) && motorPower(feed) > machine.spindlePowerKw)
            return true;
        if (kcKnown && known(machine.spindleTorqueNm) && torque(feed) > machine.spindleTorqueNm)
            return true;
        return false;
    };
    while (feedUm > 0 && over(feedUm))
        --feedUm;
    if (feedUm < 1)
        return tl::unexpected(invalid("These cutting data give a feed below 1 um/min."));

    // 9. Everything from the rounded values.
    r.rpm = n;
    r.feedMicronPerMin = static_cast<std::int32_t>(feedUm);
    r.feedMmMin = static_cast<double>(feedUm) / 1000;
    r.feedPerRevMm = r.feedMmMin / n;
    r.chipLoadMm = r.feedMmMin / (n * z);
    r.effectiveChipLoadMm = r.chipLoadMm / r.chipThinningFactor;
    r.actualSurfaceSpeedMMin = kPi * deff * n / 1000;
    r.mrrCm3Min = mrrPerFeed * r.feedMmMin;
    r.cuttingPowerKw = cuttingPower(r.feedMmMin);
    r.powerKw = kcKnown ? motorPower(r.feedMmMin) : 0;
    r.torqueNm = kcKnown ? torque(r.feedMmMin) : 0;
    r.meanChipThicknessMm = chipModel ? hmPerFz * r.chipLoadMm : 0;
    r.specificCuttingForceNmm2 = kcKnown ? kcAt(r.feedMmMin) : 0;
    r.limitedByRpm = r.limits.maxRpm.state == LimitState::Applied;
    r.limitedByMinRpm = r.limits.minRpm.state == LimitState::Applied;
    r.limitedByFeed = r.limits.maxFeed.state == LimitState::Applied;
    r.limitedByPower = r.limits.power.state == LimitState::Applied;
    r.limitedByTorque = r.limits.torque.state == LimitState::Applied;

    if (r.limitedByMinRpm)
        notes.emplace_back("The spindle speed is raised to the machine's minimum: the surface speed "
                           "is above the cutting data's.");
    if (r.limitedByFeed || r.limitedByPower || r.limitedByTorque)
        notes.emplace_back("A machine limit lowered the feed: the chip load is below the cutting data's.");
    return r;
}

// ---------- Tapping ----------

Result<FeedsResult> tap(const FeedsInput& in, FeedsResult r)
{
    const auto& tool = in.tool;
    const auto& machine = in.machine;
    const double d = tool.diameter;
    auto& notes = r.notes;

    const std::int64_t pitchNm = toNm(tool.threadPitch);
    if (pitchNm <= 0)
        return tl::unexpected(invalid("The tap's thread pitch must be greater than 0."));
    const double pitch = static_cast<double>(pitchNm) / kNmPerMm;
    const std::int64_t exactStep = 1000 / std::gcd(pitchNm, std::int64_t{1000});
    const std::int64_t step = exactStep <= kMaxTapRpmStep ? exactStep : 1;
    const bool minKnown = known(machine.minRpm);
    // The lowest rpm on the step grid that the machine can run (or 1 step).
    const auto minStep = [&] {
        auto lo = minKnown ? static_cast<std::int64_t>(std::ceil(machine.minRpm - kRpmSlack)) : std::int64_t{1};
        return (std::max<std::int64_t>(lo, 1) + step - 1) / step * step;
    }();
    auto down = [&](double v) { // down to the step grid
        return static_cast<std::int64_t>(std::floor(v + kRpmSlack)) / step * step;
    };

    r.effectiveDiameterMm = d;
    r.radialEngagementMm = d / 2;
    r.axialDepthMm = in.axialDepthMm;
    r.targetChipLoadMm = 0;
    r.powerModel = known(in.material.specificCuttingForceNmm2) ? PowerModel::TappingKc11 : PowerModel::Unknown;

    // 1-4. Spindle speed.
    const double nRequested = 1000 * r.surfaceSpeedMMin / (kPi * d);
    double nd = nRequested;
    setLimit(r.limits.maxRpm, machine.maxRpm, nRequested, known(machine.maxRpm) && nd > machine.maxRpm);
    if (r.limits.maxRpm.state == LimitState::Applied)
        nd = machine.maxRpm;
    setLimit(r.limits.minRpm, machine.minRpm, nRequested, minKnown && nd < machine.minRpm);
    if (r.limits.minRpm.state == LimitState::Applied)
        nd = machine.minRpm;
    std::int64_t n = down(roundRpm(nd, machine.minRpm, machine.maxRpm));
    if (n < minStep && (minKnown || n < 1))
        n = minStep;
    if (known(machine.maxRpm) && static_cast<double>(n) > machine.maxRpm)
        n = down(machine.maxRpm);
    if (n < 1)
        return tl::unexpected(invalid("These cutting data give a spindle speed below 1 rpm."));

    // Lowers n to at most `limitN` on the grid, but not below the minimum; returns false if the minimum stopped it.
    auto lowerTo = [&](double limitN) {
        std::int64_t target = down(limitN);
        if (target >= n)
            return true;
        if (target < minStep) {
            n = std::min(n, minStep);
            return false;
        }
        n = target;
        return true;
    };

    // 5. Feed of the Z axis: P n.
    const double feedLimit = axisFeedLimit(machine, FeedsMode::Tapping);
    const double vfRequested = pitch * static_cast<double>(n);
    setLimit(r.limits.maxFeed, feedLimit, vfRequested, known(feedLimit) && vfRequested > feedLimit);
    if (r.limits.maxFeed.state == LimitState::Applied) {
        if (!lowerTo(feedLimit / pitch))
            r.limits.maxFeed.state = LimitState::Exceeded;
        while (n > minStep && pitch * static_cast<double>(n) > feedLimit)
            n -= step;
    }

    // 6-7. Power and torque. Torque per revolution: kc W V / (2 pi), V the groove volume cut per revolution.
    const double kc = in.material.specificCuttingForceNmm2;
    const double d1 = std::max(0.0, d - kIsoMinorDiameterPerPitch * pitch);
    const double volumePerRev = kTapGrooveFraction * kPi / 4 * (d * d - d1 * d1) * pitch; // mm3
    const bool kcKnown = r.powerModel != PowerModel::Unknown;
    const double torqueNm = kcKnown ? kc * kToolWearFactor * volumePerRev / (2 * kPi) / 1000 : 0;
    auto motorPower = [&](std::int64_t rpm) {
        return torqueNm * 2 * kPi * static_cast<double>(rpm) / 60000 / kSpindleEfficiency;
    };
    if (kcKnown) {
        const double p = motorPower(n);
        setLimit(r.limits.power, machine.spindlePowerKw, p,
                 known(machine.spindlePowerKw) && p > machine.spindlePowerKw);
        if (r.limits.power.state == LimitState::Applied) {
            if (!lowerTo(static_cast<double>(n) * machine.spindlePowerKw / p))
                r.limits.power.state = LimitState::Exceeded;
            while (n > minStep && motorPower(n) > machine.spindlePowerKw)
                n -= step;
        }
        setLimit(r.limits.torque, machine.spindleTorqueNm, torqueNm,
                 known(machine.spindleTorqueNm) && torqueNm > machine.spindleTorqueNm);
        if (r.limits.torque.state == LimitState::Applied) {
            r.limits.torque.state = LimitState::Exceeded;
            notes.emplace_back("The tapping torque is above the machine's spindle torque, and no spindle speed lowers "
                               "it: use a smaller tap, a thread mill, or a larger tap drill.");
        }
    } else { // not checked: Unknown, with the machine's values for the panel
        r.limits.power.machine = known(machine.spindlePowerKw) ? machine.spindlePowerKw : 0;
        r.limits.torque.machine = known(machine.spindleTorqueNm) ? machine.spindleTorqueNm : 0;
    }
    if (r.limits.maxFeed.state == LimitState::Exceeded || r.limits.power.state == LimitState::Exceeded)
        notes.emplace_back("The machine's minimum spindle speed keeps this tap above another machine limit.");

    // Feed: pitch x rpm in um/min, exact on the step grid, else nearest (ties to even).
    const std::int64_t numerator = pitchNm * n;
    std::int64_t feedUm = numerator / 1000;
    const std::int64_t rem = numerator % 1000;
    if (rem > 500 || (rem == 500 && (feedUm % 2) == 1))
        ++feedUm;
    r.tapFeedExact = rem == 0;
    if (!r.tapFeedExact)
        notes.emplace_back("This pitch is not a whole number of micrometres per revolution step: the tapping feed is "
                           "pitch x rpm rounded to 1 um/min.");
    if (feedUm < 1 || feedUm > std::numeric_limits<std::int32_t>::max())
        return tl::unexpected(invalid("These cutting data give a tapping feed outside the toolpath feed range."));

    r.rpm = static_cast<double>(n);
    r.feedMicronPerMin = static_cast<std::int32_t>(feedUm);
    r.feedMmMin = static_cast<double>(feedUm) / 1000;
    r.feedPerRevMm = pitch;
    r.chipLoadMm = 0;
    r.effectiveChipLoadMm = 0;
    r.actualSurfaceSpeedMMin = kPi * d * r.rpm / 1000;
    r.mrrCm3Min = volumePerRev * r.rpm / 1000;
    r.torqueNm = torqueNm;
    r.powerKw = kcKnown ? motorPower(n) : 0;
    r.specificCuttingForceNmm2 = kcKnown ? kc : 0;
    r.cuttingPowerKw = r.powerKw * kSpindleEfficiency;
    r.limitedByRpm = r.limits.maxRpm.state == LimitState::Applied;
    r.limitedByMinRpm = r.limits.minRpm.state == LimitState::Applied;
    r.limitedByFeed = r.limits.maxFeed.state == LimitState::Applied || r.limits.maxFeed.state == LimitState::Exceeded;
    r.limitedByPower = r.limits.power.state == LimitState::Applied || r.limits.power.state == LimitState::Exceeded;
    r.limitedByTorque = r.limits.torque.state == LimitState::Exceeded;
    if (r.limitedByMinRpm)
        notes.emplace_back("The spindle speed is raised to the machine's minimum: the surface speed "
                           "is above the cutting data's.");
    if (r.limitedByFeed || r.limitedByPower)
        notes.emplace_back("A machine limit lowered the tapping speed (the feed follows the pitch).");
    if (!machine.rigidTapping)
        notes.emplace_back("The machine is not marked as having rigid tapping: use a tapping head or a floating tap "
                           "holder.");
    return r;
}

} // namespace

Result<FeedsResult> computeFeeds(const FeedsInput& in)
{
    FeedsResult r;
    r.mode = modeOf(in.tool.type, in.opType);
    if (auto problem = checkInput(in, r.mode); !problem.empty())
        return tl::unexpected(invalid(std::move(problem)));
    const double d = in.tool.diameter;

    // The preset: named, or the default class for the tool. Not needed when the overrides replace what it gives.
    const bool needsPreset = !in.surfaceSpeedMMin || (r.mode != FeedsMode::Tapping && !in.chipLoadMm);
    const std::string_view opClass =
        !in.opClass.empty() ? in.opClass : defaultOpClass(in.tool.type, in.opType, in.radialEngagementMm, d);
    const lib::ToolPreset* preset = findPreset(in.tool, in.material.id, opClass);
    if (!preset && needsPreset) {
        std::string classes = presetClassesFor(in.tool, in.material.id);
        return tl::unexpected(Error{
            ErrorCode::NotFound,
            "The tool \"" + in.tool.name + "\" has no \"" + std::string(opClass) + "\" cutting data for " +
                in.material.name +
                (classes.empty() ? std::string(" (it has none for that material).") : " (it has: " + classes + ")."),
            {"computeFeeds"}});
    }
    if (preset)
        r.opClass = preset->opClass;

    r.surfaceSpeedMMin = in.surfaceSpeedMMin ? *in.surfaceSpeedMMin : (preset ? preset->surfaceSpeedMMin : 0);
    if (r.mode != FeedsMode::Tapping)
        r.targetChipLoadMm = in.chipLoadMm ? *in.chipLoadMm : (preset ? preset->chipLoadMm : 0);
    else if (in.chipLoadMm)
        r.notes.emplace_back("A tap's feed is its pitch: the chip load given is not used.");
    if (!finite(r.surfaceSpeedMMin) || r.surfaceSpeedMMin <= 0)
        return tl::unexpected(invalid("The cutting data have no surface speed."));
    if (r.mode != FeedsMode::Tapping && (!finite(r.targetChipLoadMm) || r.targetChipLoadMm <= 0))
        return tl::unexpected(invalid("The cutting data have no chip load."));
    if (r.mode == FeedsMode::Drilling && in.opClass.empty() &&
        (in.tool.type == lib::ToolType::FlatEndMill || in.tool.type == lib::ToolType::BallEndMill ||
         in.tool.type == lib::ToolType::BullEndMill))
        r.notes.emplace_back("An end mill plunging: its slot cutting data are used as drilling data.");

    // Engagement defaults (milling): 0 = the preset's largest engagement.
    r.radialEngagementMm = in.radialEngagementMm;
    r.axialDepthMm = in.axialDepthMm;
    if (r.mode == FeedsMode::Milling) {
        if (r.radialEngagementMm == 0 && preset) {
            r.radialEngagementMm = std::min(1.0, preset->maxRadialFraction) * d;
            r.notes.emplace_back("No width of cut given: the cutting data's largest radial engagement is used.");
        }
        if (r.axialDepthMm == 0 && preset) {
            r.axialDepthMm = preset->maxAxialFraction * d;
            r.notes.emplace_back("No depth of cut given: the cutting data's largest depth of cut is used.");
        }
        if (preset && preset->maxRadialFraction > 0 &&
            r.radialEngagementMm > preset->maxRadialFraction * d * (1 + 1e-9))
            r.notes.emplace_back("The width of cut is above the cutting data's largest radial engagement.");
        if (preset && preset->maxAxialFraction > 0 && r.axialDepthMm > preset->maxAxialFraction * d * (1 + 1e-9))
            r.notes.emplace_back("The depth of cut is above the cutting data's largest depth of cut.");
    }

    auto result = r.mode == FeedsMode::Tapping ? tap(in, std::move(r)) : millOrDrill(in, std::move(r));
    if (!result)
        return result;
    if (result->powerModel == PowerModel::Unknown) {
        result->notes.emplace_back("The material's specific cutting force is not known: power and torque are not "
                                   "estimated.");
    } else if (result->powerModel == PowerModel::TappingKc11) {
        result->notes.emplace_back("Tapping power and torque use kc1.1 without a chip-thickness correction (a tap's "
                                   "chip thickness depends on its chamfer, which the tool does not record).");
    } else if (result->powerModel == PowerModel::MillingKc11 || result->powerModel == PowerModel::DrillingKc11) {
        result->notes.emplace_back("Power and torque use kc1.1 without a chip-thickness correction (the material has "
                                   "no mc), so they read low for thin chips.");
    }
    noteUnknownLimits(*result, result->notes);
    if (!allFinite(*result))
        return tl::unexpected(Error{ErrorCode::Internal, "Feeds: a value is not finite.", {"computeFeeds"}});
    return result;
}

} // namespace ray::cam
