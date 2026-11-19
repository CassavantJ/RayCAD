// The "revolve" feature type (revolve_feature.h).
#include "revolve_feature.h"

#include "extrude_feature.h"
#include "sketch_json.h"

#include <cmath>
#include <exception>
#include <new>
#include <utility>

namespace ray::doc::features {
namespace {

using json::Value;

constexpr double kTwoPi = 6.283185307179586476925286766559;

const std::string& schema()
{
    static const std::string text = objectSchema(
        R"("sketch","axis","operation","target")",
        R"("sketch":)" + uuidSchema() + R"(,"regions":)" + regionsSchema() +
            R"(,"axis":{"type":"object","minProperties":1,"maxProperties":1,"additionalProperties":false,)"
            R"("properties":{"entity":)" +
            uuidSchema() +
            R"(,"principal":{"enum":["x","y","z"]}},"description":"A line of the sketch, or a world axis through )"
            R"(the origin"},"angle":)" +
            angleSchema("Angle to turn; default one full turn") +
            R"(,"direction":{"enum":["normal","reverse"],"default":"normal"},"operation":)" + operationSchema() +
            R"(,"target":)" + uuidSchema());
    return text;
}

struct RevolveProps {
    SweepCommon common;
    std::optional<SketchEntId> axisEntity;
    std::string axisPrincipal;
    Value angle;
    bool reverse = false;
};

