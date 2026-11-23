// The 2D contour operation's plan (contour2d.h, task T-053): parameters, checks, chains, sides, offsets, depths, tabs,
// rooms, linking and the conversion to the programmed path for compensation in the control. Every rule is stated in
// contour2d.h; this file follows them in its order. Deterministic: integer nanometres, geom2d's exact offsets, dmath
// for trigonometry, every container ordered, no clock.
#include "contour2d.h"
#include "op_params.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <utility>

namespace ray::cam::ops::contour2d {

namespace {

using geom2d::Coord;
using geom2d::Path;
using geom2d::Paths;
using geom2d::Pt;
using geom2d::Region;
using geom2d::Regions;
using geom2d::Side;
using geom2d::Span;
using geom2d::SpanKind;

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kAreaEps = 1e6; // nm^2: a boolean remainder below 1 um^2 is a rounding sliver (link.cpp's value)

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

// Returns the error of a Status or Result expression.
#define RAY_TRY_STATUS(expr)                                                                                           \
    do {                                                                                                               \
        if (auto ray_contour_status_ = (expr); !ray_contour_status_) {                                                 \
            return tl::unexpected(std::move(ray_contour_status_.error()));                                             \
        }                                                                                                              \
    } while (false)

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

// Round half to even to an integer, for feeds (the default rounding mode of IEEE arithmetic).
std::int64_t roundHalfEven(double v)
{
    return static_cast<std::int64_t>(std::nearbyint(v));
}

// ---------- Small geometry ----------

struct D2 {
    double x = 0, y = 0;
};
D2 d2(Pt p)
{
    return D2{static_cast<double>(p.x), static_cast<double>(p.y)};
}
D2 unit(D2 a)
{
    const double n = dmath::hypot(a.x, a.y);
    return n > 0 ? D2{a.x / n, a.y / n} : D2{0, 0};
}
D2 leftOf(D2 t)
{
    return D2{-t.y, t.x};
}
D2 rightOf(D2 t)
{
    return D2{t.y, -t.x};
}
double dist(Pt a, Pt b)
{
    return dmath::hypot(static_cast<double>(a.x - b.x), static_cast<double>(a.y - b.y));
}
Pt offsetPt(Pt p, D2 n, double d)
{
    return Pt{geom2d::roundNm(static_cast<double>(p.x) + n.x * d), geom2d::roundNm(static_cast<double>(p.y) + n.y * d)};
}
Pt endOf(const Path& p)
{
    return p.spans.empty() ? p.start : p.spans.back().end;
}
double spanLength(Pt start, const Span& s)
{
    if (s.kind == SpanKind::Line) {
        return dist(start, s.end);
    }
    return static_cast<double>(s.radius) * geom2d::sweepOf(start, s);
}
// The normal on side `side` of direction t.
D2 sideNormal(D2 t, Side side)
{
    return side == Side::Left ? leftOf(t) : rightOf(t);
}
D2 tangent(const Path& p, double s)
{
    const Vec2 t = geom2d::tangentAt(p, s);
    return D2{t.x, t.y};
}
// The arc length of the middle of the longest span of a path (ties: the first).
double longestSpanMiddle(const Path& p)
{
    double best = -1, bestAt = 0, s = 0;
    Pt cur = p.start;
    for (const Span& span : p.spans) {
        const double len = spanLength(cur, span);
        if (len > best) {
            best = len;
            bestAt = s + len / 2;
        }
        s += len;
        cur = span.end;
    }
    return bestAt;
}
// `p` oriented so its signed area has the sign of `like`'s (closed paths).
Path orientedLike(Path p, const Path& like)
{
    if ((geom2d::area(p) > 0) != (geom2d::area(like) > 0)) {
        p = geom2d::reverse(p);
    }
    return p;
}
Regions discRegion(Pt c, Coord r)
{
    return Regions{Region{Path::circle(c, r, true), {}}};
}

// ---------- Parameters ----------

enum class SideParam { Auto, Outside, Inside, Left, Right };
enum class FaceLoops { Outer, Inner, All };
enum class LeadType { Arc, Line, None };

struct Params {
    SideParam side = SideParam::Auto;
    geom2d::CutMode cut = geom2d::CutMode::Climb;
    Comp comp = Comp::Computer;
    bool followTangent = true;
    FaceLoops faceLoops = FaceLoops::Outer;
    double stock = 0, stockAxial = 0, maxStepdown = 0;
    int roughPasses = 1;
    double roughStepover = 1;
    bool finishPass = false;
    double finishAllowance = 0.2;
    bool springPass = false;
    LeadType lead = LeadType::Arc;
    double leadRadius = 0, leadSweep = 90, leadLength = 0, rampAngle = 3;
    bool tabs = false;
    TabShape tabShape = TabShape::Rectangular;
    int tabCount = 4;
    double tabWidth = 5, tabHeight = 1;
};

const OpSchema* contourSchema()
{
    // Parsed once; immutable afterwards, so workers share it without a lock.
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("contour2d"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = contourSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The 2D contour operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    static const std::map<std::string, SideParam, std::less<>> sides = {{"auto", SideParam::Auto},
                                                                        {"outside", SideParam::Outside},
                                                                        {"inside", SideParam::Inside},
                                                                        {"left", SideParam::Left},
                                                                        {"right", SideParam::Right}};
    out.side = sides.at(p->text("side")); // the schema's enums list exactly these
    out.cut = p->text("direction") == "conventional" ? geom2d::CutMode::Conventional : geom2d::CutMode::Climb;
    const std::string& comp = p->text("compensation");
    out.comp = comp == "control" ? Comp::Control : comp == "wear" ? Comp::Wear : Comp::Computer;
    out.followTangent = p->flag("followTangent");
    const std::string& loops = p->text("faceLoops");
    out.faceLoops = loops == "inner" ? FaceLoops::Inner : loops == "all" ? FaceLoops::All : FaceLoops::Outer;
    out.stock = p->number("stockToLeave");
    out.stockAxial = p->number("stockToLeaveAxial");
    out.maxStepdown = p->number("maxStepdown");
    out.roughPasses = static_cast<int>(p->number("roughPasses"));
    out.roughStepover = p->number("roughStepover");
    out.finishPass = p->flag("finishPass");
    out.finishAllowance = p->number("finishAllowance");
    out.springPass = p->flag("springPass");
    const std::string& lead = p->text("leadType");
    out.lead = lead == "line" ? LeadType::Line : lead == "none" ? LeadType::None : LeadType::Arc;
    out.leadRadius = p->number("leadRadius");
    out.leadSweep = p->number("leadSweep");
    out.leadLength = p->number("leadLength");
    out.rampAngle = p->number("rampAngle");
    out.tabs = p->flag("tabs");
    out.tabShape = p->text("tabShape") == "triangular" ? TabShape::Triangular : TabShape::Rectangular;
    out.tabCount = static_cast<int>(p->number("tabCount"));
    out.tabWidth = p->number("tabWidth");
    out.tabHeight = p->number("tabHeight");
    return out;
}

// ---------- The contours ----------

struct Contour {
    Path path;                  // closed: counter-clockwise as found; then oriented for the tool (orient())
    Coord zc = 0;               // the chain's height (bottom reference)
    Coord zTop = 0;             // its top (probe)
    std::string what;           // for messages
    Side toolSide = Side::Left; // of travel along `path`
    bool toolOutside = false;   // closed: the tool outside the loop
    Coord bottom = 0;
    std::vector<Nm> depths;
};

// The material at height z: the union of the slices of every part and fixture body (cached by z).
class Material {
public:
    Material(const ResolvedGeometry& g, JobContext& ctx) : g_(g), ctx_(ctx) {}

    Result<const Regions*> at(Coord z, bool fixtures)
    {
        auto& cache = fixtures ? withFixtures_ : partsOnly_;
        if (const auto it = cache.find(z); it != cache.end()) {
            return &it->second;
        }
        Regions all;
        const auto addShape = [&](const kernel::Shape& s) -> Status {
            if (s.isNull()) {
                return {};
            }
            auto r = geom2d::sliceRegions(s, g_.wcsFromModel, z, ctx_);
            if (!r) {
                return tl::unexpected(r.error());
            }
            if (r->empty()) {
                return {};
            }
            if (all.empty()) {
                all = std::move(*r);
                return {};
            }
            auto u = geom2d::unite(all, *r, ctx_);
            if (!u) {
                return tl::unexpected(u.error());
            }
            all = std::move(*u);
            return {};
        };
        for (const kernel::Shape& s : g_.partShapes) {
            RAY_TRY_STATUS(addShape(s));
        }
        if (fixtures) {
            for (const kernel::Shape& s : g_.fixtureShapes) {
                RAY_TRY_STATUS(addShape(s));
            }
        }
        return &cache.emplace(z, std::move(all)).first->second;
    }

private:
    const ResolvedGeometry& g_;
    JobContext& ctx_;
    std::map<Coord, Regions> partsOnly_, withFixtures_;
};

// The tool-centre (or programmed) paths of a contour offset by d (OFFSETS in contour2d.h), oriented like the contour.
Result<Paths> offsetContour(const Path& oriented, bool toolOutside, Side side, Coord d, JobContext& ctx, bool& enclosed)
{
    if (d == 0) {
        return Paths{oriented};
    }
    if (!oriented.closed) {
        return geom2d::offsetOpen(oriented, d, side, ctx);
    }
    Region r;
    r.outer = geom2d::area(oriented) > 0 ? oriented : geom2d::reverse(oriented);
    auto regions = geom2d::offset(Regions{geom2d::canonicalize(r)}, toolOutside ? d : -d, ctx);
    if (!regions) {
        return tl::unexpected(regions.error());
    }
    Paths out;
    for (const Region& g : *regions) {
        out.push_back(orientedLike(g.outer, oriented));
        if (toolOutside && !g.holes.empty()) {
            enclosed = true; // a cavity the tool cannot enter from outside
        }
    }
    return out;
}

// The span of `c` that is the offset of span `s` (starting at `a`) by d to `side`, if any (TOOL TOO LARGE).
bool hasCounterpart(Pt a, const Span& s, Side side, Coord d, const Path& c)
{
    if (s.kind == SpanKind::Line) {
        const D2 t = unit(D2{static_cast<double>(s.end.x - a.x), static_cast<double>(s.end.y - a.y)});
        const D2 n = sideNormal(t, side);
        const D2 o{static_cast<double>(a.x) + n.x * static_cast<double>(d),
                   static_cast<double>(a.y) + n.y * static_cast<double>(d)};
        const double len = spanLength(a, s);
        Pt cur = c.start;
        for (const Span& q : c.spans) {
            if (q.kind == SpanKind::Line) {
                const D2 u = unit(D2{static_cast<double>(q.end.x - cur.x), static_cast<double>(q.end.y - cur.y)});
                const auto off = [&](Pt p) {
                    return (static_cast<double>(p.x) - o.x) * n.x + (static_cast<double>(p.y) - o.y) * n.y;
                };
                const auto along = [&](Pt p) {
                    return (static_cast<double>(p.x) - o.x) * t.x + (static_cast<double>(p.y) - o.y) * t.y;
                };
                const double tol = static_cast<double>(kSameCurveTol);
                if (u.x * t.x + u.y * t.y > 0.999999 && std::abs(off(cur)) <= tol && std::abs(off(q.end)) <= tol &&
                    along(q.end) > -tol && along(cur) < len + tol) {
                    return true;
                }
            }
            cur = q.end;
        }
        return false;
    }
    // An arc: the tool on the left of a counter-clockwise arc is towards its centre.
    const bool towardsCentre = (s.kind == SpanKind::ArcCCW) == (side == Side::Left);
    const Coord r = towardsCentre ? s.radius - d : s.radius + d;
    if (r <= 0) {
        return false;
    }
    for (const Span& q : c.spans) {
        if (q.kind == s.kind && dist(q.centre, s.centre) <= static_cast<double>(kSameCurveTol) &&
            std::abs(static_cast<double>(q.radius - r)) <= static_cast<double>(kSameCurveTol)) {
            return true;
        }
    }
    return false;
}

// The first span of `p` (longer than kSpanCheckMin) without a counterpart in any of `cs`, as its middle point.
std::optional<Pt> missingSpan(const Path& p, Side side, Coord d, const Paths& cs)
{
    Pt cur = p.start;
    double s = 0;
    for (const Span& span : p.spans) {
        const double len = spanLength(cur, span);
        if (len > static_cast<double>(kSpanCheckMin)) {
            bool found = false;
            for (const Path& c : cs) {
                found = found || hasCounterpart(cur, span, side, d, c);
            }
            if (!found) {
                return geom2d::pointAt(p, s + len / 2);
            }
        }
        s += len;
        cur = span.end;
    }
    return std::nullopt;
}

// The planar path of moves from `from` (XY lines and arcs; Z ignored).
Path projection(Pt from, std::span<const Move> moves)
{
    Path p;
    p.start = from;
    Pt prev = from;
    for (const Move& m : moves) {
        const Pt e{m.x, m.y};
        if (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW) {
            p.spans.push_back(Span{m.kind == MoveKind::ArcCCW ? SpanKind::ArcCCW : SpanKind::ArcCW, e, Pt{m.i, m.j},
                                   geom2d::roundNm(dist(prev, Pt{m.i, m.j}))});
        } else if (e != prev) {
            p.spans.push_back(Span{SpanKind::Line, e, {}, 0});
        }
        prev = e;
    }
    return p;
}

// ---------- The plan ----------

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx), material_(in.geom, ctx) {}

