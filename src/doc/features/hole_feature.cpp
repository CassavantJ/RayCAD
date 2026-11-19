// The "hole" feature type (hole_feature.h).
#include "hole_feature.h"

#include "extrude_feature.h"
#include "sketch_feature.h"
#include "sketch_json.h"

#include <ray/naming.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <new>
#include <set>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace ray::doc::features {

using json::Value;

namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kDegree = kPi / 180.0;
constexpr double kInch = 25.4;
constexpr double kThroughMargin = 1.0; // mm past the far side of the body's bounding box

const std::string& schema()
{
    static const std::string text = objectSchema(
        R"("sketch","target","kind","end")",
        R"("sketch":)" + uuidSchema() + R"(,"points":{"type":"array","minItems":1,"items":)" + uuidSchema() +
            R"(,"description":"Point entities of the sketch to drill at; all free-standing points when omitted"},)"
            R"("target":)" +
            uuidSchema() +
            R"(,"flip":{"type":"boolean","default":false,"description":"Drill along the sketch normal instead of )"
            R"(into the face the sketch is on"},"kind":{"enum":["simple","tapped","counterbore","countersink",)"
            R"("counterbore_tapped","countersink_tapped"]},"diameter":)" +
            lengthSchema("Drilled diameter (not for a tapped hole, which is drilled at thread.tapDrill)") +
            R"(,"end":{"enum":["blind","through"]},"depth":)" +
            lengthSchema("Blind hole: depth of the full-diameter bore from the sketch plane") +
            R"(,"bottom":{"enum":["angle","flat"],"default":"angle","description":"Blind hole: drill point or )"
            R"(flat bottom"},"tipAngle":)" +
            angleSchema("Included angle of the drill point; default 118 deg") +
            R"(,"counterbore":{"type":"object","additionalProperties":false,"required":["diameter","depth"],)"
            R"("properties":{"diameter":)" +
            lengthSchema("Counterbore diameter") + R"(,"depth":)" + lengthSchema("Counterbore depth") +
            R"(}},"countersink":{"type":"object","additionalProperties":false,"required":["diameter"],)"
            R"("properties":{"diameter":)" +
            lengthSchema("Countersink diameter at the sketch plane") + R"(,"angle":)" +
            angleSchema("Included angle of the countersink; default 90 deg") +
            R"(}},"thread":{"type":"object","additionalProperties":false,)"
            R"("required":["designation","standard","nominalDiameter","tapDrill"],"properties":{)"
            R"("designation":{"type":"string","minLength":1,"description":"e.g. M6x1 or 1/4-20 UNC"},)"
            R"("standard":{"enum":["iso_metric_coarse","iso_metric_fine","unc","unf","custom"]},)"
            R"("nominalDiameter":)" +
            lengthSchema("Nominal (major) diameter") + R"(,"pitch":)" + lengthSchema("Thread pitch") +
            R"j(,"tpi":{"type":"number","exclusiveMinimum":0,"description":"Threads per inch (instead of pitch)"},)j"
            R"("class":{"type":"string","minLength":1,"description":"Tolerance class, e.g. 6H or 2B"},"depth":)" +
            lengthSchema("Thread depth; the whole tapped length when omitted") + R"(,"tapDrill":)" +
            lengthSchema("Tap drill diameter; the hole is drilled at this diameter") +
            R"j(},"description":"The thread of a tapped hole (standards.threads.list gives the standard sizes)"})j");
    return text;
}

bool tappedKind(const std::string& kind)
{
    return kind == "tapped" || kind == "counterbore_tapped" || kind == "countersink_tapped";
}
bool counterboreKind(const std::string& kind)
{
    return kind == "counterbore" || kind == "counterbore_tapped";
}
bool countersinkKind(const std::string& kind)
{
    return kind == "countersink" || kind == "countersink_tapped";
}

struct ThreadProps {
    std::string designation;
    std::string standard;
    std::optional<std::string> threadClass;
    Value nominal;
    std::optional<Value> pitch;
    std::optional<double> tpi;
    std::optional<Value> depth;
    Value tapDrill;
};

