// The drilling operation's plan (drill.h, task T-035): parameters, checks, the holes, their depths and levels. Every
// rule is stated in drill.h; this file follows them in its order. Deterministic: integer nanometres after the point
// length, dmath for the one trigonometric call, holes from recognizeHoles (sorted) and orderHolesShortestPath.
#include "drill.h"
#include "op_params.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <utility>

// Returns the error of a Status or Result expression.
#define RAY_TRY_STATUS(expr)                                                                                           \
    do {                                                                                                               \
        if (auto ray_drill_status_ = (expr); !ray_drill_status_) {                                                     \
            return tl::unexpected(std::move(ray_drill_status_.error()));                                               \
        }                                                                                                              \
    } while (false)

namespace ray::cam::ops {

namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}
tl::unexpected<Error> invalid(std::string message)
{
    return failure(ErrorCode::InvalidArgument, std::move(message));
}

// ---------- Words ----------

// A length in mm for messages: rounded to 1 um (half to even), trailing zeros dropped ("12.5", "0.002", "-3").
std::string mmText(Nm nm)
{
    std::int64_t um = nm / 1000;
    std::int64_t rest = nm % 1000;
    if (rest < 0) {
        rest += 1000;
        --um;
    }
    if (rest > 500 || (rest == 500 && (um % 2) != 0)) {
        ++um;
    }
    const bool negative = um < 0;
    const std::uint64_t a = negative ? static_cast<std::uint64_t>(-(um + 1)) + 1 : static_cast<std::uint64_t>(um);
    std::string out = (negative ? "-" : "") + std::to_string(a / 1000);
    std::uint64_t frac = a % 1000;
    if (frac != 0) {
        std::string digits = std::to_string(frac);
        digits.insert(0, 3 - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0') {
            digits.pop_back();
        }
        out += "." + digits;
    }
    return out;
}
std::string mmText(double mm)
{
    return std::isfinite(mm) && std::abs(mm) < 1e12 ? mmText(toNm(mm)) : std::string("?");
}

std::string where(const RecognizedHole& h)
{
    return "the hole at X" + mmText(h.x) + " Y" + mmText(h.y);
}

const char* cycleWord(DrillCycle c)
{
    switch (c) {
    case DrillCycle::Spot:
        return "spot drilling";
    case DrillCycle::Drill:
        return "drilling";
    case DrillCycle::Peck:
        return "peck drilling";
    case DrillCycle::ChipBreak:
        return "chip-break drilling";
    case DrillCycle::Tap:
        return "tapping";
    case DrillCycle::Bore:
        return "boring";
    case DrillCycle::Ream:
        return "reaming";
    }
    return "drilling";
}

const char* toolWord(lib::ToolType t)
{
    switch (t) {
    case lib::ToolType::FlatEndMill:
        return "flat end mill";
    case lib::ToolType::BallEndMill:
        return "ball end mill";
    case lib::ToolType::BullEndMill:
        return "bull nose end mill";
    case lib::ToolType::ChamferMill:
        return "chamfer mill";
    case lib::ToolType::FaceMill:
        return "face mill";
    case lib::ToolType::SpotDrill:
        return "spot drill";
    case lib::ToolType::Drill:
        return "drill";
    case lib::ToolType::Tap:
        return "tap";
    case lib::ToolType::Reamer:
        return "reamer";
    case lib::ToolType::BoringBar:
        return "boring bar";
    case lib::ToolType::ThreadMill:
        return "thread mill";
    case lib::ToolType::EngraveCutter:
        return "engraving cutter";
    }
    return "tool";
}