    Result<Contour2dPlan> run()
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        plan_.comp = p_.comp;
        RAY_TRY_STATUS(checkTool());
        RAY_TRY_STATUS(checkCutting());
        RAY_TRY_STATUS(checkHeights());
        RAY_TRY_STATUS(gather());
        for (Contour& c : contours_) {
            RAY_TRY_STATUS(ctx_.cancel.check());
            RAY_TRY_STATUS(orient(c));
            RAY_TRY_STATUS(levels(c));
        }
        RAY_TRY_STATUS(checkTabs());
        for (std::size_t i = 0; i < contours_.size(); ++i) {
            RAY_TRY_STATUS(passes(i));
        }
        RAY_TRY_STATUS(link());
        return std::move(plan_);
    }

private:
    const lib::Tool& tool() const { return in_.tool; }
    bool comp() const { return p_.comp != Comp::Computer; }

    // ---------- Checks ----------

    Status checkTool()
    {
        const lib::Tool& t = tool();
        if (!(t.diameter > 0) || !std::isfinite(t.diameter) || t.diameter > 1000) {
            return invalid("The tool has no diameter");
        }
        using T = lib::ToolType;
        if (t.type != T::FlatEndMill && t.type != T::BullEndMill && t.type != T::BallEndMill && t.type != T::FaceMill) {
            return invalid(std::string("A contour is cut with an end mill (flat, bull nose or ball) or a face mill; "
                                       "the tool is a ") +
                           toolWord(t.type));
        }
        R_ = toNm(t.diameter) / 2;
        if (R_ < 1000) {
            return invalid("The tool is smaller than 0.002 mm");
        }
        centreCutting_ = t.type == T::BallEndMill;
        plan_.toolRadius = R_;
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
        const double cut = c.feedCut * 1000.0;
        if (!(c.feedCut > 0) || !std::isfinite(cut) || cut > kMaxFeed || roundHalfEven(cut) < 1) {
            return invalid("Set a cutting feed");
        }
        feed_ = static_cast<std::int32_t>(roundHalfEven(cut));
        const auto other = [&](double mmPerMin) {
            const double f = mmPerMin * 1000.0;
            return mmPerMin > 0 && std::isfinite(f) && f <= kMaxFeed && roundHalfEven(f) >= 1
                       ? static_cast<std::int32_t>(roundHalfEven(f))
                       : feed_;
        };
        plunge_ = other(c.feedPlunge);
        ramp_ = other(c.feedRamp);
        leadIn_ = other(c.feedLeadIn);
        leadOut_ = other(c.feedLeadOut);
        return {};
    }

