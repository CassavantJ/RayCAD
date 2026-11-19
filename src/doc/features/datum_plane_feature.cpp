// The "datum_plane" feature type (datum_plane_feature.h).
#include "datum_plane_feature.h"

#include "feature_props.h"

#include <exception>
#include <new>
#include <utility>

namespace ray::doc::features {
namespace {

const std::string& schema()
{
    static const std::string text = objectSchema(
        R"("base")",
        R"("base":)" + planeSchema() + R"(,"offset":)" +
            lengthSchema("Distance along the base plane's normal; default 0") +
            R"j(,"flip":{"type":"boolean","default":false,"description":"Reverse the normal (and the x direction)"})j");
    return text;
}

struct DatumProps {
    PlaneRef base;
    json::Value offset;
    bool flip = false;
};

Result<DatumProps> parse(const Feature& f, bool strict)
{
    const std::string where = featureLabel(f);
    auto root = PropsReader::parse(where, f.propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    const PropsReader r(where, *root);
    if (strict) {
        if (auto ok = r.onlyKeys({"base", "flip", "offset"}); !ok) {
            return tl::unexpected(ok.error());
        }
    }
    DatumProps out;
    auto base = readPlaneRef(r, "base", "base");
    if (!base) {
        return tl::unexpected(base.error());
    }
    out.base = std::move(*base);
    auto offset = r.numeric("offset", "offset", json::Value::number(0.0));
    if (!offset) {
        return tl::unexpected(offset.error());
    }
    out.offset = std::move(*offset);
    auto flip = r.optBool("flip", "flip", false);
    if (!flip) {
        return tl::unexpected(flip.error());
    }
    out.flip = *flip;
    return out;
}

class DatumPlaneType final : public IFeatureType,
                             public ParametricType,
                             public detail::BodyDeclaration,
                             public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kDatumPlaneType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Datum Plane"; }

    std::vector<naming::TopoRef> references(const Feature& f) const override
    {
        auto props = parse(f, false);
        if (props && props->base.face) {
            return {*props->base.face};
        }
        return {};
    }

    std::vector<BodyId> bodiesWritten(const Feature&) const override { return {}; }
    bool writesOnlyDeclared() const override { return true; }

    Status validate(const DocState& state, std::size_t, const Feature& f) const override
    {
        auto props = parse(f, true);
        if (!props) {
            return tl::unexpected(props.error());
        }
        if (auto ok =
                checkNumeric(props->offset, documentParams(state), Parameter::Kind::Length, featureLabel(f), "offset");
            !ok) {
            return ok;
        }
        if (props->base.datum) {
            if (*props->base.datum == f.id) {
                return invalid(featureLabel(f) + ": a datum plane cannot be based on itself.");
            }
            auto datum = earlierFeature(state, f, *props->base.datum, kDatumPlaneType, "datum plane");
            if (!datum) {
                return tl::unexpected(datum.error());
            }
        }
        return {};
    }

    Result<FeatureOutput> rebuild(const Feature& f, RebuildContext& ctx) const override
    {
        try {
            auto props = parse(f, false);
            if (!props) {
                return tl::unexpected(props.error());
            }
            const std::string where = featureLabel(f);
            auto offset = evaluateNumeric(props->offset, ctx.paramValues, Parameter::Kind::Length, where, "offset");
            if (!offset) {
                return tl::unexpected(offset.error());
            }
            auto base = resolvePlaneRef(props->base, ctx, f, where + " (its base plane)");
            if (!base) {
                return tl::unexpected(base.error());
            }
            auto artifact = std::make_shared<PlaneArtifact>();
            artifact->plane = *base;
            artifact->plane.origin = vadd(base->origin, vmul(base->normal, *offset));
            if (props->flip) {
                artifact->plane.normal = vmul(base->normal, -1.0);
                artifact->plane.xDir = vmul(base->xDir, -1.0);
            }
            detail::publishArtifact(ctx, std::move(artifact));
            return FeatureOutput{};
        } catch (const std::bad_alloc&) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": out of memory");
        } catch (...) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": unexpected failure");
        }
    }
};

} // namespace

std::unique_ptr<IFeatureType> makeDatumPlaneFeatureType()
{
    return std::make_unique<DatumPlaneType>();
}

std::string_view datumPlanePropsSchema()
{
    return schema();
}

} // namespace ray::doc::features