// The tool types made for each cycle (allowOtherTools lets any but a tap do the others; only a tap taps).
bool madeFor(DrillCycle c, lib::ToolType t)
{
    using T = lib::ToolType;
    switch (c) {
    case DrillCycle::Spot:
        return t == T::SpotDrill || t == T::Drill || t == T::ChamferMill;
    case DrillCycle::Drill:
    case DrillCycle::Peck:
    case DrillCycle::ChipBreak:
        return t == T::Drill;
    case DrillCycle::Tap:
        return t == T::Tap;
    case DrillCycle::Bore:
        return t == T::BoringBar;
    case DrillCycle::Ream:
        return t == T::Reamer;
    }
    return false;
}
const char* madeForWords(DrillCycle c)
{
    switch (c) {
    case DrillCycle::Spot:
        return "a spot drill, a drill or a chamfer mill";
    case DrillCycle::Drill:
    case DrillCycle::Peck:
    case DrillCycle::ChipBreak:
        return "a drill";
    case DrillCycle::Tap:
        return "a tap";
    case DrillCycle::Bore:
        return "a boring bar";
    case DrillCycle::Ream:
        return "a reamer";
    }
    return "a drill";
}

// ---------- Small geometry ----------

Vec3 apply(const Xform& f, Vec3 p)
{
    const double* m = f.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

// The highest WCS Z of a model-frame box moved into the WCS (its eight corners).
double topInWcs(const Xform& f, const Box3& b)
{
    double top = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 8; ++i) {
        const Vec3 c{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
        top = std::max(top, apply(f, c).z);
    }
    return top;
}

// Round half to even to an integer, for feeds and dwells (the default rounding mode of IEEE arithmetic).
std::int64_t roundHalfEven(double v)
{
    return static_cast<std::int64_t>(std::nearbyint(v));
}

// ---------- Parameters ----------

struct Params {
    DrillCycle cycle = DrillCycle::Drill;
    bool depthFromValue = false;
    double depth = 0;
    bool depthToTip = false;
    double breakthrough = 0;
    bool allowBelowFloor = false;
    double spotDiameter = 0;
    double peck = 0, peckClearance = 0;
    double tapLeadPitches = 0;
    bool leftHand = false;
    double dwell = 0;
    bool selectionOrder = false;
    bool allowOtherTools = false;
};

const OpSchema* drillSchema()
{
    // Parsed once; immutable afterwards, so workers share it without a lock.
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("drill"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = drillSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The drilling operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    static const std::map<std::string, DrillCycle, std::less<>> cycles = {
        {"spot", DrillCycle::Spot}, {"drill", DrillCycle::Drill},
        {"peck", DrillCycle::Peck}, {"chipBreak", DrillCycle::ChipBreak},
        {"tap", DrillCycle::Tap},   {"bore", DrillCycle::Bore},
        {"ream", DrillCycle::Ream}};
    out.cycle = cycles.at(p->text("cycle")); // the schema's enum lists exactly these
    out.depthFromValue = p->text("depthFrom") == "value";
    out.depth = p->number("depth");
    out.depthToTip = p->text("depthReference") == "tip";
    out.breakthrough = p->number("breakthrough");
    out.allowBelowFloor = p->flag("allowBelowFloor");
    out.spotDiameter = p->number("spotDiameter");
    out.peck = p->number("peck");
    out.peckClearance = p->number("peckClearance");
    out.tapLeadPitches = p->number("tapLeadPitches");
    out.leftHand = p->flag("leftHand");
    out.dwell = p->number("dwell");
    out.selectionOrder = p->text("holeOrder") == "selection";
    out.allowOtherTools = p->flag("allowOtherTools");
    return out;
}

// ---------- The plan ----------

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx) {}

    Result<DrillPlan> run()
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        plan_.cycle = p_.cycle;
        RAY_TRY_STATUS(checkTool());
        RAY_TRY_STATUS(checkCutting());
        RAY_TRY_STATUS(checkHeights());
        RAY_TRY_STATUS(findHoles());
        RAY_TRY_STATUS(depths());
        RAY_TRY_STATUS(levels());
        RAY_TRY_STATUS(reach());
        if (p_.cycle == DrillCycle::Tap && !in_.machine.rigidTapping) {
            plan_.warnings.push_back("The machine is not set up for rigid tapping: use a floating (tension and "
                                     "compression) tap holder; the post decides how the tapping cycle is written");
        }
        return std::move(plan_);
    }