struct HoleProps {
    FeatureId sketch;
    std::optional<std::vector<SketchEntId>> points;
    BodyId target;
    bool flip = false;
    std::string kind;
    std::optional<Value> diameter;
    bool through = false;
    std::optional<Value> depth;
    bool flatBottom = false;
    Value tipAngle;
    std::optional<Value> cboreDiameter, cboreDepth;
    std::optional<Value> csinkDiameter;
    Value csinkAngle;
    std::optional<ThreadProps> thread;
};

Result<ThreadProps> parseThread(const PropsReader& root, const Value& v, bool strict)
{
    if (!v.isObject()) {
        return root.error("thread", "must be an object with designation, standard, nominalDiameter, pitch or tpi, "
                                    "and tapDrill");
    }
    const PropsReader r = root.child(v);
    if (strict) {
        if (auto ok =
                r.onlyKeys({"class", "depth", "designation", "nominalDiameter", "pitch", "standard", "tapDrill", "tpi"},
                           "thread.");
            !ok) {
            return tl::unexpected(ok.error());
        }
    }
    ThreadProps out;
    const Value* designation = r.find("designation");
    if (designation == nullptr || !designation->isString() || designation->asString().empty()) {
        return r.error("thread.designation", "is required (e.g. \"M6x1\" or \"1/4-20 UNC\")");
    }
    out.designation = designation->asString();
    auto standard = r.choice("standard", "thread.standard",
                             {"iso_metric_coarse", "iso_metric_fine", "unc", "unf", "custom"}, std::nullopt);
    if (!standard) {
        return tl::unexpected(standard.error());
    }
    out.standard = *standard;
    if (const Value* c = r.find("class")) {
        if (!c->isString() || c->asString().empty()) {
            return r.error("thread.class", "must be a non-empty string such as \"6H\" or \"2B\"");
        }
        out.threadClass = c->asString();
    }
    auto nominal = r.numeric("nominalDiameter", "thread.nominalDiameter");
    auto tapDrill = r.numeric("tapDrill", "thread.tapDrill");
    if (!nominal || !tapDrill) {
        return tl::unexpected(!nominal ? nominal.error() : tapDrill.error());
    }
    out.nominal = std::move(*nominal);
    out.tapDrill = std::move(*tapDrill);
    const Value* pitch = r.find("pitch");
    const Value* tpi = r.find("tpi");
    if ((pitch == nullptr) == (tpi == nullptr)) {
        return r.error("thread", "must give exactly one of 'pitch' (metric) and 'tpi' (threads per inch)");
    }
    if (pitch != nullptr) {
        auto p = r.numeric("pitch", "thread.pitch");
        if (!p) {
            return tl::unexpected(p.error());
        }
        out.pitch = std::move(*p);
    } else {
        if (!tpi->isNumber() || !std::isfinite(tpi->asNumber()) || !(tpi->asNumber() > 0)) {
            return r.error("thread.tpi", "must be a number of threads per inch greater than 0");
        }
        out.tpi = tpi->asNumber();
    }
    if (r.find("depth") != nullptr) {
        auto d = r.numeric("depth", "thread.depth");
        if (!d) {
            return tl::unexpected(d.error());
        }
        out.depth = std::move(*d);
    }
    return out;
}

Result<std::optional<Value>> optNumeric(const PropsReader& r, std::string_view key, std::string_view field)
{
    if (r.find(key) == nullptr) {
        return std::optional<Value>();
    }
    auto v = r.numeric(key, field);
    if (!v) {
        return tl::unexpected(v.error());
    }
    return std::optional<Value>(std::move(*v));
}

