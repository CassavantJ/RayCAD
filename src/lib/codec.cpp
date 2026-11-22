// Library items to and from their JSON files (see codec.h), and the public toJson / *FromJson of libraries.h.
#include "codec.h"

#include "schema.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ray::lib::detail {
namespace {

using json::Member;
using json::Value;

constexpr std::array<KindInfo, 5> kKinds = {{
    {Kind::Tool, "tool", "tools", ".tool.json", "tool"},
    {Kind::Holder, "holder", "holders", ".holder.json", "holder"},
    {Kind::Material, "material", "materials", ".material.json", "material"},
    {Kind::Machine, "machine", "machines", ".machine.json", "machine"},
    {Kind::Template, "template", "templates", ".optemplate.json", "optemplate"},
}};

constexpr std::array<std::pair<ToolType, std::string_view>, 12> kToolTypes = {{
    {ToolType::FlatEndMill, "flatEndMill"},
    {ToolType::BallEndMill, "ballEndMill"},
    {ToolType::BullEndMill, "bullEndMill"},
    {ToolType::ChamferMill, "chamferMill"},
    {ToolType::FaceMill, "faceMill"},
    {ToolType::SpotDrill, "spotDrill"},
    {ToolType::Drill, "drill"},
    {ToolType::Tap, "tap"},
    {ToolType::Reamer, "reamer"},
    {ToolType::BoringBar, "boringBar"},
    {ToolType::ThreadMill, "threadMill"},
    {ToolType::EngraveCutter, "engraveCutter"},
}};

constexpr std::array<std::pair<MachineDef::Kind, std::string_view>, 4> kMachineKinds = {{
    {MachineDef::Kind::Mill3Axis, "mill3"},
    {MachineDef::Kind::Mill4Axis, "mill4"},
    {MachineDef::Kind::Mill5Axis, "mill5"},
    {MachineDef::Kind::Lathe, "lathe"},
}};

tl::unexpected<Error> bad(std::string message)
{
    return tl::unexpected<Error>(Error{ErrorCode::FormatError, std::move(message), {}});
}

Value num(double d)
{
    return Value::number(d);
}
Value str(std::string s)
{
    return Value::string(std::move(s));
}
Value boolean(bool b)
{
    return Value::boolean(b);
}
Value uuid(const Uuid& id)
{
    return Value::string(id.toString());
}

// Accessors for values that passed the schema: a missing optional key reads as the default.
double numberOr(const Value& object, std::string_view key, double fallback = 0)
{
    const Value* v = object.find(key);
    return v && v->isNumber() ? v->asNumber() : fallback;
}
int intOr(const Value& object, std::string_view key, int fallback = 0)
{
    return static_cast<int>(numberOr(object, key, fallback));
}
std::string stringOr(const Value& object, std::string_view key, std::string fallback = {})
{
    const Value* v = object.find(key);
    return v && v->isString() ? v->asString() : fallback;
}
bool boolOr(const Value& object, std::string_view key, bool fallback = false)
{
    const Value* v = object.find(key);
    return v && v->isBool() ? v->asBool() : fallback;
}
const Value& objectOr(const Value& object, std::string_view key)
{
    static const Value empty = Value::object({});
    const Value* v = object.find(key);
    return v && v->isObject() ? *v : empty;
}

Result<Uuid> idOf(const Value& object, std::string_view key)
{
    const Uuid id = Uuid::fromString(stringOr(object, key));
    if (id == Uuid{}) {
        return bad("/" + std::string(key) + ": the nil id is not allowed");
    }
    return id;
}

std::string fmt(double d)
{
    return json::write(Value::number(d));
}

// The known members, the comment when there is one, then the item's unknown keys (known keys win).
Value finish(Value::Object members, const std::string& comment, const std::string& extraJson)
{
    if (!comment.empty()) {
        members.push_back({"comment", str(comment)});
    }
    Value known = Value::object(std::move(members));
    if (extraJson.empty()) {
        return known;
    }
    auto extra = json::parse(extraJson);
    if (!extra || !extra->isObject()) {
        return known; // not an object: nothing a reader would have put there
    }
    return mergeUnknownKeys(*extra, std::move(known));
}

} // namespace

const KindInfo& kindInfo(Kind kind)
{
    return kKinds[static_cast<std::size_t>(kind)];
}