private:
    const lib::Tool& tool() const { return in_.tool; }

    Status checkTool()
    {
        const lib::Tool& t = tool();
        if (!(t.diameter > 0) || !std::isfinite(t.diameter)) {
            return invalid("The tool has no diameter");
        }
        const bool tap = p_.cycle == DrillCycle::Tap;
        if (tap && t.type != lib::ToolType::Tap) {
            return invalid(std::string("Tapping needs a tap; the tool is a ") + toolWord(t.type));
        }
        if (!tap && t.type == lib::ToolType::Tap) {
            return invalid(std::string("A tap can only tap: choose the tapping cycle or another tool for ") +
                           cycleWord(p_.cycle));
        }
        if (!madeFor(p_.cycle, t.type) && !p_.allowOtherTools) {
            return invalid(std::string("The tool is a ") + toolWord(t.type) + "; " + cycleWord(p_.cycle) + " needs " +
                           madeForWords(p_.cycle) + " (or turn on 'Allow other tools')");
        }
        if (tap && !(t.threadPitch > 0)) {
            return invalid("The tap has no thread pitch: set it in the tool library");
        }
        if (p_.cycle == DrillCycle::Spot && pointLengthNm(t.diameter, t.tipAngleDeg, t.tipDiameter) == 0 &&
            !p_.depthFromValue) {
            return invalid("Spot drilling to a diameter needs a tool with a point angle; this " +
                           std::string(toolWord(t.type)) + " has none (or give the depth as a value)");
        }
        return {};
    }

    Status checkCutting()
    {
        const CuttingData& c = in_.op.cutting;
        const double rpm = c.spindleRpm;
        if (!(rpm > 0) || !std::isfinite(rpm)) {
            return invalid("Set a spindle speed");
        }
        if (p_.cycle != DrillCycle::Tap && !c.clockwise) {
            return invalid("The spindle is set to turn counter-clockwise (M4): a right-hand " +
                           std::string(toolWord(tool().type)) + " does not cut that way; set it clockwise (M3)");
        }
        if (in_.machine.maxRpm > 0 && rpm > in_.machine.maxRpm) {
            return invalid("The spindle speed " + mmText(rpm) + " rpm is above the machine's maximum of " +
                           mmText(in_.machine.maxRpm) + " rpm");
        }
        if (in_.machine.minRpm > 0 && rpm < in_.machine.minRpm) {
            return invalid("The spindle speed " + mmText(rpm) + " rpm is below the machine's minimum of " +
                           mmText(in_.machine.minRpm) + " rpm");
        }
        constexpr double kMaxFeed = static_cast<double>(std::numeric_limits<std::int32_t>::max());
        if (p_.cycle == DrillCycle::Tap) {
            if (std::floor(rpm) != rpm || rpm > 1e7) {
                return invalid("Tapping needs a whole-number spindle speed (the feed is the pitch times the speed); "
                               "the speed is " +
                               mmText(rpm) + " rpm");
            }
            plan_.pitchNm = static_cast<std::int32_t>(toNm(tool().threadPitch));
            const std::int64_t feed = tapFeedMicronPerMin(plan_.pitchNm, static_cast<std::int64_t>(rpm));
            if (feed < 1 || static_cast<double>(feed) > kMaxFeed) {
                return invalid("The tapping feed (pitch times spindle speed) is out of range");
            }
            plan_.feed = static_cast<std::int32_t>(feed);
        } else {
            const double feed = c.feedCut * 1000.0;
            if (!(c.feedCut > 0) || !std::isfinite(feed) || feed > kMaxFeed || roundHalfEven(feed) < 1) {
                return invalid("Set a cutting feed: it is the feed into the hole");
            }
            plan_.feed = static_cast<std::int32_t>(roundHalfEven(feed));
        }
        plan_.dwellMs = static_cast<std::int32_t>(roundHalfEven(p_.dwell * 1000.0));
        const bool pecks = p_.cycle == DrillCycle::Peck || p_.cycle == DrillCycle::ChipBreak;
        plan_.peck = pecks ? toNm(p_.peck) : 0;
        plan_.peckClearance = pecks && plan_.peck > 0 ? toNm(p_.peckClearance) : 0;
        switch (p_.cycle) {
        case DrillCycle::Spot:
        case DrillCycle::Drill:
            plan_.kind = MoveKind::CycleDrill;
            break;
        case DrillCycle::Peck:
            plan_.kind = plan_.peck > 0 ? MoveKind::CyclePeck : MoveKind::CycleDrill;
            break;
        case DrillCycle::ChipBreak:
            plan_.kind = plan_.peck > 0 ? MoveKind::CycleChipBreak : MoveKind::CycleDrill;
            break;
        case DrillCycle::Tap:
            plan_.kind = MoveKind::CycleTap;
            break;
        case DrillCycle::Bore:
            plan_.kind = MoveKind::CycleBore;
            break;
        case DrillCycle::Ream:
            plan_.kind = MoveKind::CycleReam;
            break;
        }
        return {};
    }

    Status checkHeights()
    {
        const Heights& h = in_.op.heights;
        for (const auto& [v, word] :
             {std::pair{h.clearance, "clearance"}, std::pair{h.retract, "retract"}, std::pair{h.top, "top"}}) {
            if (!std::isfinite(v) || std::abs(v) > 1e6) {
                return invalid(std::string("The ") + word + " height is not a usable number");
            }
        }
        if (h.retract < h.top) {
            return invalid("The retract height (R plane, " + mmText(h.retract) +
                           " above the hole top) must not be below the top height (" + mmText(h.top) + ")");
        }
        if (h.clearance < h.retract) {
            return invalid("The clearance height (" + mmText(h.clearance) + ") must not be below the retract height (" +
                           mmText(h.retract) + ")");
        }
        return {};
    }

    // The recognised holes of part `part`, recognised once.
    Result<const std::vector<RecognizedHole>*> holesOf(std::size_t part)
    {
        if (const auto it = recognised_.find(part); it != recognised_.end()) {
            return &it->second;
        }
        const HoleOptions options = holeOptionsFor(in_.geom, part);
        auto found = recognizeHoles(in_.geom.partShapes[part], in_.geom.wcsFromModel, ctx_, options);
        if (!found) {
            return tl::unexpected(found.error());
        }
        return &recognised_.emplace(part, std::move(*found)).first->second;
    }

    std::string selectedWords(std::size_t i) const
    {
        const kernel::SubId sub = in_.geom.selected[i];
        const std::size_t part = static_cast<std::size_t>(in_.geom.selectedShapeIndex[i]);
        std::string text = "Selection " + std::to_string(i + 1);
        if (sub.kind == kernel::SubKind::Face) {
            auto s = kernel::surfaceInfo(in_.geom.partShapes[part], sub);
            if (s) {
                const Vec3 c = apply(in_.geom.wcsFromModel, s->centroid);
                text += " (the face centred at X" + mmText(c.x) + " Y" + mmText(c.y) + " Z" + mmText(c.z) + ")";
            }
        }
        return text;
    }

    Status findHoles()
    {
        const ResolvedGeometry& g = in_.geom;
        if (g.selected.empty()) {
            return invalid("Pick the holes to machine: select a face of each hole");
        }
        if (g.selectedShapeIndex.size() != g.selected.size()) {
            return failure(ErrorCode::Internal, "The selection and its part indices differ in length");
        }
        std::set<std::pair<std::size_t, std::size_t>> taken; // (part, hole index)
        for (std::size_t i = 0; i < g.selected.size(); ++i) {
            RAY_TRY_STATUS(ctx_.cancel.check());
            const kernel::SubId sub = g.selected[i];
            const int partIndex = g.selectedShapeIndex[i];
            if (partIndex < 0 || static_cast<std::size_t>(partIndex) >= g.partShapes.size()) {
                return failure(ErrorCode::Internal, "A selection lies on no part body");
            }
            const std::size_t part = static_cast<std::size_t>(partIndex);
            if (sub.kind != kernel::SubKind::Face) {
                return invalid(selectedWords(i) + " is not a face: pick a face of each hole");
            }
            auto holes = holesOf(part);
            if (!holes) {
                return tl::unexpected(holes.error());
            }
            std::optional<std::size_t> found;
            for (std::size_t k = 0; k < (*holes)->size() && !found; ++k) {
                const auto& faces = (**holes)[k].faces;
                if (std::binary_search(faces.begin(), faces.end(), sub)) {
                    found = k;
                }
            }
            if (!found) {
                return failure(ErrorCode::ToolpathFailed,
                               selectedWords(i) +
                                   " is not part of a hole that can be machined from this setup: a hole must run "
                                   "along the setup's Z and be open from above");
            }
            if (taken.insert({part, *found}).second) {
                PlannedHole ph;
                ph.part = part;
                ph.hole = (**holes)[*found];
                plan_.holes.push_back(std::move(ph));
            }
        }
        if (!p_.selectionOrder && plan_.holes.size() > 1) {
            std::vector<Vec2> xy;
            xy.reserve(plan_.holes.size());
            for (const PlannedHole& h : plan_.holes) {
                xy.push_back(Vec2{toMm(h.hole.x), toMm(h.hole.y)});
            }
            const std::vector<std::size_t> order = orderHolesShortestPath(xy, Vec2{0, 0});
            std::vector<PlannedHole> ordered;
            ordered.reserve(order.size());
            for (const std::size_t k : order) {
                ordered.push_back(std::move(plan_.holes[k]));
            }
            plan_.holes = std::move(ordered);
        }
        return {};
    }

    // ---------- Depth (drill.h DEPTH) ----------

    Status depths()
    {
        for (PlannedHole& ph : plan_.holes) {
            RAY_TRY_STATUS(depthOf(ph));
        }
        return {};
    }

    Status depthOf(PlannedHole& ph)
    {
        const RecognizedHole& h = ph.hole;
        const lib::Tool& t = tool();
        const HoleSegment& bore = h.segments[h.bore];
        const Nm point = pointLengthNm(t.diameter, t.tipAngleDeg, t.tipDiameter);
        const Nm breakthrough = toNm(p_.breakthrough);
        const std::string at = where(h);
        switch (p_.cycle) {
        case DrillCycle::Spot: {
            if (p_.depthFromValue) {
                ph.tip = h.zTop - toNm(p_.depth) - (p_.depthToTip ? 0 : point);
                break;
            }
            const HoleSegment& entry = h.segments.front();
            const double spot = p_.spotDiameter > 0                                             ? p_.spotDiameter
                                : entry.conical && (h.chamfered || h.countersunk) && h.bore > 0 ? entry.diameter
                                                                                                : bore.diameter;
            if (spot > t.diameter + kDiameterTolMm) {
                return invalid("The " + mmText(t.diameter) + " mm " + toolWord(t.type) + " cannot make a " +
                               mmText(spot) + " mm spot at " + at);
            }
            ph.tip = h.zTop - pointLengthNm(spot, t.tipAngleDeg, t.tipDiameter);
            break;
        }
        case DrillCycle::Tap:
            RAY_TRY_STATUS(tapDepth(ph, breakthrough));
            break;
        default:
            if (t.diameter > bore.diameter + kDiameterTolMm) {
                return invalid("The " + mmText(t.diameter) + " mm " + toolWord(t.type) + " is larger than the " +
                               mmText(bore.diameter) + " mm hole at X" + mmText(h.x) + " Y" + mmText(h.y));
            }
            if (p_.depthFromValue) {
                ph.tip = h.zTop - toNm(p_.depth) - (p_.depthToTip ? 0 : point);
                break;
            }
            switch (h.bottom) {
            case HoleBottom::Through:
                ph.tip = h.zBottom - breakthrough - point;
                break;
            case HoleBottom::Conical: {
                const HoleSegment& cone = h.segments.back();
                if (point > 0 && std::abs(t.tipAngleDeg - cone.coneAngleDeg) <= kSameAngleDeg) {
                    ph.tip = h.zBottom; // the modelled point
                } else {
                    ph.tip = bore.zBottom - point;
                    if (ph.tip < h.zBottom) {
                        plan_.warnings.push_back("At " + at + " the " + mmText(t.tipAngleDeg) + " degree point goes " +
                                                 mmText(h.zBottom - ph.tip) + " mm below the hole's modelled " +
                                                 mmText(cone.coneAngleDeg) +
                                                 " degree point, to reach its full diameter depth");
                    }
                }
                break;
            }
            case HoleBottom::Flat:
                if (p_.allowBelowFloor) {
                    ph.tip = h.zBottom - point;
                } else {
                    ph.tip = h.zBottom;
                    if (point > 0) {
                        plan_.warnings.push_back("At " + at + " the tip stops at the flat floor, which keeps a " +
                                                 mmText(point) +
                                                 " mm deep drill point (finish it with a flat tool, or allow the "
                                                 "point below the floor)");
                    }
                }
                break;
            }
            break;
        }
        ph.top = h.zTop + toNm(in_.op.heights.top);
        ph.retract = h.zTop + toNm(in_.op.heights.retract);
        if (ph.tip >= ph.top) {
            return invalid("At " + at + " the tool tip would end at Z" + mmText(ph.tip) + ", not below the hole top Z" +
                           mmText(ph.top) + ": give a depth");
        }
        // Flutes and stick-out against what goes into the hole, measured from its top.
        const Nm deep = h.zTop - ph.tip;
        if (t.fluteLength > 0 && deep > toNm(t.fluteLength) + toNm(kDiameterTolMm)) {
            return invalid("At " + at + " the tool goes " + mmText(deep) + " mm deep, but its flutes are " +
                           mmText(t.fluteLength) + " mm long");
        }
        const double stickOut = in_.op.tool.stickOut;
        if (stickOut > 0 && toNm(stickOut) < deep + toNm(kHolderClearanceMm)) {
            return invalid("At " + at + " the tool goes " + mmText(deep) + " mm deep, but it sticks out only " +
                           mmText(stickOut) + " mm: the holder needs " + mmText(kHolderClearanceMm) +
                           " mm above the hole top");
        }
        return {};
    }

    Status tapDepth(PlannedHole& ph, Nm breakthrough)
    {
        const RecognizedHole& h = ph.hole;
        const lib::Tool& t = tool();
        const HoleSegment& bore = h.segments[h.bore];
        const std::string at = where(h);
        const Nm pitch = toNm(t.threadPitch);
        if (!h.threadSpec.empty()) {
            ph.leftHand = isLeftHandThread(h.threadSpec);
            const Nm holePitch = toNm(h.threadPitch);
            if (holePitch > 0 && (pitch - holePitch > kThreadPitchTolNm || holePitch - pitch > kThreadPitchTolNm)) {
                return invalid("The tap's pitch (" + mmText(t.threadPitch) + " mm) does not match the " + h.threadSpec +
                               " thread of " + at + " (pitch " + mmText(h.threadPitch) + " mm)");
            }
            if (h.threadNominalDiameter > 0 && std::abs(t.diameter - h.threadNominalDiameter) > kThreadDiameterTolMm) {
                return invalid("The " + mmText(t.diameter) + " mm tap does not fit the " + h.threadSpec +
                               " thread of " + at + " (" + mmText(h.threadNominalDiameter) + " mm)");
            }
        } else {
            ph.leftHand = p_.leftHand;
            plan_.warnings.push_back("The model gives no thread for " + at +
                                     " (an imported part, or a hole not made as tapped): it is tapped " +
                                     (p_.leftHand ? "left-hand" : "right-hand") + " with the tap's " +
                                     mmText(t.threadPitch) + " mm pitch; check the drawing");
        }
        if (bore.diameter >= t.diameter - kDiameterTolMm) {
            return invalid("The " + mmText(bore.diameter) + " mm hole at X" + mmText(h.x) + " Y" + mmText(h.y) +
                           " is not smaller than the " + mmText(t.diameter) +
                           " mm tap: drill it at the tap drill size");
        }
        // The basic minor diameter of a 60 degree thread, D - 1.082532 P (ISO 68-1, ASME B1.1: D - 5/4 H).
        const double minor = t.diameter - 1.0825317547305483 * t.threadPitch;
        if (bore.diameter + kDiameterTolMm < minor) {
            plan_.warnings.push_back("The " + mmText(bore.diameter) + " mm hole at X" + mmText(h.x) + " Y" +
                                     mmText(h.y) + " is smaller than the thread's minor diameter (" + mmText(minor) +
                                     " mm): the tap would cut the full thread form");
        }
        if (in_.op.cutting.clockwise == ph.leftHand) {
            return invalid(std::string("The thread of ") + at + " is " + (ph.leftHand ? "left" : "right") +
                           "-hand, which taps with the spindle turning " +
                           (ph.leftHand ? "counter-clockwise (M4)" : "clockwise (M3)") +
                           "; the operation's spindle direction is the other way");
        }
        const Nm lead = toNm(p_.tapLeadPitches * t.threadPitch);
        if (p_.depthFromValue) {
            ph.tip = h.zTop - toNm(p_.depth) - (p_.depthToTip ? 0 : lead);
        } else if (h.threadDepth > 0) {
            ph.tip = bore.zTop - toNm(h.threadDepth) - lead;
        } else if (h.bottom == HoleBottom::Through) {
            ph.tip = h.zBottom - breakthrough - lead;
        } else {
            ph.tip = bore.zBottom;
        }
        if (h.bottom != HoleBottom::Through && ph.tip < bore.zBottom) {
            return invalid("At " + at + " the tap would go to Z" + mmText(ph.tip) + ", below the drilled depth Z" +
                           mmText(bore.zBottom) + ": drill deeper, or tap less deep (thread depth or tap lead)");
        }
        return {};
    }

    // ---------- Levels and return (drill.h LEVELS, RETURN) ----------

    Status levels()
    {
        const ResolvedGeometry& g = in_.geom;
        Nm highestTop = std::numeric_limits<Nm>::min();
        for (const PlannedHole& ph : plan_.holes) {
            highestTop = std::max(highestTop, ph.hole.zTop);
        }
        plan_.clearance = highestTop + toNm(in_.op.heights.clearance);
        double safe = g.stockBounds.max.z;
        for (const std::vector<kernel::Shape>* shapes : {&g.partShapes, &g.fixtureShapes}) {
            for (const kernel::Shape& s : *shapes) {
                if (!s.isNull()) {
                    safe = std::max(safe, topInWcs(g.wcsFromModel, kernel::bounds(s)));
                }
            }
        }
        plan_.safeTravelZ = std::isfinite(safe) ? toNm(safe) : plan_.clearance;
        if (plan_.clearance < plan_.safeTravelZ) {
            return invalid("The clearance height ends at Z" + mmText(plan_.clearance) +
                           ", below the top of the stock, the part or a fixture (Z" + mmText(plan_.safeTravelZ) +
                           "): raise it so the tool can travel to the first hole");
        }
        for (std::size_t i = 0; i < plan_.holes.size(); ++i) {
            PlannedHole& ph = plan_.holes[i];
            const bool last = i + 1 == plan_.holes.size();
            ph.ret = !last && ph.retract >= plan_.safeTravelZ ? CycleReturn::RPlane : CycleReturn::Initial;
        }
        return {};
    }

    // The holes' extent against the machine's travel, where the machine gives it (the work offset is not known here,
    // so only the spans can be checked).
    Status reach()
    {
        Nm xMin = std::numeric_limits<Nm>::max(), xMax = std::numeric_limits<Nm>::min();
        Nm yMin = xMin, yMax = xMax, zMin = xMin;
        for (const PlannedHole& ph : plan_.holes) {
            xMin = std::min(xMin, ph.hole.x);
            xMax = std::max(xMax, ph.hole.x);
            yMin = std::min(yMin, ph.hole.y);
            yMax = std::max(yMax, ph.hole.y);
            zMin = std::min(zMin, ph.tip);
        }
        for (const lib::AxisDef& a : in_.machine.axes) {
            if (!(a.maxTravel > a.minTravel)) {
                continue; // not known
            }
            const Nm travel = toNm(a.maxTravel - a.minTravel);
            Nm span = 0;
            std::string what;
            if (a.letter == 'X') {
                span = xMax - xMin;
                what = "The holes span " + mmText(span) + " mm in X";
            } else if (a.letter == 'Y') {
                span = yMax - yMin;
                what = "The holes span " + mmText(span) + " mm in Y";
            } else if (a.letter == 'Z') {
                span = plan_.clearance - zMin;
                what = "From the clearance height to the deepest point is " + mmText(span) + " mm";
            } else {
                continue;
            }
            if (span > travel) {
                return failure(ErrorCode::ToolpathFailed, what + "; the machine's " + std::string(1, a.letter) +
                                                              " travel is " + mmText(travel) + " mm");
            }
        }
        return {};
    }