Result<HoleProps> parse(const Feature& f, bool strict)
{
    const std::string where = featureLabel(f);
    auto root = PropsReader::parse(where, f.propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    const PropsReader r(where, *root);
    if (strict) {
        if (auto ok = r.onlyKeys({"bottom", "counterbore", "countersink", "depth", "diameter", "end", "flip", "kind",
                                  "points", "sketch", "target", "thread", "tipAngle"});
            !ok) {
            return tl::unexpected(ok.error());
        }
    }
    HoleProps out;
    auto sketch = r.uuid("sketch", "sketch");
    if (!sketch) {
        return tl::unexpected(sketch.error());
    }
    out.sketch = FeatureId{*sketch};
    if (const Value* points = r.find("points")) {
        if (!points->isArray() || points->asArray().empty()) {
            return r.error("points", "must list at least one point of the sketch (or be left out for all points)");
        }
        std::vector<SketchEntId> list;
        for (std::size_t i = 0; i < points->asArray().size(); ++i) {
            const Value& v = points->asArray()[i];
            const Uuid id = v.isString() ? Uuid::fromString(v.asString()) : Uuid{};
            const std::string field = "points[" + std::to_string(i) + "]";
            if (!v.isString() || id.toString() != v.asString() || id == Uuid{}) {
                return r.error(field, "must be the id of a point of the sketch (a lower-case UUID)");
            }
            if (std::find(list.begin(), list.end(), SketchEntId{id}) != list.end()) {
                return r.error(field, "lists the point " + v.asString() + " a second time");
            }
            list.push_back(SketchEntId{id});
        }
        out.points = std::move(list);
    }
    auto target = r.uuid("target", "target");
    auto flip = r.optBool("flip", "flip", false);
    auto kind = r.choice("kind", "kind",
                         {"simple", "tapped", "counterbore", "countersink", "counterbore_tapped", "countersink_tapped"},
                         std::nullopt);
    auto end = r.choice("end", "end", {"blind", "through"}, std::nullopt);
    auto bottom = r.choice("bottom", "bottom", {"angle", "flat"}, "angle");
    auto tipAngle = r.numeric("tipAngle", "tipAngle", Value::number(118.0 * kDegree));
    for (const Status& st :
         {target ? Status{} : Status(tl::unexpected(target.error())),
          flip ? Status{} : Status(tl::unexpected(flip.error())),
          kind ? Status{} : Status(tl::unexpected(kind.error())), end ? Status{} : Status(tl::unexpected(end.error())),
          bottom ? Status{} : Status(tl::unexpected(bottom.error())),
          tipAngle ? Status{} : Status(tl::unexpected(tipAngle.error()))}) {
        if (!st) {
            return tl::unexpected(st.error());
        }
    }
    out.target = BodyId{*target};
    out.flip = *flip;
    out.kind = *kind;
    out.through = *end == "through";
    out.flatBottom = *bottom == "flat";
    out.tipAngle = std::move(*tipAngle);
    auto diameter = optNumeric(r, "diameter", "diameter");
    auto depth = optNumeric(r, "depth", "depth");
    if (!diameter || !depth) {
        return tl::unexpected(!diameter ? diameter.error() : depth.error());
    }
    out.diameter = std::move(*diameter);
    out.depth = std::move(*depth);
    const bool tapped = tappedKind(out.kind);
    if (tapped && out.diameter && strict) {
        return r.error("diameter", "is set by 'thread.tapDrill' for a tapped hole; leave it out");
    }
    if (!tapped && !out.diameter) {
        return r.error("diameter", "is required for a " + out.kind + " hole");
    }
    if (!out.through && !out.depth) {
        return r.error("depth", "is required for a blind hole");
    }
    out.csinkAngle = Value::number(90.0 * kDegree);
    if (const Value* cb = r.find("counterbore")) {
        if (!cb->isObject()) {
            return r.error("counterbore", "must be {\"diameter\": <length>, \"depth\": <length>}");
        }
        const PropsReader c = r.child(*cb);
        if (strict) {
            if (auto ok = c.onlyKeys({"depth", "diameter"}, "counterbore."); !ok) {
                return tl::unexpected(ok.error());
            }
        }
        auto d = c.numeric("diameter", "counterbore.diameter");
        auto h = c.numeric("depth", "counterbore.depth");
        if (!d || !h) {
            return tl::unexpected(!d ? d.error() : h.error());
        }
        out.cboreDiameter = std::move(*d);
        out.cboreDepth = std::move(*h);
    } else if (counterboreKind(out.kind)) {
        return r.error("counterbore", "is required for a " + out.kind + " hole: {\"diameter\", \"depth\"}");
    }
    if (const Value* cs = r.find("countersink")) {
        if (!cs->isObject()) {
            return r.error("countersink", "must be {\"diameter\": <length>, \"angle\"?: <angle>}");
        }
        const PropsReader c = r.child(*cs);
        if (strict) {
            if (auto ok = c.onlyKeys({"angle", "diameter"}, "countersink."); !ok) {
                return tl::unexpected(ok.error());
            }
        }
        auto d = c.numeric("diameter", "countersink.diameter");
        auto a = c.numeric("angle", "countersink.angle", Value::number(90.0 * kDegree));
        if (!d || !a) {
            return tl::unexpected(!d ? d.error() : a.error());
        }
        out.csinkDiameter = std::move(*d);
        out.csinkAngle = std::move(*a);
    } else if (countersinkKind(out.kind)) {
        return r.error("countersink", "is required for a " + out.kind + " hole: {\"diameter\", \"angle\"?}");
    }
    if (const Value* t = r.find("thread")) {
        auto thread = parseThread(r, *t, strict);
        if (!thread) {
            return tl::unexpected(thread.error());
        }
        out.thread = std::move(*thread);
    } else if (tapped) {
        return r.error("thread", "is required for a " + out.kind + " hole (standards.threads.list gives the sizes)");
    }
    return out;
}

// The numbers of a hole, evaluated.
struct HoleSize {
    double diameter = 0;
    std::optional<double> depth; // blind
    bool flatBottom = false;
    double tipAngle = 0;
    std::optional<double> cboreDiameter, cboreDepth;
    std::optional<double> csinkDiameter;
    double csinkAngle = 0;
    std::optional<HoleThread> thread;
};

std::string mm(double v)
{
    return json::write(Value::number(std::round(v * 1e6) / 1e6)) + " mm";
}

Result<HoleThread> evaluateThread(const ThreadProps& t, const std::map<std::string, double>& params,
                                  const std::string& where)
{
    HoleThread out;
    out.designation = t.designation;
    out.standard = t.standard;
    out.threadClass = t.threadClass;
    auto nominal = evaluateNumeric(t.nominal, params, Parameter::Kind::Length, where, "thread.nominalDiameter");
    auto tapDrill = evaluateNumeric(t.tapDrill, params, Parameter::Kind::Length, where, "thread.tapDrill");
    if (!nominal || !tapDrill) {
        return tl::unexpected(!nominal ? nominal.error() : tapDrill.error());
    }
    out.nominalDiameter = *nominal;
    out.tapDrill = *tapDrill;
    if (t.pitch) {
        auto pitch = evaluateNumeric(*t.pitch, params, Parameter::Kind::Length, where, "thread.pitch");
        if (!pitch) {
            return tl::unexpected(pitch.error());
        }
        out.pitch = *pitch;
    } else {
        out.tpi = t.tpi;
        out.pitch = kInch / *t.tpi;
    }
    if (t.depth) {
        auto depth = evaluateNumeric(*t.depth, params, Parameter::Kind::Length, where, "thread.depth");
        if (!depth) {
            return tl::unexpected(depth.error());
        }
        out.depth = *depth;
    }
    return out;
}

// Evaluates the props that the kind and end use, and checks the sizes fit together.
Result<HoleSize> evaluate(const HoleProps& p, const std::map<std::string, double>& params, const std::string& where)
{
    HoleSize s;
    auto length = [&](const Value& v, std::string_view field) {
        return evaluateNumeric(v, params, Parameter::Kind::Length, where, field);
    };
    auto angle = [&](const Value& v, std::string_view field) {
        return evaluateNumeric(v, params, Parameter::Kind::Angle, where, field);
    };
    const bool tapped = tappedKind(p.kind);
    if (tapped) {
        auto thread = evaluateThread(*p.thread, params, where);
        if (!thread) {
            return tl::unexpected(thread.error());
        }
        s.thread = std::move(*thread);
        s.diameter = s.thread->tapDrill;
    } else {
        auto d = length(*p.diameter, "diameter");
        if (!d) {
            return tl::unexpected(d.error());
        }
        s.diameter = *d;
    }
    if (!(s.diameter > 0)) {
        return invalid(where + ": the " + std::string(tapped ? "tap drill" : "hole") +
                       " diameter must be greater than 0.");
    }
    if (!p.through) {
        auto depth = length(*p.depth, "depth");
        if (!depth) {
            return tl::unexpected(depth.error());
        }
        if (!(*depth > 0)) {
            return invalid(where + ": the depth must be greater than 0.");
        }
        s.depth = *depth;
        s.flatBottom = p.flatBottom;
        if (!p.flatBottom) {
            auto tip = angle(p.tipAngle, "tipAngle");
            if (!tip) {
                return tl::unexpected(tip.error());
            }
            if (!(*tip > 1e-6) || !(*tip < kPi - 1e-6)) {
                return invalid(where + ": the tip angle must be more than 0 and less than 180 degrees.");
            }
            s.tipAngle = *tip;
        }
    }
    double start = 0; // where the full-diameter bore (and a thread) begins, below the sketch plane
    if (counterboreKind(p.kind)) {
        auto d = length(*p.cboreDiameter, "counterbore.diameter");
        auto h = length(*p.cboreDepth, "counterbore.depth");
        if (!d || !h) {
            return tl::unexpected(!d ? d.error() : h.error());
        }
        if (!(*d > s.diameter + 1e-6)) {
            return invalid(where + ": the counterbore diameter (" + mm(*d) +
                           ") must be larger than the hole diameter (" + mm(s.diameter) + ").");
        }
        if (!(*h > 1e-6) || (s.depth && !(*h < *s.depth - 1e-6))) {
            return invalid(where + ": the counterbore depth (" + mm(*h) + ") must be greater than 0" +
                           (s.depth ? " and less than the hole depth (" + mm(*s.depth) + ")." : "."));
        }
        s.cboreDiameter = *d;
        s.cboreDepth = *h;
        start = *h;
    }
    if (countersinkKind(p.kind)) {
        auto d = length(*p.csinkDiameter, "countersink.diameter");
        auto a = angle(p.csinkAngle, "countersink.angle");
        if (!d || !a) {
            return tl::unexpected(!d ? d.error() : a.error());
        }
        if (!(*d > s.diameter + 1e-6)) {
            return invalid(where + ": the countersink diameter (" + mm(*d) +
                           ") must be larger than the hole diameter (" + mm(s.diameter) + ").");
        }
        if (!(*a > 1e-6) || !(*a < kPi - 1e-6)) {
            return invalid(where + ": the countersink angle must be more than 0 and less than 180 degrees.");
        }
        const double h = (*d - s.diameter) / 2 / dmath::tan(*a / 2);
        if (s.depth && !(h < *s.depth - 1e-6)) {
            return invalid(where + ": the countersink (" + mm(h) + " deep) must be shallower than the hole depth (" +
                           mm(*s.depth) + ").");
        }
        s.csinkDiameter = *d;
        s.csinkAngle = *a;
        start = h;
    }
    if (s.thread) {
        const HoleThread& t = *s.thread;
        if (!(t.nominalDiameter > 0) || !(t.pitch > 0)) {
            return invalid(where + ": the thread's nominal diameter and pitch must be greater than 0.");
        }
        if (!(t.tapDrill < t.nominalDiameter - 1e-6)) {
            return invalid(where + ": the tap drill (" + mm(t.tapDrill) +
                           ") must be smaller than the thread's nominal diameter (" + mm(t.nominalDiameter) + ").");
        }
        if (t.depth) {
            if (!(*t.depth > 0)) {
                return invalid(where + ": the thread depth must be greater than 0.");
            }
            if (s.depth && *t.depth > *s.depth - start + 1e-6) {
                return invalid(where + ": the thread depth (" + mm(*t.depth) +
                               ") is more than the tapped length of the hole (" + mm(*s.depth - start) + ").");
            }
        }
    }
    return s;
}

// ---------- Points ----------

std::set<SketchEntId> usedByCurves(const sketch::Sketch& s)
{
    std::set<SketchEntId> used;
    for (const sketch::Entity& e : s.entities) {
        std::visit(
            [&](const auto& g) {
                using T = std::decay_t<decltype(g)>;
                if constexpr (std::is_same_v<T, sketch::Line> || std::is_same_v<T, sketch::Slot>) {
                    used.insert(g.a);
                    used.insert(g.b);
                } else if constexpr (std::is_same_v<T, sketch::Arc>) {
                    used.insert(g.center);
                    used.insert(g.start);
                    used.insert(g.end);
                } else if constexpr (std::is_same_v<T, sketch::Circle>) {
                    used.insert(g.center);
                } else if constexpr (std::is_same_v<T, sketch::Spline>) {
                    used.insert(g.controlPoints.begin(), g.controlPoints.end());
                }
            },
            e.geom);
    }
    return used;
}

struct HolePoint {
    SketchEntId id;
    Vec2 at;
};

// The points to drill at, in order; errors name the point and the sketch.
Result<std::vector<HolePoint>> holePoints(const sketch::Sketch& s,
                                          const std::optional<std::vector<SketchEntId>>& listed,
                                          const std::string& where, const std::string& sketchName)
{
    std::vector<HolePoint> out;
    if (!listed) {
        const std::set<SketchEntId> used = usedByCurves(s);
        for (const sketch::Entity& e : s.entities) {
            const auto* p = std::get_if<sketch::Point>(&e.geom);
            if (p != nullptr && !e.construction && used.count(e.id) == 0) {
                out.push_back(HolePoint{e.id, p->p});
            }
        }
        if (out.empty()) {
            return invalid(where + ": the sketch '" + sketchName +
                           "' has no free-standing point to drill at; add a point or list the points.");
        }
        return out;
    }
    for (const SketchEntId& id : *listed) {
        const auto it =
            std::find_if(s.entities.begin(), s.entities.end(), [&](const sketch::Entity& e) { return e.id == id; });
        if (it == s.entities.end()) {
            return failed(ErrorCode::ReferenceBroken, where + ": its point " + entityText(id) +
                                                          " is no longer in the sketch '" + sketchName +
                                                          "'; pick the points again or remove it.");
        }
        const auto* p = std::get_if<sketch::Point>(&it->geom);
        if (p == nullptr) {
            return failed(ErrorCode::ReferenceBroken,
                          where + ": " + entityText(id) + " is not a point of the sketch '" + sketchName + "'.");
        }
        out.push_back(HolePoint{id, p->p});
    }
    return out;
}

// ---------- Geometry ----------

std::uint64_t sectionTag(const SketchEntId& point, std::string_view part)
{
    Hasher h;
    h.str("ray.doc.hole/1");
    h.bytes(std::as_bytes(std::span<const std::uint8_t>(point.value.bytes)));
    h.str(part);
    const Hash256 d = h.finish();
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | d.bytes[static_cast<std::size_t>(i)];
    }
    return v;
}