    Status checkHeights()
    {
        const Heights& h = in_.op.heights;
        for (const auto& [v, word] :
             {std::pair{h.clearance, "clearance"}, std::pair{h.retract, "retract"},
              std::pair{h.feedPlane, "feed plane"}, std::pair{h.top, "top"}, std::pair{h.bottom, "bottom"}}) {
            if (!std::isfinite(v) || std::abs(v) > 1e6) {
                return invalid(std::string("The ") + word + " height is not a usable number");
            }
        }
        const Box3& b = in_.geom.stockBounds;
        if (!std::isfinite(b.max.z) || !(b.max.x > b.min.x) || !(b.max.y > b.min.y)) {
            return invalid("The setup has no stock");
        }
        stockTop_ = toNm(b.max.z);
        clearance_ = stockTop_ + toNm(h.clearance);
        retract_ = stockTop_ + toNm(h.retract);
        feedPlane_ = stockTop_ + toNm(h.feedPlane);
        top_ = stockTop_ + toNm(h.top);
        if (clearance_ < retract_) {
            return invalid("The clearance height (" + mmText(h.clearance) + ") must not be below the retract height (" +
                           mmText(h.retract) + ")");
        }
        if (retract_ < feedPlane_) {
            return invalid("The retract height (" + mmText(h.retract) + ") must not be below the feed plane (" +
                           mmText(h.feedPlane) + ")");
        }
        if (feedPlane_ < top_) {
            return invalid("The feed plane (" + mmText(h.feedPlane) + ") must not be below the top height (" +
                           mmText(h.top) + ")");
        }
        if (retract_ < stockTop_) {
            return invalid("The retract height (" + mmText(h.retract) +
                           " above the stock top) must not be below the top of the stock");
        }
        if (p_.roughPasses < 1 || p_.tabCount < 1) {
            return invalid("Roughing passes and tab count must be at least 1");
        }
        return {};
    }

    // ---------- Geometry ----------

    std::string selectionWords(std::size_t i) const { return "Selection " + std::to_string(i + 1); }

    Status gather()
    {
        const ResolvedGeometry& g = in_.geom;
        if (g.selected.empty()) {
            return invalid("Pick the edges of the contour");
        }
        if (g.selectedShapeIndex.size() != g.selected.size()) {
            return failure(ErrorCode::Internal, "The selection and its part indices differ in length");
        }
        std::map<std::size_t, std::vector<kernel::SubId>> edges; // per part
        std::vector<std::pair<std::size_t, std::size_t>> faces;  // (selection, part) in selection order
        for (std::size_t i = 0; i < g.selected.size(); ++i) {
            const int partIndex = g.selectedShapeIndex[i];
            if (partIndex < 0 || static_cast<std::size_t>(partIndex) >= g.partShapes.size()) {
                return failure(ErrorCode::Internal, "A selection lies on no part body");
            }
            const std::size_t part = static_cast<std::size_t>(partIndex);
            const kernel::SubId sub = g.selected[i];
            if (sub.kind == kernel::SubKind::Edge) {
                edges[part].push_back(sub);
            } else if (sub.kind == kernel::SubKind::Face) {
                faces.emplace_back(i, part);
            } else {
                return invalid(selectionWords(i) + " is not an edge or a face: pick the edges of the contour");
            }
        }
        for (const auto& [part, list] : edges) {
            geom2d::ChainOptions options;
            options.tangentExtend = p_.followTangent;
            auto chains = geom2d::chainEdges(g.partShapes[part], list, g.wcsFromModel, options, ctx_);
            if (!chains) {
                if (chains.error().code == ErrorCode::Cancelled) {
                    return tl::unexpected(chains.error());
                }
                return cannot("The picked edges do not make a contour: " + chains.error().message);
            }
            // Picked edges that branch: chainEdges stops at a point where more than two meet; an open end there.
            std::vector<Pt> ends;
            for (const geom2d::Chain& c : *chains) {
                if (!c.path.closed) {
                    ends.push_back(c.path.start);
                    ends.push_back(endOf(c.path));
                }
            }
            for (std::size_t a = 0; a < ends.size(); ++a) {
                for (std::size_t b = a + 1; b < ends.size(); ++b) {
                    if (dist(ends[a], ends[b]) <= static_cast<double>(geom2d::kJoinTolerance)) {
                        return cannot("The picked edges branch at " + at(ends[a]) +
                                      ": more than two of them meet there, so the contour's way is not clear; pick "
                                      "the edges of one way only");
                    }
                }
            }
            for (const geom2d::Chain& c : *chains) {
                if (c.path.spans.empty() || geom2d::length(c.path) < 1000) {
                    continue; // an edge along Z, or a point
                }
                Contour k;
                k.path = c.path;
                k.zc = c.zMin;
                k.zTop = c.zMax;
                k.what = std::string(c.path.closed ? "the closed chain" : "the open chain") + " starting at " +
                         at(c.path.start);
                contours_.push_back(std::move(k));
            }
        }
        for (const auto& [i, part] : faces) {
            auto fb = geom2d::faceBoundary(g.partShapes[part], g.selected[i], g.wcsFromModel);
            if (!fb) {
                return cannot(selectionWords(i) + " is not a planar face perpendicular to the setup's Z axis (" +
                              fb.error().message + ")");
            }
            const auto add = [&](Path loop, const char* word) {
                if (geom2d::area(loop) < 0) {
                    loop = geom2d::reverse(loop);
                }
                Contour k;
                k.path = geom2d::canonicalize(std::move(loop));
                k.zc = k.zTop = fb->z;
                k.what = std::string("the ") + word + " of " + selectionWords(i);
                contours_.push_back(std::move(k));
            };
            if (p_.faceLoops != FaceLoops::Inner) {
                add(fb->region.outer, "outer boundary");
            }
            if (p_.faceLoops != FaceLoops::Outer) {
                for (const Path& h : fb->region.holes) {
                    add(h, "inner loop");
                }
            }
        }
        if (contours_.empty()) {
            return cannot("The picked geometry gives no contour (edges along the setup's Z only?)");
        }
        return {};
    }

