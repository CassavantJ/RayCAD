// The engraving operation's plan (engrave.h, task T-056): parameters, checks, the paths (picked edges and text), the
// levels and the linked moves. Every rule is stated in engrave.h. Deterministic: integer nanometres, dmath, geom2d and
// link (both deterministic), the chains sorted by geom2d, the text in reading order.
#include "drill.h" // pointLengthNm, kHolderClearanceMm
#include "engrave.h"
#include "engrave_font.h"
#include "op_params.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <utility>

// Returns the error of a Status or Result expression.
#define RAY_TRY_STATUS(expr)                                                                                           \
    do {                                                                                                               \
        if (auto ray_engrave_status_ = (expr); !ray_engrave_status_) {                                                 \
            return tl::unexpected(std::move(ray_engrave_status_.error()));                                             \
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

bool pointed(const lib::Tool& t)
{
    return t.tipAngleDeg > 0 && t.tipAngleDeg < 180 &&
           (t.type == lib::ToolType::EngraveCutter || t.type == lib::ToolType::SpotDrill ||
            t.type == lib::ToolType::ChamferMill);
}

// How far above its tip the tool reaches its diameter (link.h Options::tipHeight): a pointed tool's point length, a
// ball end mill's radius, else 0.
Nm tipHeightNm(const lib::Tool& t)
{
    if (pointed(t)) {
        return pointLengthNm(t.diameter, t.tipAngleDeg, t.tipDiameter);
    }
    if (t.type == lib::ToolType::BallEndMill) {
        return toNm(t.diameter) / 2;
    }
    return 0;
}

// Round half to even to an integer, for feeds (the default rounding mode of IEEE arithmetic).
std::int64_t roundHalfEven(double v)
{
    return static_cast<std::int64_t>(std::nearbyint(v));
}

struct Params {
    double depth = 0, stepDown = 0;
    std::string text;
    double height = 0, spacing = 0, x = 0, y = 0, angle = 0;
    double lineSpacing = 0; // percent of the height, baseline to baseline
    TextAlign align = TextAlign::Left;
    bool fitArcs = true;
};

const OpSchema* engraveSchema()
{
    // Parsed once; immutable afterwards, so workers share it without a lock.
    static const std::shared_ptr<const OpSchema> schema = [] {
        auto loaded = OpSchema::load(embeddedOpSchema("engrave"));
        return loaded ? *loaded : std::shared_ptr<const OpSchema>();
    }();
    return schema.get();
}

Result<Params> readParams(std::string_view paramsJson)
{
    const OpSchema* schema = engraveSchema();
    if (schema == nullptr) {
        return failure(ErrorCode::Internal, "The engraving operation's parameter schema does not load");
    }
    auto p = schema->read(paramsJson);
    if (!p) {
        return tl::unexpected(p.error());
    }
    Params out;
    out.depth = p->number("depth");
    out.stepDown = p->number("stepDown");
    out.text = p->text("text");
    out.height = p->number("textHeight");
    out.spacing = p->number("textSpacing");
    out.x = p->number("textX");
    out.y = p->number("textY");
    out.angle = p->number("textAngle");
    out.lineSpacing = p->number("lineSpacing");
    const std::string& align = p->text("textAlign");
    out.align = align == "centre" ? TextAlign::Centre : align == "right" ? TextAlign::Right : TextAlign::Left;
    out.fitArcs = p->flag("fitArcs");
    return out;
}

class Planner {
public:
    Planner(const GenerateInput& in, JobContext& ctx) : in_(in), ctx_(ctx) {}

    Result<EngravePlan> run()
    {
        auto params = readParams(in_.op.paramsJson);
        if (!params) {
            return tl::unexpected(params.error());
        }
        p_ = *params;
        RAY_TRY_STATUS(checkHeights());
        RAY_TRY_STATUS(checkTool());
        RAY_TRY_STATUS(checkCutting());
        RAY_TRY_STATUS(edges());
        RAY_TRY_STATUS(text());
        if (plan_.paths.empty()) {
            return invalid("Nothing to engrave: pick edges or type a text");
        }
        levels();
        RAY_TRY_STATUS(link());
        return std::move(plan_);
    }

private:
    Status checkHeights()
    {
        const Heights& h = in_.op.heights;
        for (const auto& [v, word] : {std::pair{h.clearance, "clearance"}, std::pair{h.retract, "retract"},
                                      std::pair{h.feedPlane, "feed plane"}, std::pair{h.top, "top"}}) {
            if (!std::isfinite(v) || std::abs(v) > 1e6) {
                return invalid(std::string("The ") + word + " height is not a usable number");
            }
        }
        if (h.feedPlane < h.top) {
            return invalid("The feed plane (Z" + mmText(h.feedPlane) + ") must not be below the top height, the " +
                           "engraving plane (Z" + mmText(h.top) + ")");
        }
        if (h.retract < h.feedPlane) {
            return invalid("The retract height (Z" + mmText(h.retract) + ") must not be below the feed plane (Z" +
                           mmText(h.feedPlane) + ")");
        }
        if (h.clearance < h.retract) {
            return invalid("The clearance height (Z" + mmText(h.clearance) +
                           ") must not be below the retract height (Z" + mmText(h.retract) + ")");
        }
        plan_.plane = toNm(h.top);
        return {};
    }

    Status checkTool()
    {
        const lib::Tool& t = in_.tool;
        if (!(t.diameter > 0) || !std::isfinite(t.diameter)) {
            return invalid("The tool has no diameter");
        }
        using T = lib::ToolType;
        const bool centreCutting = t.type == T::EngraveCutter || t.type == T::SpotDrill || t.type == T::BallEndMill ||
                                   (t.type == T::ChamferMill && pointed(t) && !(t.tipDiameter > 0));
        if (!centreCutting) {
            return invalid(
                std::string("Engraving plunges straight into the material, so it needs a tool that cuts at ") +
                "its centre: an engraving cutter, a spot drill, a pointed chamfer mill or a ball end mill; " +
                "the tool is a " + toolWord(t.type) + (t.type == T::ChamferMill ? " with a flat tip" : std::string()));
        }
        const Nm depth = toNm(p_.depth);
        if (pointed(t)) {
            const Nm reach = pointLengthNm(t.diameter, t.tipAngleDeg, t.tipDiameter);
            if (depth > reach) {
                return invalid("The " + mmText(t.diameter) + " mm " + mmText(t.tipAngleDeg) + " degree " +
                               toolWord(t.type) + " reaches its full diameter " + mmText(reach) +
                               " mm above its tip; the engraving is " + mmText(depth) +
                               " mm deep: engrave less deep or use a larger tool");
            }
        }
        if (t.fluteLength > 0 && depth > toNm(t.fluteLength)) {
            return invalid("The engraving is " + mmText(depth) + " mm deep, but the tool's flutes are " +
                           mmText(t.fluteLength) + " mm long");
        }
        const double stickOut = in_.op.tool.stickOut;
        if (stickOut > 0 && toNm(stickOut) < depth + toNm(kHolderClearanceMm)) {
            return invalid("The tool sticks out " + mmText(stickOut) + " mm; the engraving is " + mmText(depth) +
                           " mm deep and the holder needs " + mmText(kHolderClearanceMm) + " mm above the plane");
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
                           std::string(toolWord(in_.tool.type)) + " does not cut that way; set it clockwise (M3)");
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
            return invalid("Set a cutting feed: it is the feed along the strokes");
        }
        feed_ = static_cast<std::int32_t>(roundHalfEven(feed));
        const double plunge = c.feedPlunge * 1000.0;
        if (c.feedPlunge > 0 && std::isfinite(plunge) && plunge <= kMaxFeed && roundHalfEven(plunge) >= 1) {
            plungeFeed_ = static_cast<std::int32_t>(roundHalfEven(plunge));
        }
        return {};
    }

    std::string selectedWords(std::size_t i) const { return "Selection " + std::to_string(i + 1); }

    Status edges()
    {
        const ResolvedGeometry& g = in_.geom;
        if (g.selectedShapeIndex.size() != g.selected.size()) {
            return failure(ErrorCode::Internal, "The selection and its part indices differ in length");
        }
        std::map<std::size_t, std::vector<kernel::SubId>> byPart;
        for (std::size_t i = 0; i < g.selected.size(); ++i) {
            const int part = g.selectedShapeIndex[i];
            if (part < 0 || static_cast<std::size_t>(part) >= g.partShapes.size()) {
                return failure(ErrorCode::Internal, "A selection lies on no part body");
            }
            if (g.selected[i].kind != kernel::SubKind::Edge) {
                return invalid(selectedWords(i) + " is not an edge: pick the edges to engrave");
            }
            byPart[static_cast<std::size_t>(part)].push_back(g.selected[i]);
        }
        for (const auto& [part, list] : byPart) {
            RAY_TRY_STATUS(ctx_.cancel.check());
            auto chains = geom2d::chainEdges(g.partShapes[part], list, g.wcsFromModel, {}, ctx_);
            if (!chains) {
                return tl::unexpected(chains.error());
            }
            for (const geom2d::Chain& c : *chains) {
                if (c.path.spans.empty()) {
                    continue;
                }
                if (std::max(std::abs(c.zMax - plan_.plane), std::abs(c.zMin - plan_.plane)) > 1000) {
                    plan_.warnings.push_back("The edges from X" + mmText(c.path.start.x) + " Y" +
                                             mmText(c.path.start.y) + " lie at Z" + mmText(c.zMin) +
                                             (c.zMax != c.zMin ? " to Z" + mmText(c.zMax) : std::string()) +
                                             "; they are engraved on the plane at Z" + mmText(plan_.plane));
                }
                plan_.paths.push_back(p_.fitArcs ? fitLineRuns(c.path) : c.path);
            }
        }
        if (!g.selected.empty() && plan_.paths.empty()) {
            return invalid("The picked edges all run along the setup's Z: there is nothing to engrave on the plane");
        }
        plan_.edgePaths = plan_.paths.size();
        return {};
    }

    Status text()
    {
        if (p_.text.empty()) {
            return {};
        }
        if (p_.text.size() > 800) {
            return invalid("The text is longer than 200 characters");
        }
        const StrokeFont* font = engravingFont();
        if (font == nullptr) {
            return failure(ErrorCode::Internal, "The engraving font does not load");
        }
        const auto codes = decodeUtf8(p_.text);
        if (codes && codes->size() > 200) {
            return invalid("The text is longer than 200 characters");
        }
        TextLayout layout;
        layout.text = p_.text;
        layout.height = p_.height;
        layout.spacing = p_.spacing;
        layout.align = p_.align;
        layout.x = p_.x;
        layout.y = p_.y;
        layout.angleDeg = p_.angle;
        layout.lineSpacing = p_.height * p_.lineSpacing / 100; // baseline to baseline, mm
        auto strokes = layoutText(*font, layout);
        if (!strokes) {
            return tl::unexpected(strokes.error());
        }
        for (geom2d::Path& s : *strokes) {
            plan_.paths.push_back(std::move(s));
        }
        const double width = engraveWidthMm(in_.tool, p_.depth);
        if (!strokes->empty() && width > p_.height / 4) {
            plan_.warnings.push_back("The tool cuts a groove " + mmText(width) +
                                     " mm wide at this depth, more than a "
                                     "quarter of the " +
                                     mmText(p_.height) + " mm text height: the strokes may run together");
        }
        return {};
    }

    void levels()
    {
        const Nm depth = toNm(p_.depth);
        const Nm step = toNm(p_.stepDown);
        std::int64_t n = 1;
        if (step > 0 && depth > step) {
            n = (depth + step - 1) / step;
        }
        for (std::int64_t k = 1; k <= n; ++k) {
            // depth * k / n rounded half to even, in integers (depth < 2^40, so no overflow).
            const std::int64_t num = depth * k;
            std::int64_t q = num / n;
            const std::int64_t r = num % n;
            if (2 * r > n || (2 * r == n && (q % 2) != 0)) {
                ++q;
            }
            plan_.levels.push_back(plan_.plane - (k == n ? depth : q));
        }
    }

    Status link()
    {
        const Heights& h = in_.op.heights;
        std::vector<link::Pass> passes;
        passes.reserve(plan_.paths.size() * plan_.levels.size());
        for (std::size_t i = 0; i < plan_.paths.size(); ++i) {
            const geom2d::Path& path = plan_.paths[i];
            for (std::size_t k = 0; k < plan_.levels.size(); ++k) {
                link::Pass pass = link::passFromPath(path, plan_.levels[k], feed_);
                pass.cluster = static_cast<int>(i);
                pass.level = static_cast<int>(k);
                pass.reversible = true;
                // A closed path starts where linking finds best (it keeps a start that is not a vertex at least
                // geom2d::kMinArcChord from the vertices of an arc it splits).
                const std::optional<geom2d::Path> flat = link::planarPath(pass);
                pass.rotatable = path.closed && flat && flat->closed;
                pass.entryOrder = {EntryKind::Plunge};
                passes.push_back(std::move(pass));
            }
        }
        link::Options o;
        o.clearance = toNm(h.clearance);
        o.retract = toNm(h.retract);
        o.feedPlane = toNm(h.feedPlane);
        // Rapids over the stock stay at or above its top: a feed plane below the stock's top is raised to it for
        // linking, and the tool feeds from there. Linking itself would be safe lower (the tool's end below), but only
        // over what an earlier pass cut, which nothing independent checks (engrave.h, PASSES AND LINKING, says why the
        // invariant checker's rapid floor is not claimed lower).
        const Nm stockTop = toNm(in_.geom.stockBounds.max.z);
        if (o.feedPlane < stockTop && stockTop <= o.retract) {
            plan_.warnings.push_back("The feed plane (Z" + mmText(o.feedPlane) + ") is below the top of the stock (Z" +
                                     mmText(stockTop) + "): the tool feeds down from the stock's top");
            o.feedPlane = stockTop;
        }
        o.toolRadius = std::max<Nm>(1, toNm(in_.tool.diameter) / 2);
        // The tool's end (link.h THE TOOL'S END): a cut leaves the full tool width free only this far above it, so no
        // feed descent before a plunge goes into an earlier groove below that height.
        o.tipHeight = tipHeightNm(in_.tool);
        o.centreCutting = true;
        o.feeds.plunge = plungeFeed_;
        o.entryOrder = {EntryKind::Plunge};
        auto linked = link::linkPasses(passes, o, link::stockOf(in_.geom), ctx_);
        if (!linked) {
            return tl::unexpected(linked.error());
        }
        plan_.linked = std::move(*linked);
        for (const std::string& w : plan_.linked.warnings) {
            plan_.warnings.push_back(w);
        }
        if (plan_.levels.back() > toNm(in_.geom.stockBounds.max.z)) {
            plan_.warnings.push_back("The engraving goes to Z" + mmText(plan_.levels.back()) +
                                     ", above the top of the stock (Z" + mmText(in_.geom.stockBounds.max.z) +
                                     "): it cuts nothing; check the top height");
        }
        return {};
    }

#undef RAY_TRY_STATUS

    const GenerateInput& in_;
    JobContext& ctx_;
    Params p_;
    EngravePlan plan_;
    std::int32_t feed_ = 0, plungeFeed_ = 0;
};

} // namespace

