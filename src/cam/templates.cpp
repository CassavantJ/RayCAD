// Operation templates (task T-057): variables, expressions, feature kinds, the template's rules and the plan of an
// application (templates.h states the rules).
#include "templates.h"

#include "json.h"
#include "json_number.h"
#include "ops/op_params.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <set>
#include <utility>

namespace ray::cam::templates {

namespace {

using json::Member;
using json::Value;

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

// ---------- Text ----------

// A length or plain number for people: rounded to 0.001 (half away from zero), trailing zeros dropped. Integer
// arithmetic on the rounded value, so the text is the same everywhere.
std::string shortNumber(double value)
{
    if (!std::isfinite(value)) {
        return "?";
    }
    const double scaled = std::round(value * 1000.0);
    if (std::fabs(scaled) > 9.0e15) {
        return detail::formatDouble(value);
    }
    long long thousandths = static_cast<long long>(scaled);
    std::string sign;
    if (thousandths < 0) {
        sign = "-";
        thousandths = -thousandths;
    }
    std::string out = sign + std::to_string(thousandths / 1000);
    long long frac = thousandths % 1000;
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

std::string inQuotes(std::string_view s)
{
    return "'" + std::string(s) + "'";
}

bool isIdentStart(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool isIdentChar(char c)
{
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// ---------- Variable tables ----------

constexpr VarInfo kFeatureVars[] = {
    {"feature.diameter", VarUnit::Length, true, "The bore's diameter: the lowest cylinder of the hole, the drilled size "
                                                "(a tapped hole of the hole feature is modelled at its tap drill)."},
    {"feature.depth", VarUnit::Length, true,
     "From the hole's top to the bottom of its full-diameter bore (a through hole: to where it breaks out)."},
    {"feature.totalDepth", VarUnit::Length, true, "From the hole's top to its lowest point (the tip of a drill point)."},
    {"feature.depthRatio", VarUnit::Plain, true, "feature.depth / feature.diameter."},
    {"feature.through", VarUnit::Plain, true, "1 for a through hole, else 0."},
    {"feature.blind", VarUnit::Plain, true, "1 for a blind hole, else 0."},
    {"feature.flatBottom", VarUnit::Plain, true, "1 for a blind hole with a flat floor, else 0."},
    {"feature.topDiameter", VarUnit::Length, true,
     "The diameter at the hole's top: its countersink, chamfer or counterbore, else the bore."},
    {"feature.spotDiameter", VarUnit::Length, true,
     "The diameter a spot drill makes when it spots the hole by its model (the drilling cycle's spot diameter 0): its "
     "countersink or chamfer, else the bore."},
    {"feature.tapped", VarUnit::Plain, true, "1 when the hole has a thread spec (a tapped hole feature), else 0."},
    {"feature.threadSpec", VarUnit::Text, false, "Tapped holes: the thread designation, \"M6x1\", \"1/4-20 UNC\"."},
    {"feature.threadPitch", VarUnit::Length, false, "Tapped holes: the thread pitch."},
    {"feature.threadsPerInch", VarUnit::Plain, false, "Tapped holes: 25.4 / the pitch in mm."},
    {"feature.threadMajor", VarUnit::Length, false,
     "Tapped holes whose thread gives it: the nominal (major) diameter."},
    {"feature.threadDepth", VarUnit::Length, false,
     "Tapped holes: the full-thread depth (the whole bore when the thread gives none)."},
    {"feature.counterbored", VarUnit::Plain, true, "1 when the hole is counterbored, else 0."},
    {"feature.counterboreDiameter", VarUnit::Length, false, "Counterbored holes: the counterbore's diameter."},
    {"feature.counterboreDepth", VarUnit::Length, false, "Counterbored holes: the counterbore's depth."},
    {"feature.countersunk", VarUnit::Plain, true, "1 when the hole is countersunk, else 0."},
    {"feature.countersinkDiameter", VarUnit::Length, false, "Countersunk holes: the countersink's top diameter."},
    {"feature.countersinkAngle", VarUnit::Angle, false, "Countersunk holes: the countersink's included angle."},
    {"feature.chamfered", VarUnit::Plain, true, "1 when the hole's edge is chamfered (an edge break), else 0."},
    {"feature.chamferDiameter", VarUnit::Length, false, "Chamfered holes: the chamfer's top diameter."},
};

constexpr VarInfo kToolVars[] = {
    {"tool.diameter", VarUnit::Length, true, "The cutting diameter."},
    {"tool.shankDiameter", VarUnit::Length, true, "The shank diameter."},
    {"tool.fluteLength", VarUnit::Length, true, "The flute length (length of cut)."},
    {"tool.shoulderLength", VarUnit::Length, true, "The shoulder length."},
    {"tool.overallLength", VarUnit::Length, true, "The overall length."},
    {"tool.cornerRadius", VarUnit::Length, true, "The corner radius."},
    {"tool.tipDiameter", VarUnit::Length, true, "The tip diameter (chamfer and spot tools)."},
    {"tool.pointAngle", VarUnit::Angle, true, "The included point angle (drills, spot drills, chamfer mills); 0 if none."},
    {"tool.pointLength", VarUnit::Length, true,
     "The length of the point: half of (diameter - tip diameter), divided by the tangent of half the point angle; 0 "
     "without a point angle."},
    {"tool.flutes", VarUnit::Plain, true, "The number of flutes."},
    {"tool.threadPitch", VarUnit::Length, true, "The thread pitch (taps and thread mills); 0 if none."},
    {"tool.threadSpec", VarUnit::Text, true, "The thread designation (taps); \"\" if none."},
    {"tool.type", VarUnit::Text, true, "The tool type: \"drill\", \"tap\", \"spotDrill\", ..."},
};

constexpr std::string_view kToolTypeNames[] = {"flatEndMill", "ballEndMill", "bullEndMill", "chamferMill",
                                               "faceMill",    "spotDrill",   "drill",       "tap",
                                               "reamer",      "boringBar",   "threadMill",  "engraveCutter"};

constexpr std::pair<CuttingData::Coolant, std::string_view> kCoolants[] = {
    {CuttingData::Coolant::Off, "off"},   {CuttingData::Coolant::Flood, "flood"},
    {CuttingData::Coolant::Mist, "mist"}, {CuttingData::Coolant::ThroughTool, "throughTool"},
    {CuttingData::Coolant::Air, "air"},
};

void putNumber(Vars& vars, std::string_view name, VarUnit unit, double value)
{
    vars.insert_or_assign(std::string(name), Var{unit, value, {}});
}
void putText(Vars& vars, std::string_view name, std::string value)
{
    vars.insert_or_assign(std::string(name), Var{VarUnit::Text, 0, std::move(value)});
}

// ---------- JSON helpers ----------

std::string textOf(const Value& v)
{
    return json::write(v);
}

const Value* member(const Value& object, std::string_view key)
{
    return object.isObject() ? object.find(key) : nullptr;
}

Value objectWithout(const Value& object, std::initializer_list<std::string_view> keys)
{
    Value::Object out;
    for (const Member& m : object.asObject()) {
        if (std::find(keys.begin(), keys.end(), m.key) == keys.end()) {
            out.push_back(m);
        }
    }
    return Value::object(std::move(out));
}

// ---------- Kinds ----------

const HoleSegment* boreOf(const RecognizedHole& h)
{
    return h.bore < h.segments.size() ? &h.segments[h.bore] : nullptr;
}

// The widest cylinder above the bore (a counterbore), or null.
const HoleSegment* counterboreOf(const RecognizedHole& h)
{
    if (!h.counterbored) {
        return nullptr;
    }
    const HoleSegment* best = nullptr;
    for (std::size_t i = 0; i < h.bore && i < h.segments.size(); ++i) {
        const HoleSegment& s = h.segments[i];
        if (!s.conical && (best == nullptr || s.diameter > best->diameter)) {
            best = &s;
        }
    }
    return best;
}

// The entry cone (above the first cylinder), or null.
const HoleSegment* entryConeOf(const RecognizedHole& h)
{
    if (!h.segments.empty() && h.segments.front().conical && (h.countersunk || h.chamfered)) {
        return &h.segments.front();
    }
    return nullptr;
}

double fullDepthOf(const RecognizedHole& h)
{
    const HoleSegment* bore = boreOf(h);
    return toMm(h.zTop - (bore != nullptr ? bore->zBottom : h.zBottom));
}

// ---------- Tool queries ----------

struct NumericKey {
    std::string_view key;
    FieldKind kind;
    std::string_view toolVar; // the tool variable "prefer" compares
};
constexpr NumericKey kNumericKeys[] = {
    {"diameter", FieldKind::Length, "tool.diameter"},
    {"shankDiameter", FieldKind::Length, "tool.shankDiameter"},
    {"fluteLength", FieldKind::Length, "tool.fluteLength"},
    {"shoulderLength", FieldKind::Length, "tool.shoulderLength"},
    {"overallLength", FieldKind::Length, "tool.overallLength"},
    {"cornerRadius", FieldKind::Length, "tool.cornerRadius"},
    {"tipAngle", FieldKind::Angle, "tool.pointAngle"},
    {"tipDiameter", FieldKind::Length, "tool.tipDiameter"},
    {"threadPitch", FieldKind::Length, "tool.threadPitch"},
    {"flutes", FieldKind::Count, "tool.flutes"},
};
const NumericKey* numericKey(std::string_view key)
{
    for (const NumericKey& k : kNumericKeys) {
        if (k.key == key) {
            return &k;
        }
    }
    return nullptr;
}
constexpr std::string_view kTextKeys[] = {"threadSpec", "bodyMaterial", "material", "opClass"};

std::string unitText(FieldKind kind)
{
    return kind == FieldKind::Length ? " mm" : kind == FieldKind::Angle ? " deg" : "";
}

// A text value: a literal, or "$name" (the whole value) for a text variable.
Result<std::string> textValue(const lib::OpTemplate& t, std::string_view field, const std::string& value,
                              const Vars& vars)
{
    if (value.empty() || value.front() != '$') {
        return value;
    }
    const std::string name = value.substr(1);
    const auto it = vars.find(name);
    if (it == vars.end()) {
        return failure(ErrorCode::InvalidArgument, "Template " + inQuotes(t.name) + ", " + std::string(field) + ": '" +
                                                       value + "': " + name +
                                                       " is not known here (a hole without a thread has no "
                                                       "feature.threadSpec; tool.* are not known in a tool query)");
    }
    if (it->second.unit != VarUnit::Text) {
        return failure(ErrorCode::InvalidArgument, "Template " + inQuotes(t.name) + ", " + std::string(field) + ": '" +
                                                       value + "': " + name + " is a number, not a text");
    }
    return it->second.text;
}

// One numeric value of a query, params, heights or feeds: a number, or an expression string.
Result<double> numericValue(const lib::OpTemplate& t, const std::string& field, const Value& v, const Vars& vars,
                            FieldKind kind, const EvaluateFn& evaluate)
{
    if (v.isNumber()) {
        return v.asNumber();
    }
    if (v.isBool()) {
        return v.asBool() ? 1.0 : 0.0;
    }
    if (v.isString()) {
        return evaluateField(t, field, v.asString(), vars, kind, evaluate);
    }
    return failure(ErrorCode::InvalidArgument,
                   "Template " + inQuotes(t.name) + ", " + field + ": must be a number or an expression");
}

struct EvaluatedQuery {
    std::string json;        // for LibraryStack::queryTools
    std::string description; // in words
};

Result<EvaluatedQuery> evaluateQuery(const lib::OpTemplate& t, const TemplateRules& rules, const Vars& vars,
                                     const EvaluateFn& evaluate)
{
    auto parsed = json::parse(rules.toolQueryJson.empty() ? std::string_view("{}") : rules.toolQueryJson);
    if (!parsed || !parsed->isObject()) {
        return failure(ErrorCode::FormatError, "Template " + inQuotes(t.name) + ": toolQuery is not a JSON object");
    }
    Value::Object out;
    std::vector<std::string> words;
    for (const Member& m : parsed->asObject()) {
        const std::string field = "toolQuery." + m.key;
        if (m.key == "type") {
            out.push_back(m);
            if (m.value.isString()) {
                words.push_back(m.value.asString());
            } else if (m.value.isArray()) {
                std::string list;
                for (const Value& x : m.value.asArray()) {
                    if (x.isString()) {
                        list += (list.empty() ? "" : " or ") + x.asString();
                    }
                }
                words.push_back(list);
            }
            continue;
        }
        if (const NumericKey* nk = numericKey(m.key)) {
            const std::string unit = unitText(nk->kind);
            if (m.value.isObject()) {
                Value::Object range;
                std::optional<double> lo, hi;
                for (const Member& r : m.value.asObject()) {
                    if (r.key != "min" && r.key != "max") {
                        range.push_back(r); // queryTools names the bad key
                        continue;
                    }
                    auto x = numericValue(t, field + "." + r.key, r.value, vars, nk->kind, evaluate);
                    if (!x) {
                        return tl::unexpected(x.error());
                    }
                    (r.key == "min" ? lo : hi) = *x;
                    range.push_back(Member{r.key, Value::number(*x)});
                }
                out.push_back(Member{m.key, Value::object(std::move(range))});
                if (lo && hi) {
                    words.push_back(m.key + " " + shortNumber(*lo) + " to " + shortNumber(*hi) + unit);
                } else if (lo) {
                    words.push_back(m.key + " at least " + shortNumber(*lo) + unit);
                } else if (hi) {
                    words.push_back(m.key + " at most " + shortNumber(*hi) + unit);
                }
                continue;
            }
            auto x = numericValue(t, field, m.value, vars, nk->kind, evaluate);
            if (!x) {
                return tl::unexpected(x.error());
            }
            out.push_back(Member{m.key, Value::number(*x)});
            words.push_back(m.key + " " + shortNumber(*x) + unit);
            continue;
        }
        if (std::find(std::begin(kTextKeys), std::end(kTextKeys), m.key) != std::end(kTextKeys) &&
            m.value.isString()) {
            auto text = textValue(t, field, m.value.asString(), vars);
            if (!text) {
                return tl::unexpected(text.error());
            }
            out.push_back(Member{m.key, Value::string(*text)});
            if (m.key == "threadSpec") {
                words.push_back("thread " + *text);
            } else if (m.key == "bodyMaterial") {
                words.push_back(*text);
            } else if (m.key == "opClass") {
                words.push_back("with " + *text + " cutting data");
            } else {
                words.push_back("with cutting data for material " + *text);
            }
            continue;
        }
        out.push_back(m); // queryTools refuses what it does not know, naming the key
    }
    EvaluatedQuery q;
    q.json = json::write(Value::object(std::move(out)));
    for (const std::string& w : words) {
        q.description += (q.description.empty() ? "" : ", ") + w;
    }
    if (q.description.empty()) {
        q.description = "any tool";
    }
    if (rules.exactTool) {
        q.description = "the tool " + rules.exactTool->value.toString() + " (" + q.description + ")";
    }
    return q;
}

// ---------- Conditions ----------

bool sameNumber(double a, double b)
{
    const double scale = std::max({1.0, std::fabs(a), std::fabs(b)});
    return std::fabs(a - b) <= 1e-9 * scale;
}

bool holds(const Condition& c, const Vars& vars)
{
    const auto it = vars.find(c.variable);
    if (it == vars.end()) {
        return false; // a hole without the value (a counterbore size on a plain hole) does not match
    }
    const Var& v = it->second;
    if (c.text) {
        return v.unit == VarUnit::Text && lower(v.text) == lower(*c.text);
    }
    if (v.unit == VarUnit::Text) {
        return false;
    }
    const double x = v.number;
    if (c.equals && !sameNumber(x, *c.equals)) {
        return false;
    }
    if (c.min && x < *c.min && !sameNumber(x, *c.min)) {
        return false;
    }
    if (c.max && x > *c.max && !sameNumber(x, *c.max)) {
        return false;
    }
    if (c.above && (x <= *c.above || sameNumber(x, *c.above))) {
        return false;
    }
    if (c.below && (x >= *c.below || sameNumber(x, *c.below))) {
        return false;
    }
    return true;
}

// ---------- Operation params ----------

FieldKind kindOfProperty(const ops::OpSchema::Property& p)
{
    const Value* unit = member(p.schema, "x-unit");
    const std::string u = unit != nullptr && unit->isString() ? unit->asString() : std::string();
    if (u == "length") {
        return FieldKind::Length;
    }
    if (u == "angle") {
        return FieldKind::Angle;
    }
    return p.kind == ops::OpSchema::Property::Kind::Integer ? FieldKind::Count : FieldKind::Unitless;
}

struct TypeSchema {
    const IOperationType* type = nullptr;
    std::shared_ptr<const ops::OpSchema> schema;
};

Result<std::string> evaluateParams(const lib::OpTemplate& t, const TypeSchema& ts, const Vars& vars,
                                   const EvaluateFn& evaluate)
{
    auto parsed = json::parse(t.paramsJson.empty() ? std::string_view("{}") : std::string_view(t.paramsJson));
    if (!parsed || !parsed->isObject()) {
        return failure(ErrorCode::FormatError, "Template " + inQuotes(t.name) + ": params is not a JSON object");
    }
    Value::Object out;
    const auto& props = ts.schema->properties();
    for (const Member& m : parsed->asObject()) {
        const std::string field = "params." + m.key;
        const auto p = props.find(m.key);
        if (p == props.end()) {
            return failure(ErrorCode::InvalidArgument, "Template " + inQuotes(t.name) + ", " + field +
                                                           ": the operation type '" + t.opType +
                                                           "' has no such parameter");
        }
        using Kind = ops::OpSchema::Property::Kind;
        const Kind kind = p->second.kind;
        if (m.value.isString() && (kind == Kind::Number || kind == Kind::Integer || kind == Kind::Boolean)) {
            auto x = evaluateField(t, field, m.value.asString(), vars,
                                   kind == Kind::Boolean ? FieldKind::Unitless : kindOfProperty(p->second), evaluate);
            if (!x) {
                return tl::unexpected(x.error());
            }
            out.push_back(Member{m.key, kind == Kind::Boolean ? Value::boolean(*x != 0.0) : Value::number(*x)});
        } else if (m.value.isString()) {
            auto text = textValue(t, field, m.value.asString(), vars);
            if (!text) {
                return tl::unexpected(text.error());
            }
            out.push_back(Member{m.key, Value::string(*text)});
        } else {
            out.push_back(m);
        }
    }
    std::string text = json::write(Value::object(std::move(out)));
    if (auto read = ts.schema->read(text); !read) {
        return failure(ErrorCode::InvalidArgument,
                       "Template " + inQuotes(t.name) + ", params: " + read.error().message);
    }
    return text;
}

// ---------- Plan helpers ----------

const lib::ToolPreset* presetFor(const lib::Tool& tool, const MaterialId& material, std::string_view opClass)
{
    for (const lib::ToolPreset& p : tool.presets) {
        if (p.material == material && (opClass.empty() || p.opClass == opClass)) {
            return &p;
        }
    }
    return nullptr;
}

std::string sizeLabel(const RecognizedHole& h)
{
    if (!h.threadSpec.empty()) {
        return h.threadSpec;
    }
    const HoleSegment* bore = boreOf(h);
    return shortNumber(bore != nullptr ? bore->diameter : 0.0) + " mm";
}

struct Built {
    std::string key; // what makes two holes share an operation
    Operation op;
    lib::Tool tool;
};

} // namespace

// ---------- Variables ----------

std::span<const VarInfo> featureVariables()
{
    return kFeatureVars;
}

std::span<const VarInfo> toolVariables()
{
    return kToolVars;
}

Vars featureVars(const RecognizedHole& h)
{
    Vars v;
    const HoleSegment* bore = boreOf(h);
    const double diameter = bore != nullptr ? bore->diameter : 0.0;
    const double depth = fullDepthOf(h);
    putNumber(v, "feature.diameter", VarUnit::Length, diameter);
    putNumber(v, "feature.depth", VarUnit::Length, depth);
    putNumber(v, "feature.totalDepth", VarUnit::Length, toMm(h.zTop - h.zBottom));
    putNumber(v, "feature.depthRatio", VarUnit::Plain, diameter > 0 ? depth / diameter : 0.0);
    putNumber(v, "feature.through", VarUnit::Plain, h.through ? 1.0 : 0.0);
    putNumber(v, "feature.blind", VarUnit::Plain, h.through ? 0.0 : 1.0);
    putNumber(v, "feature.flatBottom", VarUnit::Plain, h.bottom == HoleBottom::Flat ? 1.0 : 0.0);
    putNumber(v, "feature.topDiameter", VarUnit::Length, h.segments.empty() ? diameter : h.segments.front().diameter);
    const HoleSegment* entry = entryConeOf(h);
    putNumber(v, "feature.spotDiameter", VarUnit::Length, entry != nullptr ? entry->diameter : diameter);
    const bool tapped = !h.threadSpec.empty();
    putNumber(v, "feature.tapped", VarUnit::Plain, tapped ? 1.0 : 0.0);
    if (tapped) {
        putText(v, "feature.threadSpec", h.threadSpec);
        if (h.threadPitch > 0) {
            putNumber(v, "feature.threadPitch", VarUnit::Length, h.threadPitch);
            putNumber(v, "feature.threadsPerInch", VarUnit::Plain, 25.4 / h.threadPitch);
        }
        if (h.threadNominalDiameter > 0) {
            putNumber(v, "feature.threadMajor", VarUnit::Length, h.threadNominalDiameter);
        }
        putNumber(v, "feature.threadDepth", VarUnit::Length, h.threadDepth > 0 ? h.threadDepth : depth);
    }
    const HoleSegment* cb = counterboreOf(h);
    putNumber(v, "feature.counterbored", VarUnit::Plain, cb != nullptr ? 1.0 : 0.0);
    if (cb != nullptr) {
        putNumber(v, "feature.counterboreDiameter", VarUnit::Length, cb->diameter);
        putNumber(v, "feature.counterboreDepth", VarUnit::Length, toMm(h.zTop - cb->zBottom));
    }
    const HoleSegment* cone = entryConeOf(h);
    putNumber(v, "feature.countersunk", VarUnit::Plain, h.countersunk ? 1.0 : 0.0);
    putNumber(v, "feature.chamfered", VarUnit::Plain, h.chamfered ? 1.0 : 0.0);
    if (cone != nullptr && h.countersunk) {
        putNumber(v, "feature.countersinkDiameter", VarUnit::Length, cone->diameter);
        putNumber(v, "feature.countersinkAngle", VarUnit::Angle, cone->coneAngleDeg);
    }
    if (cone != nullptr && h.chamfered) {
        putNumber(v, "feature.chamferDiameter", VarUnit::Length, cone->diameter);
    }
    return v;
}

Vars toolVars(const lib::Tool& t)
{
    Vars v;
    putNumber(v, "tool.diameter", VarUnit::Length, t.diameter);
    putNumber(v, "tool.shankDiameter", VarUnit::Length, t.shankDiameter);
    putNumber(v, "tool.fluteLength", VarUnit::Length, t.fluteLength);
    putNumber(v, "tool.shoulderLength", VarUnit::Length, t.shoulderLength);
    putNumber(v, "tool.overallLength", VarUnit::Length, t.overallLength);
    putNumber(v, "tool.cornerRadius", VarUnit::Length, t.cornerRadius);
    putNumber(v, "tool.tipDiameter", VarUnit::Length, t.tipDiameter);
    putNumber(v, "tool.pointAngle", VarUnit::Angle, t.tipAngleDeg);
    double pointLength = 0;
    if (t.tipAngleDeg > 0 && t.tipAngleDeg < 180 && t.diameter > t.tipDiameter) {
        const double half = t.tipAngleDeg * 3.14159265358979323846 / 360.0;
        pointLength = (t.diameter - t.tipDiameter) / 2.0 / dmath::tan(half);
    }
    putNumber(v, "tool.pointLength", VarUnit::Length, pointLength);
    putNumber(v, "tool.flutes", VarUnit::Plain, t.flutes);
    putNumber(v, "tool.threadPitch", VarUnit::Length, t.threadPitch);
    putText(v, "tool.threadSpec", t.threadSpec);
    const auto type = static_cast<std::size_t>(t.type);
    putText(v, "tool.type", std::string(type < std::size(kToolTypeNames) ? kToolTypeNames[type] : "flatEndMill"));
    return v;
}

// ---------- Expressions ----------

Result<std::string> substitute(std::string_view text, const Vars& vars)
{
    std::string out;
    out.reserve(text.size() + 16);
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        const bool dollar = c == '$';
        // A name starts here when it is not the continuation of a number ("1e5") or of another name.
        const bool startsName = isIdentStart(c) && (i == 0 || (!isIdentChar(text[i - 1]) && text[i - 1] != '.'));
        if (!dollar && !startsName) {
            out += c;
            ++i;
            continue;
        }
        if (!dollar && i > 0 && (text[i - 1] >= '0' && text[i - 1] <= '9')) {
            out += c; // a unit after a number ("4mm") is not a name
            ++i;
            continue;
        }
        std::size_t start = dollar ? i + 1 : i;
        std::size_t end = start;
        // A dotted name: ident ('.' ident)*
        while (end < text.size() && isIdentStart(text[end])) {
            ++end;
            while (end < text.size() && isIdentChar(text[end])) {
                ++end;
            }
            if (end + 1 < text.size() && text[end] == '.' && isIdentStart(text[end + 1])) {
                ++end;
                continue;
            }
            break;
        }
        const std::string_view name = text.substr(start, end - start);
        const bool templated = name.rfind("feature.", 0) == 0 || name.rfind("tool.", 0) == 0;
        if (!templated) {
            if (dollar) {
                return failure(ErrorCode::InvalidArgument,
                               "'$" + std::string(name) + "' is not a variable: templates know feature.* and tool.*");
            }
            if (name.find('.') != std::string_view::npos) {
                return failure(ErrorCode::InvalidArgument,
                               "'" + std::string(name) + "' is not a variable: templates know feature.* and tool.*");
            }
            out.append(text.substr(i, end - i)); // a parameter, function or unit of the document's language
            i = end;
            continue;
        }
        const auto it = vars.find(name);
        if (it == vars.end()) {
            const bool known =
                std::any_of(std::begin(kFeatureVars), std::end(kFeatureVars),
                            [&](const VarInfo& v) { return v.name == name; }) ||
                std::any_of(std::begin(kToolVars), std::end(kToolVars), [&](const VarInfo& v) { return v.name == name; });
            if (!known) {
                return failure(ErrorCode::InvalidArgument, "there is no variable " + std::string(name) +
                                                               " (docs/template_expressions.md lists them)");
            }
            if (name.rfind("tool.", 0) == 0) {
                return failure(ErrorCode::InvalidArgument,
                               std::string(name) + " is not known here: the tool is not chosen yet (only feature.* "
                                                   "in a tool query and in \"when\")");
            }
            return failure(ErrorCode::InvalidArgument, std::string(name) + " is not known for this hole");
        }
        const Var& v = it->second;
        if (v.unit == VarUnit::Text) {
            return failure(ErrorCode::InvalidArgument,
                           std::string(name) + " is a text (\"" + v.text +
                               "\"); it can only be the whole value of a text key, as \"$" + std::string(name) + "\"");
        }
        out += "(" + detail::formatDouble(v.number) +
               (v.unit == VarUnit::Length  ? " mm"
                : v.unit == VarUnit::Angle ? " deg"
                                           : "") +
               ")";
        i = end;
    }
    return out;
}

Result<double> evaluateField(const lib::OpTemplate& t, std::string_view field, std::string_view expression,
                             const Vars& vars, FieldKind kind, const EvaluateFn& evaluate)
{
    const std::string where =
        "Template " + inQuotes(t.name) + ", " + std::string(field) + ": '" + std::string(expression) + "': ";
    auto text = substitute(expression, vars);
    if (!text) {
        return failure(ErrorCode::InvalidArgument, where + text.error().message);
    }
    if (!evaluate) {
        return failure(ErrorCode::Internal, where + "no expression evaluator");
    }
    auto value = evaluate(*text, kind);
    if (!value) {
        std::string message = value.error().message;
        // The evaluator says "Expression '<substituted text>': <what>"; the template's own words are above.
        const std::string prefix = "Expression '" + *text + "': ";
        if (message.rfind(prefix, 0) == 0) {
            message = message.substr(prefix.size());
        }
        return failure(ErrorCode::InvalidArgument, where + message);
    }
    if (!std::isfinite(*value)) {
        return failure(ErrorCode::InvalidArgument, where + "the result is not a finite number");
    }
    return *value;
}

// ---------- Kinds and groups ----------

std::vector<std::string> featureKindsOf(const RecognizedHole& h)
{
    std::vector<std::string> out = {"hole", h.threadSpec.empty() ? "hole.plain" : "hole.tapped",
                                    h.through ? "hole.through" : "hole.blind"};
    if (h.counterbored) {
        out.emplace_back("hole.counterbored");
    }
    if (h.countersunk) {
        out.emplace_back("hole.countersunk");
    }
    return out;
}

bool kindMatches(std::string_view templateKind, const RecognizedHole& h)
{
    if (templateKind.empty()) {
        return true;
    }
    const auto kinds = featureKindsOf(h);
    return std::find(kinds.begin(), kinds.end(), templateKind) != kinds.end();
}

std::vector<HoleGroup> groupHoles(std::span<const RecognizedHole> holes)
{
    std::vector<HoleGroup> out;
    std::map<std::string, std::size_t> byKey;
    for (std::size_t i = 0; i < holes.size(); ++i) {
        const RecognizedHole& h = holes[i];
        HoleGroup g;
        const bool tapped = !h.threadSpec.empty();
        g.kind = tapped ? "hole.tapped" : "hole.plain";
        g.threadSpec = h.threadSpec;
        const HoleSegment* bore = boreOf(h);
        g.diameter = bore != nullptr ? bore->diameter : 0.0;
        g.depth = fullDepthOf(h);
        g.through = h.through;
        if (const HoleSegment* cb = counterboreOf(h)) {
            g.counterboreDiameter = cb->diameter;
            g.counterboreDepth = toMm(h.zTop - cb->zBottom);
        }
        if (const HoleSegment* cone = entryConeOf(h); cone != nullptr && h.countersunk) {
            g.countersinkDiameter = cone->diameter;
        }
        // Sizes in the key to 1 um, so float noise from the kernel does not split a group.
        g.key = std::string(tapped ? "tapped|" + g.threadSpec : "plain|" + shortNumber(g.diameter)) +
                (g.through ? "|through" : "|blind:" + shortNumber(g.depth)) +
                (g.counterboreDiameter > 0
                     ? "|cb" + shortNumber(g.counterboreDiameter) + ":" + shortNumber(g.counterboreDepth)
                     : "") +
                (g.countersinkDiameter > 0 ? "|cs" + shortNumber(g.countersinkDiameter) : "");
        const auto it = byKey.find(g.key);
        if (it != byKey.end()) {
            out[it->second].holes.push_back(i);
            continue;
        }
        g.holes.push_back(i);
        byKey.emplace(g.key, out.size());
        out.push_back(std::move(g));
    }
    for (HoleGroup& g : out) {
        std::string what = g.kind == "hole.tapped" ? g.threadSpec + " tapped" : shortNumber(g.diameter) + " mm";
        what += g.through ? ", through" : ", " + shortNumber(g.depth) + " mm deep";
        if (g.counterboreDiameter > 0) {
            what += ", counterbored " + shortNumber(g.counterboreDiameter) + " x " + shortNumber(g.counterboreDepth);
        }
        if (g.countersinkDiameter > 0) {
            what += ", countersunk " + shortNumber(g.countersinkDiameter);
        }
        g.label = std::to_string(g.holes.size()) + " x " + what;
    }
    return out;
}

std::string describeHole(const RecognizedHole& h)
{
    return sizeLabel(h) + " at X " + shortNumber(toMm(h.x)) + " Y " + shortNumber(toMm(h.y));
}

// ---------- Rules ----------

Result<TemplateRules> rulesOf(const lib::OpTemplate& t)
{
    try {
        const auto bad = [&t](const std::string& key, const std::string& what) {
            return failure(ErrorCode::FormatError, "Template " + inQuotes(t.name) + ": " + key + ": " + what);
        };
        TemplateRules r;
        auto extra = json::parse(t.extraJson.empty() ? std::string_view("{}") : std::string_view(t.extraJson));
        if (!extra || !extra->isObject()) {
            return bad("(extra keys)", "not a JSON object");
        }
        if (const Value* set = member(*extra, "set")) {
            if (!set->isString()) {
                return bad("set", "must be a text");
            }
            r.set = set->asString();
        }
        if (const Value* seq = member(*extra, "sequence")) {
            if (!seq->isNumber() || seq->asNumber() != std::floor(seq->asNumber()) || std::fabs(seq->asNumber()) > 1e9) {
                return bad("sequence", "must be a whole number");
            }
            r.sequence = static_cast<int>(seq->asNumber());
        }
        if (const Value* when = member(*extra, "when")) {
            if (!when->isObject()) {
                return bad("when", "must be an object of conditions");
            }
            for (const Member& m : when->asObject()) {
                Condition c;
                c.variable = !m.key.empty() && m.key.front() == '$' ? m.key.substr(1) : m.key;
                if (c.variable.rfind("feature.", 0) != 0) {
                    return bad("when." + m.key, "conditions name feature.* variables");
                }
                if (m.value.isNumber()) {
                    c.equals = m.value.asNumber();
                } else if (m.value.isBool()) {
                    c.equals = m.value.asBool() ? 1.0 : 0.0;
                } else if (m.value.isString()) {
                    c.text = m.value.asString();
                } else if (m.value.isObject()) {
                    for (const Member& b : m.value.asObject()) {
                        if (!b.value.isNumber()) {
                            return bad("when." + m.key + "." + b.key, "must be a number");
                        }
                        const double x = b.value.asNumber();
                        if (b.key == "min") {
                            c.min = x;
                        } else if (b.key == "max") {
                            c.max = x;
                        } else if (b.key == "above") {
                            c.above = x;
                        } else if (b.key == "below") {
                            c.below = x;
                        } else {
                            return bad("when." + m.key + "." + b.key, "is not min, max, above or below");
                        }
                    }
                } else {
                    return bad("when." + m.key, "must be a number, a text, true/false or {min, max, above, below}");
                }
                r.when.push_back(std::move(c));
            }
        }
        if (const Value* feeds = member(*extra, "feeds")) {
            if (!feeds->isObject()) {
                return bad("feeds", "must be an object");
            }
            for (const Member& m : feeds->asObject()) {
                if (m.key == "mode") {
                    if (!m.value.isString() || (m.value.asString() != "calculated" && m.value.asString() != "fixed")) {
                        return bad("feeds.mode", "must be \"calculated\" or \"fixed\"");
                    }
                    r.feeds.calculated = m.value.asString() == "calculated";
                } else if (m.key == "opClass") {
                    if (!m.value.isString()) {
                        return bad("feeds.opClass", "must be a text");
                    }
                    r.feeds.opClass = m.value.asString();
                } else if (m.key == "coolant") {
                    const auto it = std::find_if(std::begin(kCoolants), std::end(kCoolants), [&](const auto& c) {
                        return m.value.isString() && c.second == m.value.asString();
                    });
                    if (it == std::end(kCoolants)) {
                        return bad("feeds.coolant", "must be off, flood, mist, throughTool or air");
                    }
                    r.feeds.coolant = it->first;
                } else if (m.key == "clockwise") {
                    if (!m.value.isBool()) {
                        return bad("feeds.clockwise", "must be true or false");
                    }
                    r.feeds.clockwise = m.value.asBool();
                } else if (m.key == "rpm" || m.key == "feedCut" || m.key == "feedPlunge" || m.key == "feedRamp") {
                    if (!m.value.isNumber() && !m.value.isString()) {
                        return bad("feeds." + m.key, "must be a number or an expression");
                    }
                    r.feeds.fixed[m.key] = textOf(m.value);
                } else {
                    return bad("feeds." + m.key, "is not a key of a feeds rule");
                }
            }
            if (!r.feeds.calculated && (r.feeds.fixed.count("rpm") == 0 || r.feeds.fixed.count("feedCut") == 0)) {
                return bad("feeds", "fixed feeds need rpm and feedCut");
            }
        }
        if (const Value* heights = member(*extra, "heights")) {
            if (!heights->isObject()) {
                return bad("heights", "must be an object");
            }
            for (const Member& m : heights->asObject()) {
                if (m.key != "clearance" && m.key != "retract" && m.key != "feedPlane" && m.key != "top" &&
                    m.key != "bottom") {
                    return bad("heights." + m.key, "is not clearance, retract, feedPlane, top or bottom");
                }
                if (!m.value.isNumber() && !m.value.isString()) {
                    return bad("heights." + m.key, "must be a number or an expression");
                }
                r.heights[m.key] = textOf(m.value);
            }
        }
        auto query = json::parse(t.toolQueryJson.empty() ? std::string_view("{}") : std::string_view(t.toolQueryJson));
        if (!query || !query->isObject()) {
            return bad("toolQuery", "must be a JSON object");
        }
        if (const Value* prefer = member(*query, "prefer")) {
            if (!prefer->isObject()) {
                return bad("toolQuery.prefer", "must be an object");
            }
            for (const Member& m : prefer->asObject()) {
                if (numericKey(m.key) == nullptr || (!m.value.isNumber() && !m.value.isString())) {
                    return bad("toolQuery.prefer." + m.key, "must name a numeric tool field with a number or expression");
                }
                r.prefer[m.key] = textOf(m.value);
            }
        }
        if (const Value* id = member(*query, "id")) {
            const Uuid u = id->isString() ? Uuid::fromString(id->asString()) : Uuid{};
            if (u == Uuid{}) {
                return bad("toolQuery.id", "must be a tool id (a UUID)");
            }
            r.exactTool = ToolId{u};
        }
        r.toolQueryJson = json::write(objectWithout(*query, {"prefer", "id"}));
        return r;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory");
    }
}

std::string extraJsonOf(const TemplateRules& r, std::string_view otherExtraJson)
{
    Value::Object members;
    if (auto other = json::parse(otherExtraJson.empty() ? std::string_view("{}") : otherExtraJson);
        other && other->isObject()) {
        for (const Member& m : other->asObject()) {
            if (m.key != "set" && m.key != "sequence" && m.key != "when" && m.key != "feeds" && m.key != "heights") {
                members.push_back(m);
            }
        }
    }
    const auto valueOf = [](const std::string& text) {
        auto v = json::parse(text);
        return v ? std::move(*v) : Value::string(text);
    };
    if (!r.set.empty()) {
        members.push_back(Member{"set", Value::string(r.set)});
    }
    members.push_back(Member{"sequence", Value::number(r.sequence)});
    if (!r.when.empty()) {
        Value::Object when;
        for (const Condition& c : r.when) {
            if (c.text) {
                when.push_back(Member{c.variable, Value::string(*c.text)});
            } else if (c.equals) {
                when.push_back(Member{c.variable, Value::number(*c.equals)});
            } else {
                Value::Object range;
                const std::pair<const char*, const std::optional<double>*> bounds[] = {
                    {"min", &c.min}, {"max", &c.max}, {"above", &c.above}, {"below", &c.below}};
                for (const auto& [k, x] : bounds) {
                    if (*x) {
                        range.push_back(Member{k, Value::number(**x)});
                    }
                }
                when.push_back(Member{c.variable, Value::object(std::move(range))});
            }
        }
        members.push_back(Member{"when", Value::object(std::move(when))});
    }
    Value::Object feeds;
    feeds.push_back(Member{"mode", Value::string(r.feeds.calculated ? "calculated" : "fixed")});
    if (r.feeds.calculated && !r.feeds.opClass.empty()) {
        feeds.push_back(Member{"opClass", Value::string(r.feeds.opClass)});
    }
    if (!r.feeds.calculated) {
        for (const auto& [k, text] : r.feeds.fixed) {
            feeds.push_back(Member{k, valueOf(text)});
        }
    }
    if (r.feeds.coolant) {
        for (const auto& [c, name] : kCoolants) {
            if (c == *r.feeds.coolant) {
                feeds.push_back(Member{"coolant", Value::string(std::string(name))});
            }
        }
    }
    if (r.feeds.clockwise) {
        feeds.push_back(Member{"clockwise", Value::boolean(*r.feeds.clockwise)});
    }
    members.push_back(Member{"feeds", Value::object(std::move(feeds))});
    if (!r.heights.empty()) {
        Value::Object heights;
        for (const auto& [k, text] : r.heights) {
            heights.push_back(Member{k, valueOf(text)});
        }
        members.push_back(Member{"heights", Value::object(std::move(heights))});
    }
    return json::write(Value::object(std::move(members)));
}

// ---------- Planning ----------

Result<Plan> planTemplates(std::span<const lib::OpTemplate> templates, std::span<const PlanHole> holes,
                           const PlanContext& ctx)
{
    try {
        std::vector<TemplateRules> rules;
        std::vector<TypeSchema> schemas;
        for (const lib::OpTemplate& t : templates) {
            auto r = rulesOf(t);
            if (!r) {
                return tl::unexpected(r.error());
            }
            rules.push_back(std::move(*r));
            TypeSchema ts;
            ts.type = ctx.types != nullptr ? ctx.types->find(t.opType) : nullptr;
            if (ts.type == nullptr) {
                return failure(ErrorCode::InvalidArgument, "Template " + inQuotes(t.name) + ": the operation type '" +
                                                               t.opType + "' is not available in this RayCAD");
            }
            auto schema = ops::OpSchema::load(ts.type->paramsSchemaJson());
            if (!schema) {
                return failure(ErrorCode::Internal, "The schema of operation type '" + t.opType +
                                                        "' cannot be read: " + schema.error().message);
            }
            ts.schema = std::move(*schema);
            schemas.push_back(std::move(ts));
        }
        std::vector<Vars> holeVars;
        holeVars.reserve(holes.size());
        for (const PlanHole& h : holes) {
            holeVars.push_back(featureVars(h.hole));
        }

        // Template order: by sequence, then as given.
        std::vector<std::size_t> order(templates.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t a, std::size_t b) { return rules[a].sequence < rules[b].sequence; });

        // Which template each hole takes at each sequence.
        std::vector<std::vector<std::size_t>> assigned(templates.size());
        std::vector<bool> touched(holes.size(), false);
        for (std::size_t k = 0; k < order.size();) {
            std::size_t end = k;
            while (end < order.size() && rules[order[end]].sequence == rules[order[k]].sequence) {
                ++end;
            }
            for (std::size_t h = 0; h < holes.size(); ++h) {
                for (std::size_t j = k; j < end; ++j) {
                    const std::size_t ti = order[j];
                    const bool when = std::all_of(rules[ti].when.begin(), rules[ti].when.end(),
                                                  [&](const Condition& c) { return holds(c, holeVars[h]); });
                    if (when && kindMatches(templates[ti].featureKind, holes[h].hole)) {
                        assigned[ti].push_back(h);
                        touched[h] = true;
                        break;
                    }
                }
            }
            k = end;
        }

        Plan plan;
        std::map<std::string, std::vector<lib::Tool>> queryCache;
        for (const std::size_t ti : order) {
            const lib::OpTemplate& t = templates[ti];
            const TemplateRules& r = rules[ti];
            std::vector<Built> built;                         // this template's operations, in first-hole order
            std::map<std::string, std::size_t> missingByRule; // rule text -> index in plan.missing
            for (const std::size_t h : assigned[ti]) {
                const Vars& fv = holeVars[h];
                auto query = evaluateQuery(t, r, fv, ctx.evaluate);
                if (!query) {
                    return tl::unexpected(query.error());
                }
                auto cached = queryCache.find(query->json);
                if (cached == queryCache.end()) {
                    if (!ctx.queryTools) {
                        return failure(ErrorCode::Internal, "No tool library to query");
                    }
                    auto found = ctx.queryTools(query->json);
                    if (!found) {
                        return failure(found.error().code,
                                       "Template " + inQuotes(t.name) + ", toolQuery: " + found.error().message);
                    }
                    cached = queryCache.emplace(query->json, std::move(*found)).first;
                }
                std::vector<const lib::Tool*> candidates;
                for (const lib::Tool& tool : cached->second) {
                    if (!r.exactTool || tool.id == *r.exactTool) {
                        candidates.push_back(&tool);
                    }
                }
                // Calculated feeds need a preset for the material: those tools first.
                if (r.feeds.calculated && ctx.material != nullptr) {
                    std::stable_partition(candidates.begin(), candidates.end(), [&](const lib::Tool* tool) {
                        return presetFor(*tool, ctx.material->id, r.feeds.opClass) != nullptr;
                    });
                    const auto withPreset = std::find_if(candidates.begin(), candidates.end(), [&](const lib::Tool* x) {
                        return presetFor(*x, ctx.material->id, r.feeds.opClass) == nullptr;
                    });
                    if (withPreset != candidates.begin()) {
                        candidates.erase(withPreset, candidates.end());
                    }
                }
                if (!r.prefer.empty() && candidates.size() > 1) {
                    // Nearest by the first preferred field (then the next ones), stable: ties keep the query's order.
                    std::vector<std::pair<std::vector<double>, const lib::Tool*>> ranked;
                    for (const lib::Tool* tool : candidates) {
                        std::vector<double> distances;
                        const Vars tv = toolVars(*tool);
                        for (const auto& [key, text] : r.prefer) {
                            const NumericKey* nk = numericKey(key);
                            auto v = json::parse(text);
                            if (!v) {
                                return failure(ErrorCode::FormatError, "Template " + inQuotes(t.name) +
                                                                           ": toolQuery.prefer." + key +
                                                                           " is not valid JSON");
                            }
                            auto target = numericValue(t, "toolQuery.prefer." + key, *v, fv, nk->kind, ctx.evaluate);
                            if (!target) {
                                return tl::unexpected(target.error());
                            }
                            distances.push_back(std::fabs(tv.at(std::string(nk->toolVar)).number - *target));
                        }
                        ranked.emplace_back(std::move(distances), tool);
                    }
                    std::stable_sort(ranked.begin(), ranked.end(),
                                     [](const auto& a, const auto& b) { return a.first < b.first; });
                    candidates.clear();
                    for (const auto& [d, tool] : ranked) {
                        candidates.push_back(tool);
                    }
                }
                if (candidates.empty()) {
                    const auto [it, isNew] = missingByRule.emplace(query->description, plan.missing.size());
                    if (isNew) {
                        plan.missing.push_back(MissingTool{ti, query->description, {}});
                    }
                    plan.missing[it->second].holes.push_back(h);
                    continue;
                }
                const lib::Tool& tool = *candidates.front();
                Vars vars = fv;
                for (auto& [name, v] : toolVars(tool)) {
                    vars.insert_or_assign(name, v);
                }

                Built b;
                b.tool = tool;
                Operation& op = b.op;
                op.type = t.opType;
                op.name = t.name;
                op.templateId = t.id.toString();
                op.tool.tool = tool.id;
                auto params = evaluateParams(t, schemas[ti], vars, ctx.evaluate);
                if (!params) {
                    return tl::unexpected(params.error());
                }
                op.paramsJson = std::move(*params);
                // Heights.
                op.heights = ctx.defaultHeights;
                for (const auto& [key, text] : r.heights) {
                    auto v = json::parse(text);
                    if (!v) {
                        return failure(ErrorCode::FormatError, "Template " + inQuotes(t.name) + ": heights." + key +
                                                                   " is not valid JSON");
                    }
                    auto x = numericValue(t, "heights." + key, *v, vars, FieldKind::Length, ctx.evaluate);
                    if (!x) {
                        return tl::unexpected(x.error());
                    }
                    double* slot = key == "clearance"   ? &op.heights.clearance
                                   : key == "retract"   ? &op.heights.retract
                                   : key == "feedPlane" ? &op.heights.feedPlane
                                   : key == "top"       ? &op.heights.top
                                                        : &op.heights.bottom;
                    *slot = *x;
                }
                // Cutting data.
                op.cutting.coolant = r.feeds.coolant.value_or(CuttingData::Coolant::Flood);
                op.cutting.clockwise = r.feeds.clockwise.value_or(true);
                if (r.feeds.calculated) {
                    if (ctx.material == nullptr) {
                        return failure(ErrorCode::InvalidArgument,
                                       "Template " + inQuotes(t.name) +
                                           " calculates its feeds, but the setup has no stock material: choose the "
                                           "material in the setup, or give the template fixed feeds");
                    }
                    const FeedsInput in{tool, *ctx.material, ctx.machine, 0, 0, t.opType, r.feeds.opClass};
                    auto feeds = computeFeeds(in);
                    if (!feeds) {
                        return failure(ErrorCode::InvalidArgument,
                                       "Template " + inQuotes(t.name) + ": the feeds of tool " + inQuotes(tool.name) +
                                           " in " + ctx.material->name + " cannot be calculated: " +
                                           feeds.error().message);
                    }
                    op.cutting.spindleRpm = feeds->rpm;
                    op.cutting.feedCut = feeds->feedMmMin;
                    op.cutting.feedPlunge = feeds->feedMmMin * 0.5; // the operation panel's "Use calculated"
                    op.cutting.feedRamp = feeds->feedMmMin * 0.75;
                    op.cutting.feedLeadIn = op.cutting.feedLeadOut = feeds->feedMmMin;
                    CalculatedFeeds record;
                    record.tool = tool.id;
                    record.material = ctx.material->id;
                    record.machine = ctx.machine.id;
                    record.opClass = r.feeds.opClass;
                    record.spindleRpm = op.cutting.spindleRpm;
                    record.feedCut = op.cutting.feedCut;
                    record.feedPlunge = op.cutting.feedPlunge;
                    record.feedRamp = op.cutting.feedRamp;
                    op.calculated = record;
                } else {
                    double* slots[] = {&op.cutting.spindleRpm, &op.cutting.feedCut, &op.cutting.feedPlunge,
                                       &op.cutting.feedRamp};
                    const char* keys[] = {"rpm", "feedCut", "feedPlunge", "feedRamp"};
                    for (std::size_t k = 0; k < 4; ++k) {
                        const auto it = r.feeds.fixed.find(keys[k]);
                        if (it == r.feeds.fixed.end()) {
                            *slots[k] = op.cutting.feedCut; // plunge and ramp: the cut feed
                            continue;
                        }
                        auto v = json::parse(it->second);
                        if (!v) {
                            return failure(ErrorCode::FormatError, "Template " + inQuotes(t.name) + ": feeds." +
                                                                       keys[k] + " is not valid JSON");
                        }
                        auto x = numericValue(t, std::string("feeds.") + keys[k], *v, vars, FieldKind::Unitless,
                                              ctx.evaluate);
                        if (!x) {
                            return tl::unexpected(x.error());
                        }
                        if (!(*x > 0)) {
                            return failure(ErrorCode::InvalidArgument, "Template " + inQuotes(t.name) + ", feeds." +
                                                                           keys[k] + ": must be more than 0");
                        }
                        *slots[k] = *x;
                    }
                    op.cutting.feedLeadIn = op.cutting.feedLeadOut = op.cutting.feedCut;
                }
                b.key = tool.id.value.toString() + "|" + op.paramsJson + "|" + detail::formatDouble(op.heights.clearance) +
                        "," + detail::formatDouble(op.heights.retract) + "," +
                        detail::formatDouble(op.heights.feedPlane) + "," + detail::formatDouble(op.heights.top) + "," +
                        detail::formatDouble(op.heights.bottom) + "|" + detail::formatDouble(op.cutting.spindleRpm) +
                        "," + detail::formatDouble(op.cutting.feedCut) + "," +
                        detail::formatDouble(op.cutting.feedPlunge);
                const auto same = std::find_if(built.begin(), built.end(), [&](const Built& x) { return x.key == b.key; });
                if (same != built.end()) {
                    same->op.geometry.push_back(holes[h].ref);
                    plan.operations[plan.operations.size() - built.size() +
                                    static_cast<std::size_t>(same - built.begin())]
                        .holes.push_back(h);
                    continue;
                }
                b.op.geometry.push_back(holes[h].ref);
                PlannedOperation p;
                p.templateIndex = ti;
                p.tool = b.tool;
                p.holes.push_back(h);
                plan.operations.push_back(std::move(p));
                built.push_back(std::move(b));
            }
            // This template's operations: their ops, and names that tell them apart when there are several.
            const std::size_t first = plan.operations.size() - built.size();
            for (std::size_t k = 0; k < built.size(); ++k) {
                PlannedOperation& p = plan.operations[first + k];
                p.op = std::move(built[k].op);
                if (built.size() > 1) {
                    p.op.name += " (" + sizeLabel(holes[p.holes.front()].hole) + ")";
                }
            }
        }
        for (std::size_t h = 0; h < holes.size(); ++h) {
            if (!touched[h]) {
                plan.untouched.push_back(h);
            }
        }
        return plan;
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::Internal, "Out of memory");
    }
}

std::string missingToolsMessage(const Plan& plan, std::span<const lib::OpTemplate> templates,
                                std::span<const PlanHole> holes)
{
    if (plan.missing.empty()) {
        return {};
    }
    std::string out = "Nothing was created: ";
    bool firstEntry = true;
    for (const MissingTool& m : plan.missing) {
        out += firstEntry ? "" : "; ";
        firstEntry = false;
        const std::string name = m.templateIndex < templates.size() ? templates[m.templateIndex].name : "?";
        out += "no tool in the libraries for " + inQuotes(name) + " (" + m.rule + ") for " +
               std::to_string(m.holes.size()) + (m.holes.size() == 1 ? " hole (" : " holes (");
        constexpr std::size_t kListed = 3;
        for (std::size_t i = 0; i < m.holes.size() && i < kListed; ++i) {
            out += (i == 0 ? "" : ", ") + describeHole(holes[m.holes[i]].hole);
        }
        if (m.holes.size() > kListed) {
            out += ", and " + std::to_string(m.holes.size() - kListed) + " more";
        }
        out += ")";
    }
    return out + ". Add the tool to a library, or change the template's tool rule.";
}

} // namespace ray::cam::templates