    // SIDE in contour2d.h, then the direction.
    Status orient(Contour& c)
    {
        switch (p_.side) {
        case SideParam::Outside:
        case SideParam::Inside:
            if (!c.path.closed) {
                return invalid(std::string("'") + (p_.side == SideParam::Outside ? "Outside" : "Inside") +
                               "' needs a closed chain; " + c.what + " is open: choose left or right");
            }
            c.toolSide = p_.side == SideParam::Inside ? Side::Left : Side::Right; // the path is counter-clockwise
            break;
        case SideParam::Left:
            c.toolSide = Side::Left;
            break;
        case SideParam::Right:
            c.toolSide = Side::Right;
            break;
        case SideParam::Auto: {
            auto side = probeSide(c);
            if (!side) {
                return tl::unexpected(side.error());
            }
            c.toolSide = *side;
            break;
        }
        }
        if (c.path.closed) {
            c.toolOutside = (c.toolSide == Side::Right) == (geom2d::area(c.path) > 0);
        }
        want_ = geom2d::toolSideFor(p_.cut, true);
        c.path = geom2d::orientForTool(c.path, c.toolSide, want_);
        c.toolSide = want_;
        return {};
    }

    Result<Side> probeSide(const Contour& c)
    {
        // The spans by length, longest first (ties: path order).
        std::vector<std::pair<double, double>> spans; // (-length, middle arc length)
        Pt cur = c.path.start;
        double s = 0;
        for (const Span& span : c.path.spans) {
            const double len = spanLength(cur, span);
            spans.emplace_back(-len, s + len / 2);
            s += len;
            cur = span.end;
        }
        std::stable_sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (spans.size() > 5) {
            spans.resize(5);
        }
        const Coord levels[2] = {c.zc, c.zTop - kProbeBelow};
        for (const Coord z : levels) {
            auto m = material_.at(z, false);
            if (!m) {
                return tl::unexpected(m.error());
            }
            for (const auto& [negLen, mid] : spans) {
                const Pt q = geom2d::pointAt(c.path, mid);
                const D2 n = leftOf(tangent(c.path, mid));
                const bool left =
                    geom2d::locate(offsetPt(q, n, static_cast<double>(kProbeOffset)), **m) == geom2d::Where::Inside;
                const bool right =
                    geom2d::locate(offsetPt(q, n, -static_cast<double>(kProbeOffset)), **m) == geom2d::Where::Inside;
                if (left != right) {
                    return left ? Side::Right : Side::Left;
                }
            }
        }
        return cannot("Cannot tell which side of " + c.what +
                      " the material is on (the part is on both sides of it, or on neither): set the tool side");
    }

    Status levels(Contour& c)
    {
        c.bottom = c.zc + toNm(in_.op.heights.bottom) + toNm(p_.stockAxial);
        if (!(top_ > c.bottom)) {
            return invalid("The top height (Z" + mmText(top_) + ") must be above the bottom of " + c.what + " (Z" +
                           mmText(c.bottom) + ")");
        }
        const Nm step = toNm(p_.maxStepdown);
        c.depths = depthLevels(top_, c.bottom, step);
        if (c.depths.empty()) {
            return invalid("From the top (Z" + mmText(top_) + ") to the bottom (Z" + mmText(c.bottom) + ") at most " +
                           mmText(step) + " mm a depth needs more than " + std::to_string(kMaxDepths) +
                           " depths: raise the maximum stepdown");
        }
        // Flutes and holder against the deepest cut.
        const lib::Tool& t = tool();
        Nm deepest = 0;
        Nm prev = top_;
        for (const Nm z : c.depths) {
            deepest = std::max(deepest, prev - z);
            prev = z;
        }
        if (p_.finishPass || p_.springPass) {
            deepest = top_ - c.bottom; // the finish and spring passes cut the whole wall
        }
        if (t.fluteLength > 0 && deepest > toNm(t.fluteLength) + 1000) {
            return invalid("At " + c.what + " a pass cuts " + mmText(deepest) + " mm deep, but the tool's flutes are " +
                           mmText(t.fluteLength) + " mm long: set a smaller maximum stepdown" +
                           (p_.finishPass || p_.springPass ? " (finish and spring passes cut the whole depth)" : ""));
        }
        const double stickOut = in_.op.tool.stickOut;
        const Nm reach = stockTop_ - c.bottom;
        if (stickOut > 0 && toNm(stickOut) < reach + toNm(kHolderClearanceMm)) {
            return invalid("At " + c.what + " the tool goes " + mmText(reach) +
                           " mm below the stock top, but it sticks out only " + mmText(stickOut) +
                           " mm: the holder needs " + mmText(kHolderClearanceMm) + " mm above the stock");
        }
        return {};
    }

    Status checkTabs()
    {
        if (!p_.tabs) {
            return {};
        }
        for (const Contour& c : contours_) {
            const Nm zt = c.bottom + toNm(p_.tabHeight);
            if (zt >= top_) {
                return invalid("The tabs (" + mmText(p_.tabHeight) + " mm above the bottom, Z" + mmText(zt) +
                               ") are not below the top (Z" + mmText(top_) + ") of " + c.what);
            }
        }
        if (centreCutting_) {
            return {};
        }
        const std::string tw = mmText(tool().diameter) + " mm " + toolWord(tool().type);
        if (p_.tabShape == TabShape::Rectangular) {
            if (comp()) {
                return invalid("Rectangular tabs with a " + tw +
                               ", which does not cut at its centre, ramp back down along the contour backwards, which "
                               "cutter compensation (G41/G42) cannot follow: use compensation in the computer, "
                               "triangular tabs, or a tool that cuts at its centre");
            }
            if (!(p_.rampAngle > 0)) {
                return invalid("Rectangular tabs with a " + tw +
                               ", which does not cut at its centre, need a ramp angle to come back down");
            }
        } else {
            const double flank = dmath::atan2(2 * p_.tabHeight, p_.tabWidth) * 180 / kPi;
            if (flank > p_.rampAngle + 1e-9) {
                return invalid("Triangular tabs " + mmText(p_.tabWidth) + " mm wide and " + mmText(p_.tabHeight) +
                               " mm high come down at " + mmText(flank) + " degrees, steeper than the ramp angle (" +
                               mmText(p_.rampAngle) + " degrees) the " + tw +
                               " may enter material at: widen or lower the tabs, or raise the ramp angle");
            }
        }
        return {};
    }

    // ---------- Passes ----------

    // The room the tool centre may use at depth z of contour c (ROOM in contour2d.h).
    Result<const Regions*> room(const Contour& c, std::size_t depthIndex)
    {
        const auto key = std::make_pair(&c, depthIndex);
        if (const auto it = rooms_.find(key); it != rooms_.end()) {
            return &it->second;
        }
        Regions m;
        for (std::size_t k = 0; k <= depthIndex; ++k) {
            auto s = material_.at(c.depths[k], true);
            if (!s) {
                return tl::unexpected(s.error());
            }
            if (m.empty()) {
                m = **s;
            } else if (!(*s)->empty()) {
                auto u = geom2d::unite(m, **s, ctx_);
                if (!u) {
                    return tl::unexpected(u.error());
                }
                m = std::move(*u);
            }
        }
        Regions free = link::unbounded();
        if (!m.empty()) {
            auto grown = geom2d::offset(m, R_ + toNm(p_.stock), ctx_);
            if (!grown) {
                return tl::unexpected(grown.error());
            }
            auto rest = geom2d::subtract(free, *grown, ctx_);
            if (!rest) {
                return tl::unexpected(rest.error());
            }
            free = std::move(*rest);
        }
        return &rooms_.emplace(key, std::move(free)).first->second;
    }