std::optional<Kind> kindFromName(std::string_view name)
{
    for (const KindInfo& k : kKinds) {
        if (k.name == name) {
            return k.kind;
        }
    }
    return std::nullopt;
}

std::string_view toolTypeName(ToolType type)
{
    for (const auto& [t, name] : kToolTypes) {
        if (t == type) {
            return name;
        }
    }
    return "flatEndMill";
}

std::optional<ToolType> toolTypeFromName(std::string_view name)
{
    for (const auto& [t, spelled] : kToolTypes) {
        if (spelled.size() == name.size()) {
            bool same = true;
            for (std::size_t i = 0; i < name.size() && same; ++i) {
                // Only the first letter may differ in case: "FlatEndMill" is the enum spelling.
                same = i == 0 ? std::tolower(static_cast<unsigned char>(name[i])) ==
                                    std::tolower(static_cast<unsigned char>(spelled[i]))
                              : name[i] == spelled[i];
            }
            if (same) {
                return t;
            }
        }
    }
    return std::nullopt;
}

// ---------- items to values ----------

Value toValue(const Tool& t)
{
    Value::Array presets;
    for (const ToolPreset& p : t.presets) {
        presets.push_back(Value::object({
            {"material", uuid(p.material.value)},
            {"opClass", str(p.opClass)},
            {"surfaceSpeed", num(p.surfaceSpeedMMin)},
            {"chipLoad", num(p.chipLoadMm)},
            {"maxRadialFraction", num(p.maxRadialFraction)},
            {"maxAxialFraction", num(p.maxAxialFraction)},
            {"rampAngle", num(p.rampAngleDeg)},
        }));
        if (!p.comment.empty()) {
            Value::Object members = presets.back().asObject();
            members.push_back({"comment", str(p.comment)});
            presets.back() = Value::object(std::move(members));
        }
    }
    return finish(
        {
            {"schema", num(kFormatVersion)},
            {"id", uuid(t.id.value)},
            {"name", str(t.name)},
            {"vendor", str(t.vendor)},
            {"productId", str(t.productId)},
            {"type", str(std::string(toolTypeName(t.type)))},
            {"flutes", num(t.flutes)},
            {"diameter", num(t.diameter)},
            {"shankDiameter", num(t.shankDiameter)},
            {"fluteLength", num(t.fluteLength)},
            {"shoulderLength", num(t.shoulderLength)},
            {"overallLength", num(t.overallLength)},
            {"cornerRadius", num(t.cornerRadius)},
            {"tipAngle", num(t.tipAngleDeg)},
            {"tipDiameter", num(t.tipDiameter)},
            {"threadPitch", num(t.threadPitch)},
            {"threadSpec", str(t.threadSpec)},
            {"bodyMaterial", str(t.bodyMaterial)},
            {"defaultToolNumber", num(t.defaultToolNumber)},
            {"presets", Value::array(std::move(presets))},
        },
        t.comment, t.extraJson);
}

Value toValue(const Holder& h)
{
    Value::Array segments;
    for (const HolderSegment& s : h.segments) {
        segments.push_back(Value::object({
            {"height", num(s.height)},
            {"lowerDiameter", num(s.lowerDiameter)},
            {"upperDiameter", num(s.upperDiameter)},
        }));
    }
    return finish(
        {
            {"schema", num(kFormatVersion)},
            {"id", uuid(h.id.value)},
            {"name", str(h.name)},
            {"vendor", str(h.vendor)},
            {"gaugeLength", num(h.gaugeLength)},
            {"segments", Value::array(std::move(segments))},
        },
        h.comment, h.extraJson);
}

Value toValue(const Material& m)
{
    return finish(
        {
            {"schema", num(kFormatVersion)},
            {"id", uuid(m.id.value)},
            {"name", str(m.name)},
            {"group", str(m.group)},
            {"hardnessHb", num(m.hardnessHb)},
            {"specificCuttingForce", num(m.specificCuttingForceNmm2)},
            {"density", num(m.densityGcm3)},
        },
        m.comment, m.extraJson);
}

