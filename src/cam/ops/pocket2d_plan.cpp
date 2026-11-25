// The 2D pocket operation's plan (pocket2d.h, task T-054): parameters, checks, the pockets and their regions at every
// depth, the rings, the coverage check, the passes and the linking options. Every rule is stated in pocket2d.h; this
// file follows them in its order. Deterministic: integer nanometres (geom2d), dmath for the one angle, std::map and
// sorted vectors only.
#include "area_depths.h"
#include "op_params.h"
#include "plunge_entry.h"
#include "pocket2d.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <utility>

// Returns the error of a Status or Result expression.
#define RAY_P2D_TRY(expr)                                                                                              \
    do {                                                                                                               \
        if (auto ray_p2d_status_ = (expr); !ray_p2d_status_) {                                                         \
            return tl::unexpected(std::move(ray_p2d_status_.error()));                                                 \
        }                                                                                                              \
    } while (false)
#define RAY_P2D_CAT2(a, b) a##b
#define RAY_P2D_CAT(a, b) RAY_P2D_CAT2(a, b)
// `lhs = *expr`, or returns the error of the Result expression.
#define RAY_P2D_ASSIGN(lhs, expr)                                                                                      \
    auto RAY_P2D_CAT(ray_p2d_result_, __LINE__) = (expr);                                                              \
    if (!RAY_P2D_CAT(ray_p2d_result_, __LINE__)) {                                                                     \
        return tl::unexpected(std::move(RAY_P2D_CAT(ray_p2d_result_, __LINE__).error()));                              \
    }                                                                                                                  \
    lhs = std::move(*RAY_P2D_CAT(ray_p2d_result_, __LINE__))