    // Where `path` (a tool-centre path) leaves `room` by more than kRoomTol, if it does.
    Result<std::optional<Pt>> outsideRoom(const Path& path, const Regions& room)
    {
        const auto key = &room;
        auto it = grownRooms_.find(key);
        if (it == grownRooms_.end()) {
            auto g = geom2d::offset(room, kRoomTol - 500, ctx_);
            if (!g) {
                return tl::unexpected(g.error());
            }
            it = grownRooms_.emplace(key, std::move(*g)).first;
        }
        auto probe = geom2d::sweptArea(std::span<const Path>(&path, 1), 500, ctx_);
        if (!probe) {
            return tl::unexpected(probe.error());
        }
        auto rest = geom2d::subtract(*probe, it->second, ctx_);
        if (!rest) {
            return tl::unexpected(rest.error());
        }
        if (geom2d::area(*rest) <= kAreaEps) {
            return std::optional<Pt>{};
        }
        // The middle of the largest piece outside.
        const Region* worst = nullptr;
        double worstArea = 0;
        for (const Region& r : *rest) {
            const double a = geom2d::area(r);
            if (worst == nullptr || a > worstArea) {
                worst = &r;
                worstArea = a;
            }
        }
        const geom2d::Bounds b = geom2d::bounds(*worst);
        return std::optional<Pt>{Pt{(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2}};
    }

    // The tab centres of contour c along `path` (TABS in contour2d.h), `d` the path's distance from the chain.
    std::vector<Tab> tabsOn(const Contour& c, const Path& path, Coord d) const
    {
        std::vector<Tab> out;
        for (const Pt& q : tabPoints_.at(&c)) {
            const double s = nearestS(path, q);
            if (dist(geom2d::pointAt(path, s), q) <= static_cast<double>(d) + 10'000.0) {
                out.push_back(Tab{s});
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const Tab& a, const Tab& b) { return a.centre < b.centre; });
        return out;
    }

    // One pass along `centre` (and `programmed` in the control) at depth k of contour c.
    Status addPass(const Contour& c, std::size_t contourIndex, std::size_t k, int radial, const Path& centre,
                   const Path& programmed, Coord extra)
    {
        const Nm z = c.depths[k];
        auto rm = room(c, k);
        if (!rm) {
            return tl::unexpected(rm.error());
        }
        auto out = outsideRoom(centre, **rm);
        if (!out) {
            return tl::unexpected(out.error());
        }
        if (*out) {
            return cannot("At " + c.what + " the " + mmText(tool().diameter) +
                          " mm tool would cut into the part near " + at(**out) + " at Z" + mmText(z) +
                          ": the contour comes too close to other walls there (or an open chain does not end clear "
                          "of the part); pick the whole contour, choose another side, or use a smaller tool");
        }
        const std::uint8_t compFlag = comp() ? (want_ == Side::Left ? 1 : 2) : 0;
        PlannedPass pp;
        pp.centre = centre;
        pp.chain = contourIndex;
        pp.depth = static_cast<int>(k) + 1;
        pp.radial = radial;
        pp.pass.start = link::Pt3{centre.start.x, centre.start.y, z};
        const Nm zt = c.bottom + toNm(p_.tabHeight);
        const bool tabbed = p_.tabs && z < zt;
        TabMoves tm;
        tm.shape = p_.tabShape;
        tm.zt = zt;
        tm.width = static_cast<double>(toNm(p_.tabWidth));
        tm.radius = static_cast<double>(R_);
        tm.centreCutting = centreCutting_;
        tm.rampAngleDeg = p_.rampAngle;
        if (tabbed) {
            auto moves = tabbedMoves(centre, z, tabsOn(c, centre, R_ + extra), tm, feed_, plunge_, ramp_, compFlag);
            if (!moves) {
                return cannot("At " + c.what + ", Z" + mmText(z) + ": " + moves.error().message);
            }
            pp.pass.moves = std::move(*moves);
        } else {
            geom2d::appendMoves(centre, z, MoveRole::Cut, feed_, pp.pass.moves);
            for (Move& m : pp.pass.moves) {
                m.compensation = compFlag;
            }
        }
        if (p_.comp == Comp::Control) {
            pp.programmed = programmed;
            if (tabbed) {
                auto moves =
                    tabbedMoves(programmed, z, tabsOn(c, programmed, extra), tm, feed_, plunge_, ramp_, compFlag);
                if (!moves) {
                    return cannot("At " + c.what + ", Z" + mmText(z) + ": " + moves.error().message);
                }
                pp.programmedMoves = std::move(*moves);
            } else {
                geom2d::appendMoves(programmed, z, MoveRole::Cut, feed_, pp.programmedMoves);
                for (Move& m : pp.programmedMoves) {
                    m.compensation = compFlag;
                }
            }
        }
        pp.pass.leadIn = pp.pass.leadOut = lead_;
        pp.pass.room = **rm;
        plan_.passes.push_back(std::move(pp));
        return {};
    }

    // The paths of one radial offset of contour c: centre paths and (control) the programmed ones, started.
    struct Offsets {
        Paths centre, programmed;
    };
    Result<Offsets> offsetsOf(const Contour& c, Coord extra, bool warn)
    {
        Offsets o;
        bool enclosed = false;
        if (p_.comp == Comp::Control) {
            auto ps = offsetContour(c.path, c.toolOutside, want_, extra, ctx_, enclosed);
            if (!ps) {
                return tl::unexpected(ps.error());
            }
            if (ps->empty()) {
                return cannot("At " + c.what + " the stock left (" + mmText(extra) + " mm) closes the contour");
            }
            for (Path pp : *ps) {
                if (pp.closed) {
                    pp = geom2d::startAt(pp, longestSpanMiddle(pp));
                }
                bool enc = false;
                auto cs = offsetContour(pp, c.toolOutside, want_, R_, ctx_, enc);
                if (!cs) {
                    return tl::unexpected(cs.error());
                }
                if (cs->empty()) {
                    return cannot("The " + mmText(tool().diameter) + " mm tool does not fit inside " + c.what);
                }
                if (cs->size() != 1 || enc) {
                    return cannot("At " + c.what + " the " + mmText(tool().diameter) +
                                  " mm tool cannot follow the whole contour (a part of it narrower than the tool), so "
                                  "with compensation in the control it would gouge: use compensation in the computer "
                                  "or a smaller tool");
                }
                if (const auto miss = missingSpan(pp, want_, R_, *cs)) {
                    return cannot("At " + c.what + " near " + at(*miss) + " the " + mmText(tool().diameter) +
                                  " mm tool cannot follow the contour (an inside corner or arc smaller than the tool, "
                                  "or a gap narrower than it): with compensation in the control it would gouge or "
                                  "stop with an alarm; use compensation in the computer or a smaller tool");
                }
                Path cc = cs->front();
                if (cc.closed) {
                    const Pt q = offsetPt(pp.start, sideNormal(tangent(pp, 0), want_), static_cast<double>(R_));
                    cc = geom2d::startAt(cc, nearestS(cc, q));
                }
                o.programmed.push_back(std::move(pp));
                o.centre.push_back(std::move(cc));
            }
            return o;
        }
        auto cs = offsetContour(c.path, c.toolOutside, want_, R_ + extra, ctx_, enclosed);
        if (!cs) {
            return tl::unexpected(cs.error());
        }
        if (cs->empty()) {
            return cannot("The " + mmText(tool().diameter) + " mm tool does not fit inside " + c.what +
                          (extra > 0 ? " with " + mmText(extra) + " mm of stock left" : std::string()));
        }
        if (warn) {
            if (const auto miss = missingSpan(c.path, want_, R_ + extra, *cs)) {
                plan_.warnings.push_back("At " + c.what + " near " + at(*miss) + " the " + mmText(tool().diameter) +
                                         " mm tool cannot reach the contour (an inside corner, arc or gap smaller "
                                         "than the tool): material is left there");
            } else if (enclosed || cs->size() > 1) {
                plan_.warnings.push_back("At " + c.what + " the " + mmText(tool().diameter) +
                                         " mm tool cannot reach the whole contour: material is left there");
            }
        }
        for (Path cc : *cs) {
            if (cc.closed) {
                cc = geom2d::startAt(cc, longestSpanMiddle(cc));
            }
            o.centre.push_back(std::move(cc));
        }
        return o;
    }

    Status passes(std::size_t index)
    {
        const Contour& c = contours_[index];
        const Coord stock = toNm(p_.stock);
        const Coord finish = p_.finishPass ? toNm(p_.finishAllowance) : 0;
        const Coord stepover = toNm(p_.roughStepover);
        // The innermost offset first: it places the tabs.
        auto finalOffsets = offsetsOf(c, stock, true);
        if (!finalOffsets) {
            return tl::unexpected(finalOffsets.error());
        }
        if (p_.tabs) {
            const Path& ref =
                p_.comp == Comp::Control ? finalOffsets->programmed.front() : finalOffsets->centre.front();
            const double length = geom2d::length(c.path);
            const double s0 = c.path.closed ? nearestS(c.path, ref.start) : 0.0;
            std::vector<Pt> points;
            for (int i = 0; i < p_.tabCount; ++i) {
                double s = s0 + (i + 0.5) * length / p_.tabCount;
                if (c.path.closed && s > length) {
                    s -= length;
                }
                points.push_back(geom2d::pointAt(c.path, s));
            }
            tabPoints_[&c] = std::move(points);
        }
        std::map<Coord, Offsets> byExtra;
        byExtra.emplace(stock, std::move(*finalOffsets));
        const auto offsets = [&](Coord extra) -> Result<const Offsets*> {
            if (const auto it = byExtra.find(extra); it != byExtra.end()) {
                return &it->second;
            }
            auto o = offsetsOf(c, extra, false);
            if (!o) {
                return tl::unexpected(o.error());
            }
            return &byExtra.emplace(extra, std::move(*o)).first->second;
        };
        const auto addAll = [&](std::size_t k, int radial, Coord extra) -> Status {
            auto o = offsets(extra);
            if (!o) {
                return tl::unexpected(o.error());
            }
            for (std::size_t i = 0; i < (*o)->centre.size(); ++i) {
                RAY_TRY_STATUS(ctx_.cancel.check());
                const Path empty;
                RAY_TRY_STATUS(addPass(c, index, k, radial, (*o)->centre[i],
                                       p_.comp == Comp::Control ? (*o)->programmed[i] : empty, extra));
            }
            return {};
        };
        for (std::size_t k = 0; k < c.depths.size(); ++k) {
            for (int j = p_.roughPasses - 1; j >= 0; --j) {
                RAY_TRY_STATUS(addAll(k, j, stock + finish + static_cast<Coord>(j) * stepover));
            }
        }
        const std::size_t last = c.depths.size() - 1;
        if (p_.finishPass) {
            RAY_TRY_STATUS(addAll(last, -1, stock));
        }
        if (p_.springPass) {
            RAY_TRY_STATUS(addAll(last, -2, stock));
        }
        return {};
    }

    // ---------- Linking ----------

    Status link()
    {
        // Leads (LEADS in contour2d.h).
        if (p_.lead == LeadType::Arc) {
            lead_.kind = link::LeadKind::Arc;
            lead_.radius = p_.leadRadius > 0 ? toNm(p_.leadRadius) : 2 * R_;
            lead_.sweepDeg = p_.leadSweep;
            lead_.length = toNm(p_.leadLength);
            lead_.minRadius = std::max<Coord>(1000, lead_.radius / 4);
        } else if (p_.lead == LeadType::Line) {
            lead_.kind = link::LeadKind::Line;
            lead_.length = p_.leadLength > 0 ? toNm(p_.leadLength) : 2 * R_;
            lead_.minRadius = std::max<Coord>(1000, lead_.length / 4);
        }
        lead_.side = want_;
        if (comp() && (p_.lead == LeadType::None || (p_.lead == LeadType::Arc && lead_.length <= 0))) {
            return invalid(std::string("Cutter compensation ") +
                           (p_.comp == Comp::Control ? "in the control" : "for wear") +
                           " is switched on and off on a straight move: use line leads, or give the arc leads a "
                           "straight length");
        }
        for (PlannedPass& pp : plan_.passes) {
            pp.pass.leadIn = pp.pass.leadOut = lead_;
        }
        link::Options& o = plan_.link;
        o.clearance = clearance_;
        o.retract = retract_;
        o.feedPlane = feedPlane_;
        o.toolRadius = R_;
        o.centreCutting = centreCutting_;
        o.feeds.plunge = plunge_;
        o.feeds.ramp = ramp_;
        o.feeds.leadIn = leadIn_;
        o.feeds.leadOut = leadOut_;
        o.feeds.link = feed_;
        o.entryOrder = {EntryKind::Ramp, EntryKind::Plunge};
        o.rampAngleDeg = p_.rampAngle;
        o.entryClearance = kEntryClearance;
        o.spindleClockwise = true;
        o.keepOrder = true;
        plan_.stock = link::stockOf(in_.geom);
        std::vector<link::Pass> passes;
        passes.reserve(plan_.passes.size());
        for (const PlannedPass& pp : plan_.passes) {
            passes.push_back(pp.pass);
        }
        auto linked = link::linkPasses(passes, o, plan_.stock, ctx_);
        if (!linked) {
            return tl::unexpected(linked.error());
        }
        plan_.linked = std::move(*linked);
        plan_.warnings.insert(plan_.warnings.end(), plan_.linked.warnings.begin(), plan_.linked.warnings.end());
        plan_.moves = plan_.linked.moves;
        plan_.startX = plan_.linked.start.x;
        plan_.startY = plan_.linked.start.y;
        plan_.startZ = plan_.linked.start.z;
        if (p_.comp == Comp::Control) {
            RAY_TRY_STATUS(programmed());
        }
        return {};
    }

    // COMPENSATION in contour2d.h: the linked toolpath (on C) as the control's programmed path.
    // The lead moves of src[from, to), starting at `a` (C), with every point moved by -R N(t); the first start and the
    // last end may be given (P's start and end, so the joints are exact).
    void programmedLead(const std::vector<Move>& src, std::size_t from, std::size_t to, Pt a,
                        std::optional<Pt> firstStart, std::optional<Pt> lastEnd, std::vector<Move>& out,
                        Pt& startOut) const
    {
        const double R = static_cast<double>(R_);
        Pt prev = a;
        for (std::size_t q = from; q < to; ++q) {
            Move m = src[q];
            const Pt b{m.x, m.y};
            D2 t0, t1;
            if (m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW) {
                const Pt c{m.i, m.j};
                const bool ccw = m.kind == MoveKind::ArcCCW;
                const D2 r0 = unit(D2{static_cast<double>(prev.x - c.x), static_cast<double>(prev.y - c.y)});
                const D2 r1 = unit(D2{static_cast<double>(b.x - c.x), static_cast<double>(b.y - c.y)});
                t0 = ccw ? leftOf(r0) : rightOf(r0);
                t1 = ccw ? leftOf(r1) : rightOf(r1);
            } else {
                t0 = t1 = unit(D2{static_cast<double>(b.x - prev.x), static_cast<double>(b.y - prev.y)});
            }
            if (q == from) {
                startOut = firstStart ? *firstStart : offsetPt(prev, sideNormal(t0, want_), -R);
            }
            const Pt e = q + 1 == to && lastEnd ? *lastEnd : offsetPt(b, sideNormal(t1, want_), -R);
            m.x = e.x;
            m.y = e.y;
            out.push_back(m);
            prev = b;
        }
    }

    Status programmed()
    {
        const std::vector<Move>& src = plan_.linked.moves;
        std::vector<Move> out;
        out.reserve(src.size());
        const std::size_t n = plan_.passes.size();
        std::vector<Pt> starts(n), ends(n);
        std::size_t next = 0; // the move of src to copy next
        std::optional<Pt> prevE, prevECentre;
        for (std::size_t k = 0; k < n; ++k) {
            const PlannedPass& pp = plan_.passes[k];
            const std::size_t i0 = plan_.linked.passMove[k];
            std::size_t i = i0;
            while (i < src.size() && src[i].role == MoveRole::LeadIn) {
                ++i;
            }
            const std::size_t passEnd = i + pp.pass.moves.size();
            std::size_t j = passEnd;
            while (j < src.size() && src[j].role == MoveRole::LeadOut) {
                ++j;
            }
            if (i0 == 0 || i0 < next || passEnd > src.size()) {
                return failure(ErrorCode::Internal, "The linked contour does not have the moves of its passes");
            }
            const Pt a{src[i0 - 1].x, src[i0 - 1].y}; // where linking starts the lead-in (C)
            std::vector<Move> leadIn, leadOut;
            Pt s = pp.programmed.start;
            if (i > i0) {
                programmedLead(src, i0, i, a, std::nullopt, pp.programmed.start, leadIn, s);
            }
            Pt e = endOf(pp.programmed);
            if (j > passEnd) {
                Pt unused;
                programmedLead(src, passEnd, j, Pt{src[passEnd - 1].x, src[passEnd - 1].y}, endOf(pp.programmed),
                               std::nullopt, leadOut, unused);
                e = Pt{leadOut.back().x, leadOut.back().y};
            }
            // The moves between the previous block and this one: the retract at the previous lead-out's end goes to its
            // programmed end, the link, approach and entry at this lead-in's start to the programmed start.
            for (; next < i0; ++next) {
                Move m = src[next];
                const Pt here{m.x, m.y};
                if (prevE && here == *prevECentre && m.role == MoveRole::Retract) {
                    m.x = prevE->x;
                    m.y = prevE->y;
                } else if (here == a) {
                    m.x = s.x;
                    m.y = s.y;
                }
                out.push_back(m);
            }
            out.insert(out.end(), leadIn.begin(), leadIn.end());
            out.insert(out.end(), pp.programmedMoves.begin(), pp.programmedMoves.end());
            out.insert(out.end(), leadOut.begin(), leadOut.end());
            prevE = e;
            prevECentre = Pt{src[j - 1].x, src[j - 1].y};
            next = j;
            starts[k] = s;
            ends[k] = e;
        }
        for (; next < src.size(); ++next) {
            Move m = src[next];
            if (prevE && Pt{m.x, m.y} == *prevECentre) {
                m.x = prevE->x;
                m.y = prevE->y;
            }
            out.push_back(m);
        }
        plan_.moves = std::move(out);
        plan_.startX = starts.front().x;
        plan_.startY = starts.front().y;
        // Where compensation is off the tool centre is the programmed point: it must be clear of the part.
        for (std::size_t k = 0; k < n; ++k) {
            for (const Pt& q : {starts[k], ends[k]}) {
                if (geom2d::locate(q, *plan_.passes[k].pass.room) == geom2d::Where::Outside) {
                    return cannot("With compensation in the control the tool starts and ends its leads at the "
                                  "programmed point, and at " +
                                  at(q) + " (Z" + mmText(plan_.passes[k].pass.start.z) +
                                  ") it would touch the part: lengthen the leads' straight part");
                }
            }
            RAY_TRY_STATUS(entryClear(k, starts[k]));
        }
        return {};
    }

    // In the control the descent happens at the programmed start, beside where linking planned it: the tool's disc
    // there must lie outside the stock or in area cut already at that depth or deeper.
    Status entryClear(std::size_t k, Pt s)
    {
        const PlannedPass& pp = plan_.passes[k];
        const Nm z = pp.pass.start.z;
        if (plan_.linked.entered[k] != link::Entered::Free || z >= plan_.stock.top || plan_.stock.region.empty()) {
            return {};
        }
        Regions disc = discRegion(s, std::max<Coord>(R_ - link::kFreeTolerance, R_ / 2));
        auto rest = geom2d::subtract(disc, plan_.stock.region, ctx_);
        if (!rest) {
            return tl::unexpected(rest.error());
        }
        if (geom2d::area(*rest) + kAreaEps >= geom2d::area(disc)) {
            return {}; // wholly outside the stock
        }
        auto inStock = geom2d::intersect(disc, plan_.stock.region, ctx_);
        if (!inStock) {
            return tl::unexpected(inStock.error());
        }
        std::vector<Path> cut;
        for (std::size_t q = 0; q < k; ++q) {
            if (plan_.passes[q].pass.start.z > z) {
                continue;
            }
            const std::size_t i0 = plan_.linked.passMove[q];
            std::size_t e = i0;
            while (e < plan_.linked.moves.size() &&
                   (plan_.linked.moves[e].role == MoveRole::LeadIn || plan_.linked.moves[e].role == MoveRole::LeadOut ||
                    plan_.linked.moves[e].role == MoveRole::Cut || plan_.linked.moves[e].role == MoveRole::Ramp ||
                    plan_.linked.moves[e].role == MoveRole::Plunge)) {
                ++e;
            }
            const Move& before = plan_.linked.moves[i0 - 1];
            cut.push_back(
                projection(Pt{before.x, before.y}, std::span<const Move>(plan_.linked.moves.data() + i0, e - i0)));
        }
        if (!cut.empty()) {
            auto swept = geom2d::sweptArea(cut, R_, ctx_);
            if (!swept) {
                return tl::unexpected(swept.error());
            }
            auto left = geom2d::subtract(*inStock, *swept, ctx_);
            if (!left) {
                return tl::unexpected(left.error());
            }
            if (geom2d::area(*left) <= kAreaEps) {
                return {};
            }
        }
        return cannot("With compensation in the control the tool goes down at the programmed start of its lead-in, " +
                      at(s) + " (Z" + mmText(z) +
                      "), and there it would plunge into uncut stock: lengthen the leads or start the contour outside "
                      "the stock");
    }

    const GenerateInput& in_;
    JobContext& ctx_;
    Material material_;
    Params p_;
    Contour2dPlan plan_;
    std::vector<Contour> contours_;
    std::map<const Contour*, std::vector<Pt>> tabPoints_;
    std::map<std::pair<const Contour*, std::size_t>, Regions> rooms_;
    std::map<const Regions*, Regions> grownRooms_;
    Coord R_ = 0;
    bool centreCutting_ = false;
    Side want_ = Side::Left;
    link::Lead lead_;
    std::int32_t feed_ = 0, plunge_ = 0, ramp_ = 0, leadIn_ = 0, leadOut_ = 0;
    Nm stockTop_ = 0, clearance_ = 0, retract_ = 0, feedPlane_ = 0, top_ = 0;
};

#undef RAY_TRY_STATUS

} // namespace

