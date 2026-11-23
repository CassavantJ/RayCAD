// The facing operation's plan (face.h, task T-052): parameters, checks, levels and strokes. Every rule is stated in
// face.h; this file follows them in its order. Deterministic: integer nanometres from the first rounding on, no
// trigonometry, bodies measured by kernel::boundsIn, coverage checked with geom2d.
#include "face.h"
#include "op_params.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

// Returns the error of a Status or Result expression.
#define RAY_FACE_TRY(expr)                                                                                             \
    do {                                                                                                               \
        if (auto ray_face_status_ = (expr); !ray_face_status_) {                                                       \
            return tl::unexpected(std::move(ray_face_status_.error()));                                                \
        }                                                                                                              \
    } while (false)

namespace ray::cam::ops {

namespace {

using geom2d::Pt;

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

std::string toolText(const lib::Tool& t)
{
    return "the " + mmText(t.diameter) + " mm " + toolWord(t.type);
}

// Round half to even to an integer (the default rounding mode of IEEE arithmetic), for feeds.
std::int64_t roundHalfEven(double v)
{
    return static_cast<std::int64_t>(std::nearbyint(v));
}

// ceil(a / b) for a >= 0, b > 0.
std::int64_t ceilDiv(std::int64_t a, std::int64_t b)
{
    return a / b + (a % b != 0 ? 1 : 0);
}

// ---------- Parameters ----------

struct Params {
    bool zigzag = true;
    bool alongX = true;
    bool climb = true;
    double stepover = 0, overhang = 0, stepdown = 0;
};

const OpSchema* faceSchema()
{
    // Parsed once; immutable afterwards, so workers share it without a lock.
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("face"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = faceSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The facing operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    out.zigzag = p->text("pattern") == "zigzag";
    out.alongX = p->text("direction") == "alongX";
    out.climb = p->text("cutMode") == "climb";
    out.stepover = p->number("stepover");
    out.overhang = p->number("overhang");
    out.stepdown = p->number("stepdown");
    return out;
}

// ---------- The plan ----------

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx) {}

    Result<FacePlan> run()
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        RAY_FACE_TRY(checkTool());
        RAY_FACE_TRY(checkCutting());
        RAY_FACE_TRY(checkStock());
        RAY_FACE_TRY(measureBodies());
        RAY_FACE_TRY(checkHeights());
        RAY_FACE_TRY(levels());
        RAY_FACE_TRY(strokes());
        RAY_FACE_TRY(checkBodies());
        RAY_FACE_TRY(checkDepths());
        RAY_FACE_TRY(reach());
        linkOptions();
        return std::move(plan_);
    }

private:
    const lib::Tool& tool() const { return in_.tool; }

    Status checkTool()
    {
        const lib::Tool& t = tool();
        if (!(t.diameter > 0) || !std::isfinite(t.diameter) || t.diameter > 2000) {
            return invalid("The tool has no usable diameter");
        }
        using T = lib::ToolType;
        if (t.type != T::FaceMill && t.type != T::FlatEndMill && t.type != T::BullEndMill) {
            return invalid("Facing needs a face mill, a flat end mill or a bull nose end mill; the tool is a " +
                           std::string(toolWord(t.type)));
        }
        const double corner = std::isfinite(t.cornerRadius) && t.cornerRadius > 0 ? t.cornerRadius : 0;
        plan_.toolRadius = toNm(t.diameter * 0.5);
        plan_.flatHalf = plan_.toolRadius - toNm(corner);
        if (plan_.flatHalf <= 0) {
            return invalid("The " + mmText(t.diameter) + " mm " + toolWord(t.type) +
                           " has no flat bottom (its corner radius is " + mmText(corner) +
                           " mm): a facing tool needs one");
        }
        const Nm flat = 2 * plan_.flatHalf;
        if (p_.stepover > 0) {
            plan_.stepover = toNm(p_.stepover);
            if (plan_.stepover > flat) {
                const std::string part =
                    corner > 0 ? " (its diameter less two " + mmText(corner) + " mm corner radii)" : std::string();
                return invalid("The stepover (" + mmText(plan_.stepover) + " mm) is larger than the flat part of " +
                               toolText(t) + ", " + mmText(flat) + " mm" + part +
                               ": the strokes would leave ridges between them");
            }
            if (plan_.stepover <= 0) {
                return invalid("The stepover rounds to nothing: give at least 0.001 mm, or 0 for three quarters of "
                               "the tool's flat part");
            }
        } else {
            plan_.stepover = std::max<Nm>(1, (3 * flat) / 4);
        }
        plan_.overhang = toNm(p_.overhang);
        return {};
    }