namespace ray::cam::ops {

namespace {

using geom2d::Path;
using geom2d::Pt;
using geom2d::Region;
using geom2d::Regions;
using geom2d::Where;

constexpr double kPi = 3.141592653589793238462643383279502884;

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}
tl::unexpected<Error> invalid(std::string message)
{
    return failure(ErrorCode::InvalidArgument, std::move(message));
}
tl::unexpected<Error> cannot(std::string message)
{
    return failure(ErrorCode::ToolpathFailed, std::move(message));
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
// An area in mm^2 for messages, to 0.01 mm^2.
std::string areaText(double nm2)
{
    const double mm2 = nm2 / 1e12;
    const std::int64_t hundredths = static_cast<std::int64_t>(std::nearbyint(mm2 * 100));
    return mmText(static_cast<Nm>(hundredths) * 10'000);
}
std::string at(Pt p)
{
    return "X" + mmText(p.x) + " Y" + mmText(p.y);
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

Vec3 apply(const Xform& f, Vec3 p)
{
    const double* m = f.m;
    return Vec3{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
                m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

std::int64_t roundHalfEven(double v)
{
    return static_cast<std::int64_t>(std::nearbyint(v));
}

// The centre of a region's box, for messages.
Pt middleOf(const Regions& rs)
{
    geom2d::Bounds b;
    for (const Region& r : rs) {
        const geom2d::Bounds rb = geom2d::bounds(r);
        if (rb.empty) {
            continue;
        }
        if (b.empty) {
            b = rb;
        } else {
            b.min = Pt{std::min(b.min.x, rb.min.x), std::min(b.min.y, rb.min.y)};
            b.max = Pt{std::max(b.max.x, rb.max.x), std::max(b.max.y, rb.max.y)};
        }
    }
    return b.empty ? Pt{} : Pt{b.min.x + (b.max.x - b.min.x) / 2, b.min.y + (b.max.y - b.min.y) / 2};
}

// ---------- Parameters ----------

struct Params {
    bool climb = true;
    double stepoverPercent = 45;
    double stepdown = 2;
    double stockWall = 0, stockFloor = 0;
    bool finishWalls = false;
    double wallAllowance = 0.2;
    bool finishFloor = false;
    double floorAllowance = 0.2;
    PocketEntry entry = PocketEntry::Helix;
    double helixPercent = 90;
    double rampAngle = 2;
    std::string preDrillPoints;
    double preDrillDiameter = 0;
};

const OpSchema* pocketSchema()
{
    // Parsed once; immutable afterwards, so workers share it without a lock.
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("pocket2d"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = pocketSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The 2D pocket operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    out.climb = p->text("cutMode") == "climb";
    out.stepoverPercent = p->number("stepover");
    out.stepdown = p->number("stepdown");
    out.stockWall = p->number("stockToLeaveWall");
    out.stockFloor = p->number("stockToLeaveFloor");
    out.finishWalls = p->flag("finishWalls");
    out.wallAllowance = p->number("wallAllowance");
    out.finishFloor = p->flag("finishFloor");
    out.floorAllowance = p->number("floorAllowance");
    const std::string& entry = p->text("entry");
    out.entry = entry == "ramp" ? PocketEntry::Ramp : entry == "preDrill" ? PocketEntry::PreDrill : PocketEntry::Helix;
    out.helixPercent = p->number("helixDiameter");
    out.rampAngle = p->number("rampAngle");
    out.preDrillPoints = p->text("preDrillPoints");
    out.preDrillDiameter = p->number("preDrillDiameter");
    return out;
}

// ---------- Small geometry ----------

// The loops of a region: the outer one, then the holes.
std::vector<const Path*> loopsOf(const Region& r)
{
    std::vector<const Path*> out{&r.outer};
    for (const Path& h : r.holes) {
        out.push_back(&h);
    }
    return out;
}

// The point of a path nearest to p and its arc length along the path (the first of equals in path order).
struct Nearest {
    double s = 0;
    double distance = std::numeric_limits<double>::infinity();
};
Nearest nearestOn(const Path& path, Pt p)
{
    Nearest best;
    double s0 = 0;
    Pt from = path.start;
    const auto consider = [&](double s, double d) {
        if (d < best.distance) {
            best.distance = d;
            best.s = s;
        }
    };
    const double px = static_cast<double>(p.x), py = static_cast<double>(p.y);
    for (const geom2d::Span& sp : path.spans) {
        const double ax = static_cast<double>(from.x), ay = static_cast<double>(from.y);
        const double bx = static_cast<double>(sp.end.x), by = static_cast<double>(sp.end.y);
        if (sp.kind == geom2d::SpanKind::Line) {
            const double dx = bx - ax, dy = by - ay;
            const double len2 = dx * dx + dy * dy;
            const double len = std::sqrt(len2);
            double t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
            t = std::clamp(t, 0.0, 1.0);
            const double qx = ax + t * dx - px, qy = ay + t * dy - py;
            consider(s0 + t * len, std::sqrt(qx * qx + qy * qy));
            s0 += len;
        } else {
            const double cx = static_cast<double>(sp.centre.x), cy = static_cast<double>(sp.centre.y);
            const double r = static_cast<double>(sp.radius);
            const double sweep = geom2d::sweepOf(from, sp);
            const double a0 = dmath::atan2(ay - cy, ax - cx);
            const double ap = dmath::atan2(py - cy, px - cx);
            const bool ccw = sp.kind == geom2d::SpanKind::ArcCCW;
            double delta = ccw ? ap - a0 : a0 - ap;
            while (delta < 0) {
                delta += 2 * kPi;
            }
            while (delta >= 2 * kPi) {
                delta -= 2 * kPi;
            }
            const double ex = ax - px, ey = ay - py, fx = bx - px, fy = by - py;
            consider(s0, std::sqrt(ex * ex + ey * ey));
            if (delta <= sweep) {
                const double dc = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
                consider(s0 + r * delta, std::abs(dc - r));
            }
            consider(s0 + r * sweep, std::sqrt(fx * fx + fy * fy));
            s0 += r * sweep;
        }
        from = sp.end;
    }
    return best;
}

// ---------- The plan ----------

struct Pocket {
    std::size_t selection = 0, part = 0;
    bool open = false;
    Nm floor = 0, bottom = 0, roughBottom = 0, top = 0;
    Region floorRegion;
    Regions footprint;
    std::vector<Nm> depths; // roughing, then the floor depth when finishing the floor
    std::vector<Regions> region, roughRoom, finishRoom;
    std::string where; // "the pocket with its floor at Z-5 around X20 Y30"
};

struct RingSet {
    Regions room;
    std::vector<std::pair<int, Regions>> rings; // (half steps h: the offset is h * s / 2, regions), by h
    PocketCoverage report;
};

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx) {}

    Result<Pocket2dPlan> run(PlanStage stage)
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        RAY_P2D_TRY(checkTool());
        RAY_P2D_TRY(checkCutting());
        RAY_P2D_TRY(checkHeights());
        RAY_P2D_TRY(readPreDrills());
        RAY_P2D_TRY(findPockets());
        RAY_P2D_TRY(depths());
        RAY_P2D_TRY(checkReach());
        RAY_P2D_TRY(regions());
        if (stage == PlanStage::Regions) {
            return std::move(plan_);
        }
        RAY_P2D_TRY(passes());
        options();
        plan_.stock = link::stockOf(in_.geom);
        plan_.warnings.insert(plan_.warnings.end(), preDrillWarnings_.begin(), preDrillWarnings_.end());
        return std::move(plan_);
    }

private:
    const lib::Tool& tool() const { return in_.tool; }
    std::string toolText() const { return "the " + mmText(tool().diameter) + " mm " + toolWord(tool().type); }

    // ---------- Checks ----------

    Status checkTool()
    {
        const lib::Tool& t = tool();
        if (!(t.diameter > 0) || !std::isfinite(t.diameter) || t.diameter > 1000) {
            return invalid("The tool has no usable diameter");
        }
        if (t.type != lib::ToolType::FlatEndMill && t.type != lib::ToolType::BullEndMill) {
            return invalid(std::string("A 2D pocket needs a flat or bull nose end mill; the tool is a ") +
                           toolWord(t.type));
        }
        r_ = toNm(t.diameter) / 2;
        if (r_ < 1000) {
            return invalid("The tool is too small to pocket with (" + mmText(t.diameter) + " mm)");
        }
        rf_ = r_;
        if (t.type == lib::ToolType::BullEndMill) {
            const Nm cr = std::isfinite(t.cornerRadius) ? toNm(std::max(0.0, t.cornerRadius)) : 0;
            if (cr >= r_) {
                return invalid("The bull nose end mill's corner radius (" + mmText(t.cornerRadius) +
                               " mm) is not smaller than its radius: it has no flat to cut a floor with");
            }
            rf_ = r_ - cr;
        }
        s_ = geom2d::roundNm(static_cast<double>(2 * r_) * p_.stepoverPercent / 100.0);
        if (s_ < 1000) {
            return invalid("The stepover is under 0.001 mm: make it larger");
        }
        if (s_ >= 2 * rf_) {
            return invalid("The stepover (" + mmText(s_) + " mm) is not less than the width of the bull nose's flat (" +
                           mmText(2 * rf_) + " mm): it would leave ridges on the floor; make it smaller");
        }
        stepdown_ = toNm(p_.stepdown);
        if (stepdown_ < 1000) {
            return invalid("The stepdown is under 0.001 mm: make it larger");
        }
        if (t.fluteLength > 0 && stepdown_ > toNm(t.fluteLength)) {
            return invalid("The stepdown (" + mmText(stepdown_) + " mm) is longer than the tool's flutes (" +
                           mmText(t.fluteLength) + " mm)");
        }
        stockWall_ = toNm(p_.stockWall);
        stockFloor_ = toNm(p_.stockFloor);
        wallAllowance_ = p_.finishWalls ? toNm(p_.wallAllowance) : 0;
        floorAllowance_ = p_.finishFloor ? toNm(p_.floorAllowance) : 0;
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
        const auto feedOf = [&](double mmMin, const char* what, std::int32_t& out) -> Status {
            const double f = mmMin * 1000.0;
            if (!std::isfinite(f) || f < 0 || f > kMaxFeed) {
                return invalid(std::string("The ") + what + " is not a usable number");
            }
            out = static_cast<std::int32_t>(roundHalfEven(f));
            return {};
        };
        RAY_P2D_TRY(feedOf(c.feedCut, "cutting feed", feedCut_));
        if (feedCut_ < 1) {
            return invalid("Set a cutting feed");
        }
        RAY_P2D_TRY(feedOf(c.feedPlunge, "plunge feed", feeds_.plunge));
        RAY_P2D_TRY(feedOf(c.feedRamp, "ramp feed", feeds_.ramp));
        RAY_P2D_TRY(feedOf(c.feedLeadIn, "lead-in feed", feeds_.leadIn));
        RAY_P2D_TRY(feedOf(c.feedLeadOut, "lead-out feed", feeds_.leadOut));
        return {};
    }

    Status checkHeights()
    {
        const Heights& h = in_.op.heights;
        for (const auto& [v, word] : {std::pair{h.clearance, "clearance"}, std::pair{h.retract, "retract"},
                                      std::pair{h.feedPlane, "feed plane"}, std::pair{h.top, "top"}}) {
            if (!std::isfinite(v) || std::abs(v) > 1e6) {
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
                           mmText(h.top) + ")");
        }
        if (h.retract < 0) {
            return invalid("The retract height (" + mmText(h.retract) +
                           " from the stock top) must not be below the top of the stock: rapids would run through it");
        }
        const double stockTop = in_.geom.stockBounds.max.z;
        if (!std::isfinite(stockTop) || std::abs(stockTop) > 1e6) {
            return failure(ErrorCode::Internal, "The setup has no usable stock");
        }
        stockTop_ = toNm(stockTop);
        top_ = stockTop_ + toNm(h.top);
        return {};
    }

    Status readPreDrills()
    {
        auto points = parsePointList(p_.preDrillPoints);
        if (!points) {
            return tl::unexpected(points.error());
        }
        for (const Vec2& v : *points) {
            preDrills_.push_back(Pt{toNm(v.x), toNm(v.y)});
        }
        if (p_.entry == PocketEntry::PreDrill && preDrills_.empty()) {
            return invalid("The entry is at pre-drilled holes, but none are given: list their centres under "
                           "'Pre-drilled holes'");
        }
        if (!preDrills_.empty()) {
            preDrillDiameter_ = toNm(p_.preDrillDiameter);
            if (preDrillDiameter_ < 2 * r_) {
                return invalid("The pre-drilled holes (" + mmText(preDrillDiameter_) + " mm) are narrower than " +
                               toolText() + ": give their diameter, at least the tool's");
            }
        }
        return {};
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

    // The material just above z of a part (kernel::sliceAt's rule), sliced once.
    Result<const Regions*> slice(std::size_t part, Nm z)
    {
        const auto key = std::make_pair(part, z);
        if (const auto it = slices_.find(key); it != slices_.end()) {
            return &it->second;
        }
        auto s = geom2d::sliceRegions(in_.geom.partShapes[part], in_.geom.wcsFromModel, z, ctx_);
        if (!s) {
            return tl::unexpected(s.error());
        }
        return &slices_.emplace(key, std::move(*s)).first->second;
    }

    // An open pocket's footprint: the floor's outer loop and a strip outside its open edges (pocket2d.h THE POCKET).
    Result<Regions> openFootprint(const Pocket& pk)
    {
        const Regions* above = nullptr;
        RAY_P2D_ASSIGN(above, slice(pk.part, pk.floor));
        std::vector<Path> open;
        for (const Path* loop : loopsOf(pk.floorRegion)) {
            RAY_P2D_TRY(ctx_.cancel.check());
            const double len = geom2d::length(*loop);
            if (!(len > 0)) {
                continue;
            }
            // Pieces of at most kOpenPieceNm inside each span, so no piece straddles a corner (whose probe would look
            // across the next edge).
            std::vector<double> at{0.0};
            Pt from = loop->start;
            for (const geom2d::Span& sp : loop->spans) {
                const double dx = static_cast<double>(sp.end.x - from.x), dy = static_cast<double>(sp.end.y - from.y);
                const double ls = sp.kind == geom2d::SpanKind::Line
                                      ? std::sqrt(dx * dx + dy * dy)
                                      : static_cast<double>(sp.radius) * geom2d::sweepOf(from, sp);
                const double s0 = at.back();
                const std::size_t m = std::max<std::size_t>(
                    1, static_cast<std::size_t>(std::ceil(ls / static_cast<double>(kOpenPieceNm))));
                for (std::size_t i = 1; i <= m; ++i) {
                    at.push_back(s0 + ls * static_cast<double>(i) / static_cast<double>(m));
                }
                from = sp.end;
            }
            const std::size_t n = at.size() - 1;
            std::vector<bool> isOpen(n, false);
            for (std::size_t j = 0; j < n; ++j) {
                const double sm = std::min(len, (at[j] + at[j + 1]) / 2);
                const Pt p = geom2d::pointAt(*loop, sm);
                const Vec2 t = geom2d::tangentAt(*loop, sm);
                const double d = static_cast<double>(kOpenProbeNm);
                // Right of the direction of travel: outside the region (which lies on the left of its loops).
                const Pt probe{p.x + geom2d::roundNm(t.y * d), p.y - geom2d::roundNm(t.x * d)};
                isOpen[j] = geom2d::locate(probe, *above) == Where::Outside;
            }
            if (std::all_of(isOpen.begin(), isOpen.end(), [](bool b) { return b; })) {
                open.push_back(*loop);
                continue;
            }
            // Runs of open pieces, starting after a closed one so a run that wraps round the start stays whole.
            std::size_t first = 0;
            while (isOpen[first]) {
                ++first;
            }
            for (std::size_t k = 1; k <= n; ++k) {
                const std::size_t j = (first + k) % n;
                if (!isOpen[j]) {
                    continue;
                }
                std::size_t end = k;
                while (end + 1 <= n && isOpen[(first + end + 1) % n]) {
                    ++end;
                }
                // Arc lengths of piece boundaries, unwrapped past the loop's end.
                const auto boundary = [&](std::size_t q) {
                    return q >= n ? len + at[q - n] : at[q];
                };
                const double s0 = boundary(first + k);
                const double s1 = std::min(boundary(first + end + 1), s0 + len);
                const double wrap = s0 >= len ? len : 0;
                open.push_back(geom2d::subPath(*loop, s0 - wrap, s1 - wrap));
                k = end;
            }
        }
        Regions footprint{Region{pk.floorRegion.outer, {}}};
        if (open.empty()) {
            return footprint;
        }
        // Square-ended: the run, out to its offset, back along it, in again; a round-ended strip would wrap round the
        // ends of the open edge and run the tool along the faces beside it. Where the offset comes in pieces (an
        // open run folding back on itself) the run's swept area is used instead.
        const Nm width = 2 * r_ + stockWall_ + wallAllowance_;
        for (const Path& run : open) {
            RAY_P2D_TRY(ctx_.cancel.check());
            Regions strip;
            geom2d::Paths off;
            if (!run.closed) {
                RAY_P2D_ASSIGN(off, geom2d::offsetOpen(run, width, geom2d::Side::Right, ctx_));
            }
            if (off.size() == 1 && !off[0].spans.empty()) {
                const Path back = geom2d::reverse(off[0]);
                Path loop;
                loop.start = run.start;
                loop.spans = run.spans;
                loop.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, back.start, {}, 0});
                loop.spans.insert(loop.spans.end(), back.spans.begin(), back.spans.end());
                loop.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, run.start, {}, 0});
                loop.closed = true;
                const Region r = geom2d::canonicalize(Region{geom2d::canonicalize(std::move(loop)), {}});
                // Regularised through a union with nothing: the loop is simple for the runs this makes.
                RAY_P2D_ASSIGN(strip, geom2d::unite(Regions{r}, Regions{}, ctx_));
            } else {
                RAY_P2D_ASSIGN(strip, geom2d::sweptArea(std::span<const Path>(&run, 1), width, ctx_));
            }
            RAY_P2D_ASSIGN(footprint, geom2d::unite(footprint, strip, ctx_));
        }
        return footprint;
    }

    Status findPockets()
    {
        const ResolvedGeometry& g = in_.geom;
        if (g.selected.empty()) {
            return invalid("Pick the floor of each pocket to machine");
        }
        if (g.selectedShapeIndex.size() != g.selected.size()) {
            return failure(ErrorCode::Internal, "The selection and its part indices differ in length");
        }
        std::set<std::pair<std::size_t, std::uint32_t>> taken;
        for (std::size_t i = 0; i < g.selected.size(); ++i) {
            RAY_P2D_TRY(ctx_.cancel.check());
            const kernel::SubId sub = g.selected[i];
            const int partIndex = g.selectedShapeIndex[i];
            if (partIndex < 0 || static_cast<std::size_t>(partIndex) >= g.partShapes.size()) {
                return failure(ErrorCode::Internal, "A selection lies on no part body");
            }
            const std::size_t part = static_cast<std::size_t>(partIndex);
            if (sub.kind != kernel::SubKind::Face) {
                return invalid(selectedWords(i) + " is not a face: pick the floor of each pocket");
            }
            if (!taken.insert({part, sub.index}).second) {
                continue;
            }
            const kernel::Shape& shape = g.partShapes[part];
            auto floor = geom2d::faceBoundary(shape, sub, g.wcsFromModel);
            if (!floor) {
                if (floor.error().code == ErrorCode::InvalidArgument) {
                    return invalid(selectedWords(i) +
                                   " is not a flat face square to the setup's Z axis: pick the floor of each pocket");
                }
                return tl::unexpected(floor.error());
            }
            if (!floor->facesUp) {
                return invalid(selectedWords(i) +
                               " looks down the setup's Z axis: pick the floor of each pocket, which looks up");
            }
            Pocket pk;
            pk.selection = i;
            pk.part = part;
            pk.floor = floor->z;
            pk.floorRegion = floor->region;
            auto closed = geom2d::pocketAboveFace(shape, sub, g.wcsFromModel, ctx_);
            if (closed) {
                pk.footprint = Regions{Region{closed->region.outer, {}}};
            } else if (closed.error().code == ErrorCode::InvalidArgument) {
                pk.open = true;
                RAY_P2D_ASSIGN(pk.footprint, openFootprint(pk));
            } else {
                return tl::unexpected(closed.error());
            }
            Regions floorOnly{pk.floorRegion};
            pk.where = "the pocket with its floor at Z" + mmText(pk.floor) + " around " + at(middleOf(floorOnly));
            pockets_.push_back(std::move(pk));
        }
        return {};
    }

    Status depths()
    {
        for (Pocket& pk : pockets_) {
            pk.top = top_;
            pk.bottom = pk.floor + stockFloor_;
            pk.roughBottom = pk.bottom + floorAllowance_;
            if (pk.top <= pk.bottom) {
                return invalid("The top height (Z" + mmText(pk.top) + ") is not above the bottom of " + pk.where +
                               " (Z" + mmText(pk.bottom) +
                               ", the stock to leave on the floor included): raise the "
                               "top height");
            }
            if (pk.top > pk.roughBottom) {
                const Nm span = pk.top - pk.roughBottom;
                const std::int64_t n = (span + stepdown_ - 1) / stepdown_;
                if (static_cast<std::size_t>(n) > kMaxDepths) {
                    return invalid("The stepdown would cut " + pk.where + " in " + std::to_string(n) +
                                   " depths: make it larger");
                }
                for (std::int64_t i = 1; i <= n; ++i) {
                    // round(span * i / n), half away from zero (all positive)
                    const Nm down = (span * i * 2 + n) / (2 * n);
                    pk.depths.push_back(pk.top - down);
                }
            }
            if (p_.finishFloor) {
                pk.depths.push_back(pk.bottom);
            }
        }
        return {};
    }

    Status checkReach()
    {
        Nm deepest = std::numeric_limits<Nm>::max();
        for (const Pocket& pk : pockets_) {
            deepest = std::min(deepest, pk.bottom);
        }
        const Nm deep = stockTop_ - deepest;
        const lib::Tool& t = tool();
        // A neck the tool states (libraries.h, after M3) is part of its reach unless it is wider than the cut.
        if (t.neckDiameter > t.diameter + 0.001 && t.fluteLength > 0 && deep > toNm(t.fluteLength)) {
            return invalid("The tool's " + mmText(t.neckDiameter) + " mm neck is wider than its cut and it goes " +
                           mmText(deep) + " mm below the stock top, but its flutes are only " + mmText(t.fluteLength) +
                           " mm long: the neck would rub the walls");
        }
        if (t.shankDiameter > t.diameter + 0.001) {
            double reach = t.shoulderLength > 0 ? t.shoulderLength : t.fluteLength;
            if (t.neckDiameter > 0) {
                reach = std::max(reach, t.neckLength);
            }
            if (reach > 0 && deep > toNm(reach)) {
                return invalid("The tool's " + mmText(t.shankDiameter) + " mm shank is wider than its cut and " +
                               "it goes " + mmText(deep) + " mm below the stock top, but it is only " + mmText(reach) +
                               " mm long below the shank: the shank would rub the walls");
            }
        }
        const double stickOut = in_.op.tool.stickOut;
        if (stickOut > 0 && toNm(stickOut) < deep + toNm(kPocketHolderClearanceMm)) {
            return invalid("The tool goes " + mmText(deep) + " mm below the stock top, but it sticks out only " +
                           mmText(stickOut) + " mm: the holder needs " + mmText(kPocketHolderClearanceMm) +
                           " mm above the stock");
        }
        return {};
    }

    // The pocket's depths with its flat faces, and its region, roughing room and finishing room at every depth
    // (pocket2d.h THE POCKET, DEPTHS, RINGS; area_depths.h).
    Status regions()
    {
        for (Pocket& pk : pockets_) {
            AreaDepthsInput a;
            a.part = &in_.geom.partShapes[pk.part];
            a.frame = in_.geom.wcsFromModel;
            a.footprint = pk.footprint;
            a.floor = pk.floorRegion;
            a.top = pk.top;
            a.low = pk.roughBottom;
            a.stockFloor = stockFloor_;
            a.keep = r_ + stockWall_; // the finishing pass's room; without finishWalls the roughing room's
            a.reach = r_;
            a.levels = pk.depths;
            AreaDepths area;
            RAY_P2D_ASSIGN(area, areaDepths(a, [&](Nm z) { return slice(pk.part, z); }, ctx_));
            pk.depths = area.depths;
            for (std::size_t d = 0; d < pk.depths.size(); ++d) {
                const Nm z = pk.depths[d];
                RAY_P2D_TRY(ctx_.cancel.check());
                Regions& kept = area.region[d];
                Regions rough, finish;
                RAY_P2D_ASSIGN(rough, shrink(kept, r_ + stockWall_ + wallAllowance_));
                RAY_P2D_ASSIGN(rough, withoutSlivers(std::move(rough)));
                if (rough.empty()) {
                    return cannot(capitalised(toolText()) + " does not fit " + pk.where + " at Z" + mmText(z) +
                                  ": nowhere in it is the tool centre " + mmText(r_ + stockWall_ + wallAllowance_) +
                                  " mm (its radius" +
                                  (stockWall_ + wallAllowance_ > 0 ? std::string(", the stock "
                                                                                 "to leave and the finishing allowance")
                                                                   : std::string()) +
                                  ") from every wall and island");
                }
                if (p_.finishWalls) {
                    RAY_P2D_ASSIGN(finish, shrink(kept, r_ + stockWall_));
                    RAY_P2D_ASSIGN(finish, withoutSlivers(std::move(finish)));
                }
                pk.region.push_back(std::move(kept));
                pk.roughRoom.push_back(std::move(rough));
                pk.finishRoom.push_back(std::move(finish));
            }
            PocketReport report;
            report.selection = pk.selection;
            report.open = pk.open;
            report.floor = pk.floor;
            report.bottom = pk.bottom;
            report.top = pk.top;
            report.depths = pk.depths;
            report.footprint = pk.footprint;
            plan_.pockets.push_back(std::move(report));
        }
        return {};
    }

    // geom2d::offset shrinking by d >= 0 (past the inradius it is empty: geom2d_clip.cpp, an arc offset past its
    // centre).
    Result<Regions> shrink(const Regions& in, geom2d::Coord d) { return geom2d::offset(in, -d, ctx_); }

    // `rs` without its slivers: parts narrower than 2 kSliverNm (an offset landing within micrometres of a part's
    // half width), whose rings are a line of micrometre spans; what they would cut is the coverage check's to find.
    Result<Regions> withoutSlivers(Regions rs)
    {
        Regions out;
        for (Region& r : rs) {
            Regions inner;
            RAY_P2D_ASSIGN(inner, shrink(Regions{r}, kSliverNm));
            if (!inner.empty()) {
                out.push_back(std::move(r));
            }
        }
        return out;
    }

    static std::string capitalised(std::string s)
    {
        if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') {
            s[0] = static_cast<char>(s[0] - 'a' + 'A');
        }
        return s;
    }