// The half-section in (radius, depth below the sketch plane): a closed chain from the axis at the top round to the
// axis at the bottom, each line with the part it makes.
struct SectionPoint {
    Vec2 at;
    const char* part; // the part made by the line from this point to the next
};

std::vector<SectionPoint> section(const HoleSize& s, double throughDepth)
{
    const double r = s.diameter / 2;
    std::vector<SectionPoint> out;
    double depth = throughDepth;
    if (s.cboreDiameter) {
        const double R = *s.cboreDiameter / 2;
        out.push_back({{0, 0}, "top"});
        out.push_back({{R, 0}, "cbore.wall"});
        out.push_back({{R, *s.cboreDepth}, "cbore.floor"});
        out.push_back({{r, *s.cboreDepth}, "wall"});
    } else if (s.csinkDiameter) {
        const double R = *s.csinkDiameter / 2;
        const double h = (R - r) / dmath::tan(s.csinkAngle / 2);
        out.push_back({{0, 0}, "top"});
        out.push_back({{R, 0}, "csink"});
        out.push_back({{r, h}, "wall"});
    } else {
        out.push_back({{0, 0}, "top"});
        out.push_back({{r, 0}, "wall"});
    }
    if (s.depth) {
        depth = *s.depth;
    }
    if (s.depth && !s.flatBottom) {
        out.push_back({{r, depth}, "tip"});
        out.push_back({{0, depth + r / dmath::tan(s.tipAngle / 2)}, "axis"});
    } else {
        out.push_back({{r, depth}, "bottom"});
        out.push_back({{0, depth}, "axis"});
    }
    return out;
}

