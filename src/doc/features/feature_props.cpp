// Shared helpers of the parametric feature types (feature_props.h).
#include "feature_props.h"

#include "../commands/args.h"

#include <ray/naming.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <utility>

namespace ray::doc::features {

using json::Member;
using json::Value;

std::string uuidSchema()
{
    return R"({"type":"string","pattern":"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"})";
}

std::string hashesSchema()
{
    return R"({"type":"array","items":{"type":"string","pattern":"^[0-9a-f]{64}$"}})";
}

std::string lengthSchema(std::string_view description)
{
    return R"({"type":["number","string"],"minLength":1,"x-unit":"length","description":")" + std::string(description) +
           R"json( (a number in mm, or an expression such as \"12 mm\", \"0.5 in\" or \"width / 2\")"})json";
}

std::string angleSchema(std::string_view description)
{
    return R"({"type":["number","string"],"minLength":1,"x-unit":"angle","description":")" + std::string(description) +
           R"json( (a number in radians, or an expression such as \"30 deg\")"})json";
}

std::string topoRefSchema()
{
    return R"({"type":"object","additionalProperties":false,"required":["body","kind","names"],"properties":{"body":)" +
           uuidSchema() + R"(,"kind":{"enum":["face","edge","vertex"]},"names":)" + hashesSchema() + R"(,"families":)" +
           hashesSchema() + R"(,"ambiguous":{"type":"array","items":{"type":"boolean"}},)" + R"("splitParents":)" +
           hashesSchema() + R"(,"neighbourNames":)" + hashesSchema() +
           R"(,"hint":{"type":"object"},"repairConfirmed":{"type":"boolean"},"territory":{"type":"object"},)" +
           R"("pickNeighbours":{"type":"array","items":{"type":"object"}}},)" +
           R"j("description":"A persistent reference to a face, edge or vertex (made by the model.reference action)"})j";
}

std::string planeSchema()
{
    return R"({"type":"object","minProperties":1,"maxProperties":1,"additionalProperties":false,"properties":{)" +
           std::string(R"("principal":{"enum":["xy","yz","xz"]},"datum":)") + uuidSchema() + R"(,"face":)" +
           topoRefSchema() + R"(},"description":"A principal plane, a datum plane feature or a planar face"})";
}

std::string objectSchema(std::string_view required, std::string_view properties)
{
    return R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)" +
           std::string(R"("required":[)") + std::string(required) + R"(],"properties":{)" + std::string(properties) +
           "}}";
}

tl::unexpected<Error> invalid(std::string message)
{
    return tl::unexpected(Error{ErrorCode::InvalidArgument, std::move(message), {}});
}