    // ---------- Rings and coverage (pocket2d.h RINGS, COVERAGE) ----------

    Result<std::size_t> ringsFor(const Regions& room, Nm z)
    {
        for (std::size_t i = 0; i < ringSets_.size(); ++i) {
            if (ringSets_[i].room == room) {
                ringSets_[i].report.depths.push_back(z);
                return i;
            }
        }
        RingSet rs;
        rs.room = room;
        for (std::size_t k = 0;; ++k) {
            RAY_P2D_TRY(ctx_.cancel.check());
            if (k > kMaxRings) {
                return invalid("The stepover is too small for the pocket: more than " + std::to_string(kMaxRings) +
                               " rings; make it larger");
            }
            Regions ring;
            if (k == 0) {
                ring = room;
            } else {
                RAY_P2D_ASSIGN(ring, shrink(room, static_cast<Nm>(k) * s_));
                RAY_P2D_ASSIGN(ring, withoutSlivers(std::move(ring)));
            }
            if (ring.empty()) {
                break;
            }
            rs.rings.emplace_back(static_cast<int>(2 * k), std::move(ring));
        }
        rs.report.rings = rs.rings.size();
        rs.report.room = room;
        rs.report.depths.push_back(z);
        RAY_P2D_TRY(cover(rs));
        ringSets_.push_back(std::move(rs));
        return ringSets_.size() - 1;
    }

