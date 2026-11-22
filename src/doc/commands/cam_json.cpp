// The JSON form of the CAM state (cam_json.h, task T-032).
#include "cam_json.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>

namespace ray::doc::camjson {

using json::Member;
using json::Value;

constexpr int kSchema = 1;

// ---------- Source ----------

tl::unexpected<Error> Source::error(std::string_view field, const std::string& what) const
{
    return error("'" + std::string(field) + "' " + what);
}

tl::unexpected<Error> Source::error(const std::string& what) const
{
    if (kind == Kind::Stored) {
        return tl::unexpected(Error{ErrorCode::FormatError, "Invalid document: " + where + ": " + what, {}});
    }
    return tl::unexpected(Error{ErrorCode::InvalidArgument, where + ": " + what, {}});
}

namespace {

#define RAY_CAMJSON_TRY(var, expr)                                                                                     \
    auto var = (expr);                                                                                                 \
    if (!var) {                                                                                                        \
        return tl::unexpected(var.error());                                                                            \
    }

#define RAY_CAMJSON_CHECK(expr)                                                                                        \
    do {                                                                                                               \
        if (auto ray_camjson_status_ = (expr); !ray_camjson_status_) {                                                 \
            return tl::unexpected(ray_camjson_status_.error());                                                        \
        }                                                                                                              \
    } while (false)

std::string join(std::string_view parent, std::string_view key)
{
    return parent.empty() ? std::string(key) : std::string(parent) + "." + std::string(key);
}

Value text(std::string s)
{
    return Value::string(std::move(s));
}

Value idValue(const Uuid& id)
{
    return text(id.toString());
}

Value nullableId(const Uuid& id)
{
    return id == Uuid{} ? Value() : idValue(id);
}

Value idArray(const std::vector<BodyId>& ids)
{
    Value::Array out;
    for (const BodyId& id : ids) {
        out.push_back(idValue(id.value));
    }
    return Value::array(std::move(out));
}

Hash256 hashOf(std::string_view bytes)
{
    Hasher hasher;
    hasher.bytes(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    return hasher.finish();
}

// One JSON object being read: typed members with messages that name the field ("cutting.rpm").
class Obj {
public:
    static Result<Obj> of(const Value& value, const Source& source, std::string field)
    {
        if (!value.isObject()) {
            return source.error(field.empty() ? source.where : field, "must be a JSON object");
        }
        return Obj(value, source, std::move(field));
    }

    const Value& value() const { return *value_; }
    const Value* get(std::string_view key) const { return value_->find(key); }
    std::string path(std::string_view key) const { return join(field_, key); }
    tl::unexpected<Error> error(std::string_view key, const std::string& what) const
    {
        return source_->error(path(key), what);
    }
    tl::unexpected<Error> error(const std::string& what) const
    {
        return field_.empty() ? source_->error(what) : source_->error(field_, what);
    }

    // Command arguments refuse keys this version does not know; a stored object keeps them (extrasOf).
    Status only(std::initializer_list<std::string_view> keys) const
    {
        if (source_->kind != Source::Kind::Args) {
            return {};
        }
        for (const Member& m : value_->asObject()) {
            if (std::find(keys.begin(), keys.end(), m.key) == keys.end()) {
                return error(m.key, "is not a known member");
            }
        }
        return {};
    }

    Result<std::optional<double>> optNumber(std::string_view key) const
    {
        const Value* v = get(key);
        if (v == nullptr) {
            return std::optional<double>();
        }
        if (!v->isNumber() || !std::isfinite(v->asNumber())) {
            return error(key, "must be a number");
        }
        return std::optional<double>(v->asNumber());
    }
    Result<double> number(std::string_view key, std::optional<double> fallback) const
    {
        RAY_CAMJSON_TRY(v, optNumber(key));
        if (*v) {
            return **v;
        }
        if (fallback) {
            return *fallback;
        }
        return error(key, "is required");
    }
    Result<std::optional<int>> optInt(std::string_view key, int min, int max) const
    {
        RAY_CAMJSON_TRY(v, optNumber(key));
        if (!*v) {
            return std::optional<int>();
        }
        const double d = **v;
        if (!(d >= min && d <= max) || static_cast<double>(static_cast<int>(d)) != d) {
            return error(key, "must be a whole number from " + std::to_string(min) + " to " + std::to_string(max));
        }
        return std::optional<int>(static_cast<int>(d));
    }
    Result<int> integer(std::string_view key, std::optional<int> fallback, int min, int max) const
    {
        RAY_CAMJSON_TRY(v, optInt(key, min, max));
        if (*v) {
            return **v;
        }
        if (fallback) {
            return *fallback;
        }
        return error(key, "is required");
    }
    Result<std::optional<std::string>> optString(std::string_view key) const
    {
        const Value* v = get(key);
        if (v == nullptr) {
            return std::optional<std::string>();
        }
        if (!v->isString()) {
            return error(key, "must be a string");
        }
        return std::optional<std::string>(v->asString());
    }
    Result<std::string> string(std::string_view key, std::optional<std::string> fallback) const
    {
        RAY_CAMJSON_TRY(v, optString(key));
        if (*v) {
            return std::move(**v);
        }
        if (fallback) {
            return std::move(*fallback);
        }
        return error(key, "is required");
    }
    Result<std::optional<bool>> optBool(std::string_view key) const
    {
        const Value* v = get(key);
        if (v == nullptr) {
            return std::optional<bool>();
        }
        if (!v->isBool()) {
            return error(key, "must be true or false");
        }
        return std::optional<bool>(v->asBool());
    }
    Result<bool> boolean(std::string_view key, std::optional<bool> fallback) const
    {
        RAY_CAMJSON_TRY(v, optBool(key));
        if (*v) {
            return **v;
        }
        if (fallback) {
            return *fallback;
        }
        return error(key, "is required");
    }
    // A lower-case UUID other than nil; with `nullable`, absent or null reads as the nil id.
    Result<Uuid> uuid(std::string_view key, bool nullable = false) const
    {
        const Value* v = get(key);
        if (v == nullptr || v->isNull()) {
            if (nullable) {
                return Uuid{};
            }
            return error(key, "is required");
        }
        return uuidOf(*v, path(key));
    }
    Result<Uuid> uuidOf(const Value& v, const std::string& field) const
    {
        if (!v.isString()) {
            return source_->error(field, "must be a lower-case UUID");
        }
        const Uuid id = Uuid::fromString(v.asString());
        if (id.toString() != v.asString() || id == Uuid{}) {
            return source_->error(field, "must be a lower-case UUID other than the nil one");
        }
        return id;
    }
    // "schema": absent or 1; a newer one is FormatTooNew for a stored object.
    Status schema() const
    {
        RAY_CAMJSON_TRY(s, optInt("schema", 0, 1 << 30));
        if (*s && **s > kSchema) {
            if (source_->kind == Source::Kind::Stored) {
                return tl::unexpected(Error{ErrorCode::FormatTooNew,
                                            "This document needs a newer RayCAD: " + source_->where + " has format " +
                                                std::to_string(**s) + ", this version reads format " +
                                                std::to_string(kSchema) + ".",
                                            {}});
            }
            return error("schema", "is newer than this RayCAD reads (" + std::to_string(kSchema) + ")");
        }
        if (*s && **s < kSchema) {
            return error("schema", "unknown schema " + std::to_string(**s));
        }
        return {};
    }
    Result<Obj> child(std::string_view key) const
    {
        const Value* v = get(key);
        if (v == nullptr) {
            return error(key, "is required");
        }
        return of(*v, *source_, path(key));
    }
    const Source& source() const { return *source_; }

private:
    Obj(const Value& value, const Source& source, std::string field)
        : value_(&value), source_(&source), field_(std::move(field))
    {
    }
    const Value* value_;
    const Source* source_;
    std::string field_;
};

template <class Enum, std::size_t N>
Result<Enum> enumOf(const Obj& o, std::string_view key, const std::pair<const char*, Enum> (&names)[N],
                    std::optional<Enum> fallback)
{
    RAY_CAMJSON_TRY(s, o.optString(key));
    if (!*s) {
        if (fallback) {
            return *fallback;
        }
        return o.error(key, "is required");
    }
    std::string allowed;
    for (const auto& [name, value] : names) {
        if (**s == name) {
            return value;
        }
        allowed += (allowed.empty() ? "" : ", ") + std::string(name);
    }
    return o.error(key, "must be one of " + allowed);
}

template <class Enum, std::size_t N>
const char* nameOf(Enum value, const std::pair<const char*, Enum> (&names)[N])
{
    for (const auto& [name, v] : names) {
        if (v == value) {
            return name;
        }
    }
    return names[0].first;
}

constexpr std::pair<const char*, cam::CuttingData::Coolant> kCoolants[] = {
    {"off", cam::CuttingData::Coolant::Off},
    {"flood", cam::CuttingData::Coolant::Flood},
    {"mist", cam::CuttingData::Coolant::Mist},
    {"throughTool", cam::CuttingData::Coolant::ThroughTool},
    {"air", cam::CuttingData::Coolant::Air}};
constexpr std::pair<const char*, cam::Wcs::OriginMode> kOrigins[] = {
    {"stockCorner", cam::Wcs::OriginMode::StockCorner},
    {"stockTopCenter", cam::Wcs::OriginMode::StockTopCenter},
    {"modelPoint", cam::Wcs::OriginMode::ModelPoint}};
constexpr std::pair<const char*, int> kAxes[] = {{"x", 0}, {"y", 1}, {"z", 2}};

Status nonNegative(const Obj& o, std::string_view key, double v)
{
    if (!(v >= 0)) {
        return o.error(key, "must not be negative");
    }
    return {};
}

Status positive(const Obj& o, std::string_view key, double v)
{
    if (!(v > 0)) {
        return o.error(key, "must be greater than 0");
    }
    return {};
}

Result<naming::TopoRef> readRef(const Value& value, const Source& source, const std::string& field)
{
    auto ref = naming::topoRefFromJson(json::write(value));
    if (!ref) {
        std::string member = ref.error().context.empty() ? std::string() : "." + ref.error().context[0];
        return source.error(field + member, ref.error().message);
    }
    return std::move(*ref);
}

} // namespace

// ---------- Unknown keys ----------

namespace {
Value extrasValue(const Value& original, const Value& known)
{
    Value::Object out;
    for (const Member& m : original.asObject()) {
        const Value* k = known.find(m.key);
        if (k == nullptr) {
            out.push_back(m);
        } else if (m.value.isObject() && k->isObject()) {
            Value nested = extrasValue(m.value, *k);
            if (!nested.asObject().empty()) {
                out.push_back(Member{m.key, std::move(nested)});
            }
        }
    }
    return Value::object(std::move(out));
}

Value merged(const Value& known, const Value& extras)
{
    Value::Object out = known.asObject();
    for (const Member& e : extras.asObject()) {
        auto it = std::find_if(out.begin(), out.end(), [&](const Member& m) { return m.key == e.key; });
        if (it == out.end()) {
            out.push_back(e);
        } else if (it->value.isObject() && e.value.isObject()) {
            it->value = merged(it->value, e.value);
        }
    }
    return Value::object(std::move(out));
}
} // namespace

std::string extrasOf(const Value& original, const Value& known)
{
    if (!original.isObject() || !known.isObject()) {
        return {};
    }
    Value extras = extrasValue(original, known);
    return extras.asObject().empty() ? std::string() : json::write(extras);
}

Value withExtras(Value known, const std::string& extraJson)
{
    if (extraJson.empty() || !known.isObject()) {
        return known;
    }
    auto extra = json::parse(extraJson);
    if (!extra || !extra->isObject()) {
        return known;
    }
    return merged(known, *extra);
}

// ---------- Values ----------

Result<cam::StockDef> readStock(const Value& value, const Source& source, std::string_view field)
{
    RAY_CAMJSON_TRY(o, Obj::of(value, source, std::string(field)));
    RAY_CAMJSON_TRY(type, o->string("type", std::nullopt));
    if (*type == "box") {
        RAY_CAMJSON_CHECK(o->only({"type", "xNeg", "xPos", "yNeg", "yPos", "zNeg", "zPos"}));
        cam::BoxStock box{};
        const std::pair<const char*, double*> members[] = {{"xNeg", &box.xNeg}, {"xPos", &box.xPos},
                                                           {"yNeg", &box.yNeg}, {"yPos", &box.yPos},
                                                           {"zNeg", &box.zNeg}, {"zPos", &box.zPos}};
        for (const auto& [key, slot] : members) {
            RAY_CAMJSON_TRY(v, o->number(key, 0.0));
            RAY_CAMJSON_CHECK(nonNegative(*o, key, *v));
            *slot = *v;
        }
        return cam::StockDef(box);
    }
    if (*type == "cylinder") {
        RAY_CAMJSON_CHECK(o->only({"type", "axis", "diameter", "endOffset", "length"}));
        cam::CylinderStock cyl{};
        RAY_CAMJSON_TRY(diameter, o->number("diameter", std::nullopt));
        RAY_CAMJSON_TRY(length, o->number("length", std::nullopt));
        RAY_CAMJSON_TRY(endOffset, o->number("endOffset", 0.0));
        RAY_CAMJSON_TRY(axis, enumOf(*o, "axis", kAxes, std::optional<int>(2)));
        RAY_CAMJSON_CHECK(positive(*o, "diameter", *diameter));
        RAY_CAMJSON_CHECK(positive(*o, "length", *length));
        RAY_CAMJSON_CHECK(nonNegative(*o, "endOffset", *endOffset));
        cyl.diameter = *diameter;
        cyl.length = *length;
        cyl.endOffset = *endOffset;
        cyl.axis = *axis;
        return cam::StockDef(cyl);
    }
    if (*type == "solid") {
        RAY_CAMJSON_CHECK(o->only({"type", "body"}));
        RAY_CAMJSON_TRY(body, o->uuid("body"));
        return cam::StockDef(cam::SolidStock{BodyId{*body}});
    }
    if (*type == "fromSetup") {
        RAY_CAMJSON_CHECK(o->only({"type", "setup"}));
        RAY_CAMJSON_TRY(setup, o->uuid("setup"));
        return cam::StockDef(cam::FromSetupStock{SetupId{*setup}});
    }
    return o->error("type", "must be one of box, cylinder, solid, fromSetup");
}

Value toJson(const cam::StockDef& stock)
{
    if (const auto* box = std::get_if<cam::BoxStock>(&stock)) {
        return Value::object({{"type", text("box")},
                              {"xNeg", Value::number(box->xNeg)},
                              {"xPos", Value::number(box->xPos)},
                              {"yNeg", Value::number(box->yNeg)},
                              {"yPos", Value::number(box->yPos)},
                              {"zNeg", Value::number(box->zNeg)},
                              {"zPos", Value::number(box->zPos)}});
    }
    if (const auto* cyl = std::get_if<cam::CylinderStock>(&stock)) {
        const int axis = cyl->axis >= 0 && cyl->axis <= 2 ? cyl->axis : 2;
        return Value::object({{"type", text("cylinder")},
                              {"axis", text(kAxes[axis].first)},
                              {"diameter", Value::number(cyl->diameter)},
                              {"endOffset", Value::number(cyl->endOffset)},
                              {"length", Value::number(cyl->length)}});
    }
    if (const auto* solid = std::get_if<cam::SolidStock>(&stock)) {
        return Value::object({{"type", text("solid")}, {"body", idValue(solid->body.value)}});
    }
    const auto& from = std::get<cam::FromSetupStock>(stock);
    return Value::object({{"type", text("fromSetup")}, {"setup", idValue(from.previous.value)}});
}

Result<cam::Wcs> readWcs(const Value& value, const Source& source, std::string_view field)
{
    RAY_CAMJSON_TRY(o, Obj::of(value, source, std::string(field)));
    RAY_CAMJSON_CHECK(o->only({"flipX", "flipZ", "origin", "originVertex", "workOffset", "xEdge", "zFace"}));
    cam::Wcs wcs{};
    // T-044: absent (every document written before them) means not flipped.
    RAY_CAMJSON_TRY(flipX, o->boolean("flipX", false));
    RAY_CAMJSON_TRY(flipZ, o->boolean("flipZ", false));
    wcs.flipX = *flipX;
    wcs.flipZ = *flipZ;
    wcs.originMode = cam::Wcs::OriginMode::StockCorner;
    if (o->get("origin") != nullptr) {
        RAY_CAMJSON_TRY(origin, o->child("origin"));
        RAY_CAMJSON_TRY(mode, enumOf(*origin, "mode", kOrigins, std::optional<cam::Wcs::OriginMode>()));
        wcs.originMode = *mode;
        if (*mode == cam::Wcs::OriginMode::StockCorner) {
            RAY_CAMJSON_CHECK(origin->only({"mode", "corner"}));
            RAY_CAMJSON_TRY(corner, origin->integer("corner", 0, 0, 7));
            wcs.stockCornerIndex = *corner;
        } else {
            RAY_CAMJSON_CHECK(origin->only({"mode"}));
        }
    }
    RAY_CAMJSON_TRY(workOffset, o->integer("workOffset", 1, 1, 99));
    wcs.workOffset = *workOffset;
    const std::pair<const char*, std::optional<naming::TopoRef>*> refs[] = {
        {"zFace", &wcs.zFace}, {"xEdge", &wcs.xEdge}, {"originVertex", &wcs.originVertex}};
    for (const auto& [key, slot] : refs) {
        const Value* v = o->get(key);
        if (v == nullptr || v->isNull()) {
            continue;
        }
        RAY_CAMJSON_TRY(ref, readRef(*v, source, o->path(key)));
        *slot = std::move(*ref);
    }
    if (wcs.zFace && wcs.zFace->kind != kernel::SubKind::Face) {
        return o->error("zFace", "must be a face");
    }
    if (wcs.xEdge && wcs.xEdge->kind != kernel::SubKind::Edge) {
        return o->error("xEdge", "must be an edge");
    }
    if (wcs.originVertex && wcs.originVertex->kind != kernel::SubKind::Vertex) {
        return o->error("originVertex", "must be a vertex");
    }
    if (wcs.originMode == cam::Wcs::OriginMode::ModelPoint && !wcs.originVertex) {
        return o->error("originVertex", "is required when the origin mode is modelPoint");
    }
    return wcs;
}

Value refToJson(const naming::TopoRef& ref)
{
    auto parsed = json::parse(naming::toJson(ref));
    return parsed ? std::move(*parsed) : Value();
}

Value toJson(const std::vector<naming::TopoRef>& refs)
{
    Value::Array out;
    for (const naming::TopoRef& ref : refs) {
        out.push_back(refToJson(ref));
    }
    return Value::array(std::move(out));
}

Value toJson(const cam::Wcs& wcs)
{
    Value origin = wcs.originMode == cam::Wcs::OriginMode::StockCorner
                       ? Value::object({{"mode", text(nameOf(wcs.originMode, kOrigins))},
                                        {"corner", Value::number(wcs.stockCornerIndex)}})
                       : Value::object({{"mode", text(nameOf(wcs.originMode, kOrigins))}});
    Value::Object members{{"origin", std::move(origin)}, {"workOffset", Value::number(wcs.workOffset)}};
    // Written only when set, so a setup without flips is stored (and hashed) as before T-044.
    if (wcs.flipX) {
        members.push_back({"flipX", Value::boolean(true)});
    }
    if (wcs.flipZ) {
        members.push_back({"flipZ", Value::boolean(true)});
    }
    if (wcs.zFace) {
        members.push_back({"zFace", refToJson(*wcs.zFace)});
    }
    if (wcs.xEdge) {
        members.push_back({"xEdge", refToJson(*wcs.xEdge)});
    }
    if (wcs.originVertex) {
        members.push_back({"originVertex", refToJson(*wcs.originVertex)});
    }
    return Value::object(std::move(members));
}

Result<cam::ToolRef> readToolRef(const Value& value, const Source& source, std::string_view field,
                                 const cam::ToolRef* base, const cam::CamState* forDefaults)
{
    RAY_CAMJSON_TRY(o, Obj::of(value, source, std::string(field)));
    RAY_CAMJSON_CHECK(o->only({"diameterOffset", "holder", "lengthOffset", "number", "stickOut", "tool"}));
    cam::ToolRef ref = base ? *base : cam::ToolRef{};
    if (o->get("tool") != nullptr || base == nullptr) {
        RAY_CAMJSON_TRY(tool, o->uuid("tool"));
        ref.tool = ToolId{*tool};
    }
    if (o->get("holder") != nullptr || base == nullptr) {
        RAY_CAMJSON_TRY(holder, o->uuid("holder", true));
        ref.holder = HolderId{*holder};
    }
    // Defaults of a new reference come from the embedded tool when it is there.
    const lib::Tool* tool = nullptr;
    if (forDefaults != nullptr) {
        if (const auto t = forDefaults->tools.find(ref.tool); t != forDefaults->tools.end()) {
            tool = &t->second.item;
        }
    }
    const int defaultNumber = tool != nullptr && tool->defaultToolNumber > 0 ? tool->defaultToolNumber : 1;
    RAY_CAMJSON_TRY(number, o->optInt("number", 0, 99999));
    if (*number || base == nullptr) {
        ref.toolNumber = number->value_or(defaultNumber);
    }
    RAY_CAMJSON_TRY(lengthOffset, o->optInt("lengthOffset", 0, 99999));
    if (*lengthOffset || base == nullptr) {
        ref.lengthOffset = lengthOffset->value_or(ref.toolNumber);
    }
    RAY_CAMJSON_TRY(diameterOffset, o->optInt("diameterOffset", 0, 99999));
    if (*diameterOffset || base == nullptr) {
        ref.diameterOffset = diameterOffset->value_or(ref.toolNumber);
    }
    RAY_CAMJSON_TRY(stickOut, o->optNumber("stickOut"));
    if (*stickOut || base == nullptr) {
        const double fallback =
            tool != nullptr ? (tool->shoulderLength > 0 ? tool->shoulderLength : tool->fluteLength) : 0.0;
        ref.stickOut = stickOut->value_or(fallback);
        RAY_CAMJSON_CHECK(nonNegative(*o, "stickOut", ref.stickOut));
    }
    return ref;
}

Value toJson(const cam::ToolRef& ref)
{
    return Value::object({{"diameterOffset", Value::number(ref.diameterOffset)},
                          {"holder", nullableId(ref.holder.value)},
                          {"lengthOffset", Value::number(ref.lengthOffset)},
                          {"number", Value::number(ref.toolNumber)},
                          {"stickOut", Value::number(ref.stickOut)},
                          {"tool", idValue(ref.tool.value)}});
}

Result<cam::CuttingData> readCutting(const Value& value, const Source& source, std::string_view field,
                                     const cam::CuttingData* base)
{
    RAY_CAMJSON_TRY(o, Obj::of(value, source, std::string(field)));
    RAY_CAMJSON_CHECK(
        o->only({"clockwise", "coolant", "feedCut", "feedLeadIn", "feedLeadOut", "feedPlunge", "feedRamp", "rpm"}));
    cam::CuttingData c = base ? *base : cam::CuttingData{};
    if (base == nullptr) {
        c.coolant = cam::CuttingData::Coolant::Off;
    }
    const auto num = [&](std::string_view key, double& slot, std::optional<double> fallback) -> Status {
        RAY_CAMJSON_TRY(v, o->optNumber(key));
        if (*v) {
            slot = **v;
        } else if (base == nullptr) {
            if (!fallback) {
                return o->error(key, "is required");
            }
            slot = *fallback;
        }
        return positive(*o, key, slot);
    };
    RAY_CAMJSON_CHECK(num("rpm", c.spindleRpm, std::nullopt));
    RAY_CAMJSON_CHECK(num("feedCut", c.feedCut, std::nullopt));
    RAY_CAMJSON_CHECK(num("feedPlunge", c.feedPlunge, std::nullopt));
    RAY_CAMJSON_CHECK(num("feedRamp", c.feedRamp, c.feedCut));
    RAY_CAMJSON_CHECK(num("feedLeadIn", c.feedLeadIn, c.feedCut));
    RAY_CAMJSON_CHECK(num("feedLeadOut", c.feedLeadOut, c.feedCut));
    RAY_CAMJSON_TRY(clockwise, o->optBool("clockwise"));
    if (*clockwise) {
        c.clockwise = **clockwise;
    } else if (base == nullptr) {
        c.clockwise = true;
    }
    if (o->get("coolant") != nullptr) {
        RAY_CAMJSON_TRY(coolant, enumOf(*o, "coolant", kCoolants, std::optional<cam::CuttingData::Coolant>()));
        c.coolant = *coolant;
    }
    return c;
}

Value toJson(const cam::CuttingData& c)
{
    return Value::object({{"clockwise", Value::boolean(c.clockwise)},
                          {"coolant", text(nameOf(c.coolant, kCoolants))},
                          {"feedCut", Value::number(c.feedCut)},
                          {"feedLeadIn", Value::number(c.feedLeadIn)},
                          {"feedLeadOut", Value::number(c.feedLeadOut)},
                          {"feedPlunge", Value::number(c.feedPlunge)},
                          {"feedRamp", Value::number(c.feedRamp)},
                          {"rpm", Value::number(c.spindleRpm)}});
}

Result<cam::Heights> readHeights(const Value& value, const Source& source, std::string_view field,
                                 const cam::Heights* base)
{
    RAY_CAMJSON_TRY(o, Obj::of(value, source, std::string(field)));
    RAY_CAMJSON_CHECK(o->only({"bottom", "clearance", "feedPlane", "retract", "top"}));
    cam::Heights z = base ? *base : cam::Heights{};
    const std::pair<const char*, double*> members[] = {{"clearance", &z.clearance},
                                                       {"retract", &z.retract},
                                                       {"feedPlane", &z.feedPlane},
                                                       {"top", &z.top},
                                                       {"bottom", &z.bottom}};
    for (const auto& [key, slot] : members) {
        RAY_CAMJSON_TRY(v, o->optNumber(key));
        if (*v) {
            *slot = **v;
        } else if (base == nullptr) {
            return o->error(key, "is required");
        }
    }
    if (source.kind == Source::Kind::Args) {
        if (!(z.clearance >= z.retract && z.retract >= z.feedPlane && z.feedPlane >= z.top)) {
            return o->error("must be ordered clearance >= retract >= feedPlane >= top");
        }
        if (!(z.top >= z.bottom)) {
            return o->error("bottom", "must not be above top");
        }
    }
    return z;
}

Value toJson(const cam::Heights& z)
{
    return Value::object({{"bottom", Value::number(z.bottom)},
                          {"clearance", Value::number(z.clearance)},
                          {"feedPlane", Value::number(z.feedPlane)},
                          {"retract", Value::number(z.retract)},
                          {"top", Value::number(z.top)}});
}

// T-045: what "Use calculated" wrote (cam.h CalculatedFeeds). Ids: tool required, material and machine nullable;
// opClass "" by default; surfaceSpeed and chipLoad absent or null when not given; the values written non-negative.
Result<cam::CalculatedFeeds> readCalculatedFeeds(const Value& value, const Source& source, std::string_view field)
{
    RAY_CAMJSON_TRY(o, Obj::of(value, source, std::string(field)));
    RAY_CAMJSON_CHECK(o->only({"chipLoad", "feedCut", "feedPlunge", "feedRamp", "machine", "material", "opClass", "rpm",
                               "surfaceSpeed", "tool"}));
    cam::CalculatedFeeds c;
    RAY_CAMJSON_TRY(tool, o->uuid("tool"));
    RAY_CAMJSON_TRY(material, o->uuid("material", true));
    RAY_CAMJSON_TRY(machine, o->uuid("machine", true));
    RAY_CAMJSON_TRY(opClass, o->string("opClass", std::string()));
    c.tool = ToolId{*tool};
    c.material = MaterialId{*material};
    c.machine = MachineId{*machine};
    c.opClass = std::move(*opClass);
    for (const auto& [key, slot] : {std::pair<const char*, std::optional<double>*>{"surfaceSpeed", &c.surfaceSpeedMMin},
                                    std::pair<const char*, std::optional<double>*>{"chipLoad", &c.chipLoadMm}}) {
        if (const Value* v = o->get(key); v != nullptr && !v->isNull()) {
            RAY_CAMJSON_TRY(n, o->number(key, std::nullopt));
            *slot = *n;
        }
    }
    for (const auto& [key, slot] :
         {std::pair<const char*, double*>{"rpm", &c.spindleRpm}, std::pair<const char*, double*>{"feedCut", &c.feedCut},
          std::pair<const char*, double*>{"feedPlunge", &c.feedPlunge},
          std::pair<const char*, double*>{"feedRamp", &c.feedRamp}}) {
        RAY_CAMJSON_TRY(n, o->number(key, 0.0));
        RAY_CAMJSON_CHECK(nonNegative(*o, key, *n));
        *slot = *n;
    }
    return c;
}

Value toJson(const cam::CalculatedFeeds& c)
{
    const auto optional = [](const std::optional<double>& v) {
        return v ? Value::number(*v) : Value();
    };
    return Value::object({{"chipLoad", optional(c.chipLoadMm)},
                          {"feedCut", Value::number(c.feedCut)},
                          {"feedPlunge", Value::number(c.feedPlunge)},
                          {"feedRamp", Value::number(c.feedRamp)},
                          {"machine", nullableId(c.machine.value)},
                          {"material", nullableId(c.material.value)},
                          {"opClass", text(c.opClass)},
                          {"rpm", Value::number(c.spindleRpm)},
                          {"surfaceSpeed", optional(c.surfaceSpeedMMin)},
                          {"tool", idValue(c.tool.value)}});
}

Result<std::vector<naming::TopoRef>> readGeometry(const Value& value, const Source& source, std::string_view field)
{
    if (!value.isArray()) {
        return source.error(field, "must be an array of references");
    }
    std::vector<naming::TopoRef> out;
    for (std::size_t i = 0; i < value.asArray().size(); ++i) {
        RAY_CAMJSON_TRY(ref, readRef(value.asArray()[i], source, std::string(field) + "[" + std::to_string(i) + "]"));
        out.push_back(std::move(*ref));
    }
    return out;
}

Result<std::vector<BodyId>> readBodies(const Value& value, const Source& source, std::string_view field)
{
    if (!value.isArray()) {
        return source.error(field, "must be an array of body ids");
    }
    std::vector<BodyId> out;
    std::set<Uuid> seen;
    for (std::size_t i = 0; i < value.asArray().size(); ++i) {
        const Value& v = value.asArray()[i];
        const std::string where = std::string(field) + "[" + std::to_string(i) + "]";
        if (!v.isString()) {
            return source.error(where, "must be a lower-case UUID");
        }
        const Uuid id = Uuid::fromString(v.asString());
        if (id.toString() != v.asString() || id == Uuid{}) {
            return source.error(where, "must be a lower-case UUID other than the nil one");
        }
        if (!seen.insert(id).second) {
            return source.error(where, "lists body " + id.toString() + " twice");
        }
        out.push_back(BodyId{id});
    }
    return out;
}

// ---------- Embedded library items ----------

namespace {

// One item through the library's codec (cam.h embeddedJson and friends): FormatError / FormatTooNew of a stored item
// become the document's errors, an argument's become InvalidArgument naming the field.
template <class E, class Parse>
Result<E> itemFromText(std::string_view text, const Source& source, std::string_view field, Parse parse)
{
    auto item = parse(text);
    if (!item) {
        const Error& e = item.error();
        if (source.kind == Source::Kind::Stored && e.code == ErrorCode::FormatTooNew) {
            return tl::unexpected(Error{
                ErrorCode::FormatTooNew, "This document needs a newer RayCAD: " + source.where + ": " + e.message, {}});
        }
        if (field.empty()) {
            return source.error(e.message);
        }
        return source.error(field, e.message);
    }
    return std::move(*item);
}

template <class E, class Parse>
Result<E> itemFromValue(const Value& value, const Source& source, std::string_view field, Parse parse)
{
    if (!value.isObject()) {
        return source.error(field.empty() ? source.where : std::string(field), "must be a JSON object");
    }
    return itemFromText<E>(json::write(value), source, field, parse);
}

} // namespace

Result<cam::Embedded<lib::Tool>> readTool(const Value& value, const Source& source, std::string_view field)
{
    return itemFromValue<cam::Embedded<lib::Tool>>(value, source, field, &cam::embeddedToolFromJson);
}

Result<cam::Embedded<lib::Holder>> readHolder(const Value& value, const Source& source, std::string_view field)
{
    return itemFromValue<cam::Embedded<lib::Holder>>(value, source, field, &cam::embeddedHolderFromJson);
}

Result<cam::Embedded<lib::Material>> readMaterial(const Value& value, const Source& source, std::string_view field)
{
    return itemFromValue<cam::Embedded<lib::Material>>(value, source, field, &cam::embeddedMaterialFromJson);
}

Result<cam::Embedded<lib::MachineDef>> readMachine(const Value& value, const Source& source, std::string_view field)
{
    return itemFromValue<cam::Embedded<lib::MachineDef>>(value, source, field, &cam::embeddedMachineFromJson);
}

// ---------- Whole objects ----------

Value toJson(const cam::Setup& s)
{
    Value::Array ops;
    for (const OperationId& id : s.operations) {
        ops.push_back(idValue(id.value));
    }
    Value::Object members{{"fixtureBodies", idArray(s.fixtureBodies)},
                          {"id", idValue(s.id.value)},
                          {"machine", nullableId(s.machine.value)},
                          {"name", text(s.name)},
                          {"operations", Value::array(std::move(ops))},
                          {"partBodies", idArray(s.partBodies)},
                          {"post", text(s.postId)},
                          {"programComment", text(s.programComment)},
                          {"programNumber", Value::number(s.programNumber)},
                          {"stock", toJson(s.stock)},
                          {"wcs", toJson(s.wcs)}};
    // T-045: written only when set, so a setup without one keeps the bytes (and hash) it had before the member.
    if (s.material.value != Uuid{}) {
        members.push_back({"material", idValue(s.material.value)});
    }
    Value known = Value::object(std::move(members));
    return withExtras(std::move(known), s.extraJson);
}

Result<cam::Setup> readSetup(const Value& value, const Source& source)
{
    RAY_CAMJSON_TRY(o, Obj::of(value, source, ""));
    cam::Setup s;
    RAY_CAMJSON_TRY(id, o->uuid("id"));
    RAY_CAMJSON_TRY(name, o->string("name", std::nullopt));
    RAY_CAMJSON_TRY(machine, o->uuid("machine", true));
    RAY_CAMJSON_TRY(material, o->uuid("material", true)); // T-045; absent in older documents
    RAY_CAMJSON_TRY(post, o->string("post", std::string()));
    RAY_CAMJSON_TRY(comment, o->string("programComment", std::string()));
    RAY_CAMJSON_TRY(number, o->integer("programNumber", 1001, 0, 99999999));
    s.id = SetupId{*id};
    s.name = std::move(*name);
    s.machine = MachineId{*machine};
    s.material = MaterialId{*material};
    s.postId = std::move(*post);
    s.programComment = std::move(*comment);
    s.programNumber = *number;
    const Value empty = Value::array({});
    const Value* parts = o->get("partBodies");
    RAY_CAMJSON_TRY(partBodies, readBodies(parts ? *parts : empty, source, "partBodies"));
    const Value* fixtures = o->get("fixtureBodies");
    RAY_CAMJSON_TRY(fixtureBodies, readBodies(fixtures ? *fixtures : empty, source, "fixtureBodies"));
    s.partBodies = std::move(*partBodies);
    s.fixtureBodies = std::move(*fixtureBodies);
    const Value* stock = o->get("stock");
    if (stock == nullptr) {
        s.stock = cam::BoxStock{};
    } else {
        RAY_CAMJSON_TRY(read, readStock(*stock, source, "stock"));
        s.stock = std::move(*read);
    }
    const Value emptyObject = Value::object({});
    const Value* wcs = o->get("wcs");
    RAY_CAMJSON_TRY(readW, readWcs(wcs ? *wcs : emptyObject, source, "wcs"));
    s.wcs = std::move(*readW);
    if (const Value* ops = o->get("operations")) {
        if (!ops->isArray()) {
            return o->error("operations", "must be an array of operation ids");
        }
        for (std::size_t i = 0; i < ops->asArray().size(); ++i) {
            RAY_CAMJSON_TRY(op, o->uuidOf(ops->asArray()[i], "operations[" + std::to_string(i) + "]"));
            s.operations.push_back(OperationId{*op});
        }
    }
    s.extraJson = extrasOf(value, toJson(s));
    return s;
}

Value toJson(const cam::Operation& op)
{
    Value params = Value::object({});
    if (!op.paramsJson.empty()) {
        if (auto parsed = json::parse(op.paramsJson); parsed && parsed->isObject()) {
            params = std::move(*parsed);
        }
    }
    Value::Object members{{"cutting", toJson(op.cutting)},
                          {"geometry", toJson(op.geometry)},
                          {"heights", toJson(op.heights)},
                          {"id", idValue(op.id.value)},
                          {"name", text(op.name)},
                          {"params", std::move(params)},
                          {"suppressed", Value::boolean(op.suppressed)},
                          {"template", text(op.templateId)},
                          {"tool", toJson(op.tool)},
                          {"type", text(op.type)}};
    // T-045: written only when set, so an operation without one keeps its bytes (and hash).
    if (op.calculated) {
        members.push_back({"calculatedFeeds", toJson(*op.calculated)});
    }
    Value known = Value::object(std::move(members));
    return withExtras(std::move(known), op.extraJson);
}

Result<cam::Operation> readOperation(const Value& value, const Source& source)
{
    RAY_CAMJSON_TRY(o, Obj::of(value, source, ""));
    cam::Operation op;
    RAY_CAMJSON_TRY(id, o->uuid("id"));
    RAY_CAMJSON_TRY(type, o->string("type", std::nullopt));
    RAY_CAMJSON_TRY(name, o->string("name", std::nullopt));
    RAY_CAMJSON_TRY(templ, o->string("template", std::string()));
    RAY_CAMJSON_TRY(suppressed, o->boolean("suppressed", false));
    op.id = OperationId{*id};
    op.type = std::move(*type);
    op.name = std::move(*name);
    op.templateId = std::move(*templ);
    op.suppressed = *suppressed;
    RAY_CAMJSON_TRY(tool, o->child("tool"));
    RAY_CAMJSON_TRY(toolRef, readToolRef(tool->value(), source, "tool", nullptr, nullptr));
    op.tool = *toolRef;
    RAY_CAMJSON_TRY(cutting, o->child("cutting"));
    RAY_CAMJSON_TRY(cuttingData, readCutting(cutting->value(), source, "cutting", nullptr));
    op.cutting = *cuttingData;
    RAY_CAMJSON_TRY(heights, o->child("heights"));
    RAY_CAMJSON_TRY(heightsData, readHeights(heights->value(), source, "heights", nullptr));
    op.heights = *heightsData;
    if (const Value* geometry = o->get("geometry")) {
        RAY_CAMJSON_TRY(refs, readGeometry(*geometry, source, "geometry"));
        op.geometry = std::move(*refs);
    }
    if (const Value* params = o->get("params")) {
        if (!params->isObject()) {
            return o->error("params", "must be a JSON object");
        }
        op.paramsJson = json::write(*params);
    } else {
        op.paramsJson = "{}";
    }
    if (const Value* calculated = o->get("calculatedFeeds"); calculated != nullptr && !calculated->isNull()) {
        RAY_CAMJSON_TRY(read, readCalculatedFeeds(*calculated, source, "calculatedFeeds"));
        op.calculated = std::move(*read);
    }
    op.extraJson = extrasOf(value, toJson(op));
    return op;
}

// ---------- The CAM object and the embedded items in a snapshot ----------

namespace {

template <class Map>
Value writeKind(const Map& items, std::vector<std::pair<Hash256, std::string>>& objects)
{
    Value::Object out;
    for (const auto& [id, item] : items) {
        std::string bytes = cam::embeddedJson(item); // the library's file format (cam.h)
        const Hash256 hash = hashOf(bytes);
        out.push_back({id.value.toString(), text(hash.hex())});
        objects.emplace_back(hash, std::move(bytes));
    }
    return Value::object(std::move(out));
}

Result<Hash256> parseHash(const Value& value, const Source& source, const std::string& field)
{
    if (!value.isString() || value.asString().size() != 64) {
        return source.error(field, "must be a hash of 64 lower-case hex digits");
    }
    Hash256 hash;
    const std::string& s = value.asString();
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = s[i];
        int digit = 0;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else {
            return source.error(field, "must be a hash of 64 lower-case hex digits");
        }
        hash.bytes[i / 2] = static_cast<std::uint8_t>(hash.bytes[i / 2] * 16 + digit);
    }
    return hash;
}

// Fetches an object named in the manifest and checks it against its hash.
Result<Value> fetch(const Hash256& hash, const std::function<Result<std::string>(const Hash256&)>& object,
                    const std::string& what)
{
    auto bytes = object(hash);
    if (!bytes) {
        Error error = bytes.error();
        error.context.push_back("while reading " + what + " " + hash.hex());
        return tl::unexpected(error);
    }
    if (hashOf(*bytes) != hash) {
        return tl::unexpected(
            Error{ErrorCode::FormatError, "Invalid document: " + what + " does not match its hash " + hash.hex(), {}});
    }
    auto parsed = json::parse(*bytes);
    if (!parsed) {
        return tl::unexpected(Error{ErrorCode::FormatError,
                                    "Invalid document: " + what + " " + hash.hex() + ": " + parsed.error().message,
                                    {}});
    }
    return std::move(*parsed);
}

// Fetches an object named in the manifest and checks its bytes against its hash.
Result<std::string> fetchBytes(const Hash256& hash, const std::function<Result<std::string>(const Hash256&)>& object,
                               const std::string& what)
{
    auto bytes = object(hash);
    if (!bytes) {
        Error error = bytes.error();
        error.context.push_back("while reading " + what + " " + hash.hex());
        return tl::unexpected(error);
    }
    if (hashOf(*bytes) != hash) {
        return tl::unexpected(
            Error{ErrorCode::FormatError, "Invalid document: " + what + " does not match its hash " + hash.hex(), {}});
    }
    return std::move(*bytes);
}

template <class E, class Map, class Parse>
Status readKind(const Value& embedded, std::string_view kind, const char* what, Map& out,
                const std::function<Result<std::string>(const Hash256&)>& object, Parse parse)
{
    const Source manifest{Source::Kind::Stored, "manifest"};
    const Value* items = embedded.find(kind);
    if (items == nullptr) {
        return {};
    }
    if (!items->isObject()) {
        return manifest.error("embedded." + std::string(kind), "must be an object of id: hash");
    }
    for (const Member& m : items->asObject()) {
        const std::string field = "embedded." + std::string(kind) + "." + m.key;
        const Uuid id = Uuid::fromString(m.key);
        if (id.toString() != m.key || id == Uuid{}) {
            return manifest.error(field, "is not keyed by a lower-case UUID");
        }
        RAY_CAMJSON_TRY(hash, parseHash(m.value, manifest, field));
        RAY_CAMJSON_TRY(bytes, fetchBytes(*hash, object, what));
        const Source source{Source::Kind::Stored, std::string(what) + " " + hash->hex()};
        RAY_CAMJSON_TRY(item, itemFromText<E>(*bytes, source, std::string_view(), parse));
        if (item->item.id.value != id) {
            return manifest.error(field, "names an item whose id is " + item->item.id.value.toString());
        }
        out[item->item.id] = std::move(*item);
    }
    return {};
}

} // namespace

std::vector<Member> writeCam(const std::shared_ptr<const cam::CamState>& cam,
                             std::vector<std::pair<Hash256, std::string>>& objects)
{
    std::vector<Member> members;
    if (!cam || cam->empty()) {
        return members;
    }
    Value::Array setups;
    for (const cam::Setup& s : cam->setups) {
        setups.push_back(toJson(s));
    }
    Value::Array operations;
    for (const auto& [id, op] : cam->operations) {
        operations.push_back(toJson(op));
    }
    Value object = withExtras(Value::object({{"operations", Value::array(std::move(operations))},
                                             {"schema", Value::number(kSchema)},
                                             {"setups", Value::array(std::move(setups))}}),
                              cam->extraJson);
    std::string bytes = json::write(object);
    const Hash256 hash = hashOf(bytes);
    objects.emplace_back(hash, std::move(bytes));
    members.push_back({"cam", text(hash.hex())});
    members.push_back({"embedded", Value::object({{"holders", writeKind(cam->holders, objects)},
                                                  {"machines", writeKind(cam->machines, objects)},
                                                  {"materials", writeKind(cam->materials, objects)},
                                                  {"tools", writeKind(cam->tools, objects)}})});
    return members;
}

Result<std::shared_ptr<const cam::CamState>> readCam(const Value& manifest,
                                                     const std::function<Result<std::string>(const Hash256&)>& object)
{
    const Value* camHash = manifest.find("cam");
    const Value* embedded = manifest.find("embedded");
    if (camHash == nullptr && embedded == nullptr) {
        return std::shared_ptr<const cam::CamState>();
    }
    const Source manifestSource{Source::Kind::Stored, "manifest"};
    auto state = std::make_shared<cam::CamState>();
    if (camHash != nullptr) {
        RAY_CAMJSON_TRY(hash, parseHash(*camHash, manifestSource, "cam"));
        RAY_CAMJSON_TRY(value, fetch(*hash, object, "CAM object"));
        const Source source{Source::Kind::Stored, "CAM object " + hash->hex()};
        RAY_CAMJSON_TRY(o, Obj::of(*value, source, ""));
        RAY_CAMJSON_CHECK(o->schema());
        if (o->get("schema") == nullptr) {
            return o->error("schema", "is required");
        }
        const Value* setups = o->get("setups");
        const Value* operations = o->get("operations");
        if (setups == nullptr || !setups->isArray()) {
            return o->error("setups", "must be an array");
        }
        if (operations == nullptr || !operations->isArray()) {
            return o->error("operations", "must be an array");
        }
        std::set<Uuid> setupIds;
        for (std::size_t i = 0; i < setups->asArray().size(); ++i) {
            const Source where{Source::Kind::Stored, source.where + ": setups[" + std::to_string(i) + "]"};
            RAY_CAMJSON_TRY(setup, readSetup(setups->asArray()[i], where));
            if (!setupIds.insert(setup->id.value).second) {
                return where.error("duplicate setup id " + setup->id.value.toString());
            }
            state->setups.push_back(std::move(*setup));
        }
        for (std::size_t i = 0; i < operations->asArray().size(); ++i) {
            const Source where{Source::Kind::Stored, source.where + ": operations[" + std::to_string(i) + "]"};
            RAY_CAMJSON_TRY(op, readOperation(operations->asArray()[i], where));
            const OperationId id = op->id;
            if (!state->operations.emplace(id, std::move(*op)).second) {
                return where.error("duplicate operation id " + id.value.toString());
            }
        }
        // Every operation is listed by exactly one setup, and every listed id is an operation.
        std::set<OperationId> listed;
        for (const cam::Setup& s : state->setups) {
            for (const OperationId& id : s.operations) {
                if (state->operations.count(id) == 0) {
                    return source.error("setup " + s.id.value.toString() + " lists operation " + id.value.toString() +
                                        ", which is not in the CAM object");
                }
                if (!listed.insert(id).second) {
                    return source.error("operation " + id.value.toString() + " is listed by more than one setup");
                }
            }
        }
        if (listed.size() != state->operations.size()) {
            return source.error("an operation is listed by no setup");
        }
        Value::Object known;
        for (const char* key : {"operations", "schema", "setups"}) {
            if (const Value* v = o->get(key)) {
                known.push_back({key, *v});
            }
        }
        state->extraJson = extrasOf(*value, Value::object(std::move(known)));
    }
    if (embedded != nullptr) {
        if (!embedded->isObject()) {
            return manifestSource.error("embedded", "must be an object");
        }
        RAY_CAMJSON_CHECK(readKind<cam::Embedded<lib::Tool>>(*embedded, "tools", "tool", state->tools, object,
                                                             &cam::embeddedToolFromJson));
        RAY_CAMJSON_CHECK(readKind<cam::Embedded<lib::Holder>>(*embedded, "holders", "holder", state->holders, object,
                                                               &cam::embeddedHolderFromJson));
        RAY_CAMJSON_CHECK(readKind<cam::Embedded<lib::MachineDef>>(*embedded, "machines", "machine", state->machines,
                                                                   object, &cam::embeddedMachineFromJson));
        RAY_CAMJSON_CHECK(readKind<cam::Embedded<lib::Material>>(*embedded, "materials", "material", state->materials,
                                                                 object, &cam::embeddedMaterialFromJson));
    }
    return std::shared_ptr<const cam::CamState>(std::move(state));
}

#undef RAY_CAMJSON_TRY
#undef RAY_CAMJSON_CHECK

} // namespace ray::doc::camjson
