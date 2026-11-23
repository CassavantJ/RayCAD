// The 2D adaptive roughing operation's plan (op.h, task T-061): parameters, checks, the areas and their regions at
// every depth (pocket2d's rules, as op.h says), planAdaptive per depth, the passes, the statistics and the linking
// options. Every rule is stated in op.h; this file follows them in its order. Deterministic: integer nanometres,
// std::map and sorted vectors only.
#include "../op_params.h"
#include "../pocket2d.h" // parsePointList: the pre-drilled points' grammar, shared with the form
#include "op.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <utility>

// Returns the error of a Status or Result expression.
#define RAY_A2D_TRY(expr)                                                                                              \
    do {                                                                                                               \
        if (auto ray_a2d_status_ = (expr); !ray_a2d_status_) {                                                         \
            return tl::unexpected(std::move(ray_a2d_status_.error()));                                                 \
        }                                                                                                              \
    } while (false)
#define RAY_A2D_CAT2(a, b) a##b
#define RAY_A2D_CAT(a, b) RAY_A2D_CAT2(a, b)
// `lhs = *expr`, or returns the error of the Result expression.
#define RAY_A2D_ASSIGN(lhs, expr)                                                                                      \
    auto RAY_A2D_CAT(ray_a2d_result_, __LINE__) = (expr);                                                              \
    if (!RAY_A2D_CAT(ray_a2d_result_, __LINE__)) {                                                                     \
        return tl::unexpected(std::move(RAY_A2D_CAT(ray_a2d_result_, __LINE__).error()));                              \
    }                                                                                                                  \
    lhs = std::move(*RAY_A2D_CAT(ray_a2d_result_, __LINE__))

namespace ray::cam::ops {

namespace {

namespace ad = adaptive2d;
using geom2d::Path;
using geom2d::Pt;
using geom2d::Region;
using geom2d::Regions;
using geom2d::Where;

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

std::string capitalised(std::string s)
{
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') {
        s[0] = static_cast<char>(s[0] - 'a' + 'A');
    }
    return s;
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

// The engine's and the entry planner's messages say where as "(x, y) mm" after an "adaptive: " prefix (engine.h,
// entry.h). For the operation panel: the prefix dropped, every "(x, y) mm" written "X.. Y..", "region" as "area".
std::string machinistWords(std::string m)
{
    const std::string prefix = "adaptive: ";
    if (m.rfind(prefix, 0) == 0) {
        m.erase(0, prefix.size());
    }
    for (std::size_t from = 0;;) {
        const std::size_t open = m.find('(', from);
        if (open == std::string::npos) {
            break;
        }
        const std::size_t comma = m.find(", ", open);
        const std::size_t close = m.find(") mm", open);
        if (comma == std::string::npos || close == std::string::npos || comma > close) {
            from = open + 1;
            continue;
        }
        const std::string x = m.substr(open + 1, comma - open - 1);
        const std::string y = m.substr(comma + 2, close - comma - 2);
        const bool numbers = !x.empty() && !y.empty() && x.find_first_not_of("-0123456789.") == std::string::npos &&
                             y.find_first_not_of("-0123456789.") == std::string::npos;
        if (!numbers) {
            from = open + 1;
            continue;
        }
        const std::string words = "X" + x + " Y" + y;
        m.replace(open, close + 4 - open, words);
        from = open + words.size();
    }
    for (std::size_t p = m.find("region"); p != std::string::npos; p = m.find("region", p + 4)) {
        m.replace(p, 6, "area");
    }
    return m;
}

// The first "X.. Y.." of a message made by machinistWords ("" when there is none).
std::string whereIn(const std::string& m)
{
    const std::size_t x = m.find(" X");
    if (x == std::string::npos) {
        return {};
    }
    const std::size_t y = m.find(" Y", x + 2);
    if (y == std::string::npos) {
        return {};
    }
    std::size_t end = y + 2;
    while (end < m.size() &&
           (std::isdigit(static_cast<unsigned char>(m[end])) != 0 || m[end] == '.' || m[end] == '-')) {
        ++end;
    }
    return m.substr(x + 1, end - x - 1);
}

// ---------- Parameters ----------

struct Params {
    double stepoverPercent = 10;
    double stepdown = 4;
    double reducedFeedPercent = 50;
    double stockWall = 0, stockFloor = 0;
    std::string entry = "helix";
    double helixPercent = 75, helixMinPercent = 30;
    double rampAngle = 2;
    std::string preDrillPoints;
    double preDrillDiameter = 0;
};

const OpSchema* adaptiveSchema()
{
    // Parsed once; immutable afterwards, so workers share it without a lock.
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("adaptive2d"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = adaptiveSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The 2D adaptive operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    out.stepoverPercent = p->number("stepover");
    out.stepdown = p->number("stepdown");
    out.reducedFeedPercent = p->number("reducedFeed");
    out.stockWall = p->number("stockToLeaveWall");
    out.stockFloor = p->number("stockToLeaveFloor");
    out.entry = p->text("entry");
    out.helixPercent = p->number("helixDiameter");
    out.helixMinPercent = p->number("helixMinDiameter");
    out.rampAngle = p->number("rampAngle");
    out.preDrillPoints = p->text("preDrillPoints");
    out.preDrillDiameter = p->number("preDrillDiameter");
    return out;
}

// ---------- Small geometry ----------

std::vector<const Path*> loopsOf(const Region& r)
{
    std::vector<const Path*> out{&r.outer};
    for (const Path& h : r.holes) {
        out.push_back(&h);
    }
    return out;
}

Region rectangle(Pt lo, Pt hi)
{
    Path p;
    p.start = lo;
    p.closed = true;
    for (const Pt& q : {Pt{hi.x, lo.y}, hi, Pt{lo.x, hi.y}, lo}) {
        p.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, q, {}, 0});
    }
    return geom2d::canonicalize(Region{geom2d::canonicalize(std::move(p)), {}});
}

double lengthOf(const std::vector<Pt>& pts)
{
    double s = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        s += dmath::hypot(static_cast<double>(pts[i].x - pts[i - 1].x), static_cast<double>(pts[i].y - pts[i - 1].y));
    }
    return s;
}