std::vector<Nm> depthLevels(Nm top, Nm bottom, Nm maxStepdown)
{
    std::vector<Nm> out;
    if (top <= bottom) {
        return out;
    }
    const Nm d = top - bottom;
    std::int64_t n = 1;
    if (maxStepdown > 0) {
        n = d / maxStepdown + (d % maxStepdown != 0 ? 1 : 0);
    }
    if (n > kMaxDepths) {
        return out;
    }
    for (std::int64_t k = 1; k <= n; ++k) {
        // round(d k / n), half to even, in integers (d k < 2^63: d below 1 km, k at most kMaxDepths).
        const std::int64_t a = d * k;
        std::int64_t q = a / n;
        const std::int64_t r = a % n;
        if (2 * r > n || (2 * r == n && (q % 2) != 0)) {
            ++q;
        }
        out.push_back(top - q);
    }
    return out;
}

double nearestS(const Path& p, Pt q)
{
    double best = std::numeric_limits<double>::infinity(), bestS = 0, s = 0;
    Pt cur = p.start;
    const D2 qd = d2(q);
    for (const Span& span : p.spans) {
        const double len = spanLength(cur, span);
        double ds = 0, dd = 0;
        if (span.kind == SpanKind::Line) {
            const D2 a = d2(cur), b = d2(span.end);
            const D2 ab{b.x - a.x, b.y - a.y};
            const double l2 = ab.x * ab.x + ab.y * ab.y;
            double t = l2 > 0 ? ((qd.x - a.x) * ab.x + (qd.y - a.y) * ab.y) / l2 : 0;
            t = std::clamp(t, 0.0, 1.0);
            ds = t * len;
            dd = dmath::hypot(a.x + ab.x * t - qd.x, a.y + ab.y * t - qd.y);
        } else {
            const D2 c = d2(span.centre);
            const double r = static_cast<double>(span.radius);
            const bool ccw = span.kind == SpanKind::ArcCCW;
            const double a0 = dmath::atan2(static_cast<double>(cur.y) - c.y, static_cast<double>(cur.x) - c.x);
            const double aq = dmath::atan2(qd.y - c.y, qd.x - c.x);
            double phi = ccw ? aq - a0 : a0 - aq;
            while (phi < 0) {
                phi += 2 * kPi;
            }
            while (phi >= 2 * kPi) {
                phi -= 2 * kPi;
            }
            const double sweep = geom2d::sweepOf(cur, span);
            if (phi <= sweep) {
                ds = r * phi;
                dd = std::abs(dmath::hypot(qd.x - c.x, qd.y - c.y) - r);
            } else {
                const double d0 = dist(cur, q), d1 = dist(span.end, q);
                ds = d0 <= d1 ? 0 : len;
                dd = std::min(d0, d1);
            }
        }
        if (dd < best) {
            best = dd;
            bestS = s + ds;
        }
        s += len;
        cur = span.end;
    }
    return bestS;
}