#undef RAY_TRY_STATUS

    const GenerateInput& in_;
    JobContext& ctx_;
    Params p_;
    DrillPlan plan_;
    std::map<std::size_t, std::vector<RecognizedHole>> recognised_;
};

} // namespace

Nm pointLengthNm(double diameterMm, double tipAngleDeg, double tipDiameterMm)
{
    if (!(tipAngleDeg > 0) || !(tipAngleDeg < 180) || !(diameterMm > tipDiameterMm) || !std::isfinite(diameterMm)) {
        return 0;
    }
    const double half = (diameterMm - tipDiameterMm) / 2;
    const double t = dmath::tan((tipAngleDeg * kRadPerDeg) / 2);
    return toNm(half / t);
}

std::int64_t tapFeedMicronPerMin(Nm threadPitchNm, std::int64_t rpm)
{
    const std::int64_t a = threadPitchNm * rpm; // < 2^63 for any pitch below 1 m and rpm below 1e7
    std::int64_t q = a / 1000;
    const std::int64_t r = a % 1000;
    if (r > 500 || (r == 500 && (q % 2) != 0)) {
        ++q;
    }
    return q;
}

bool isLeftHandThread(std::string_view spec)
{
    for (std::size_t i = 0; i + 1 < spec.size(); ++i) {
        const auto upper = [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        };
        const auto alnum = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        };
        if (upper(spec[i]) == 'L' && upper(spec[i + 1]) == 'H' && (i == 0 || !alnum(spec[i - 1])) &&
            (i + 2 == spec.size() || !alnum(spec[i + 2]))) {
            return true;
        }
    }
    return false;
}

Result<DrillPlan> planDrill(const GenerateInput& in, JobContext& ctx)
{
    try {
        return Planner(in, ctx).run();
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the drilling operation");
    }
}

} // namespace ray::cam::ops