// ---------- The plan ----------

struct Area {
    std::size_t selection = 0, part = 0;
    bool open = false;
    Nm floor = 0, bottom = 0, top = 0;
    Region floorRegion;
    Regions footprint;
    std::vector<Nm> depths;
    std::vector<Regions> region;
    std::vector<Regions> material; // per depth: the part's material at the depth and above (op.h THE AREAS)
    std::string where;             // "the area with its floor at Z-5 around X20 Y30"
};

// What a depth's engine call was given, for reuse (op.h AT EACH DEPTH): the regions, stock and cleared area (the rest
// of the input is the operation's, the same at every depth).
struct EngineKey {
    Regions regions, stock, cleared;
    std::vector<ad::PreDrillHole> holes;
    bool operator==(const EngineKey& o) const
    {
        if (!(regions == o.regions && stock == o.stock && cleared == o.cleared && holes.size() == o.holes.size())) {
            return false;
        }
        for (std::size_t i = 0; i < holes.size(); ++i) {
            if (holes[i].centre != o.holes[i].centre || holes[i].diameter != o.holes[i].diameter) {
                return false;
            }
        }
        return true;
    }
};

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx) {}

    Result<Adaptive2dPlan> run(Adaptive2dStage stage)
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        RAY_A2D_TRY(checkTool());
        RAY_A2D_TRY(checkCutting());
        RAY_A2D_TRY(checkHeights());
        RAY_A2D_TRY(readPreDrills());
        RAY_A2D_TRY(stockBox());
        RAY_A2D_TRY(findAreas());
        RAY_A2D_TRY(depths());
        RAY_A2D_TRY(checkReach());
        RAY_A2D_TRY(regions());
        RAY_A2D_TRY(usePreDrills());
        if (stage == Adaptive2dStage::Regions) {
            report();
            plan_.warnings = preDrillWarnings_;
            return std::move(plan_);
        }
        options();
        RAY_A2D_TRY(stockSlices());
        RAY_A2D_TRY(passes());
        report();
        plan_.warnings.insert(plan_.warnings.begin(), preDrillWarnings_.begin(), preDrillWarnings_.end());
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
            return invalid(std::string("Adaptive roughing needs a flat or bull nose end mill; the tool is a ") +
                           toolWord(t.type));
        }
        r_ = toNm(t.diameter) / 2;
        if (r_ < 50'000) {
            return invalid("The tool is too small for adaptive roughing (" + mmText(t.diameter) +
                           " mm; at least 0.1 mm)");
        }
        if (t.type == lib::ToolType::BullEndMill) {
            const Nm cr = std::isfinite(t.cornerRadius) ? toNm(std::max(0.0, t.cornerRadius)) : 0;
            if (cr >= r_) {
                return invalid("The bull nose end mill's corner radius (" + mmText(t.cornerRadius) +
                               " mm) is not smaller than its radius");
            }
            tip_ = cr;
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
        helixD_ = geom2d::roundNm(static_cast<double>(2 * r_) * p_.helixPercent / 100.0);
        helixMinD_ = geom2d::roundNm(static_cast<double>(2 * r_) * p_.helixMinPercent / 100.0);
        if (p_.entry != "preDrill" && helixMinD_ > helixD_) {
            return invalid("The smallest helix (" + mmText(p_.helixMinPercent) + " %) is larger than the helix (" +
                           mmText(p_.helixPercent) + " %): make it at most the helix diameter");
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
        RAY_A2D_TRY(feedOf(c.feedCut, "cutting feed", feedCut_));
        if (feedCut_ < 1) {
            return invalid("Set a cutting feed");
        }
        feedReduced_ = static_cast<std::int32_t>(
            std::max<std::int64_t>(1, roundHalfEven(static_cast<double>(feedCut_) * p_.reducedFeedPercent / 100.0)));
        RAY_A2D_TRY(feedOf(c.feedPlunge, "plunge feed", feeds_.plunge));
        RAY_A2D_TRY(feedOf(c.feedRamp, "ramp feed", feeds_.ramp));
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
        if (p_.entry == "preDrill" && preDrills_.empty()) {
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

    Status stockBox()
    {
        const Box3& b = in_.geom.stockBounds;
        for (const double v : {b.min.x, b.min.y, b.max.x, b.max.y}) {
            if (!std::isfinite(v) || std::abs(v) > 1e6) {
                return failure(ErrorCode::Internal, "The setup has no usable stock");
            }
        }
        box_ = Regions{rectangle(Pt{toNm(b.min.x), toNm(b.min.y)}, Pt{toNm(b.max.x), toNm(b.max.y)})};
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

    // An open area's footprint: the floor's outer loop and a strip outside its open edges (pocket2d.h THE POCKET).
    Result<Regions> openFootprint(const Area& a)
    {
        const Regions* above = nullptr;
        RAY_A2D_ASSIGN(above, slice(a.part, a.floor));
        std::vector<Path> open;
        constexpr Nm kOpenPiece = 1'000'000; // edges probed in pieces of at most 1 mm
        constexpr Nm kOpenProbe = 20'000;    // 20 um outside an edge
        for (const Path* loop : loopsOf(a.floorRegion)) {
            RAY_A2D_TRY(ctx_.cancel.check());
            const double len = geom2d::length(*loop);
            if (!(len > 0)) {
                continue;
            }
            std::vector<double> sAt{0.0};
            Pt from = loop->start;
            for (const geom2d::Span& sp : loop->spans) {
                const double dx = static_cast<double>(sp.end.x - from.x), dy = static_cast<double>(sp.end.y - from.y);
                const double ls = sp.kind == geom2d::SpanKind::Line
                                      ? std::sqrt(dx * dx + dy * dy)
                                      : static_cast<double>(sp.radius) * geom2d::sweepOf(from, sp);
                const double s0 = sAt.back();
                const std::size_t m =
                    std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(ls / static_cast<double>(kOpenPiece))));
                for (std::size_t i = 1; i <= m; ++i) {
                    sAt.push_back(s0 + ls * static_cast<double>(i) / static_cast<double>(m));
                }
                from = sp.end;
            }
            const std::size_t n = sAt.size() - 1;
            std::vector<bool> isOpen(n, false);
            for (std::size_t j = 0; j < n; ++j) {
                const double sm = std::min(len, (sAt[j] + sAt[j + 1]) / 2);
                const Pt p = geom2d::pointAt(*loop, sm);
                const Vec2 t = geom2d::tangentAt(*loop, sm);
                const double d = static_cast<double>(kOpenProbe);
                const Pt probe{p.x + geom2d::roundNm(t.y * d), p.y - geom2d::roundNm(t.x * d)};
                isOpen[j] = geom2d::locate(probe, *above) == Where::Outside;
            }
            if (std::all_of(isOpen.begin(), isOpen.end(), [](bool b) { return b; })) {
                open.push_back(*loop);
                continue;
            }
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
                const auto boundary = [&](std::size_t q) {
                    return q >= n ? len + sAt[q - n] : sAt[q];
                };
                const double s0 = boundary(first + k);
                const double s1 = std::min(boundary(first + end + 1), s0 + len);
                const double wrap = s0 >= len ? len : 0;
                open.push_back(geom2d::subPath(*loop, s0 - wrap, s1 - wrap));
                k = end;
            }
        }
        Regions footprint{Region{a.floorRegion.outer, {}}};
        if (open.empty()) {
            return footprint;
        }
        const Nm width = 2 * r_ + stockWall_ + kAdaptiveFitToleranceNm;
        for (const Path& run : open) {
            RAY_A2D_TRY(ctx_.cancel.check());
            Regions strip;
            geom2d::Paths off;
            if (!run.closed) {
                RAY_A2D_ASSIGN(off, geom2d::offsetOpen(run, width, geom2d::Side::Right, ctx_));
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
                RAY_A2D_ASSIGN(strip, geom2d::unite(Regions{r}, Regions{}, ctx_));
            } else {
                RAY_A2D_ASSIGN(strip, geom2d::sweptArea(std::span<const Path>(&run, 1), width, ctx_));
            }
            RAY_A2D_ASSIGN(footprint, geom2d::unite(footprint, strip, ctx_));
        }
        return footprint;
    }

    Status findAreas()
    {
        const ResolvedGeometry& g = in_.geom;
        if (g.selected.empty()) {
            return invalid("Pick the floor of each area to rough out");
        }
        if (g.selectedShapeIndex.size() != g.selected.size()) {
            return failure(ErrorCode::Internal, "The selection and its part indices differ in length");
        }
        std::set<std::pair<std::size_t, std::uint32_t>> taken;
        for (std::size_t i = 0; i < g.selected.size(); ++i) {
            RAY_A2D_TRY(ctx_.cancel.check());
            const kernel::SubId sub = g.selected[i];
            const int partIndex = g.selectedShapeIndex[i];
            if (partIndex < 0 || static_cast<std::size_t>(partIndex) >= g.partShapes.size()) {
                return failure(ErrorCode::Internal, "A selection lies on no part body");
            }
            const std::size_t part = static_cast<std::size_t>(partIndex);
            if (sub.kind != kernel::SubKind::Face) {
                return invalid(selectedWords(i) + " is not a face: pick the floor of each area");
            }
            if (!taken.insert({part, sub.index}).second) {
                continue;
            }
            const kernel::Shape& shape = g.partShapes[part];
            auto floor = geom2d::faceBoundary(shape, sub, g.wcsFromModel);
            if (!floor) {
                if (floor.error().code == ErrorCode::InvalidArgument) {
                    return invalid(selectedWords(i) +
                                   " is not a flat face square to the setup's Z axis: pick the floor of each area");
                }
                return tl::unexpected(floor.error());
            }
            if (!floor->facesUp) {
                return invalid(selectedWords(i) +
                               " looks down the setup's Z axis: pick the floor of each area, which looks up");
            }
            Area a;
            a.selection = i;
            a.part = part;
            a.floor = floor->z;
            a.floorRegion = floor->region;
            auto closed = geom2d::pocketAboveFace(shape, sub, g.wcsFromModel, ctx_);
            if (closed) {
                a.footprint = Regions{Region{closed->region.outer, {}}};
            } else if (closed.error().code == ErrorCode::InvalidArgument) {
                a.open = true;
                RAY_A2D_ASSIGN(a.footprint, openFootprint(a));
            } else {
                return tl::unexpected(closed.error());
            }
            Regions floorOnly{a.floorRegion};
            a.where = "the area with its floor at Z" + mmText(a.floor) + " around " + at(middleOf(floorOnly));
            areas_.push_back(std::move(a));
        }
        return {};
    }

    Status depths()
    {
        for (Area& a : areas_) {
            a.top = top_;
            a.bottom = a.floor + stockFloor_;
            if (a.top <= a.bottom) {
                return invalid("The top height (Z" + mmText(a.top) + ") is not above the bottom of " + a.where + " (Z" +
                               mmText(a.bottom) + ", the stock to leave on the floor included): raise the top height");
            }
            const Nm span = a.top - a.bottom;
            const std::int64_t n = (span + stepdown_ - 1) / stepdown_;
            if (static_cast<std::size_t>(n) > kAdaptiveMaxDepths) {
                return invalid("The stepdown would cut " + a.where + " in " + std::to_string(n) +
                               " depths: make it larger");
            }
            for (std::int64_t i = 1; i <= n; ++i) {
                // round(span * i / n), half away from zero (all positive)
                const Nm down = (span * i * 2 + n) / (2 * n);
                a.depths.push_back(a.top - down);
            }
            RAY_A2D_TRY(flatTops(a));
        }
        return {};
    }

    // Flat tops inside the footprint between the floor and the top: a depth each (pocket2d.h DEPTHS).
    Status flatTops(Area& a)
    {
        const kernel::Shape& shape = in_.geom.partShapes[a.part];
        const Xform& f = in_.geom.wcsFromModel;
        const std::uint32_t faces = shape.count(kernel::SubKind::Face);
        for (std::uint32_t i = 0; i < faces; ++i) {
            RAY_A2D_TRY(ctx_.cancel.check());
            const kernel::SubId face{kernel::SubKind::Face, i};
            const auto info = kernel::surfaceInfo(shape, face);
            if (!info || info->type != kernel::SurfaceType::Plane) {
                continue;
            }
            const auto sample = kernel::faceSample(shape, face);
            if (!sample) {
                continue;
            }
            const Vec3& n = sample->normal;
            const double up = f.m[8] * n.x + f.m[9] * n.y + f.m[10] * n.z;
            if (!(up > 0.999)) {
                continue;
            }
            auto fb = geom2d::faceBoundary(shape, face, f);
            if (!fb || !fb->facesUp) {
                continue;
            }
            const Nm z = fb->z + stockFloor_;
            if (z <= a.bottom || z >= a.top) {
                continue;
            }
            const std::optional<Pt> inside = geom2d::interiorPoint(fb->region);
            if (!inside || geom2d::locate(*inside, a.footprint) != Where::Inside) {
                continue;
            }
            a.depths.push_back(z);
        }
        std::sort(a.depths.begin(), a.depths.end(), std::greater<>());
        a.depths.erase(std::unique(a.depths.begin(), a.depths.end()), a.depths.end());
        return {};
    }

    Status checkReach()
    {
        Nm deepest = std::numeric_limits<Nm>::max();
        for (const Area& a : areas_) {
            deepest = std::min(deepest, a.bottom);
        }
        const Nm deep = stockTop_ - deepest;
        const lib::Tool& t = tool();
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
        if (stickOut > 0 && toNm(stickOut) < deep + toNm(kAdaptiveHolderClearanceMm)) {
            return invalid("The tool goes " + mmText(deep) + " mm below the stock top, but it sticks out only " +
                           mmText(stickOut) + " mm: the holder needs " + mmText(kAdaptiveHolderClearanceMm) +
                           " mm above the stock");
        }
        return {};
    }

    // The area at every depth, and the check that the tool fits it (op.h THE AREAS).
    Status regions()
    {
        for (Area& a : areas_) {
            Regions material;
            const Regions floorOnly{a.floorRegion};
            for (const Nm z : a.depths) {
                RAY_A2D_TRY(ctx_.cancel.check());
                const Regions* s = nullptr;
                RAY_A2D_ASSIGN(s, slice(a.part, z - stockFloor_));
                if (material.empty()) {
                    material = *s;
                } else if (!s->empty()) {
                    RAY_A2D_ASSIGN(material, geom2d::unite(material, *s, ctx_));
                }
                Regions free;
                RAY_A2D_ASSIGN(free, geom2d::subtract(a.footprint, material, ctx_));
                Regions kept;
                for (Region& piece : free) {
                    Regions meet;
                    RAY_A2D_ASSIGN(meet, geom2d::intersect(Regions{piece}, floorOnly, ctx_));
                    if (geom2d::area(meet) > 0) {
                        kept.push_back(std::move(piece));
                    }
                }
                Regions room;
                RAY_A2D_ASSIGN(room, geom2d::offset(kept, -(r_ + stockWall_ + kAdaptiveFitToleranceNm), ctx_));
                if (room.empty()) {
                    return cannot(capitalised(toolText()) + " does not fit " + a.where + " at Z" + mmText(z) +
                                  ": nowhere in it is the tool centre " + mmText(r_ + stockWall_) + " mm (its radius" +
                                  (stockWall_ > 0 ? std::string(" and the stock to leave") : "") +
                                  ") from every wall and island");
                }
                a.region.push_back(std::move(kept));
                a.material.push_back(material);
            }
        }
        return {};
    }

    // Pre-drilled points in some area (op.h WARNINGS), with the hole's bottom: the lowest floor of the areas it is in.
    Status usePreDrills()
    {
        for (const Pt& p : preDrills_) {
            std::optional<Nm> bottom;
            for (const Area& a : areas_) {
                if (geom2d::locate(p, a.footprint) != Where::Outside) {
                    bottom = bottom ? std::min(*bottom, a.bottom) : a.bottom;
                }
            }
            if (!bottom) {
                preDrillWarnings_.push_back("The pre-drilled hole at " + at(p) +
                                            " lies in none of the areas and is not used");
                continue;
            }
            usable_.push_back(link::PreDrill{p, preDrillDiameter_, *bottom});
        }
        return {};
    }

    // ---------- Stock slices (op.h STOCK SLICES) ----------

    Status stockSlices()
    {
        const auto& slices = in_.geom.stockSlices;
        if (!slices || slices->empty()) {
            return {};
        }
        // By Z, highest first; the union of every slice at that Z and above.
        std::map<Nm, std::vector<kernel::ProfileLoop>, std::greater<>> byZ;
        for (const auto& [z, loops] : *slices) {
            if (!std::isfinite(z) || std::abs(z) > 1e6) {
                return invalid("The in-process stock has a slice at a height that is not a usable number");
            }
            auto& into = byZ[toNm(z)];
            into.insert(into.end(), loops.begin(), loops.end());
        }
        Regions above;
        for (const auto& [z, loops] : byZ) {
            RAY_A2D_TRY(ctx_.cancel.check());
            Regions here;
            RAY_A2D_ASSIGN(here, geom2d::regionsFromLoops(loops, ctx_));
            if (above.empty()) {
                above = std::move(here);
            } else if (!here.empty()) {
                RAY_A2D_ASSIGN(above, geom2d::unite(above, here, ctx_));
            }
            materialAt_.emplace(z, above);
            Regions free;
            RAY_A2D_ASSIGN(free, geom2d::subtract(box_, above, ctx_));
            if (!free.empty()) {
                plan_.stock.cleared.push_back(link::Cleared{z, std::move(free)});
            }
        }
        return {};
    }

    // The material at depth z (op.h STOCK SLICES); nullptr: nothing known (the stock box).
    const Regions* materialAt(Nm z) const
    {
        // materialAt_ is ordered highest first: the first entry at or below z.
        for (const auto& [level, m] : materialAt_) {
            if (level <= z) {
                return &m;
            }
        }
        return nullptr;
    }

    // ---------- Passes ----------

    // The moves of a polyline at height z (the tool is at pts[0]): fitted in runs of equal feed (op.h PASSES).
    void appendFitted(const std::vector<Pt>& pts, const std::vector<std::uint8_t>* reduced, Nm z, MoveRole role,
                      std::vector<Move>& out) const
    {
        // Consecutive equal points dropped, each segment keeping its flag.
        std::vector<Pt> p;
        std::vector<std::uint8_t> f;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            if (!p.empty() && pts[i] == p.back()) {
                if (reduced != nullptr && i < reduced->size() && (*reduced)[i] != 0 && f.size() > 1) {
                    f.back() = 1;
                }
                continue;
            }
            p.push_back(pts[i]);
            f.push_back(reduced != nullptr && i < reduced->size() ? (*reduced)[i] : 0);
        }
        if (p.size() < 2) {
            return;
        }
        geom2d::FitOptions fit;
        fit.tolerance = kAdaptiveFitToleranceNm;
        std::size_t a = 0;
        while (a + 1 < p.size()) {
            const bool slow = f[a + 1] != 0;
            std::size_t b = a + 1;
            while (b + 1 < p.size() && (f[b + 1] != 0) == slow) {
                ++b;
            }
            const Path fitted = geom2d::fitArcs(std::span<const Pt>(p.data() + a, b - a + 1), false, fit);
            geom2d::appendMoves(fitted, z, role, slow ? feedReduced_ : feedCut_, out);
            a = b;
        }
    }

    static void feedTo(Pt xy, Nm z, MoveRole role, std::int32_t feed, std::vector<Move>& out)
    {
        Move m{};
        m.kind = MoveKind::Linear;
        m.role = role;
        m.feedMicronPerMin = feed;
        m.x = xy.x;
        m.y = xy.y;
        m.z = z;
        out.push_back(m);
    }

    // Engagement statistics of a depth's output (op.h STATS).
    void collectEngagement(const ad::Output& out)
    {
        for (const ad::Region& r : out.regions) {
            for (const ad::Path& p : r.paths) {
                if (p.motion != ad::Motion::Cut) {
                    continue;
                }
                for (std::size_t i = 1; i < p.points.size() && i < p.engagementDeg.size(); ++i) {
                    const double d = p.engagementDeg[i];
                    if (!(d >= 0)) {
                        continue;
                    }
                    const double len = dmath::hypot(static_cast<double>(p.points[i].x - p.points[i - 1].x),
                                                    static_cast<double>(p.points[i].y - p.points[i - 1].y)) /
                                       kNmPerMm;
                    if (!(len > 0)) {
                        continue;
                    }
                    engagement_.emplace_back(d, len);
                    plan_.maxEngagementDeg = std::max(plan_.maxEngagementDeg, d);
                    if (d < kAdaptiveAirDeg) {
                        plan_.airCutLengthMm += len;
                    }
                }
            }
        }
    }

    // The engine's warnings of one depth (op.h WARNINGS), gathered by kind and place over the depths.
    void collectWarnings(const ad::Output& out, Nm z)
    {
        for (const ad::Warning& w : out.warnings) {
            const std::string words = machinistWords(w.message);
            const std::string where = whereIn(words);
            const std::string near = where.empty() ? std::string() : " near " + where;
            std::string text;
            switch (w.code) {
            case ad::Warning::Code::LeadFailed:
                text = "a pass" + near +
                       " starts or ends in material without a lead from cleared area: the cut there may be heavier "
                       "than the set load";
                break;
            case ad::Warning::Code::FinishingLeadFailed:
                text = "the light pass along the walls" + near +
                       " was left out (no way in from cleared area): the walls there keep up to a tenth of the load "
                       "more stock";
                break;
            case ad::Warning::Code::FinishingSetup:
                text = "the area" + near +
                       " was not cleared close enough to the walls before the light pass along them: that pass may "
                       "cut more than the set load there";
                break;
            case ad::Warning::Code::UnclearedArea:
                text = "material is left uncut" + near + ": no pass could start cutting into it";
                break;
            case ad::Warning::Code::NotEntered:
            case ad::Warning::Code::EntryFallback:
                text = words;
                break;
            }
            const auto key = std::make_pair(static_cast<int>(w.code), text);
            auto it = warningIndex_.find(key);
            if (it == warningIndex_.end()) {
                warningIndex_.emplace(key, warningOrder_.size());
                warningOrder_.push_back({text, {z}});
            } else if (warningOrder_[it->second].second.back() != z) {
                warningOrder_[it->second].second.push_back(z);
            }
        }
    }

    // The engine's errors in words, with the depth and the area.
    Error engineError(Error e, const Area& a, Nm z) const
    {
        if (e.code == ErrorCode::InvalidArgument || e.code == ErrorCode::ToolpathFailed) {
            e.message = "At Z" + mmText(z) + " in " + a.where + ": " + machinistWords(e.message);
            // The engine saw the area with its walls moved in by these (op.h AT EACH DEPTH): widths it names are of
            // that area.
            e.message += " (widths of the area less " + mmText(stockWall_ + kAdaptiveFitToleranceNm) +
                         " mm at each wall: the stock to leave and the arc fitting's tolerance)";
        }
        return e;
    }

    Status passes()
    {
        ad::EntryOptions entry;
        if (p_.entry == "ramp") {
            entry.order = {EntryKind::Ramp, EntryKind::Helix, EntryKind::PreDrill};
        } else if (p_.entry == "preDrill") {
            entry.order = {EntryKind::PreDrill};
        } else {
            entry.order = {EntryKind::Helix, EntryKind::Ramp, EntryKind::PreDrill};
        }
        for (const link::PreDrill& h : usable_) {
            entry.preDrills.push_back(ad::PreDrillHole{h.at, h.diameter});
        }
        std::size_t total = 0;
        for (const Area& a : areas_) {
            total += a.depths.size();
        }
        std::vector<std::pair<EngineKey, ad::Output>> done;
        std::vector<Regions> doneCleared; // per entry of `done`: its cleared area shrunk (op.h PASSES, cut areas)
        for (std::size_t ai = 0; ai < areas_.size(); ++ai) {
            const Area& a = areas_[ai];
            for (std::size_t d = 0; d < a.depths.size(); ++d) {
                RAY_A2D_TRY(ctx_.cancel.check());
                const Nm z = a.depths[d];
                const Nm above = d == 0 ? a.top : a.depths[d - 1];
                ad::Input ein;
                // The walls moved in by the stock to leave and the fit tolerance (op.h AT EACH DEPTH).
                RAY_A2D_ASSIGN(ein.regions, geom2d::offset(a.region[d], -(stockWall_ + kAdaptiveFitToleranceNm), ctx_));
                // The part grown by the stock to leave and the fit tolerance is stock too (op.h AT EACH DEPTH), and
                // material whatever the slices say (op.h STOCK SLICES).
                Regions grownPart;
                RAY_A2D_ASSIGN(grownPart, geom2d::offset(a.material[d], stockWall_ + kAdaptiveFitToleranceNm, ctx_));
                if (const Regions* m = materialAt(z)) {
                    RAY_A2D_ASSIGN(ein.stock, geom2d::unite(*m, grownPart, ctx_));
                    RAY_A2D_ASSIGN(ein.cleared, geom2d::subtract(box_, ein.stock, ctx_));
                } else {
                    RAY_A2D_ASSIGN(ein.stock, geom2d::unite(box_, grownPart, ctx_));
                }
                ein.enterFromOutside = true;
                ein.toolDiameter = 2 * r_;
                ein.stepover = p_.stepoverPercent / 100.0;
                ein.tolerance = ad::kDefaultTolerance;
                ein.stockToLeave = 0;
                ein.finishingPass = true;
                ein.helixDiameter = helixD_;
                ein.helixMinDiameter = std::min(helixMinD_, helixD_);
                EngineKey key{ein.regions, ein.stock, ein.cleared, entry.preDrills};
                std::size_t oi = done.size();
                for (std::size_t i = 0; i < done.size(); ++i) {
                    if (done[i].first == key) {
                        oi = i;
                        break;
                    }
                }
                const bool reused = oi < done.size();
                if (!reused) {
                    Progress part = ctx_.progress.sub(0.95 / static_cast<double>(std::max<std::size_t>(1, total)));
                    JobContext sub{ctx_.cancel, part};
                    auto planned = ad::planAdaptive(ein, entry, sub);
                    if (!planned) {
                        return tl::unexpected(engineError(planned.error(), a, z));
                    }
                    ++plan_.engineCalls;
                    Regions shrunk;
                    RAY_A2D_ASSIGN(shrunk, geom2d::offset(planned->cleared, -kAdaptiveCutAreaMarginNm, ctx_));
                    done.emplace_back(std::move(key), std::move(*planned));
                    doneCleared.push_back(std::move(shrunk));
                }
                const ad::Output* out = &done[oi].second;
                Adaptive2dDepth rep;
                rep.area = ai;
                rep.z = z;
                rep.reused = reused;
                rep.output = oi;
                rep.engine = out->stats;
                for (const ad::Region& r : out->regions) {
                    rep.entries.push_back(r.entry.kind);
                }
                plan_.depths.push_back(std::move(rep));
                collectEngagement(*out);
                collectWarnings(*out, z);
                const Nm lift = std::min<Nm>(kAdaptiveLinkLiftNm, (above - z) / 2);
                const std::size_t depthFirst = plan_.passes.size();
                for (std::size_t ri = 0; ri < out->regions.size(); ++ri) {
                    RAY_A2D_TRY(regionPasses(out->regions[ri], ri, ai, z, lift));
                }
                const bool leadsOk = std::none_of(out->warnings.begin(), out->warnings.end(), [](const ad::Warning& w) {
                    return w.code == ad::Warning::Code::LeadFailed;
                });
                cutAreas(depthFirst, doneCleared[oi], leadsOk);
            }
        }
        for (auto& [k, o] : done) {
            plan_.outputs.push_back(std::move(o));
        }
        for (const auto& e : engagement_) {
            plan_.engagedLengthMm += e.second;
        }
        plan_.p99EngagementDeg = percentileByLength(std::move(engagement_), 0.99);
        for (const auto& w : warningOrder_) {
            std::string zs;
            for (std::size_t i = 0; i < w.second.size(); ++i) {
                zs += (i == 0                     ? ""
                       : i + 1 == w.second.size() ? " and "
                                                  : ", ") +
                      std::string("Z") + mmText(w.second[i]);
            }
            plan_.warnings.push_back("At " + zs + ": " + w.first);
        }
        return {};
    }

    // The areas linking counts as cut after each pass of a depth (op.h PASSES, cut areas): the depth's last pass, the
    // engine's cleared area shrunk by kAdaptiveCutAreaMarginNm; a pass followed in its region by another (after a link
    // that retracts), the tool's disc where that one starts, which the engine starts in its cleared area (unless it
    // warned of a lead that did not reach cleared area at this depth); any other, nothing.
    void cutAreas(std::size_t first, const Regions& cleared, bool leadsOk)
    {
        for (std::size_t i = first; i < plan_.passes.size(); ++i) {
            link::Pass& pass = plan_.passes[i];
            if (i + 1 == plan_.passes.size()) {
                pass.cutArea = cleared;
            } else if (leadsOk && region_[i + 1] == region_[i]) {
                const link::Pt3& s = plan_.passes[i + 1].start;
                pass.cutArea = Regions{Region{Path::circle(Pt{s.x, s.y}, r_, true), {}}};
            } else {
                pass.cutArea = Regions{};
            }
        }
    }

    // The passes of one region at depth z (op.h PASSES).
    Status regionPasses(const ad::Region& r, std::size_t ri, std::size_t area, Nm z, Nm lift)
    {
        if (r.entry.kind == ad::EntryMode::None || r.paths.empty()) {
            return {};
        }
        const std::size_t firstPass = plan_.passes.size();
        std::optional<link::Pass> cur;
        const auto open = [&](Pt at) {
            cur.emplace();
            cur->start = link::Pt3{at.x, at.y, z};
            cur->cluster = static_cast<int>(area);
            cur->room = r.toolBounds;
        };
        const auto close = [&]() {
            if (cur && !cur->moves.empty()) {
                plan_.passes.push_back(std::move(*cur));
                region_.push_back(ri);
            }
            cur.reset();
        };
        for (const ad::Path& p : r.paths) {
            RAY_A2D_TRY(ctx_.cancel.check());
            if (p.points.empty()) {
                continue;
            }
            if (p.role == ad::Role::Ramp) {
                // The ramp's line: a pass of its own, entered by linking's ramp along it (op.h PASSES, entry).
                close();
                open(p.points.front());
                appendFitted(p.points, &p.reducedFeed, z, MoveRole::Cut, cur->moves);
                cur->entryOrder = {EntryKind::Ramp};
                close();
                continue;
            }
            if (p.motion == ad::Motion::LinkRetract) {
                close();
                continue;
            }
            const bool firstPath = &p == &r.paths.front();
            if (!cur) {
                open(p.points.front());
            }
            if (p.motion == ad::Motion::Cut) {
                appendFitted(p.points, &p.reducedFeed, z, MoveRole::Cut, cur->moves);
                continue;
            }
            // LinkClear: lifted when long enough and when the move from the hole centre of a pre-drilled entry is not
            // what it is (that one stays at depth, inside the hole).
            const bool fromHole = r.entry.kind == ad::EntryMode::PreDrill && firstPath;
            const Nm liftFrom = std::max<Nm>(kAdaptiveMinLiftedLinkNm, 2 * r_);
            if (lift > 0 && !fromHole && lengthOf(p.points) >= static_cast<double>(liftFrom)) {
                feedTo(p.points.front(), z + lift, MoveRole::Link, feedCut_, cur->moves);
                const std::size_t from = cur->moves.size();
                appendFitted(p.points, nullptr, z + lift, MoveRole::Link, cur->moves);
                for (std::size_t i = from; i < cur->moves.size(); ++i) {
                    cur->moves[i].feedMicronPerMin = feedCut_;
                }
                feedTo(p.points.back(), z, MoveRole::Link, feedCut_, cur->moves);
            } else {
                appendFitted(p.points, nullptr, z, MoveRole::Link, cur->moves);
            }
        }
        close();
        // The entry of the region's first pass (a ramp's own pass has its order already).
        if (firstPass < plan_.passes.size()) {
            link::Pass& pass = plan_.passes[firstPass];
            if (r.entry.kind == ad::EntryMode::Helix) {
                pass.helixCentre = r.entry.centre;
            } else if (r.entry.kind == ad::EntryMode::PreDrill) {
                pass.entryOrder = {EntryKind::PreDrill};
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
        o.centreCutting = false;
        o.feeds = feeds_;
        o.feeds.leadIn = o.feeds.leadOut = o.feeds.link = 0;
        o.maxStayDown = 4 * r_;
        o.stayDownLift = kAdaptiveLinkLiftNm;
        o.stayDownEntry = true;
        o.stepIn = false;
        o.rapidRetract = false;
        if (p_.entry == "ramp") {
            o.entryOrder = {EntryKind::Ramp, EntryKind::Helix, EntryKind::PreDrill};
        } else if (p_.entry == "preDrill") {
            o.entryOrder = {EntryKind::PreDrill};
        } else {
            o.entryOrder = {EntryKind::Helix, EntryKind::Ramp, EntryKind::PreDrill};
        }
        o.helixDiameter = helixD_;
        o.helixMinDiameter = std::min(helixMinD_, helixD_);
        o.rampAngleDeg = p_.rampAngle;
        o.minRampRun = 0;
        o.entryClearance = kAdaptiveEntryClearanceNm;
        o.preDrills = usable_;
        o.spindleClockwise = true;
        o.keepOrder = true;
        o.tipHeight = tip_;
        plan_.stock = link::stockOf(in_.geom);
    }

    void report()
    {
        plan_.areas.clear();
        for (const Area& a : areas_) {
            Adaptive2dArea out;
            out.selection = a.selection;
            out.open = a.open;
            out.floor = a.floor;
            out.bottom = a.bottom;
            out.top = a.top;
            out.depths = a.depths;
            out.footprint = a.footprint;
            out.region = a.region;
            plan_.areas.push_back(std::move(out));
        }
    }

    const GenerateInput& in_;
    JobContext& ctx_;
    Params p_;
    Adaptive2dPlan plan_;
    Nm r_ = 0, tip_ = 0, stepdown_ = 0, stockWall_ = 0, stockFloor_ = 0, helixD_ = 0, helixMinD_ = 0;
    Nm stockTop_ = 0, top_ = 0;
    std::int32_t feedCut_ = 0, feedReduced_ = 0;
    link::Feeds feeds_;
    std::vector<Pt> preDrills_;
    std::vector<link::PreDrill> usable_;
    Nm preDrillDiameter_ = 0;
    std::vector<std::string> preDrillWarnings_;
    Regions box_;
    std::vector<Area> areas_;
    std::map<std::pair<std::size_t, Nm>, Regions> slices_;
    std::map<Nm, Regions, std::greater<>> materialAt_;
    std::vector<std::pair<double, double>> engagement_;
    std::vector<std::size_t> region_; // per pass: the region's index in its depth's output
    std::map<std::pair<int, std::string>, std::size_t> warningIndex_;
    std::vector<std::pair<std::string, std::vector<Nm>>> warningOrder_;
};

} // namespace

double percentileByLength(std::vector<std::pair<double, double>> samples, double fraction)
{
    double total = 0;
    for (const auto& s : samples) {
        total += s.second;
    }
    if (!(total > 0)) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    double run = 0;
    for (const auto& s : samples) {
        run += s.second;
        if (run >= fraction * total) {
            return s.first;
        }
    }
    return samples.back().first;
}

Result<Adaptive2dPlan> planAdaptive2d(const GenerateInput& in, JobContext& ctx, Adaptive2dStage stage)
{
    try {
        return Planner(in, ctx).run(stage);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the 2D adaptive roughing");
    }
}

} // namespace ray::cam::ops