Result<std::vector<Move>> tabbedMoves(const Path& path, Nm z, const std::vector<Tab>& tabs, const TabMoves& t,
                                      std::int32_t feed, std::int32_t plungeFeed, std::int32_t rampFeed,
                                      std::uint8_t comp)
{
    std::vector<Move> out;
    const double L = geom2d::length(path);
    const double half = t.width / 2;
    const double R = t.radius;
    const double lr =
        t.shape == TabShape::Rectangular && !t.centreCutting
            ? static_cast<double>(t.zt - z) / dmath::tan(t.rampAngleDeg * kPi / 180.0) * (1.0 + kRampSlack)
            : 0.0;
    // Each tab's extent along the path, checked.
    double prevEnd = 0;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const double a = tabs[i].centre - half - R;
        const double e = tabs[i].centre + half + R + lr;
        if (a < prevEnd || e > L) {
            return tl::unexpected(Error{ErrorCode::ToolpathFailed,
                                        "the " + std::to_string(tabs.size()) + " tabs do not fit along the " +
                                            mmText(geom2d::roundNm(L)) + " mm pass: each takes " +
                                            mmText(geom2d::roundNm(e - a)) +
                                            " mm with the tool's diameter (and its ramp back down); use fewer or "
                                            "narrower tabs",
                                        {}});
        }
        prevEnd = e;
    }
    Pt cur = path.start;
    const auto flat = [&](double s0, double s1, Nm zz, MoveRole role, std::int32_t f) {
        if (s1 <= s0) {
            return;
        }
        Path sp = geom2d::subPath(path, s0, s1);
        sp.start = cur;
        const std::size_t first = out.size();
        geom2d::appendMoves(sp, zz, role, f, out);
        if (out.size() > first) {
            cur = Pt{out.back().x, out.back().y};
        }
    };
    const auto sloped = [&](double s0, double s1, Nm z0, Nm z1, MoveRole role, std::int32_t f, bool backwards) {
        Path sp = geom2d::subPath(path, s0, s1);
        if (backwards) {
            sp = geom2d::reverse(sp);
        }
        sp.start = cur;
        const std::size_t first = out.size();
        link::appendSloped(sp, z0, z1, role, f, out);
        if (out.size() > first) {
            cur = Pt{out.back().x, out.back().y};
        }
    };
    const auto vertical = [&](Nm zz) {
        Move m{};
        m.kind = MoveKind::Linear;
        m.role = MoveRole::Cut;
        m.plane = ArcPlane::XY;
        m.feedMicronPerMin = plungeFeed;
        m.x = cur.x;
        m.y = cur.y;
        m.z = zz;
        out.push_back(m);
    };
    double s = 0;
    for (const Tab& tab : tabs) {
        const double a = tab.centre - half - R;
        const double b = tab.centre + half + R;
        flat(s, a, z, MoveRole::Cut, feed);
        if (t.shape == TabShape::Rectangular) {
            vertical(t.zt);
            flat(a, b, t.zt, MoveRole::Cut, feed);
            if (t.centreCutting) {
                vertical(z);
                s = b;
            } else {
                flat(b, b + lr, t.zt, MoveRole::Cut, feed);
                sloped(b, b + lr, t.zt, z, MoveRole::Ramp, rampFeed, true);
                flat(b, b + lr, z, MoveRole::Cut, feed);
                s = b + lr;
            }
        } else {
            sloped(a, tab.centre - R, z, t.zt, MoveRole::Cut, feed, false);
            flat(tab.centre - R, tab.centre + R, t.zt, MoveRole::Cut, feed);
            sloped(tab.centre + R, b, t.zt, z, MoveRole::Cut, feed, false);
            s = b;
        }
    }
    // The rest of the pass; a closed path ends exactly at its start.
    flat(s, L, z, MoveRole::Cut, feed);
    if (!out.empty() && path.closed) {
        out.back().x = path.start.x;
        out.back().y = path.start.y;
    }
    for (Move& m : out) {
        m.compensation = comp;
    }
    return out;
}

Result<Contour2dPlan> planContour2d(const GenerateInput& in, JobContext& ctx)
{
    try {
        return Planner(in, ctx).run();
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the 2D contour");
    }
}

} // namespace ray::cam::ops::contour2d