Value toValue(const MachineDef& m)
{
    Value::Array axes;
    for (const AxisDef& a : m.axes) {
        axes.push_back(Value::object({
            {"letter", str(std::string(1, a.letter))},
            {"min", num(a.minTravel)},
            {"max", num(a.maxTravel)},
            {"rapid", num(a.rapidMmMin)},
            {"maxFeed", num(a.maxFeedMmMin)},
            {"accel", num(a.accelMmS2)},
        }));
    }
    std::string_view kind = "mill3";
    for (const auto& [k, name] : kMachineKinds) {
        if (k == m.kind) {
            kind = name;
        }
    }
    json::Value::Object properties;
    for (const auto& [key, text] : m.postPropertyOverrides) {
        auto parsed = json::parse(text);
        // A value that is not JSON text is kept as a string, so nothing a caller put there is lost.
        properties.push_back({key, parsed ? std::move(*parsed) : str(text)});
    }
    json::Value::Object control = {
        {"dialect", str(m.controlDialect)},
        {"defaultPost", str(m.defaultPost)},
        {"rigidTapping", boolean(m.rigidTapping)},
        {"maxBlockNumber", num(m.maxBlockNumber)},
        {"maxProgramNameLength", num(m.maxProgramNameLength)},
    };
    if (m.peckClearance != 0) { // optional: written only when known
        control.push_back({"peckClearance", num(m.peckClearance)});
    }
    return finish(
        {
            {"schema", num(kFormatVersion)},
            {"id", uuid(m.id.value)},
            {"name", str(m.name)},
            {"vendor", str(m.vendor)},
            {"model", str(m.model)},
            {"kind", str(std::string(kind))},
            {"verified", boolean(m.verified)},
            {"axes", Value::array(std::move(axes))},
            {"spindle", Value::object({
                            {"maxRpm", num(m.maxRpm)},
                            {"minRpm", num(m.minRpm)},
                            {"powerKw", num(m.spindlePowerKw)},
                            {"torqueNm", num(m.spindleTorqueNm)},
                        })},
            {"toolChanger", Value::object({
                                {"capacity", num(m.toolCapacity)},
                                {"changeSeconds", num(m.toolChangeSec)},
                            })},
            {"control", Value::object(std::move(control))},
            {"options", Value::object({
                            {"throughSpindleCoolant", boolean(m.throughSpindleCoolant)},
                            {"probe", boolean(m.hasProbe)},
                            {"toolSetter", boolean(m.hasToolSetter)},
                        })},
            {"postProperties", Value::object(std::move(properties))},
        },
        m.comment, m.extraJson);
}

Value toValue(const OpTemplate& t)
{
    const auto objectText = [](const std::string& text) {
        auto parsed = json::parse(text.empty() ? std::string_view("{}") : std::string_view(text));
        return parsed ? std::move(*parsed) : Value::string(text); // not an object: the schema check refuses it
    };
    return finish(
        {
            {"schema", num(kFormatVersion)},
            {"id", uuid(t.id)},
            {"name", str(t.name)},
            {"opType", str(t.opType)},
            {"featureKind", str(t.featureKind)},
            {"material", t.material ? uuid(t.material->value) : Value()},
            {"machine", t.machine ? uuid(t.machine->value) : Value()},
            {"toolQuery", objectText(t.toolQueryJson)},
            {"params", objectText(t.paramsJson)},
        },
        t.comment, t.extraJson);
}

// ---------- checks ----------

Status checkValue(Kind kind, const Value& value)
{
    if (!value.isObject()) {
        return bad("/: must be an object, not " + json::write(value).substr(0, 20));
    }
    if (const Value* version = value.find("schema");
        version && version->isNumber() && version->asNumber() > kFormatVersion) {
        return tl::unexpected<Error>(Error{ErrorCode::FormatTooNew,
                                           "/schema: format " + fmt(version->asNumber()) +
                                               " was written by a newer RayCAD (this one reads format " +
                                               std::to_string(kFormatVersion) + ")",
                                           {}});
    }
    return builtinSchema(kindInfo(kind).schema).validate(value);
}

Result<Value> parseChecked(Kind kind, std::string_view text)
{
    auto parsed = json::parse(text);
    if (!parsed) {
        return tl::unexpected<Error>(parsed.error());
    }
    if (auto st = checkValue(kind, *parsed); !st) {
        return tl::unexpected<Error>(st.error());
    }
    return parsed;
}

// ---------- values to items ----------