tl::unexpected<Error> failed(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

// ---------- PropsReader ----------

Result<Value> PropsReader::parse(std::string_view where, std::string_view propsJson)
{
    auto parsed = json::parse(propsJson.empty() ? std::string_view("{}") : propsJson);
    if (!parsed) {
        return invalid(std::string(where) + ": the props are not valid JSON: " + parsed.error().message);
    }
    if (!parsed->isObject()) {
        return invalid(std::string(where) + ": the props must be a JSON object");
    }
    return std::move(*parsed);
}

tl::unexpected<Error> PropsReader::error(std::string_view field, const std::string& what) const
{
    return invalid(where_ + ": '" + std::string(field) + "' " + what);
}

tl::unexpected<Error> PropsReader::error(const std::string& what) const
{
    return invalid(where_ + ": " + what);
}

Status PropsReader::onlyKeys(std::initializer_list<std::string_view> keys, std::string_view prefix) const
{
    for (const Member& m : object_->asObject()) {
        if (std::find(keys.begin(), keys.end(), m.key) == keys.end()) {
            return error(std::string(prefix) + m.key, "is not a property of this feature");
        }
    }
    return {};
}

Result<Uuid> PropsReader::uuid(std::string_view key, std::string_view field) const
{
    auto id = optUuid(key, field);
    if (!id) {
        return tl::unexpected(id.error());
    }
    if (!*id) {
        return error(field, "is required");
    }
    return **id;
}

Result<std::optional<Uuid>> PropsReader::optUuid(std::string_view key, std::string_view field) const
{
    const Value* v = object_->find(key);
    if (v == nullptr) {
        return std::optional<Uuid>();
    }
    if (!v->isString()) {
        return error(field, "must be a lower-case UUID");
    }
    const Uuid id = Uuid::fromString(v->asString());
    if (id.toString() != v->asString() || id == Uuid{}) {
        return error(field, "must be a lower-case UUID other than the nil one");
    }
    return std::optional<Uuid>(id);
}

Result<bool> PropsReader::optBool(std::string_view key, std::string_view field, bool fallback) const
{
    const Value* v = object_->find(key);
    if (v == nullptr) {
        return fallback;
    }
    if (!v->isBool()) {
        return error(field, "must be true or false");
    }
    return v->asBool();
}

Result<std::string> PropsReader::choice(std::string_view key, std::string_view field,
                                        std::initializer_list<std::string_view> choices,
                                        std::optional<std::string_view> fallback) const
{
    const Value* v = object_->find(key);
    if (v == nullptr) {
        if (fallback) {
            return std::string(*fallback);
        }
        return error(field, "is required");
    }
    std::string list;
    for (std::string_view c : choices) {
        if (v->isString() && v->asString() == c) {
            return std::string(c);
        }
        list += list.empty() ? "" : ", ";
        list += c;
    }
    return error(field, "must be one of " + list);
}

Result<Value> PropsReader::numeric(std::string_view key, std::string_view field, std::optional<Value> fallback) const
{
    const Value* v = object_->find(key);
    if (v == nullptr) {
        if (fallback) {
            return *fallback;
        }
        return error(field, "is required");
    }
    if (v->isNumber()) {
        if (!std::isfinite(v->asNumber())) {
            return error(field, "must be a finite number");
        }
        return *v;
    }
    if (v->isString() && !v->asString().empty()) {
        return *v;
    }
    return error(field, "must be a number (mm or radians) or an expression such as \"12 mm\" or \"width / 2\"");
}

Result<double> evaluateNumeric(const Value& value, const std::map<std::string, double>& params, Parameter::Kind kind,
                               std::string_view where, std::string_view field)
{
    if (value.isNumber()) {
        return value.asNumber();
    }
    if (!value.isString()) {
        return invalid(std::string(where) + ": '" + std::string(field) + "' must be a number or an expression");
    }
    auto v = evaluate(value.asString(), params, kind);
    if (!v) {
        return invalid(std::string(where) + ": '" + std::string(field) + "': " + v.error().message);
    }
    if (!std::isfinite(*v)) {
        return invalid(std::string(where) + ": '" + std::string(field) + "' does not evaluate to a finite value");
    }
    return *v;
}

DocumentParams documentParams(const DocState& state)
{
    DocumentParams out;
    auto values = evaluateParameters(state.parameters);
    if (values) {
        out.values = std::move(*values);
    } else {
        out.error = values.error().message;
    }
    return out;
}

Status checkNumeric(const Value& value, const DocumentParams& params, Parameter::Kind kind, std::string_view where,
                    std::string_view field)
{
    auto v = evaluateNumeric(value, params.values, kind, where, field);
    if (!v) {
        if (params.error) {
            return invalid(v.error().message + " (the document parameters do not evaluate: " + *params.error + ")");
        }
        return tl::unexpected(v.error());
    }
    return {};
}

// ---------- TopoRef as JSON ----------
// One implementation: naming's (naming.h toJson / topoRefFromJson). These wrappers move it in and out of the props'
// JSON values and name the field in the props' terms.

const char* subKindName(kernel::SubKind kind)
{
    switch (kind) {
    case kernel::SubKind::Vertex:
        return "vertex";
    case kernel::SubKind::Edge:
        return "edge";
    case kernel::SubKind::Face:
        return "face";
    case kernel::SubKind::Solid:
        return "solid";
    }
    return "face";
}

Value topoRefToJson(const naming::TopoRef& ref)
{
    // naming writes canonical JSON of integers, strings and booleans only: it always parses.
    auto parsed = json::parse(naming::toJson(ref));
    return parsed ? std::move(*parsed) : Value{};
}

Result<naming::TopoRef> topoRefFromJson(const PropsReader& r, const Value& value, const std::string& field)
{
    if (!value.isObject()) {
        return r.error(field, "must be a reference to a face, edge or vertex (a JSON object)");
    }
    auto ref = naming::topoRefFromJson(json::write(value));
    if (!ref) {
        const std::vector<std::string>& at = ref.error().context;
        return r.error(at.empty() ? field : field + "." + at.front(), ref.error().message);
    }
    return std::move(*ref);
}

// ---------- Frames ----------

Vec3 vadd(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 vsub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 vmul(Vec3 a, double s)
{
    return {a.x * s, a.y * s, a.z * s};
}
double vdot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 vcross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double vnorm(Vec3 a)
{
    return std::sqrt(vdot(a, a));
}
std::optional<Vec3> vunit(Vec3 a)
{
    const double n = vnorm(a);
    if (!(n > 1e-12) || !std::isfinite(n)) {
        return std::nullopt;
    }
    return vmul(a, 1.0 / n);
}

std::optional<kernel::Plane> principalPlane(std::string_view name)
{
    if (name == "xy") {
        return kernel::Plane{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}};
    }
    if (name == "yz") {
        return kernel::Plane{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    }
    if (name == "xz") {
        return kernel::Plane{{0, 0, 0}, {0, -1, 0}, {1, 0, 0}};
    }
    return std::nullopt;
}

Vec3 planeYDir(const kernel::Plane& plane)
{
    return vcross(plane.normal, plane.xDir);
}

Vec3 planePoint(const kernel::Plane& plane, Vec2 uv)
{
    return vadd(plane.origin, vadd(vmul(plane.xDir, uv.x), vmul(planeYDir(plane), uv.y)));
}

namespace {

// True / false: the point is inside the material / outside; nullopt: cannot tell. A tiny box at `p` touching the
// solid means inside (as src/api/measure.cpp decides it).
std::optional<bool> inMaterial(const kernel::Shape& body, Vec3 p, double probe)
{
    if (body.count(kernel::SubKind::Solid) == 0) {
        return std::nullopt;
    }
    const Vec3 h{probe, probe, probe};
    auto box = kernel::makeBox(vsub(p, h), vadd(p, h));
    if (!box) {
        return std::nullopt;
    }
    auto d = kernel::minDistance(body, kernel::SubId{kernel::SubKind::Solid, 0}, *box,
                                 kernel::SubId{kernel::SubKind::Solid, 0});
    if (!d) {
        return std::nullopt;
    }
    return d->value <= probe * 1e-3;
}

// The outward normal of a planar face: SurfaceInfo's axis (flipped for a reversed face) is the surface normal only
// for a right-handed plane, so the material side is checked: the point just behind the face must be inside.
Vec3 outwardNormal(const kernel::Shape& shape, kernel::SubId face, const kernel::SurfaceInfo& s, Vec3 guess)
{
    const double size = vnorm(vsub(s.bounds.max, s.bounds.min));
    if (!(size > 0)) {
        return guess;
    }
    const double step = std::max(size * 1e-4, 1e-5);
    const double probe = step * 0.25;
    Vec3 on = s.centroid;
    const Vec3 h{probe, probe, probe};
    if (auto box = kernel::makeBox(vsub(on, h), vadd(on, h))) {
        if (auto d = kernel::minDistance(shape, face, *box, kernel::SubId{kernel::SubKind::Solid, 0})) {
            on = d->p1;
        }
    }
    const auto front = inMaterial(shape, vadd(on, vmul(guess, step)), probe);
    const auto back = inMaterial(shape, vsub(on, vmul(guess, step)), probe);
    if (front && back && *front && !*back) {
        return vmul(guess, -1.0);
    }
    return guess;
}

std::string brokenMessage(const naming::Resolved& r, const std::string& what, const char* element)
{
    if (r.state == naming::ResolveState::Repaired) {
        return what + ": the " + element + " it was placed on could not be found exactly; a likely match was found (" +
               r.reason + "). Confirm it in the reference review, or pick the " + element + " again.";
    }
    return what + ": the " + element + " it was placed on no longer exists" +
           (r.reason.empty() ? std::string(".") : " (" + r.reason + ").") + " Pick the " + element + " again.";
}

} // namespace

Result<kernel::Plane> facePlane(const RebuildContext& ctx, const naming::TopoRef& ref, const std::string& what)
{
    if (ref.kind != kernel::SubKind::Face) {
        return invalid(what + ": the reference must be a planar face, not a " + subKindName(ref.kind) + ".");
    }
    const naming::Resolved resolved = ctx.resolver.resolve(ref);
    if (!resolved.usable()) {
        return failed(ErrorCode::ReferenceBroken, brokenMessage(resolved, what, "face"));
    }
    auto shape = ctx.shapeOf(ref.body);
    if (!shape) {
        return failed(shape.error().code, what + ": " + shape.error().message);
    }
    std::optional<kernel::Plane> plane;
    for (const kernel::SubId& face : resolved.elements) {
        auto info = kernel::surfaceInfo(*shape, face);
        if (!info) {
            return failed(info.error().code, what + ": " + info.error().message);
        }
        if (info->type != kernel::SurfaceType::Plane) {
            return invalid(what + ": the face is not flat; a sketch or datum plane needs a planar face.");
        }
        const auto axis = vunit(info->axis);
        if (!axis) {
            return failed(ErrorCode::KernelFailure, what + ": the face has no usable normal.");
        }
        const Vec3 normal = outwardNormal(*shape, face, *info, info->reversed ? vmul(*axis, -1.0) : *axis);
        if (!plane) {
            const double d = vdot(info->centroid, normal);
            Vec3 x = vsub(Vec3{1, 0, 0}, vmul(normal, normal.x));
            if (vnorm(x) < 0.1) {
                x = vsub(Vec3{0, 1, 0}, vmul(normal, normal.y));
            }
            plane = kernel::Plane{vmul(normal, d), normal, *vunit(x)};
        } else if (vdot(normal, plane->normal) < 1 - 1e-9 ||
                   std::abs(vdot(vsub(info->centroid, plane->origin), plane->normal)) > 1e-6) {
            return failed(ErrorCode::ReferenceBroken,
                          what + ": the face was split into pieces that are no longer on one plane. Pick the face "
                                 "again.");
        }
    }
    return *plane;
}

Result<PlaneRef> readPlaneRef(const PropsReader& r, std::string_view key, std::string_view field)
{
    const Value* v = r.find(key);
    const std::string f(field);
    if (v == nullptr) {
        return r.error(f, "is required");
    }
    if (!v->isObject() || v->asObject().size() != 1) {
        return r.error(f, "must be {\"principal\": \"xy\" | \"yz\" | \"xz\"}, {\"datum\": \"<datum plane id>\"} or "
                          "{\"face\": <face reference>}");
    }
    const PropsReader p = r.child(*v);
    PlaneRef out;
    if (const Value* principal = v->find("principal")) {
        if (!principal->isString() || !principalPlane(principal->asString())) {
            return r.error(f + ".principal", "must be one of xy, yz, xz");
        }
        out.principal = principal->asString();
        return out;
    }
    if (v->find("datum") != nullptr) {
        auto id = p.uuid("datum", f + ".datum");
        if (!id) {
            return tl::unexpected(id.error());
        }
        out.datum = FeatureId{*id};
        return out;
    }
    if (const Value* face = v->find("face")) {
        auto ref = topoRefFromJson(r, *face, f + ".face");
        if (!ref) {
            return tl::unexpected(ref.error());
        }
        if (ref->kind != kernel::SubKind::Face) {
            return r.error(f + ".face", "must be a reference to a planar face");
        }
        out.face = std::move(*ref);
        return out;
    }
    return r.error(f, "must be {\"principal\": ...}, {\"datum\": ...} or {\"face\": ...}");
}

Result<kernel::Plane> resolvePlaneRef(const PlaneRef& ref, RebuildContext& ctx, const Feature& self,
                                      const std::string& what)
{
    if (!ref.principal.empty()) {
        return *principalPlane(ref.principal);
    }
    if (ref.face) {
        return facePlane(ctx, *ref.face, what);
    }
    auto datum = earlierFeature(ctx.state, self, *ref.datum, "datum_plane", "datum plane");
    if (!datum) {
        return tl::unexpected(datum.error());
    }
    const auto artifact = std::dynamic_pointer_cast<const PlaneArtifact>(detail::artifactOf(ctx, *ref.datum));
    if (!artifact) {
        return failed(ErrorCode::ReferenceBroken,
                      featureLabel(self) + ": the datum plane '" + featureLabel(**datum) + "' did not build.");
    }
    return artifact->plane;
}

std::string featureLabel(const Feature& feature)
{
    return feature.name.empty() ? feature.type : feature.name;
}

Result<const Feature*> earlierFeature(const DocState& state, const Feature& self, const FeatureId& id,
                                      std::string_view type, std::string_view role)
{
    const std::string me = featureLabel(self);
    bool beforeSelf = true;
    for (const Feature& f : state.features) {
        if (f.id == self.id) {
            beforeSelf = false;
            continue;
        }
        if (f.id != id) {
            continue;
        }
        if (f.type != type) {
            return invalid(me + ": '" + featureLabel(f) + "' is not a " + std::string(role) + "; pick a " +
                           std::string(role) + ".");
        }
        if (!beforeSelf) {
            return invalid(me + ": the " + std::string(role) + " '" + featureLabel(f) +
                           "' comes after it in the history; move it up or pick another " + std::string(role) + ".");
        }
        if (f.suppressed) {
            return invalid(me + ": the " + std::string(role) + " '" + featureLabel(f) +
                           "' is suppressed; unsuppress it or pick another " + std::string(role) + ".");
        }
        return &f;
    }
    return invalid(me + ": the " + std::string(role) + " it uses (" + id.value.toString() +
                   ") is not in the document; pick another " + std::string(role) + ".");
}

// ---------- Commands ----------

namespace {

constexpr std::string_view kUuidSchema =
    R"({"type":"string","pattern":"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"})";

const ParametricType* parametricType(std::string_view type, const IFeatureType** base)
{
    const auto registry = builtinFeatureRegistry();
    const IFeatureType* t = registry ? registry->find(type) : nullptr;
    if (base != nullptr) {
        *base = t;
    }
    return dynamic_cast<const ParametricType*>(t);
}

} // namespace

Result<std::string> storedPropsOf(const DocState& state, const Feature& feature, JobContext* job)
{
    const ParametricType* type = parametricType(feature.type, nullptr);
    if (type == nullptr) {
        return feature.propsJson;
    }
    auto stored = type->storedProps(state, feature, job);
    if (!stored && stored.error().code == ErrorCode::Cancelled) {
        return stored;
    }
    return stored ? std::move(*stored) : feature.propsJson;
}

namespace {

// Bodies other features of `state` (not `self`) create.
Status checkCreatedBodies(const DocState& state, const Feature& self, const std::vector<BodyId>& created,
                          const std::string& command)
{
    const auto registry = builtinFeatureRegistry();
    for (const Feature& f : state.features) {
        if (f.id == self.id || !registry) {
            continue;
        }
        const IFeatureType* t = registry->find(f.type);
        std::vector<BodyId> theirs;
        if (const auto* p = dynamic_cast<const ParametricType*>(t)) {
            theirs = p->createdBodies(f);
        } else if (const auto* d = dynamic_cast<const detail::BodyDeclaration*>(t)) {
            theirs = d->bodiesWritten(f);
        }
        for (const BodyId& b : created) {
            if (std::find(theirs.begin(), theirs.end(), b) != theirs.end()) {
                return invalid(command + ": the new body " + b.value.toString() + " is already made by '" +
                               featureLabel(f) + "'; give the new body an id of its own.");
            }
        }
    }
    return {};
}

void addBodies(DocState& state, const std::vector<BodyId>& created, const std::optional<std::string>& bodyName)
{
    for (const BodyId& id : created) {
        if (std::any_of(state.bodies.begin(), state.bodies.end(), [&](const Body& b) { return b.id == id; })) {
            continue;
        }
        Body body;
        body.id = id;
        body.name = bodyName ? *bodyName : "Body " + std::to_string(state.bodies.size() + 1);
        body.role = Body::Role::Part;
        body.color = {0.72f, 0.74f, 0.78f, 1.0f};
        body.visible = true;
        state.bodies.push_back(std::move(body));
    }
}

class TypedCommand final : public ICommandHandler {
public:
    TypedCommand(std::string type, bool edit, std::string_view propsSchema)
        : type_(std::move(type)), edit_(edit), name_("feature." + type_ + (edit ? ".edit" : ".add"))
    {
        const std::string head =
            R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","additionalProperties":false,)";
        if (edit) {
            schema_ = head + R"("required":["id"],"minProperties":2,"properties":{"id":)" + std::string(kUuidSchema) +
                      R"(,"name":{"type":"string","minLength":1},"props":{"type":"object","description":)"
                      R"("The properties to change; a null value removes an optional one"}}})";
        } else {
            schema_ = head + R"("required":["id","props"],"properties":{"id":)" + std::string(kUuidSchema) +
                      R"(,"name":{"type":"string","minLength":1},"props":)" + std::string(propsSchema) +
                      R"(,"index":{"type":"integer","minimum":0},"suppressed":{"type":"boolean"},)"
                      R"("bodyName":{"type":"string","minLength":1,"description":"Name of the body the feature )"
                      R"(creates, if it creates one"}}})";
        }
    }

    std::string_view name() const override { return name_; }
    std::string_view argsSchemaJson() const override { return schema_; }
    bool changesToolpathsOrGcode() const override { return false; }

    Result<DocStatePtr> apply(DocStatePtr before, std::string_view argsJson) const override
    {
        return run(before, argsJson, nullptr);
    }
    Result<DocStatePtr> applyWithJob(DocStatePtr before, std::string_view argsJson, JobContext& ctx) const override
    {
        return run(before, argsJson, &ctx);
    }