std::string kernelMessage(const std::string& where, const std::string& which, const Error& e)
{
    return where + ": " + which + ": " + e.message;
}

class HoleType final : public IFeatureType,
                       public ParametricType,
                       public detail::BodyDeclaration,
                       public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kHoleType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Hole"; }
    std::vector<naming::TopoRef> references(const Feature&) const override { return {}; }

    std::vector<BodyId> bodiesWritten(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? std::vector<BodyId>{props->target} : std::vector<BodyId>{};
    }
    bool writesOnlyDeclared() const override { return true; }

    Status validate(const DocState& state, std::size_t index, const Feature& f) const override
    {
        auto props = parse(f, true);
        if (!props) {
            return tl::unexpected(props.error());
        }
        const std::string where = featureLabel(f);
        const DocumentParams params = documentParams(state);
        auto size = evaluate(*props, params.values, where);
        if (!size) {
            if (params.error) {
                return invalid(size.error().message + " (the document parameters do not evaluate: " + *params.error +
                               ")");
            }
            return tl::unexpected(size.error());
        }
        const SweepCommon common{props->sketch, std::nullopt, "cut", props->target};
        if (auto ok = validateSweepCommon(state, index, f, common); !ok) {
            return ok;
        }
        if (props->points) {
            auto sketch = earlierFeature(state, f, props->sketch, kSketchType, "sketch");
            if (!sketch) {
                return tl::unexpected(sketch.error());
            }
            auto model = sketchOfFeature(**sketch, false);
            if (!model) {
                return tl::unexpected(model.error());
            }
            auto points = holePoints(*model, props->points, where, featureLabel(**sketch));
            if (!points) {
                return invalid(points.error().message);
            }
        }
        return {};
    }

    Result<FeatureOutput> rebuild(const Feature& f, RebuildContext& ctx) const override
    {
        try {
            return build(f, ctx);
        } catch (const std::bad_alloc&) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": out of memory");
        } catch (...) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": unexpected failure");
        }
    }