double engraveWidthMm(const lib::Tool& t, double depth)
{
    if (pointed(t)) {
        const double w = t.tipDiameter + 2 * depth * dmath::tan((t.tipAngleDeg * kRadPerDeg) / 2);
        return std::min(w, t.diameter);
    }
    if (t.type == lib::ToolType::BallEndMill && depth < t.diameter / 2) {
        return 2 * std::sqrt(depth * (t.diameter - depth));
    }
    return t.diameter;
}

geom2d::Path fitLineRuns(const geom2d::Path& path)
{
    using geom2d::SpanKind;
    const auto isLine = [](const geom2d::Span& s) {
        return s.kind == SpanKind::Line;
    };
    if (path.spans.size() < 3) {
        return path;
    }
    if (path.closed && std::all_of(path.spans.begin(), path.spans.end(), isLine)) {
        std::vector<geom2d::Pt> pts{path.start};
        for (std::size_t k = 0; k + 1 < path.spans.size(); ++k) {
            pts.push_back(path.spans[k].end);
        }
        return geom2d::fitArcs(pts, true);
    }
    geom2d::Path out;
    out.start = path.start;
    out.closed = path.closed;
    std::size_t k = 0;
    geom2d::Pt at = path.start;
    while (k < path.spans.size()) {
        std::size_t e = k;
        while (e < path.spans.size() && isLine(path.spans[e])) {
            ++e;
        }
        if (e - k >= 3) {
            std::vector<geom2d::Pt> pts{at};
            for (std::size_t j = k; j < e; ++j) {
                pts.push_back(path.spans[j].end);
            }
            const geom2d::Path fitted = geom2d::fitArcs(pts, false);
            out.spans.insert(out.spans.end(), fitted.spans.begin(), fitted.spans.end());
            at = path.spans[e - 1].end;
            k = e;
            continue;
        }
        if (e == k) {
            e = k + 1; // an arc: kept
        }
        for (std::size_t j = k; j < e; ++j) {
            out.spans.push_back(path.spans[j]);
        }
        at = path.spans[e - 1].end;
        k = e;
    }
    return out;
}

Result<EngravePlan> planEngrave(const GenerateInput& in, JobContext& ctx)
{
    try {
        return Planner(in, ctx).run();
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory while planning the engraving operation");
    }
}

} // namespace ray::cam::ops