Result<RevolveProps> parse(const Feature& f, bool strict)
{
    const std::string where = featureLabel(f);
    auto root = PropsReader::parse(where, f.propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    const PropsReader r(where, *root);
    if (strict) {
        if (auto ok = r.onlyKeys({"angle", "axis", "direction", "operation", "regions", "sketch", "target"}); !ok) {
            return tl::unexpected(ok.error());
        }
    }
    RevolveProps out;
    auto common = readSweepCommon(r);
    if (!common) {
        return tl::unexpected(common.error());
    }
    out.common = std::move(*common);
    const Value* axis = r.find("axis");
    if (axis == nullptr || !axis->isObject() || axis->asObject().size() != 1) {
        return r.error("axis", "must be {\"entity\": \"<line id of the sketch>\"} or {\"principal\": \"x\" | \"y\" | "
                               "\"z\"}");
    }
    if (const Value* principal = axis->find("principal")) {
        if (!principal->isString() ||
            (principal->asString() != "x" && principal->asString() != "y" && principal->asString() != "z")) {
            return r.error("axis.principal", "must be one of x, y, z");
        }
        out.axisPrincipal = principal->asString();
    } else {
        auto entity = r.child(*axis).uuid("entity", "axis.entity");
        if (!entity) {
            return tl::unexpected(entity.error());
        }
        out.axisEntity = SketchEntId{*entity};
    }
    auto angle = r.numeric("angle", "angle", Value::number(kTwoPi));
    auto direction = r.choice("direction", "direction", {"normal", "reverse"}, "normal");
    if (!angle || !direction) {
        return tl::unexpected(!angle ? angle.error() : direction.error());
    }
    out.angle = std::move(*angle);
    out.reverse = *direction == "reverse";
    return out;
}

// The axis in world coordinates: origin and direction.
Result<std::pair<Vec3, Vec3>> axisOf(const RevolveProps& props, const SketchArtifact& sketch, const std::string& where)
{
    if (!props.axisPrincipal.empty()) {
        const Vec3 dir = props.axisPrincipal == "x"   ? Vec3{1, 0, 0}
                         : props.axisPrincipal == "y" ? Vec3{0, 1, 0}
                                                      : Vec3{0, 0, 1};
        return std::pair<Vec3, Vec3>{Vec3{0, 0, 0}, dir};
    }
    const sketch::Entity* line = nullptr;
    for (const sketch::Entity& e : sketch.solved.entities) {
        if (e.id == *props.axisEntity) {
            line = &e;
        }
    }
    if (line == nullptr) {
        return invalid(where + ": its axis line (" + entityText(*props.axisEntity) +
                       ") is no longer in the sketch; pick the axis again.");
    }
    const auto* l = std::get_if<sketch::Line>(&line->geom);
    if (l == nullptr) {
        return invalid(where + ": its axis (" + entityText(*props.axisEntity) + ") is not a line of the sketch.");
    }
    std::optional<Vec2> a, b;
    for (const sketch::Entity& e : sketch.solved.entities) {
        if (const auto* p = std::get_if<sketch::Point>(&e.geom)) {
            if (e.id == l->a) {
                a = p->p;
            }
            if (e.id == l->b) {
                b = p->p;
            }
        }
    }
    if (!a || !b) {
        return invalid(where + ": its axis line has no end points.");
    }
    const Vec3 pa = planePoint(sketch.plane, *a);
    const Vec3 pb = planePoint(sketch.plane, *b);
    const auto dir = vunit(vsub(pb, pa));
    if (!dir) {
        return invalid(where + ": its axis line has no length.");
    }
    return std::pair<Vec3, Vec3>{pa, *dir};
}

class RevolveType final : public IFeatureType,
                          public ParametricType,
                          public detail::BodyDeclaration,
                          public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kRevolveType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Revolve"; }
    std::vector<naming::TopoRef> references(const Feature&) const override { return {}; }

    std::vector<BodyId> bodiesWritten(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? std::vector<BodyId>{props->common.target} : std::vector<BodyId>{};
    }
    bool writesOnlyDeclared() const override { return true; }

    std::vector<BodyId> createdBodies(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props && props->common.operation == "new" ? std::vector<BodyId>{props->common.target}
                                                         : std::vector<BodyId>{};
    }

    Status validate(const DocState& state, std::size_t index, const Feature& f) const override
    {
        auto props = parse(f, true);
        if (!props) {
            return tl::unexpected(props.error());
        }
        if (auto ok =
                checkNumeric(props->angle, documentParams(state), Parameter::Kind::Angle, featureLabel(f), "angle");
            !ok) {
            return ok;
        }
        return validateSweepCommon(state, index, f, props->common);
    }

    Result<FeatureOutput> rebuild(const Feature& f, RebuildContext& ctx) const override
    {
        try {
            const std::string where = featureLabel(f);
            auto props = parse(f, false);
            if (!props) {
                return tl::unexpected(props.error());
            }
            auto angle = evaluateNumeric(props->angle, ctx.paramValues, Parameter::Kind::Angle, where, "angle");
            if (!angle) {
                return tl::unexpected(angle.error());
            }
            if (!(std::abs(*angle) > 1e-9) || std::abs(*angle) > kTwoPi + 1e-9) {
                return invalid(where + ": the angle must be more than 0 and at most 360 degrees.");
            }
            const Feature* sketchFeature = nullptr;
            auto artifact = sketchFor(f, ctx, props->common.sketch, &sketchFeature);
            if (!artifact) {
                return tl::unexpected(artifact.error());
            }
            auto axis = axisOf(*props, **artifact, where);
            if (!axis) {
                return tl::unexpected(axis.error());
            }
            auto regions = selectRegions(**artifact, props->common, where, featureLabel(*sketchFeature));
            if (!regions) {
                return tl::unexpected(regions.error());
            }
            const double signedAngle = props->reverse ? -*angle : *angle;
            const Vec3 origin = axis->first;
            const Vec3 dir = axis->second;
            return buildSweep(f, ctx, props->common, (*artifact)->plane, *regions, "revolve",
                              [&](const kernel::Profile& profile, JobContext& job) {
                                  return kernel::revolve(profile, origin, dir, signedAngle, job);
                              });
        } catch (const std::bad_alloc&) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": out of memory");
        } catch (...) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": unexpected failure");
        }
    }
};

} // namespace

std::unique_ptr<IFeatureType> makeRevolveFeatureType()
{
    return std::make_unique<RevolveType>();
}

std::string_view revolvePropsSchema()
{
    return schema();
}

} // namespace ray::doc::features
