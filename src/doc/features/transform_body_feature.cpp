// The "transform_body" feature type (transform_body_feature.h, task T-077).
#include "transform_body_feature.h"

#include "feature_props.h"

#include <ray/naming.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <new>
#include <tuple>
#include <utility>
#include <vector>

namespace ray::doc::features {
namespace {

using json::Value;

const std::string& schema()
{
    static const std::string text = objectSchema(
        R"("body")",
        R"("body":)" + uuidSchema() +
            R"(,"mate":{"type":"object","additionalProperties":false,"required":["face","target"],"properties":{)"
            R"("face":)" +
            topoRefSchema() + R"(,"target":)" + topoRefSchema() +
            R"(},"description":"A planar face of the body and a planar face of another body: the body moves so that )"
            R"(they touch, opposed and centred"},"gap":)" +
            lengthSchema("With a mate: how far the faces stand apart, along the target face's normal; default 0") +
            R"(,"angle":)" +
            angleSchema("Turn about the target face's normal through its centre (without a mate: about model Z "
                        "through the origin); default 0") +
            R"(,"x":)" + lengthSchema("Move along model X, last; default 0") + R"(,"y":)" +
            lengthSchema("Move along model Y, last; default 0") + R"(,"z":)" +
            lengthSchema("Move along model Z, last; default 0"));
    return text;
}

struct Props {
    BodyId body;
    std::optional<naming::TopoRef> face, target;
    Value gap, angle, x, y, z;
};