private:
    Result<DocStatePtr> run(const DocStatePtr& before, std::string_view argsJson, JobContext* job) const
    {
        if (!before) {
            return invalid(name_ + ": there is no document snapshot to apply it to");
        }
        try {
            return edit_ ? applyEdit(*before, argsJson, job) : applyAdd(*before, argsJson, job);
        } catch (const std::bad_alloc&) {
            return failed(ErrorCode::Internal, name_ + ": out of memory");
        } catch (...) {
            return failed(ErrorCode::Internal, name_ + ": unexpected failure");
        }
    }

    Result<DocStatePtr> applyAdd(const DocState& before, std::string_view argsJson, JobContext* job) const
    {
        auto args = commands::Args::parse(name_, argsJson, {"bodyName", "id", "index", "name", "props", "suppressed"});
        if (!args) {
            return tl::unexpected(args.error());
        }
        auto id = args->id("id");
        if (!id) {
            return tl::unexpected(id.error());
        }
        auto name = args->optText("name");
        auto props = args->optObject("props");
        auto suppressed = args->optBool("suppressed");
        auto index = args->optIndex("index", before.features.size());
        auto bodyName = args->optText("bodyName");
        for (const Status& st : {name ? Status{} : Status(tl::unexpected(name.error())),
                                 props ? Status{} : Status(tl::unexpected(props.error())),
                                 suppressed ? Status{} : Status(tl::unexpected(suppressed.error())),
                                 index ? Status{} : Status(tl::unexpected(index.error())),
                                 bodyName ? Status{} : Status(tl::unexpected(bodyName.error()))}) {
            if (!st) {
                return tl::unexpected(st.error());
            }
        }
        if (!*props) {
            return args->error("props", "is required");
        }
        const FeatureId featureId{*id};
        if (std::any_of(before.features.begin(), before.features.end(),
                        [&](const Feature& f) { return f.id == featureId; })) {
            return args->error("id", "is already used by a feature (" + id->toString() + ")");
        }
        const ParametricType* type = parametricType(type_, nullptr);
        if (type == nullptr) {
            return failed(ErrorCode::Internal, name_ + ": the feature type '" + type_ + "' is not registered");
        }
        Feature feature;
        feature.id = featureId;
        feature.type = type_;
        if (*name) {
            feature.name = **name;
        } else {
            const auto count = std::count_if(before.features.begin(), before.features.end(),
                                             [&](const Feature& f) { return f.type == type_; });
            feature.name = std::string(type->title()) + " " + std::to_string(count + 1);
        }
        feature.propsJson = **props;
        feature.suppressed = suppressed->value_or(false);
        const std::size_t at = index->value_or(before.features.size());
        auto next = std::make_shared<DocState>(before);
        next->features.insert(next->features.begin() + static_cast<std::ptrdiff_t>(at), feature);
        if (auto ok = type->validate(*next, at, feature); !ok) {
            return tl::unexpected(ok.error());
        }
        auto stored = type->storedProps(*next, feature, job);
        if (!stored) {
            return tl::unexpected(stored.error());
        }
        next->features[at].propsJson = std::move(*stored);
        const std::vector<BodyId> created = type->createdBodies(feature);
        if (auto ok = checkCreatedBodies(*next, feature, created, name_); !ok) {
            return tl::unexpected(ok.error());
        }
        addBodies(*next, created, *bodyName);
        return DocStatePtr(std::move(next));
    }

    Result<DocStatePtr> applyEdit(const DocState& before, std::string_view argsJson, JobContext* job) const
    {
        auto args = commands::Args::parse(name_, argsJson, {"id", "name", "props"});
        if (!args) {
            return tl::unexpected(args.error());
        }
        auto id = args->id("id");
        if (!id) {
            return tl::unexpected(id.error());
        }
        auto name = args->optText("name");
        if (!name) {
            return tl::unexpected(name.error());
        }
        auto patch = args->optValue("props");
        if (*patch != nullptr && !(*patch)->isObject()) {
            return args->error("props", "must be a JSON object");
        }
        if (!*name && *patch == nullptr) {
            return args->error("nothing to change: give 'name', 'props' or both");
        }
        const auto it = std::find_if(before.features.begin(), before.features.end(),
                                     [&](const Feature& f) { return f.id == FeatureId{*id}; });
        if (it == before.features.end()) {
            return args->error("id", "names no feature of this document (" + id->toString() + ")");
        }
        if (it->type != type_) {
            return args->error("id", "names a feature of type " + it->type + "; use feature." + it->type + ".edit");
        }
        const ParametricType* type = parametricType(type_, nullptr);
        if (type == nullptr) {
            return failed(ErrorCode::Internal, name_ + ": the feature type '" + type_ + "' is not registered");
        }
        const std::size_t at = static_cast<std::size_t>(it - before.features.begin());
        auto next = std::make_shared<DocState>(before);
        Feature& feature = next->features[at];
        if (*name) {
            feature.name = **name;
        }
        if (*patch != nullptr) {
            auto current = PropsReader::parse(name_, feature.propsJson);
            if (!current) {
                return tl::unexpected(current.error());
            }
            Value::Object members;
            for (const Member& m : current->asObject()) {
                if ((*patch)->find(m.key) == nullptr) {
                    members.push_back(m);
                }
            }
            for (const Member& m : (*patch)->asObject()) {
                if (!m.value.isNull()) {
                    members.push_back(m);
                }
            }
            feature.propsJson = json::write(Value::object(std::move(members)));
        }
        if (auto ok = type->validate(*next, at, feature); !ok) {
            return tl::unexpected(ok.error());
        }
        auto stored = type->storedProps(*next, feature, job);
        if (!stored) {
            return tl::unexpected(stored.error());
        }
        feature.propsJson = std::move(*stored);
        const std::vector<BodyId> created = type->createdBodies(feature);
        if (auto ok = checkCreatedBodies(*next, feature, created, name_); !ok) {
            return tl::unexpected(ok.error());
        }
        addBodies(*next, created, std::nullopt);
        return DocStatePtr(std::move(next));
    }

    std::string type_;
    bool edit_;
    std::string name_;
    std::string schema_;
};

} // namespace

std::vector<std::unique_ptr<ICommandHandler>> makeTypedFeatureCommands(std::string_view type,
                                                                       std::string_view propsSchema)
{
    std::vector<std::unique_ptr<ICommandHandler>> out;
    out.push_back(std::make_unique<TypedCommand>(std::string(type), false, propsSchema));
    out.push_back(std::make_unique<TypedCommand>(std::string(type), true, propsSchema));
    return out;
}

} // namespace ray::doc::features