    Status cover(RingSet& rs)
    {
        Regions reach;
        RAY_P2D_ASSIGN(reach, geom2d::offset(rs.room, rf_, ctx_));
        rs.report.reachableArea = geom2d::area(reach);
        for (int attempt = 0;; ++attempt) {
            std::vector<Path> paths;
            for (const auto& [h, regs] : rs.rings) {
                for (const Region& r : regs) {
                    for (const Path* loop : loopsOf(r)) {
                        paths.push_back(*loop);
                    }
                }
            }
            Regions swept, left, wide;
            RAY_P2D_ASSIGN(swept, geom2d::sweptArea(paths, rf_, ctx_));
            RAY_P2D_ASSIGN(left, geom2d::subtract(reach, swept, ctx_));
            if (!left.empty()) {
                RAY_P2D_ASSIGN(wide, shrink(left, kScallopLimitNm / 2));
            }
            rs.report.left = left;
            if (wide.empty()) {
                return {};
            }
            if (attempt > 0) {
                return cannot("An area of about " + areaText(geom2d::area(wide)) + " mm2 near " + at(middleOf(wide)) +
                              " would be left uncut by the rings: make the stepover smaller");
            }
            // Half rings where pieces are left (pocket2d.h COVERAGE).
            std::map<int, Regions> halves;
            for (const Region& piece : wide) {
                const std::optional<Pt> pt = geom2d::interiorPoint(piece);
                if (!pt) {
                    continue;
                }
                int band = 0;
                for (const auto& [h, regs] : rs.rings) {
                    if (h % 2 == 0 && geom2d::locate(*pt, regs) != Where::Outside) {
                        band = h;
                    }
                }
                const int half = band + 1;
                Regions ring;
                RAY_P2D_ASSIGN(ring, shrink(rs.room, static_cast<Nm>(band / 2) * s_ + s_ / 2));
                RAY_P2D_ASSIGN(ring, withoutSlivers(std::move(ring)));
                Regions& into = halves[half];
                bool any = false;
                for (const Region& c : ring) {
                    if (geom2d::locate(*pt, c) != Where::Outside) {
                        any = true;
                        if (std::find(into.begin(), into.end(), c) == into.end()) {
                            into.push_back(c);
                        }
                    }
                }
                if (!any) {
                    for (const Region& c : ring) {
                        if (std::find(into.begin(), into.end(), c) == into.end()) {
                            into.push_back(c);
                        }
                    }
                }
            }
            for (auto& [h, regs] : halves) {
                if (regs.empty()) {
                    continue;
                }
                rs.report.halfRings += 1;
                rs.rings.emplace_back(h, geom2d::canonicalize(std::move(regs)));
            }
            std::stable_sort(rs.rings.begin(), rs.rings.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
        }
    }

    // What the tool cannot get near at a pocket's deepest depth (pocket2d.h COVERAGE): warned.
    Status narrow(Pocket& pk, PocketReport& report)
    {
        const std::size_t d = pk.depths.size() - 1;
        const Regions& room = p_.finishWalls ? pk.finishRoom[d] : pk.roughRoom[d];
        Regions target, near, far;
        RAY_P2D_ASSIGN(target, shrink(pk.region[d], stockWall_));
        if (pk.open) {
            // Only the floor's own footprint: the strip past the open edges is room to run out, not pocket.
            RAY_P2D_ASSIGN(target, geom2d::intersect(target, Regions{Region{pk.floorRegion.outer, {}}}, ctx_));
        }
        RAY_P2D_ASSIGN(near, geom2d::offset(room, 2 * r_, ctx_));
        RAY_P2D_ASSIGN(far, geom2d::subtract(target, near, ctx_));
        for (const Region& piece : far) {
            Regions wide;
            RAY_P2D_ASSIGN(wide, shrink(Regions{piece}, kScallopLimitNm / 2));
            if (wide.empty()) {
                continue;
            }
            report.narrow.push_back(piece);
            plan_.warnings.push_back("A part of " + pk.where + " near " + at(middleOf(Regions{piece})) + " (about " +
                                     areaText(geom2d::area(piece)) + " mm2) is narrower than " + toolText() +
                                     " can reach into, and is left uncut");
        }
        return {};
    }

    // ---------- Passes ----------

    Path oriented(const Path& loop) const { return p_.climb ? loop : geom2d::reverse(loop); }

    // Whether the straight line a -> b stays in the room (within 2 um).
    Result<bool> lineInRoom(Pt a, Pt b, const Regions& room)
    {
        if (a == b) {
            return geom2d::locate(a, room) != Where::Outside;
        }
        Path line;
        line.start = a;
        line.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, b, {}, 0});
        Regions band, grown, out;
        RAY_P2D_ASSIGN(band, geom2d::sweptArea(std::span<const Path>(&line, 1), 500, ctx_));
        RAY_P2D_ASSIGN(grown, geom2d::offset(room, 2000, ctx_));
        RAY_P2D_ASSIGN(out, geom2d::subtract(band, grown, ctx_));
        return out.empty();
    }

    // For an innermost part of a ring: the pre-drilled point to start at and the loop and arc length to go to
    // (pocket2d.h ENTRY), or nullopt.
    struct DrillStart {
        Pt point;
        std::size_t loop = 0;
        double s = 0;
    };
    Result<std::optional<DrillStart>> drillStart(const Region& part, const Regions& room)
    {
        std::optional<DrillStart> best;
        double bestDistance = std::numeric_limits<double>::infinity();
        const std::vector<const Path*> loops = loopsOf(part);
        for (int pass = 0; pass < 2 && !best; ++pass) {
            for (const Pt& p : usablePreDrills_) {
                const bool inside = geom2d::locate(p, part) != Where::Outside;
                if ((pass == 0) != inside) {
                    continue;
                }
                std::size_t li = 0;
                Nearest n;
                for (std::size_t k = 0; k < loops.size(); ++k) {
                    const Nearest nk = nearestOn(*loops[k], p);
                    if (nk.distance < n.distance) {
                        n = nk;
                        li = k;
                    }
                }
                if (!(n.distance < bestDistance)) {
                    continue;
                }
                if (pass == 1) {
                    if (geom2d::locate(p, room) == Where::Outside) {
                        continue;
                    }
                    bool ok = false;
                    RAY_P2D_ASSIGN(ok, lineInRoom(p, geom2d::pointAt(*loops[li], n.s), room));
                    if (!ok) {
                        continue;
                    }
                }
                bestDistance = n.distance;
                best = DrillStart{p, li, n.s};
            }
        }
        return best;
    }

    // The pass of a loop started at a pre-drilled point: a straight cut to the loop, then the loop.
    link::Pass drillPass(const Path& loop, const DrillStart& d, Nm z) const
    {
        const Path ring = oriented(loop);
        const double len = geom2d::length(ring);
        const double s = p_.climb ? d.s : std::max(0.0, len - d.s);
        const Path started = s > 0 && s < len ? geom2d::startAt(ring, s) : ring;
        Path path;
        path.start = d.point;
        if (started.start != d.point) {
            path.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, started.start, {}, 0});
        }
        path.spans.insert(path.spans.end(), started.spans.begin(), started.spans.end());
        path.closed = false;
        return link::passFromPath(path, z, feedCut_);
    }

    Status passes()
    {
        // Pre-drilled points in some pocket, with the hole's bottom (the lowest floor whose footprint has it).
        for (const Pt& p : preDrills_) {
            std::optional<Nm> bottom;
            for (const Pocket& pk : pockets_) {
                if (geom2d::locate(p, pk.footprint) != Where::Outside) {
                    bottom = bottom ? std::min(*bottom, pk.bottom) : pk.bottom;
                }
            }
            if (!bottom) {
                preDrillWarnings_.push_back("The pre-drilled hole at " + at(p) +
                                            " lies in none of the pockets and is not used");
                continue;
            }
            usablePreDrills_.push_back(p);
            plan_.options.preDrills.push_back(link::PreDrill{p, preDrillDiameter_, *bottom});
        }
        for (std::size_t pi = 0; pi < pockets_.size(); ++pi) {
            Pocket& pk = pockets_[pi];
            PocketReport& report = plan_.pockets[pi];
            std::vector<std::size_t> sets;
            int maxH = 0;
            for (std::size_t d = 0; d < pk.depths.size(); ++d) {
                std::size_t set = 0;
                RAY_P2D_ASSIGN(set, ringsFor(pk.roughRoom[d], pk.depths[d]));
                sets.push_back(set);
                maxH = std::max(maxH, ringSets_[set].rings.back().first);
            }
            RAY_P2D_TRY(narrow(pk, report));
            const int stride = maxH + 2;
            for (std::size_t d = 0; d < pk.depths.size(); ++d) {
                const RingSet& rs = ringSets_[sets[d]];
                const Nm z = pk.depths[d];
                for (std::size_t ri = 0; ri < rs.rings.size(); ++ri) {
                    RAY_P2D_TRY(ctx_.cancel.check());
                    const auto& [h, regs] = rs.rings[ri];
                    const Regions* next = ri + 1 < rs.rings.size() ? &rs.rings[ri + 1].second : nullptr;
                    for (const Region& part : regs) {
                        bool innermost = true;
                        if (next != nullptr) {
                            for (const Region& n : *next) {
                                const std::optional<Pt> q = geom2d::interiorPoint(n);
                                if (q && geom2d::locate(*q, part) != Where::Outside) {
                                    innermost = false;
                                    break;
                                }
                            }
                        }
                        std::optional<DrillStart> drill;
                        if (innermost && !usablePreDrills_.empty()) {
                            RAY_P2D_ASSIGN(drill, drillStart(part, pk.roughRoom[d]));
                        }
                        const std::vector<const Path*> loops = loopsOf(part);
                        for (std::size_t li = 0; li < loops.size(); ++li) {
                            link::Pass pass = drill && drill->loop == li
                                                  ? drillPass(*loops[li], *drill, z)
                                                  : link::passFromPath(oriented(*loops[li]), z, feedCut_);
                            pass.rotatable = !(drill && drill->loop == li);
                            pass.cluster = static_cast<int>(pi);
                            pass.level = static_cast<int>(d) * stride + (maxH - h);
                            pass.room = pk.roughRoom[d];
                            plan_.passes.push_back(std::move(pass));
                        }
                    }
                }
            }
            if (p_.finishWalls) {
                const int base = static_cast<int>(pk.depths.size()) * stride;
                const Nm leadR = std::max<Nm>(r_ / 2, wallAllowance_ + 10'000);
                for (std::size_t d = 0; d < pk.depths.size(); ++d) {
                    for (const Region& part : pk.finishRoom[d]) {
                        for (const Path* loop : loopsOf(part)) {
                            link::Pass pass = link::passFromPath(oriented(*loop), pk.depths[d], feedCut_);
                            pass.rotatable = true;
                            pass.cluster = static_cast<int>(pi);
                            pass.level = base + static_cast<int>(d);
                            pass.room = pk.finishRoom[d];
                            link::Lead lead;
                            lead.kind = link::LeadKind::Arc;
                            lead.radius = leadR;
                            lead.sweepDeg = 90;
                            lead.side = p_.climb ? geom2d::Side::Left : geom2d::Side::Right;
                            lead.minRadius = std::min<Nm>(leadR, wallAllowance_ + 2'000);
                            pass.leadIn = lead;
                            pass.leadOut = lead;
                            plan_.passes.push_back(std::move(pass));
                        }
                    }
                }
            }
        }
        for (std::size_t pi = 0; pi < pockets_.size(); ++pi) {
            const std::vector<Regions>& rooms = pockets_[pi].roughRoom;
            for (const RingSet& rs : ringSets_) {
                if (std::find(rooms.begin(), rooms.end(), rs.room) != rooms.end()) {
                    plan_.pockets[pi].coverage.push_back(rs.report);
                }
            }
        }
        return {};
    }

    void options()
    {
        link::Options& o = plan_.options;
        const Heights& h = in_.op.heights;
        o.clearance = stockTop_ + toNm(h.clearance);
        o.retract = stockTop_ + toNm(h.retract);
        o.feedPlane = stockTop_ + toNm(h.feedPlane);
        o.toolRadius = r_;
        // After M3 (plunge_entry.h): a tool that cuts at its centre plunges where nothing else fits, never with the
        // entry restricted to pre-drilled holes. With a tool that does not, the plunge is still asked for last, so a
        // refusal says that it cannot plunge either.
        o.centreCutting = mayPlunge(tool());
        o.feeds = feeds_;
        o.feeds.link = 0;
        o.maxStayDown = std::max<Nm>(4 * r_, 3 * s_);
        o.stayDownLift = 0;
        o.stepIn = true;
        o.stepInSlot = true; // a stepover above the tool radius steps sideways into the next ring's cut
        o.rapidRetract = false;
        switch (p_.entry) {
        case PocketEntry::Helix:
            o.entryOrder = {EntryKind::Helix, EntryKind::Ramp, EntryKind::PreDrill};
            break;
        case PocketEntry::Ramp:
            o.entryOrder = {EntryKind::Ramp, EntryKind::Helix, EntryKind::PreDrill};
            break;
        case PocketEntry::PreDrill:
            o.entryOrder = {EntryKind::PreDrill};
            break;
        }
        if (p_.entry != PocketEntry::PreDrill) {
            o.entryOrder.push_back(EntryKind::Plunge);
        }
        o.helixDiameter = geom2d::roundNm(static_cast<double>(2 * r_) * p_.helixPercent / 100.0);
        o.helixMinDiameter =
            std::min(o.helixDiameter, geom2d::roundNm(static_cast<double>(2 * r_) * kHelixMinFraction));
        o.rampAngleDeg = p_.rampAngle;
        o.minRampRun = 0;
        o.entryClearance = kEntryClearanceNm;
        o.spindleClockwise = true;
        o.keepOrder = false;
    }

#undef RAY_P2D_TRY

    const GenerateInput& in_;
    JobContext& ctx_;
    Params p_;
    Pocket2dPlan plan_;
    Nm r_ = 0, rf_ = 0, s_ = 0, stepdown_ = 0;
    Nm stockWall_ = 0, stockFloor_ = 0, wallAllowance_ = 0, floorAllowance_ = 0;
    Nm stockTop_ = 0, top_ = 0;
    std::int32_t feedCut_ = 0;
    link::Feeds feeds_;
    std::vector<Pt> preDrills_, usablePreDrills_;
    Nm preDrillDiameter_ = 0;
    std::vector<std::string> preDrillWarnings_;
    std::vector<Pocket> pockets_;
    std::map<std::pair<std::size_t, Nm>, Regions> slices_;
    std::vector<RingSet> ringSets_;
};

} // namespace