Result<Props> parse(const Feature& f, bool strict)
{
    const std::string where = featureLabel(f);
    auto root = PropsReader::parse(where, f.propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    const PropsReader r(where, *root);
    if (strict) {
        if (auto ok = r.onlyKeys({"angle", "body", "gap", "mate", "x", "y", "z"}); !ok) {
            return tl::unexpected(ok.error());
        }
    }
    Props out;
    auto body = r.uuid("body", "body");
    if (!body) {
        return tl::unexpected(body.error());
    }
    out.body = BodyId{*body};
    if (const Value* mate = r.find("mate"); mate != nullptr) {
        if (!mate->isObject()) {
            return r.error("mate", "must be {\"face\": <face reference>, \"target\": <face reference>}");
        }
        const PropsReader m = r.child(*mate);
        if (strict) {
            if (auto ok = m.onlyKeys({"face", "target"}, "mate."); !ok) {
                return tl::unexpected(ok.error());
            }
        }
        for (const auto& [key, slot] :
             {std::pair<const char*, std::optional<naming::TopoRef>*>{"face", &out.face},
              std::pair<const char*, std::optional<naming::TopoRef>*>{"target", &out.target}}) {
            const std::string field = std::string("mate.") + key;
            const Value* v = mate->find(key);
            if (v == nullptr) {
                return r.error(field, "is required");
            }
            auto ref = topoRefFromJson(r, *v, field);
            if (!ref) {
                return tl::unexpected(ref.error());
            }
            if (ref->kind != kernel::SubKind::Face) {
                return r.error(field,
                               std::string("must be a planar face; a ") + subKindName(ref->kind) + " cannot be mated");
            }
            *slot = std::move(*ref);
        }
        if (out.face->body != out.body) {
            return r.error("mate.face", "must be a face of the body that moves (" + out.body.value.toString() + ")");
        }
        if (out.target->body == out.body) {
            return r.error("mate.target", "must be a face of another body; a body cannot be mated to itself");
        }
    }
    for (const auto& [key, slot] :
         {std::pair<const char*, Value*>{"gap", &out.gap}, std::pair<const char*, Value*>{"angle", &out.angle},
          std::pair<const char*, Value*>{"x", &out.x}, std::pair<const char*, Value*>{"y", &out.y},
          std::pair<const char*, Value*>{"z", &out.z}}) {
        auto v = r.numeric(key, key, Value::number(0.0));
        if (!v) {
            return tl::unexpected(v.error());
        }
        *slot = std::move(*v);
    }
    if (!out.face && r.find("gap") != nullptr) {
        return r.error("gap", "needs a mate: it is the distance between the mated faces");
    }
    return out;
}

// ---------- Arithmetic ----------

double snapUnitEntry(double v)
{
    constexpr double kExact = 1e-15;
    if (std::fabs(v) < kExact) {
        return 0.0;
    }
    if (std::fabs(v - 1.0) < kExact) {
        return 1.0;
    }
    if (std::fabs(v + 1.0) < kExact) {
        return -1.0;
    }
    return v;
}

Vec3 unitOr(Vec3 v, Vec3 fallback)
{
    const auto u = vunit(v);
    return u ? *u : fallback;
}

// The frame of a face for the mate: rows x, y, z with z the normal (see the header, step 1).
std::array<Vec3, 3> faceFrame(Vec3 n)
{
    constexpr double kSteep = 0.70710678118654752; // cos 45 degrees
    if (std::fabs(n.z) < kSteep) {
        const Vec3 up = unitOr(vsub(Vec3{0, 0, 1}, vmul(n, n.z)), Vec3{0, 0, 1});
        return {vcross(up, n), up, n};
    }
    const Vec3 x = unitOr(vsub(Vec3{1, 0, 0}, vmul(n, n.x)), Vec3{1, 0, 0});
    return {x, vcross(n, x), n};
}

using Mat3 = std::array<std::array<double, 3>, 3>;

Mat3 multiply(const Mat3& a, const Mat3& b)
{
    Mat3 out{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
    return out;
}

Vec3 rotate(const Mat3& m, Vec3 v)
{
    return Vec3{m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

Mat3 identity3()
{
    return Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

// Rotation by `angle` about the unit axis `a` (right-handed), Rodrigues' formula.
Mat3 rotation(Vec3 a, double angle)
{
    const double c = snapUnitEntry(dmath::cos(angle));
    const double s = snapUnitEntry(dmath::sin(angle));
    const double t = 1.0 - c;
    Mat3 m{{{t * a.x * a.x + c, t * a.x * a.y - s * a.z, t * a.x * a.z + s * a.y},
            {t * a.x * a.y + s * a.z, t * a.y * a.y + c, t * a.y * a.z - s * a.x},
            {t * a.x * a.z - s * a.y, t * a.y * a.z + s * a.x, t * a.z * a.z + c}}};
    for (auto& row : m) {
        for (double& v : row) {
            v = snapUnitEntry(v);
        }
    }
    return m;
}

} // namespace

Vec3 roundedNormal(Vec3 n)
{
    constexpr double kUp = 0x1p40;
    constexpr double kDown = 0x1p-40;
    const Vec3 q{std::nearbyint(n.x * kUp) * kDown, std::nearbyint(n.y * kUp) * kDown,
                 std::nearbyint(n.z * kUp) * kDown};
    const Vec3 u = unitOr(q, Vec3{0, 0, 1});
    // Within 1e-9 of a model axis: that axis exactly.
    const double c[3] = {u.x, u.y, u.z};
    int big = 0;
    for (int i = 1; i < 3; ++i) {
        if (std::fabs(c[i]) > std::fabs(c[big])) {
            big = i;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (i != big && std::fabs(c[i]) > 1e-9) {
            return u;
        }
    }
    const double sign = c[big] < 0 ? -1.0 : 1.0;
    return Vec3{big == 0 ? sign : 0.0, big == 1 ? sign : 0.0, big == 2 ? sign : 0.0};
}

Vec3 roundedPoint(Vec3 p)
{
    constexpr double kPerMm = 1e6; // nanometres
    return Vec3{std::nearbyint(p.x * kPerMm) / kPerMm, std::nearbyint(p.y * kPerMm) / kPerMm,
                std::nearbyint(p.z * kPerMm) / kPerMm};
}

Xform placementTransform(const Placement& p)
{
    Mat3 r = identity3();
    Vec3 t{};
    if (p.face && p.target) {
        // 1. Mate: the face's frame onto the frame of the reversed target normal.
        const std::array<Vec3, 3> from = faceFrame(p.face->normal);
        const std::array<Vec3, 3> to = faceFrame(vmul(p.target->normal, -1.0));
        // R = To^T * From (columns of To^T are the target frame's axes; rows of From the face frame's).
        Mat3 toT{}, fromM{};
        for (int i = 0; i < 3; ++i) {
            const Vec3 a = to[static_cast<std::size_t>(i)];
            const Vec3 b = from[static_cast<std::size_t>(i)];
            toT[0][i] = a.x;
            toT[1][i] = a.y;
            toT[2][i] = a.z;
            fromM[i] = {b.x, b.y, b.z};
        }
        r = multiply(toT, fromM);
        for (auto& row : r) {
            for (double& v : row) {
                v = snapUnitEntry(v);
            }
        }
        t = vsub(p.target->centre, rotate(r, p.face->centre));
        // 2. Gap along the target normal.
        t = vadd(t, vmul(p.target->normal, p.gap));
        // 3. Angle about the target normal through its centre: x -> Ra (x - c) + c.
        if (p.angle != 0.0) {
            const Mat3 ra = rotation(p.target->normal, p.angle);
            r = multiply(ra, r);
            t = vadd(rotate(ra, vsub(t, p.target->centre)), p.target->centre);
        }
    } else if (p.angle != 0.0) {
        r = rotation(Vec3{0, 0, 1}, p.angle);
    }
    // 4. The offsets along the model axes.
    t = vadd(t, p.offset);
    Xform out{};
    for (int i = 0; i < 3; ++i) {
        out.m[i * 4 + 0] = r[static_cast<std::size_t>(i)][0];
        out.m[i * 4 + 1] = r[static_cast<std::size_t>(i)][1];
        out.m[i * 4 + 2] = r[static_cast<std::size_t>(i)][2];
    }
    out.m[3] = t.x;
    out.m[7] = t.y;
    out.m[11] = t.z;
    return out;
}

namespace {

// A planar face as the mate reads it, resolved where the feature stands. `what` names it in messages.
Result<MateFace> mateFace(RebuildContext& ctx, const naming::TopoRef& ref, const std::string& what,
                          std::vector<std::string>& warnings)
{
    const naming::Resolved resolved = ctx.resolver.resolve(ref);
    auto shape = ctx.shapeOf(ref.body);
    if (resolved.usable() && shape) {
        for (const kernel::SubId& face : resolved.elements) {
            auto info = kernel::surfaceInfo(*shape, face);
            if (info && info->type != kernel::SurfaceType::Plane) {
                return invalid(what + " is not flat; a mate needs a planar face on each body.");
            }
        }
    }
    auto plane = facePlane(ctx, ref, what); // usable, planar, one plane; the outward normal
    if (!plane) {
        return tl::unexpected(plane.error());
    }
    if (!shape) {
        return failed(shape.error().code, what + ": " + shape.error().message);
    }
    double area = 0;
    Vec3 sum{};
    for (const kernel::SubId& face : resolved.elements) {
        auto info = kernel::surfaceInfo(*shape, face);
        if (!info) {
            return failed(info.error().code, what + ": " + info.error().message);
        }
        area += info->area;
        sum = vadd(sum, vmul(info->centroid, info->area));
    }
    if (!(area > 0)) {
        return failed(ErrorCode::KernelFailure, what + ": the face has no area.");
    }
    if (resolved.state == naming::ResolveState::Split) {
        warnings.push_back(what + " was split by an earlier change; the " + std::to_string(resolved.elements.size()) +
                           " pieces are mated as one face.");
    } else if (resolved.state == naming::ResolveState::Merged) {
        warnings.push_back(what + " is now part of a larger face; the whole face is mated.");
    }
    // The centre projected onto the plane (the centroid of a planar face lies on it, up to rounding).
    const Vec3 normal = roundedNormal(plane->normal);
    Vec3 centre = vmul(sum, 1.0 / area);
    centre = vsub(centre, vmul(normal, vdot(vsub(centre, plane->origin), normal)));
    return MateFace{roundedPoint(centre), normal};
}

class TransformBodyType final : public IFeatureType,
                                public ParametricType,
                                public detail::BodyDeclaration,
                                public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kTransformBodyType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Move Body"; }

    std::vector<naming::TopoRef> references(const Feature& f) const override
    {
        auto props = parse(f, false);
        if (!props || !props->face) {
            return {};
        }
        return {*props->face, *props->target};
    }

    std::vector<BodyId> bodiesWritten(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? std::vector<BodyId>{props->body} : std::vector<BodyId>{};
    }
    bool writesOnlyDeclared() const override { return true; }

    Status validate(const DocState& state, std::size_t, const Feature& f) const override
    {
        auto props = parse(f, true);
        if (!props) {
            return tl::unexpected(props.error());
        }
        const std::string where = featureLabel(f);
        const auto hasBody = [&](const BodyId& id) {
            return std::any_of(state.bodies.begin(), state.bodies.end(), [&](const Body& b) { return b.id == id; });
        };
        if (!hasBody(props->body)) {
            return invalid(where + ": 'body' names body " + props->body.value.toString() +
                           ", which is not in the document");
        }
        if (props->target && !hasBody(props->target->body)) {
            return invalid(where + ": 'mate.target' is on body " + props->target->body.value.toString() +
                           ", which is not in the document");
        }
        const DocumentParams params = documentParams(state);
        for (const auto& [value, kind, field] :
             {std::tuple<const Value*, Parameter::Kind, const char*>{&props->gap, Parameter::Kind::Length, "gap"},
              std::tuple<const Value*, Parameter::Kind, const char*>{&props->angle, Parameter::Kind::Angle, "angle"},
              std::tuple<const Value*, Parameter::Kind, const char*>{&props->x, Parameter::Kind::Length, "x"},
              std::tuple<const Value*, Parameter::Kind, const char*>{&props->y, Parameter::Kind::Length, "y"},
              std::tuple<const Value*, Parameter::Kind, const char*>{&props->z, Parameter::Kind::Length, "z"}}) {
            if (auto ok = checkNumeric(*value, params, kind, where, field); !ok) {
                return ok;
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
        Placement placement;
        for (const auto& [value, kind, field, slot] :
             {std::tuple<const Value*, Parameter::Kind, const char*, double*>{&props->gap, Parameter::Kind::Length,
                                                                              "gap", &placement.gap},
              std::tuple<const Value*, Parameter::Kind, const char*, double*>{&props->angle, Parameter::Kind::Angle,
                                                                              "angle", &placement.angle},
              std::tuple<const Value*, Parameter::Kind, const char*, double*>{&props->x, Parameter::Kind::Length, "x",
                                                                              &placement.offset.x},
              std::tuple<const Value*, Parameter::Kind, const char*, double*>{&props->y, Parameter::Kind::Length, "y",
                                                                              &placement.offset.y},
              std::tuple<const Value*, Parameter::Kind, const char*, double*>{&props->z, Parameter::Kind::Length, "z",
                                                                              &placement.offset.z}}) {
            auto v = evaluateNumeric(*value, ctx.paramValues, kind, where, field);
            if (!v) {
                return tl::unexpected(v.error());
            }
            *slot = *v;
        }
        auto shape = ctx.shapeOf(props->body);
        if (!shape) {
            return failed(shape.error().code, where + ": " + shape.error().message);
        }
        if (shape->isNull()) {
            return failed(ErrorCode::ReferenceBroken, where + ": the body to move is not built.");
        }
        const naming::ElementMap& map = ctx.mapOf(props->body);
        FeatureOutput out;
        if (props->face) {
            std::vector<std::string> notes;
            auto face = mateFace(ctx, *props->face, where + ": the face to mate", notes);
            if (!face) {
                return tl::unexpected(face.error());
            }
            auto target = mateFace(ctx, *props->target, where + ": the face it mates to", notes);
            if (!target) {
                return tl::unexpected(target.error());
            }
            placement.face = *face;
            placement.target = *target;
            out.warnings = std::move(notes);
        }
        const Xform xform = placementTransform(placement);
        auto moved = kernel::transformed(*shape, xform);
        if (!moved) {
            return failed(moved.error().code, where + ": " + moved.error().message);
        }
        for (const std::string& w : moved->warnings) {
            out.warnings.push_back(where + ": " + w);
        }
        const std::array<const naming::ElementMap*, 1> maps{&map};
        const naming::NamingInput input{f.id, std::string(kTransformBodyType), maps, &moved->history, &moved->shape, {},
                                        {}};
        auto named = naming::propagate(input);
        if (!named) {
            return failed(ErrorCode::Internal, where + ": naming the result failed: " + named.error().message);
        }
        ctx.job.progress.set(1.0);
        out.bodies.push_back(BodyUpdate{props->body, moved->shape, std::move(named->map), std::move(named->lineage)});
        return out;
    }
};

} // namespace

std::unique_ptr<IFeatureType> makeTransformBodyFeatureType()
{
    return std::make_unique<TransformBodyType>();
}

std::string_view transformBodyPropsSchema()
{
    return schema();
}

} // namespace ray::doc::features