Result<Tool> decodeTool(const Value& v)
{
    Tool t{};
    auto id = idOf(v, "id");
    if (!id) {
        return tl::unexpected<Error>(id.error());
    }
    t.id = ToolId{*id};
    t.name = stringOr(v, "name");
    t.vendor = stringOr(v, "vendor");
    t.productId = stringOr(v, "productId");
    const auto type = toolTypeFromName(stringOr(v, "type"));
    if (!type) {
        return bad("/type: unknown tool type");
    }
    t.type = *type;
    t.flutes = intOr(v, "flutes");
    t.diameter = numberOr(v, "diameter");
    t.shankDiameter = numberOr(v, "shankDiameter");
    t.fluteLength = numberOr(v, "fluteLength");
    t.shoulderLength = numberOr(v, "shoulderLength");
    t.overallLength = numberOr(v, "overallLength");
    t.cornerRadius = numberOr(v, "cornerRadius");
    t.tipAngleDeg = numberOr(v, "tipAngle");
    t.tipDiameter = numberOr(v, "tipDiameter");
    t.threadPitch = numberOr(v, "threadPitch");
    t.threadSpec = stringOr(v, "threadSpec");
    t.bodyMaterial = stringOr(v, "bodyMaterial");
    t.defaultToolNumber = intOr(v, "defaultToolNumber");
    if (const Value* presets = v.find("presets")) {
        std::set<std::pair<Uuid, std::string>> seen;
        for (std::size_t i = 0; i < presets->asArray().size(); ++i) {
            const Value& p = presets->asArray()[i];
            auto material = idOf(p, "material");
            if (!material) {
                return bad("/presets/" + std::to_string(i) + material.error().message);
            }
            ToolPreset preset{};
            preset.material = MaterialId{*material};
            preset.opClass = stringOr(p, "opClass");
            preset.surfaceSpeedMMin = numberOr(p, "surfaceSpeed");
            preset.chipLoadMm = numberOr(p, "chipLoad");
            preset.maxRadialFraction = numberOr(p, "maxRadialFraction");
            preset.maxAxialFraction = numberOr(p, "maxAxialFraction");
            preset.rampAngleDeg = numberOr(p, "rampAngle");
            preset.comment = stringOr(p, "comment");
            if (!seen.insert({*material, preset.opClass}).second) {
                return bad("/presets/" + std::to_string(i) + ": a second preset for the same material and opClass");
            }
            t.presets.push_back(std::move(preset));
        }
    }
    if (t.fluteLength > t.overallLength) {
        return bad("/fluteLength: longer than the overall length");
    }
    if (t.shoulderLength > t.overallLength) {
        return bad("/shoulderLength: longer than the overall length");
    }
    if (t.cornerRadius > t.diameter / 2) {
        return bad("/cornerRadius: larger than half the diameter");
    }
    if (t.type == ToolType::Tap && t.threadPitch <= 0) {
        return bad("/threadPitch: a tap needs its thread pitch");
    }
    t.comment = stringOr(v, "comment");
    t.extraJson = unknownKeysOf(v, toValue(t));
    return t;
}

Result<Holder> decodeHolder(const Value& v)
{
    Holder h{};
    auto id = idOf(v, "id");
    if (!id) {
        return tl::unexpected<Error>(id.error());
    }
    h.id = HolderId{*id};
    h.name = stringOr(v, "name");
    h.vendor = stringOr(v, "vendor");
    h.gaugeLength = numberOr(v, "gaugeLength");
    for (const Value& s : v.find("segments")->asArray()) {
        h.segments.push_back({numberOr(s, "height"), numberOr(s, "lowerDiameter"), numberOr(s, "upperDiameter")});
    }
    h.comment = stringOr(v, "comment");
    h.extraJson = unknownKeysOf(v, toValue(h));
    return h;
}

Result<Material> decodeMaterial(const Value& v)
{
    Material m{};
    auto id = idOf(v, "id");
    if (!id) {
        return tl::unexpected<Error>(id.error());
    }
    m.id = MaterialId{*id};
    m.name = stringOr(v, "name");
    m.group = stringOr(v, "group");
    m.hardnessHb = numberOr(v, "hardnessHb");
    m.specificCuttingForceNmm2 = numberOr(v, "specificCuttingForce");
    m.densityGcm3 = numberOr(v, "density");
    m.comment = stringOr(v, "comment");
    m.extraJson = unknownKeysOf(v, toValue(m));
    return m;
}