Result<std::vector<Vec2>> parsePointList(std::string_view text)
{
    std::vector<Vec2> out;
    std::size_t i = 0;
    const auto space = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
            ++i;
        }
    };
    const auto number = [&](double& v) -> bool {
        space();
        const char* b = text.data() + i;
        const char* e = text.data() + text.size();
        if (b < e && *b == '+') {
            ++b;
        }
        const auto r = std::from_chars(b, e, v, std::chars_format::fixed);
        if (r.ec != std::errc() || !std::isfinite(v) || std::abs(v) > 1e6) {
            return false;
        }
        i = static_cast<std::size_t>(r.ptr - text.data());
        return true;
    };
    std::size_t count = 0;
    for (;;) {
        space();
        if (i >= text.size()) {
            break;
        }
        ++count;
        Vec2 p{};
        if (!number(p.x)) {
            return invalid("Pre-drilled holes: point " + std::to_string(count) +
                           " does not start with a number (write 'x y' pairs in mm separated by ';')");
        }
        space();
        if (i < text.size() && text[i] == ',') {
            ++i;
        }
        if (!number(p.y)) {
            return invalid("Pre-drilled holes: point " + std::to_string(count) +
                           " has no second number (write 'x y' pairs in mm separated by ';')");
        }
        out.push_back(p);
        space();
        if (i < text.size()) {
            if (text[i] != ';') {
                return invalid("Pre-drilled holes: after point " + std::to_string(count) +
                               " comes something other than ';'");
            }
            ++i;
        }
    }
    return out;
}

Result<Pocket2dPlan> planPocket2d(const GenerateInput& in, JobContext& ctx, PlanStage stage)
{
    try {
        return Planner(in, ctx).run(stage);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the 2D pocket");
    }
}

} // namespace ray::cam::ops