private:
    static Result<FeatureOutput> build(const Feature& f, RebuildContext& ctx)
    {
        const std::string where = featureLabel(f);
        auto props = parse(f, false);
        if (!props) {
            return tl::unexpected(props.error());
        }
        auto size = evaluate(*props, ctx.paramValues, where);
        if (!size) {
            return tl::unexpected(size.error());
        }
        const Feature* sketchFeature = nullptr;
        auto artifact = sketchFor(f, ctx, props->sketch, &sketchFeature);
        if (!artifact) {
            return tl::unexpected(artifact.error());
        }
        auto points = holePoints((*artifact)->solved, props->points, where, featureLabel(*sketchFeature));
        if (!points) {
            return tl::unexpected(points.error());
        }
        auto target = ctx.shapeOf(props->target);
        if (!target) {
            return failed(target.error().code,
                          where + ": the body it drills is not available: " + target.error().message);
        }
        kernel::Shape shape = *target;
        naming::ElementMap map = ctx.mapOf(props->target);
        const kernel::Plane& plane = (*artifact)->plane;
        const Vec3 drill = props->flip ? plane.normal : vmul(plane.normal, -1.0);
        const Box3 box = kernel::bounds(shape);

        FeatureOutput out;
        std::vector<naming::LineageEvent> lineage;
        const std::size_t n = points->size();
        const double share = 1.0 / static_cast<double>(n + 1);
        for (std::size_t i = 0; i < n; ++i) {
            if (auto ok = ctx.job.cancel.check(); !ok) {
                return tl::unexpected(ok.error());
            }
            const HolePoint& hp = (*points)[i];
            const std::string which = n == 1 ? std::string("the hole")
                                             : "the hole at point " + std::to_string(i + 1) + " of " +
                                                   std::to_string(n) + " (" + entityText(hp.id) + ")";
            const Vec3 centre = planePoint(plane, hp.at);
            double through = 0;
            if (!size->depth) {
                for (int c = 0; c < 8; ++c) {
                    const Vec3 corner{(c & 1) ? box.max.x : box.min.x, (c & 2) ? box.max.y : box.min.y,
                                      (c & 4) ? box.max.z : box.min.z};
                    through = std::max(through, vdot(vsub(corner, centre), drill));
                }
                if (!(through > 1e-6)) {
                    return invalid(where + ": " + which + " points away from the body; flip it.");
                }
                through += kThroughMargin;
                const double start = size->cboreDepth      ? *size->cboreDepth
                                     : size->csinkDiameter ? (*size->csinkDiameter - size->diameter) / 2 /
                                                                 dmath::tan(size->csinkAngle / 2)
                                                           : 0.0;
                if (!(through > start + 1e-6)) {
                    return invalid(where + ": " + which + ": the " +
                                   (size->cboreDepth ? "counterbore" : "countersink") +
                                   " is deeper than the body is thick there.");
                }
            }
            // The section plane holds the axis: x along the sketch's x, y = normal x x along the drill direction.
            const Vec3 xDir = plane.xDir;
            const auto normal = vunit(vcross(xDir, drill));
            if (!normal) {
                return failed(ErrorCode::Internal, where + ": the sketch plane has no usable x direction.");
            }
            kernel::ProfileLoop loop;
            loop.tag = sectionTag(hp.id, "section");
            const std::vector<SectionPoint> sec = section(*size, through);
            for (std::size_t k = 0; k < sec.size(); ++k) {
                kernel::ProfileCurve c;
                c.kind = kernel::ProfileCurve::Kind::Line;
                c.pts = {sec[k].at, sec[(k + 1) % sec.size()].at};
                c.tag = sectionTag(hp.id, sec[k].part);
                loop.curves.push_back(std::move(c));
            }
            const kernel::Profile profile{kernel::Plane{centre, *normal, xDir}, {loop}};
            JobContext job{ctx.job.cancel, ctx.job.progress.sub(share)};
            auto tool = kernel::revolve(profile, centre, drill, 2 * kPi, job);
            if (!tool) {
                if (tool.error().code == ErrorCode::Cancelled) {
                    return tl::unexpected(tool.error());
                }
                return failed(tool.error().code, kernelMessage(where, which, tool.error()));
            }
            out.warnings.insert(out.warnings.end(), tool->op.warnings.begin(), tool->op.warnings.end());
            const naming::NamingInput toolInput{f.id, "hole", {}, nullptr, &tool->op.shape, tool->origins};
            auto named = naming::propagate(toolInput);
            if (!named) {
                return failed(ErrorCode::Internal, where + ": naming the hole faces failed: " + named.error().message);
            }
            auto cut = kernel::boolean(kernel::BoolOp::Cut, shape, tool->op.shape, job);
            if (!cut) {
                if (cut.error().code == ErrorCode::Cancelled) {
                    return tl::unexpected(cut.error());
                }
                return failed(cut.error().code, kernelMessage(where, which, cut.error()));
            }
            out.warnings.insert(out.warnings.end(), cut->warnings.begin(), cut->warnings.end());
            const std::array<const naming::ElementMap*, 2> maps{&map, &named->map};
            const std::array<const kernel::Shape*, 2> shapes{&shape, &tool->op.shape};
            const naming::NamingInput input{f.id, "hole", maps, &cut->history, &cut->shape, {}, std::span(shapes)};
            auto result = naming::propagate(input);
            if (!result) {
                return failed(ErrorCode::Internal, where + ": naming the result failed: " + result.error().message);
            }
            shape = cut->shape;
            map = std::move(result->map);
            lineage.insert(lineage.end(), result->lineage.begin(), result->lineage.end());
        }
        for (std::string& w : out.warnings) {
            w = where + ": " + w;
        }
        ctx.job.progress.set(1.0);
        out.bodies.push_back(BodyUpdate{props->target, shape, std::move(map), std::move(lineage)});
        return out;
    }
};

} // namespace

std::unique_ptr<IFeatureType> makeHoleFeatureType()
{
    return std::make_unique<HoleType>();
}

std::string_view holePropsSchema()
{
    return schema();
}

Result<std::optional<HoleThread>> holeThreadOf(const Feature& hole, const std::map<std::string, double>& params)
{
    if (hole.type != kHoleType) {
        return invalid(featureLabel(hole) + ": is not a hole feature");
    }
    auto props = parse(hole, false);
    if (!props) {
        return tl::unexpected(props.error());
    }
    if (!tappedKind(props->kind)) {
        return std::optional<HoleThread>();
    }
    auto thread = evaluateThread(*props->thread, params, featureLabel(hole));
    if (!thread) {
        return tl::unexpected(thread.error());
    }
    return std::optional<HoleThread>(std::move(*thread));
}

} // namespace ray::doc::features