Result<MachineDef> decodeMachine(const Value& v)
{
    MachineDef m{};
    auto id = idOf(v, "id");
    if (!id) {
        return tl::unexpected<Error>(id.error());
    }
    m.id = MachineId{*id};
    m.name = stringOr(v, "name");
    m.vendor = stringOr(v, "vendor");
    m.model = stringOr(v, "model");
    const std::string kind = stringOr(v, "kind");
    bool known = false;
    for (const auto& [k, name] : kMachineKinds) {
        if (name == kind) {
            m.kind = k;
            known = true;
        }
    }
    if (!known) {
        return bad("/kind: unknown machine kind");
    }
    m.verified = boolOr(v, "verified");
    std::set<char> letters;
    const auto& axes = v.find("axes")->asArray();
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const Value& a = axes[i];
        AxisDef axis{};
        axis.letter = stringOr(a, "letter", "X")[0];
        axis.minTravel = numberOr(a, "min");
        axis.maxTravel = numberOr(a, "max");
        axis.rapidMmMin = numberOr(a, "rapid");
        axis.maxFeedMmMin = numberOr(a, "maxFeed");
        axis.accelMmS2 = numberOr(a, "accel");
        if (axis.minTravel > axis.maxTravel) {
            return bad("/axes/" + std::to_string(i) + ": the minimum travel is above the maximum");
        }
        if (!letters.insert(axis.letter).second) {
            return bad("/axes/" + std::to_string(i) + ": axis " + std::string(1, axis.letter) + " is listed twice");
        }
        m.axes.push_back(axis);
    }
    const Value& spindle = objectOr(v, "spindle");
    m.maxRpm = numberOr(spindle, "maxRpm");
    m.minRpm = numberOr(spindle, "minRpm");
    m.spindlePowerKw = numberOr(spindle, "powerKw");
    m.spindleTorqueNm = numberOr(spindle, "torqueNm");
    if (m.minRpm > m.maxRpm) {
        return bad("/spindle/minRpm: above maxRpm");
    }
    const Value& changer = objectOr(v, "toolChanger");
    m.toolCapacity = intOr(changer, "capacity");
    m.toolChangeSec = numberOr(changer, "changeSeconds");
    const Value& control = objectOr(v, "control");
    m.controlDialect = stringOr(control, "dialect");
    m.defaultPost = stringOr(control, "defaultPost");
    m.rigidTapping = boolOr(control, "rigidTapping");
    m.maxBlockNumber = intOr(control, "maxBlockNumber");
    m.maxProgramNameLength = intOr(control, "maxProgramNameLength");
    m.peckClearance = numberOr(control, "peckClearance");
    const Value& options = objectOr(v, "options");
    m.throughSpindleCoolant = boolOr(options, "throughSpindleCoolant");
    m.hasProbe = boolOr(options, "probe");
    m.hasToolSetter = boolOr(options, "toolSetter");
    for (const Member& p : objectOr(v, "postProperties").asObject()) {
        m.postPropertyOverrides.emplace(p.key, json::write(p.value));
    }
    m.comment = stringOr(v, "comment");
    m.extraJson = unknownKeysOf(v, toValue(m));
    return m;
}

Result<OpTemplate> decodeTemplate(const Value& v)
{
    OpTemplate t{};
    auto id = idOf(v, "id");
    if (!id) {
        return tl::unexpected<Error>(id.error());
    }
    t.id = *id;
    t.name = stringOr(v, "name");
    t.opType = stringOr(v, "opType");
    t.featureKind = stringOr(v, "featureKind");
    for (const char* key : {"material", "machine"}) {
        const Value* ref = v.find(key);
        if (ref && ref->isString()) {
            auto refId = idOf(v, key);
            if (!refId) {
                return tl::unexpected<Error>(refId.error());
            }
            if (std::string_view(key) == "material") {
                t.material = MaterialId{*refId};
            } else {
                t.machine = MachineId{*refId};
            }
        }
    }
    t.toolQueryJson = json::write(*v.find("toolQuery"));
    t.paramsJson = json::write(*v.find("params"));
    t.comment = stringOr(v, "comment");
    t.extraJson = unknownKeysOf(v, toValue(t));
    return t;
}

