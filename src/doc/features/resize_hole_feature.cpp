// The "resize_hole" feature type (resize_hole_feature.h, task T-082).
#include "resize_hole_feature.h"

#include "delete_face_feature.h"

#include <cmath>
#include <exception>
#include <new>
#include <utility>

namespace ray::doc::features {
namespace {

using json::Value;

const std::string& schema()
{
    static const std::string text = objectSchema(
        R"("diameter","face")",
        R"("face":)" + topoRefSchema() + R"(,"diameter":)" +
            lengthSchema("The new diameter of the hole the face belongs to (any face of its bore stands for the whole "
                         "bore)"));
    return text;
}

struct Props {
    naming::TopoRef face;
    Value diameter;
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
        if (auto ok = r.onlyKeys({"diameter", "face"}); !ok) {
            return tl::unexpected(ok.error());
        }
    }
    auto face = directedit::readFaceRef(r, "face");
    if (!face) {
        return tl::unexpected(face.error());
    }
    auto diameter = r.numeric("diameter", "diameter");
    if (!diameter) {
        return tl::unexpected(diameter.error());
    }
    return Props{std::move(*face), std::move(*diameter)};
}

// True if every image of `piece` in `op` is a cylinder of radius `radius`: the piece was resized.
bool resized(const kernel::OpResult& op, const kernel::SubId& piece, double radius)
{
    for (const kernel::HistoryEntry& e : op.history.entries) {
        if (e.inputIndex != 0 || e.source != piece || e.relation == kernel::HistoryEntry::Relation::Generated) {
            continue;
        }
        if (e.relation != kernel::HistoryEntry::Relation::Modified || e.results.empty()) {
            return false;
        }
        for (const kernel::SubId& image : e.results) {
            auto info = kernel::surfaceInfo(op.shape, image);
            if (!info || info->type != kernel::SurfaceType::Cylinder || std::fabs(info->radius - radius) > 1e-6) {
                return false;
            }
        }
        return true;
    }
    return false;
}

class ResizeHoleType final : public IFeatureType,
                             public ParametricType,
                             public detail::BodyDeclaration,
                             public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kResizeHoleType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Resize Hole"; }

    std::vector<naming::TopoRef> references(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? std::vector<naming::TopoRef>{props->face} : std::vector<naming::TopoRef>{};
    }

    std::vector<BodyId> bodiesWritten(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? std::vector<BodyId>{props->face.body} : std::vector<BodyId>{};
    }
    bool writesOnlyDeclared() const override { return true; }

    Status validate(const DocState& state, std::size_t, const Feature& f) const override
    {
        auto props = parse(f, true);
        if (!props) {
            return tl::unexpected(props.error());
        }
        const std::string where = featureLabel(f);
        if (auto ok = directedit::checkBody(state, props->face.body, where, "face"); !ok) {
            return ok;
        }
        return checkNumeric(props->diameter, documentParams(state), Parameter::Kind::Length, where, "diameter");
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
        auto diameter = evaluateNumeric(props->diameter, ctx.paramValues, Parameter::Kind::Length, where, "diameter");
        if (!diameter) {
            return tl::unexpected(diameter.error());
        }
        if (!(*diameter > 0)) {
            return invalid(where + ": the diameter must be greater than 0.");
        }
        const BodyId body = props->face.body;
        auto shape = directedit::bodyShape(ctx, body, where);
        if (!shape) {
            return tl::unexpected(shape.error());
        }
        auto picked = directedit::resolveFaces(ctx, std::span(&props->face, 1), where, "resized");
        if (!picked) {
            return tl::unexpected(picked.error());
        }
        const std::vector<kernel::SubId>& pieces = picked->perRef.front();
        JobContext job{ctx.job.cancel, ctx.job.progress.sub(0.9)};
        auto op = kernel::resizeHole(*shape, pieces.front(), *diameter, job);
        if (op && pieces.size() > 1) {
            for (const kernel::SubId& piece : pieces) {
                if (!resized(*op, piece, *diameter / 2)) {
                    return failed(ErrorCode::ReferenceBroken,
                                  where + ": the hole face was split by an earlier change into pieces that are not "
                                          "one hole, so only part of it would be resized. Pick the hole again. "
                                          "Nothing was changed.");
                }
            }
        }
        return directedit::finishEdit(f, kResizeHoleType, ctx, body, *shape, std::move(op),
                                      std::move(picked->warnings));
    }
};

} // namespace

std::unique_ptr<IFeatureType> makeResizeHoleFeatureType()
{
    return std::make_unique<ResizeHoleType>();
}

std::string_view resizeHolePropsSchema()
{
    return schema();
}

} // namespace ray::doc::features
