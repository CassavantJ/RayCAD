// The "offset_face" feature type (offset_face_feature.h, task T-082).
#include "offset_face_feature.h"

#include "delete_face_feature.h"

#include <exception>
#include <new>
#include <utility>

namespace ray::doc::features {
namespace {

using json::Value;

const std::string& schema()
{
    static const std::string text = objectSchema(
        R"("distance","faces")",
        R"("faces":)" + directedit::facesSchema("The planar faces to push or pull, all on one body") +
            R"(,"distance":)" +
            lengthSchema("How far each face moves along its outward normal: positive pushes material out, negative "
                         "pulls it in; not 0"));
    return text;
}

struct Props {
    std::vector<naming::TopoRef> faces;
    Value distance;
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
        if (auto ok = r.onlyKeys({"distance", "faces"}); !ok) {
            return tl::unexpected(ok.error());
        }
    }
    auto faces = directedit::readFaceRefs(r, "faces");
    if (!faces) {
        return tl::unexpected(faces.error());
    }
    auto distance = r.numeric("distance", "distance");
    if (!distance) {
        return tl::unexpected(distance.error());
    }
    return Props{std::move(*faces), std::move(*distance)};
}

class OffsetFaceType final : public IFeatureType,
                             public ParametricType,
                             public detail::BodyDeclaration,
                             public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kOffsetFaceType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Offset Face"; }

    std::vector<naming::TopoRef> references(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? props->faces : std::vector<naming::TopoRef>{};
    }

    std::vector<BodyId> bodiesWritten(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? std::vector<BodyId>{props->faces.front().body} : std::vector<BodyId>{};
    }
    bool writesOnlyDeclared() const override { return true; }

    Status validate(const DocState& state, std::size_t, const Feature& f) const override
    {
        auto props = parse(f, true);
        if (!props) {
            return tl::unexpected(props.error());
        }
        const std::string where = featureLabel(f);
        if (auto ok = directedit::checkBody(state, props->faces.front().body, where, "faces"); !ok) {
            return ok;
        }
        return checkNumeric(props->distance, documentParams(state), Parameter::Kind::Length, where, "distance");
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
        auto distance = evaluateNumeric(props->distance, ctx.paramValues, Parameter::Kind::Length, where, "distance");
        if (!distance) {
            return tl::unexpected(distance.error());
        }
        if (*distance == 0.0) {
            return invalid(where +
                           ": the distance must not be 0 (positive pushes material out, negative pulls it in).");
        }
        const BodyId body = props->faces.front().body;
        auto shape = directedit::bodyShape(ctx, body, where);
        if (!shape) {
            return tl::unexpected(shape.error());
        }
        auto picked = directedit::resolveFaces(ctx, props->faces, where, "moved");
        if (!picked) {
            return tl::unexpected(picked.error());
        }
        JobContext job{ctx.job.cancel, ctx.job.progress.sub(0.9)};
        auto op = kernel::offsetFaces(*shape, picked->faces, *distance, job);
        return directedit::finishEdit(f, kOffsetFaceType, ctx, body, *shape, std::move(op),
                                      std::move(picked->warnings));
    }
};

} // namespace

std::unique_ptr<IFeatureType> makeOffsetFaceFeatureType()
{
    return std::make_unique<OffsetFaceType>();
}

std::string_view offsetFacePropsSchema()
{
    return schema();
}

} // namespace ray::doc::features