    Status checkCutting()
    {
        const CuttingData& c = in_.op.cutting;
        const double rpm = c.spindleRpm;
        if (!(rpm > 0) || !std::isfinite(rpm)) {
            return invalid("Set a spindle speed");
        }
        if (!c.clockwise) {
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
        const double feed = c.feedCut * 1000.0;
        if (!(c.feedCut > 0) || !std::isfinite(feed) || feed > kMaxFeed || roundHalfEven(feed) < 1) {
            return invalid("Set a cutting feed");
        }
        plan_.feed = static_cast<std::int32_t>(roundHalfEven(feed));
        const double plunge = c.feedPlunge * 1000.0;
        if (!std::isfinite(plunge) || plunge < 0 || plunge > kMaxFeed) {
            return invalid("The plunge feed is not a usable number");
        }
        plunge_ = static_cast<std::int32_t>(roundHalfEven(plunge)); // 0: the cut feed (link.h Feeds)
        return {};
    }

    Status checkStock()
    {
        const Box3& b = in_.geom.stockBounds;
        for (const double v : {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z}) {
            if (!std::isfinite(v) || std::abs(v) > 1e6) {
                return invalid("The setup has no stock to face: its stock box is not known");
            }
        }
        x0_ = toNm(b.min.x);
        x1_ = toNm(b.max.x);
        y0_ = toNm(b.min.y);
        y1_ = toNm(b.max.y);
        plan_.stockTop = toNm(b.max.z);
        if (!(x1_ > x0_) || !(y1_ > y0_) || !(plan_.stockTop > toNm(b.min.z))) {
            return invalid("The setup has no stock to face: its stock box is empty");
        }
        plan_.stock = link::stockOf(in_.geom); // the same rounding: its region is the rectangle x0..x1, y0..y1
        if (plan_.stock.region.size() != 1) {
            return failure(ErrorCode::Internal, "The stock box gives no region");
        }
        plan_.stockRect = plan_.stock.region.front();
        return {};
    }

    // The WCS boxes of the part and fixture bodies (boundsIn: tight, the exact geometry).
    Status measureBodies()
    {
        const ResolvedGeometry& g = in_.geom;
        partTop_ = std::numeric_limits<Nm>::min();
        for (const std::vector<kernel::Shape>* shapes : {&g.partShapes, &g.fixtureShapes}) {
            const bool parts = shapes == &g.partShapes;
            for (const kernel::Shape& s : *shapes) {
                RAY_FACE_TRY(ctx_.cancel.check());
                if (s.isNull()) {
                    continue;
                }
                auto box = kernel::boundsIn(s, g.wcsFromModel);
                if (!box) {
                    return tl::unexpected(box.error());
                }
                if (parts) {
                    partTop_ = std::max(partTop_, toNm(box->max.z));
                } else {
                    fixtureBoxes_.push_back(*box);
                }
            }
        }
        hasParts_ = partTop_ != std::numeric_limits<Nm>::min();
        plan_.partTop = hasParts_ ? partTop_ : plan_.stockTop;
        return {};
    }

    Status checkHeights()
    {
        const Heights& h = in_.op.heights;
        for (const auto& [v, word] :
             {std::pair{h.clearance, "clearance"}, std::pair{h.retract, "retract"},
              std::pair{h.feedPlane, "feed plane"}, std::pair{h.top, "top"}, std::pair{h.bottom, "bottom"}}) {
            if (!std::isfinite(v) || std::abs(v) > 1e5) {
                return invalid(std::string("The ") + word + " height is not a usable number");
            }
        }
        if (h.clearance < h.retract) {
            return invalid("The clearance height (" + mmText(h.clearance) + ") must not be below the retract height (" +
                           mmText(h.retract) + ")");
        }
        if (h.retract < h.feedPlane) {
            return invalid("The retract height (" + mmText(h.retract) + ") must not be below the feed plane (" +
                           mmText(h.feedPlane) + ")");
        }
        if (h.feedPlane < h.top) {
            return invalid("The feed plane (" + mmText(h.feedPlane) + ") must not be below the top height (" +
                           mmText(h.top) + "): the tool would feed down from below where the material starts");
        }
        const Nm s = plan_.stockTop;
        clearance_ = s + toNm(h.clearance);
        retract_ = s + toNm(h.retract);
        feedPlane_ = s + toNm(h.feedPlane);
        plan_.top = s + toNm(h.top);
        plan_.bottom = plan_.partTop + toNm(h.bottom);
        if (retract_ < s) {
            return invalid("The retract height (" + mmText(h.retract) +
                           " above the stock top) is below the stock top: the tool would rapid through the stock; "
                           "give it 0 or more");
        }
        if (hasParts_ && plan_.bottom < plan_.partTop) {
            return invalid("The bottom height (" + mmText(h.bottom) + ") is below the top of the part (Z" +
                           mmText(plan_.partTop) +
                           "): facing would cut into the part; give it 0 or more (it is measured from the part top)");
        }
        if (plan_.bottom >= plan_.top) {
            const std::string bottomRef = hasParts_ ? "the part top" : "the stock top";
            return invalid("Nothing to face: the bottom height ends at Z" + mmText(plan_.bottom) + " (" + bottomRef +
                           " Z" + mmText(plan_.partTop) + " + " + mmText(h.bottom) +
                           "), not below the top height at Z" + mmText(plan_.top) + " (the stock top Z" + mmText(s) +
                           " + " + mmText(h.top) + "); add stock above the part or lower the bottom height");
        }
        // The clearance against everything the setup has, as the tool arrives there from anywhere.
        Nm safe = std::max(s, plan_.partTop);
        for (const Box3& b : fixtureBoxes_) {
            safe = std::max(safe, toNm(b.max.z));
        }
        if (clearance_ < safe) {
            return invalid("The clearance height ends at Z" + mmText(clearance_) +
                           ", below the top of the stock, the part or a fixture (Z" + mmText(safe) + "): raise it");
        }
        return {};
    }

    Status levels()
    {
        if (!(p_.stepdown > 0)) {
            return invalid("Give a maximum stepdown");
        }
        const Nm step = toNm(p_.stepdown);
        if (step <= 0) {
            return invalid("The maximum stepdown rounds to nothing: give at least 0.001 mm");
        }
        const Nm depth = plan_.top - plan_.bottom;
        if (static_cast<std::size_t>(ceilDiv(depth, step)) > kMaxFaceLevels) {
            return invalid("Facing " + mmText(depth) + " mm in passes of at most " + mmText(step) +
                           " mm takes more than " + std::to_string(kMaxFaceLevels) +
                           " passes: raise the maximum stepdown");
        }
        plan_.levels = faceLevels(plan_.top, plan_.bottom, step);
        std::size_t air = 0;
        for (const Nm z : plan_.levels) {
            air += z >= plan_.stockTop ? 1 : 0;
        }
        if (air > 0) {
            plan_.warnings.push_back("The top height is " + mmText(plan_.top - plan_.stockTop) +
                                     " mm above the stock top: the first " + std::to_string(air) +
                                     (air == 1 ? " pass cuts" : " passes cut") + " only air");
        }
        return {};
    }

    Status strokes()
    {
        const Nm r = plan_.toolRadius, a = plan_.overhang;
        // The stroke frame (face.h STROKES): u along, v across.
        const Nm u0 = p_.alongX ? x0_ : y0_, u1 = p_.alongX ? x1_ : y1_;
        const Nm v0 = p_.alongX ? y0_ : x0_, v1 = p_.alongX ? y1_ : x1_;
        const Nm lo = v0 - a, hi = v1 + a;
        const Nm length = (hi - plan_.flatHalf) - (lo + plan_.flatHalf);
        const std::size_t perLevel =
            length <= 0 ? 1
                        : static_cast<std::size_t>(std::min<std::int64_t>(ceilDiv(length, plan_.stepover) + 1,
                                                                          static_cast<std::int64_t>(kMaxStrokes) + 1));
        if (perLevel * plan_.levels.size() > kMaxStrokes) {
            return invalid("The facing would take more than " + std::to_string(kMaxStrokes) + " strokes (" +
                           std::to_string(plan_.levels.size()) + " passes, a stepover of " + mmText(plan_.stepover) +
                           " mm across " + mmText(hi - lo) + " mm): raise the stepover or the maximum stepdown");
        }
        const std::vector<Nm> across = strokePositions(lo, hi, plan_.flatHalf, plan_.stepover);
        plan_.strokesPerLevel = across.size();
        const Nm start = u0 - r - a, end = u1 + r + a;
        // +u is climb along X (the stock at +Y on the left), -u along Y (the stock at +X on the left of -Y).
        const bool climbForward = p_.alongX;
        const auto point = [&](Nm u, Nm v) {
            return p_.alongX ? Pt{u, v} : Pt{v, u};
        };
        std::vector<FaceStroke> level;
        for (std::size_t j = 0; j < across.size(); ++j) {
            const bool climb = p_.zigzag ? j % 2 == 0 : p_.climb;
            const bool forward = climb == climbForward;
            FaceStroke s;
            s.from = point(forward ? start : end, across[j]);
            s.to = point(forward ? end : start, across[j]);
            level.push_back(s);
        }
        plan_.strokes.reserve(level.size() * plan_.levels.size());
        for (std::size_t k = 0; k < plan_.levels.size(); ++k) {
            RAY_FACE_TRY(ctx_.cancel.check());
            const bool backwards = p_.zigzag && k % 2 == 1;
            for (std::size_t j = 0; j < level.size(); ++j) {
                FaceStroke s = level[backwards ? level.size() - 1 - j : j];
                if (backwards) {
                    std::swap(s.from, s.to);
                }
                s.z = plan_.levels[k];
                plan_.strokes.push_back(s);
            }
        }
        // The area the tool sweeps: the strokes' rectangle grown by the radius.
        sweepLo_ = point(start - r, lo + plan_.flatHalf - r);
        sweepHi_ = point(end + r, hi - plan_.flatHalf + r);
        if (sweepLo_.x > sweepHi_.x) {
            std::swap(sweepLo_.x, sweepHi_.x);
        }
        if (sweepLo_.y > sweepHi_.y) {
            std::swap(sweepLo_.y, sweepHi_.y);
        }
        if (across.size() == 1) { // one stroke: its band, not the covered range
            const Nm v = across.front();
            const Pt a0 = point(start - r, v - r), a1 = point(end + r, v + r);
            sweepLo_ = Pt{std::min(a0.x, a1.x), std::min(a0.y, a1.y)};
            sweepHi_ = Pt{std::max(a0.x, a1.x), std::max(a0.y, a1.y)};
        }
        return {};
    }

    // Nothing of the parts above the faced height (checkHeights), and no fixture above it inside the swept area.
    Status checkBodies()
    {
        for (const Box3& b : fixtureBoxes_) {
            const bool overlaps = toNm(b.min.x) < sweepHi_.x && toNm(b.max.x) > sweepLo_.x &&
                                  toNm(b.min.y) < sweepHi_.y && toNm(b.max.y) > sweepLo_.y;
            if (overlaps && toNm(b.max.z) > plan_.bottom) {
                return failure(ErrorCode::ToolpathFailed,
                               "A fixture reaches Z" + mmText(toNm(b.max.z)) + " inside the area the tool sweeps (X" +
                                   mmText(sweepLo_.x) + " to X" + mmText(sweepHi_.x) + ", Y" + mmText(sweepLo_.y) +
                                   " to Y" + mmText(sweepHi_.y) + "), above the faced height Z" + mmText(plan_.bottom) +
                                   ": the tool would hit it; lower the overhang or move the "
                                   "fixture");
            }
        }
        return {};
    }

    // The depth each pass cuts against the flutes and the stick-out.
    Status checkDepths()
    {
        Nm deepest = 0;
        Nm above = plan_.stockTop; // the material's top before the pass
        for (const Nm z : plan_.levels) {
            deepest = std::max(deepest, std::min(above, plan_.stockTop) - z);
            above = z;
        }
        const lib::Tool& t = tool();
        if (t.fluteLength > 0 && deepest > toNm(t.fluteLength)) {
            return invalid("A pass cuts " + mmText(deepest) + " mm deep, but the flutes of " + toolText(t) + " are " +
                           mmText(t.fluteLength) + " mm long: lower the maximum stepdown");
        }
        const double stickOut = in_.op.tool.stickOut;
        if (stickOut > 0 && toNm(stickOut) < deepest + toNm(kFaceHolderClearanceMm)) {
            return invalid("A pass cuts " + mmText(deepest) + " mm deep, but the tool sticks out only " +
                           mmText(stickOut) + " mm: the holder needs " + mmText(kFaceHolderClearanceMm) +
                           " mm above the material");
        }
        return {};
    }

    // The swept area against the machine's travel, where the machine gives it (the work offset is not known here,
    // so only the spans can be checked).
    Status reach()
    {
        for (const lib::AxisDef& ax : in_.machine.axes) {
            if (!(ax.maxTravel > ax.minTravel)) {
                continue; // not known
            }
            const Nm travel = toNm(ax.maxTravel - ax.minTravel);
            Nm span = 0;
            std::string what;
            if (ax.letter == 'X') {
                span = sweepHi_.x - sweepLo_.x - 2 * plan_.toolRadius;
                what = "The tool's centre travels " + mmText(span) + " mm in X";
            } else if (ax.letter == 'Y') {
                span = sweepHi_.y - sweepLo_.y - 2 * plan_.toolRadius;
                what = "The tool's centre travels " + mmText(span) + " mm in Y";
            } else if (ax.letter == 'Z') {
                span = clearance_ - plan_.bottom;
                what = "From the clearance height to the faced height is " + mmText(span) + " mm";
            } else {
                continue;
            }
            if (span > travel) {
                return failure(ErrorCode::ToolpathFailed, what + "; the machine's " + std::string(1, ax.letter) +
                                                              " travel is " + mmText(travel) + " mm");
            }
        }
        return {};
    }

    void linkOptions()
    {
        link::Options& o = plan_.link;
        o.clearance = clearance_;
        o.retract = retract_;
        o.feedPlane = feedPlane_;
        o.toolRadius = plan_.toolRadius;
        o.centreCutting = false; // never needed: every entry is beside the stock
        o.feeds.plunge = plunge_;
        o.maxStayDown = p_.zigzag ? plan_.stepover : 0;
        o.keepOrder = true;
    }

    const GenerateInput& in_;
    JobContext& ctx_;
    Params p_;
    FacePlan plan_;
    std::int32_t plunge_ = 0;
    Nm x0_ = 0, x1_ = 0, y0_ = 0, y1_ = 0;
    Nm clearance_ = 0, retract_ = 0, feedPlane_ = 0;
    Nm partTop_ = 0;
    bool hasParts_ = false;
    std::vector<Box3> fixtureBoxes_;
    Pt sweepLo_, sweepHi_;
};

} // namespace