Value mergeUnknownKeys(const Value& raw, Value known)
{
    if (!raw.isObject() || !known.isObject()) {
        return known;
    }
    Value::Object members = known.asObject();
    for (const Member& m : raw.asObject()) {
        auto it = std::find_if(members.begin(), members.end(), [&](const Member& k) { return k.key == m.key; });
        if (it == members.end()) {
            members.push_back(m);
        } else if (it->value.isObject() && m.value.isObject()) {
            it->value = mergeUnknownKeys(m.value, std::move(it->value));
        }
    }
    return Value::object(std::move(members));
}

namespace {
Value unknownMembers(const Value& original, const Value& known)
{
    Value::Object out;
    for (const Member& m : original.asObject()) {
        const Value* k = known.find(m.key);
        if (k == nullptr) {
            out.push_back(m);
        } else if (m.value.isObject() && k->isObject()) {
            Value nested = unknownMembers(m.value, *k);
            if (!nested.asObject().empty()) {
                out.push_back(Member{m.key, std::move(nested)});
            }
        }
    }
    return Value::object(std::move(out));
}
} // namespace

std::string unknownKeysOf(const Value& original, const Value& known)
{
    if (!original.isObject() || !known.isObject()) {
        return {};
    }
    Value extras = unknownMembers(original, known);
    return extras.asObject().empty() ? std::string() : json::write(extras);
}

} // namespace ray::lib::detail

// ---------- public ----------
namespace ray::lib {
namespace {

template <class T, class Decode>
Result<T> fromText(detail::Kind kind, std::string_view text, Decode decode)
{
    try {
        auto value = detail::parseChecked(kind, text);
        if (!value) {
            return tl::unexpected<Error>(value.error());
        }
        return decode(*value);
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(
            Error{ErrorCode::Internal, std::string("Cannot read a library item: ") + e.what(), {}});
    }
}

} // namespace

std::string toJson(const Tool& t)
{
    return json::write(detail::toValue(t));
}
std::string toJson(const Holder& h)
{
    return json::write(detail::toValue(h));
}
std::string toJson(const Material& m)
{
    return json::write(detail::toValue(m));
}
std::string toJson(const MachineDef& m)
{
    return json::write(detail::toValue(m));
}
std::string toJson(const OpTemplate& t)
{
    return json::write(detail::toValue(t));
}

Result<Tool> toolFromJson(std::string_view text)
{
    return fromText<Tool>(detail::Kind::Tool, text, detail::decodeTool);
}
Result<Holder> holderFromJson(std::string_view text)
{
    return fromText<Holder>(detail::Kind::Holder, text, detail::decodeHolder);
}
Result<Material> materialFromJson(std::string_view text)
{
    return fromText<Material>(detail::Kind::Material, text, detail::decodeMaterial);
}
Result<MachineDef> machineFromJson(std::string_view text)
{
    return fromText<MachineDef>(detail::Kind::Machine, text, detail::decodeMachine);
}
Result<OpTemplate> templateFromJson(std::string_view text)
{
    return fromText<OpTemplate>(detail::Kind::Template, text, detail::decodeTemplate);
}

Hash256 Tool::contentHash() const
{
    const std::string bytes = toJson(*this);
    Hasher hasher;
    hasher.bytes(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    return hasher.finish();
}

// The shape alone (libraries.h). Each hash starts with its own tag and version, so a tool and a holder never hash
// alike, and the field list can change in a later version without colliding with this one.
Hash256 Tool::geometryHash() const
{
    Hasher h;
    h.str("raycad.lib.tool-geometry.v1");
    h.str(detail::toolTypeName(type)).i64(flutes);
    h.f64(diameter).f64(shankDiameter).f64(fluteLength).f64(shoulderLength).f64(overallLength);
    h.f64(cornerRadius).f64(tipAngleDeg).f64(tipDiameter).f64(threadPitch);
    return h.finish();
}

Hash256 Holder::geometryHash() const
{
    Hasher h;
    h.str("raycad.lib.holder-geometry.v1");
    h.f64(gaugeLength).i64(static_cast<std::int64_t>(segments.size()));
    for (const HolderSegment& s : segments) {
        h.f64(s.height).f64(s.lowerDiameter).f64(s.upperDiameter);
    }
    return h.finish();
}

} // namespace ray::lib