#undef RAY_FACE_TRY

std::vector<Nm> faceLevels(Nm top, Nm bottom, Nm stepdown)
{
    std::vector<Nm> out;
    if (!(top > bottom) || stepdown <= 0) {
        return out;
    }
    const Nm depth = top - bottom;
    const std::int64_t n = ceilDiv(depth, stepdown);
    out.reserve(static_cast<std::size_t>(n));
    for (std::int64_t k = 1; k <= n; ++k) {
        // k * depth stays far below 2^63: depth < 3e12 nm (stock and heights within 1e6 mm) and the plan keeps
        // n <= kMaxFaceLevels.
        out.push_back(top - (k * depth) / n);
    }
    return out;
}

std::vector<Nm> strokePositions(Nm lo, Nm hi, Nm flatHalf, Nm spacing)
{
    std::vector<Nm> out;
    if (!(hi > lo) || flatHalf <= 0 || spacing <= 0) {
        return out;
    }
    const Nm first = lo + flatHalf, last = hi - flatHalf;
    const Nm length = last - first;
    if (length <= 0) {
        out.push_back(lo + (hi - lo) / 2);
        return out;
    }
    const std::int64_t gaps = ceilDiv(length, spacing);
    out.reserve(static_cast<std::size_t>(gaps + 1));
    for (std::int64_t j = 0; j <= gaps; ++j) {
        out.push_back(first + (j * length) / gaps); // below 2^63: length < 4e12 nm, gaps <= kMaxStrokes
    }
    return out;
}

Result<FacePlan> planFace(const GenerateInput& in, JobContext& ctx)
{
    try {
        return Planner(in, ctx).run();
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the facing operation");
    }
}

Result<double> faceUncovered(const FacePlan& plan, JobContext& ctx)
{
    if (plan.levels.empty() || plan.strokes.size() < plan.strokesPerLevel) {
        return geom2d::area(plan.stockRect);
    }
    std::vector<geom2d::Path> last;
    for (std::size_t i = plan.strokes.size() - plan.strokesPerLevel; i < plan.strokes.size(); ++i) {
        const FaceStroke& s = plan.strokes[i];
        geom2d::Path p;
        p.start = s.from;
        p.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, s.to, {}, 0});
        last.push_back(std::move(p));
    }
    auto swept = geom2d::sweptArea(last, plan.flatHalf, ctx);
    if (!swept) {
        return tl::unexpected(swept.error());
    }
    auto rest = geom2d::subtract(geom2d::Regions{plan.stockRect}, *swept, ctx);
    if (!rest) {
        return tl::unexpected(rest.error());
    }
    return geom2d::area(*rest);
}

} // namespace ray::cam::ops
