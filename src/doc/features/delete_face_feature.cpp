// The "delete_face" feature type and the code the three direct-edit types share (delete_face_feature.h, task T-082).
#include "delete_face_feature.h"

#include <ray/naming.h>

#include <algorithm>
#include <array>
#include <exception>
#include <new>
#include <set>
#include <utility>

namespace ray::doc::features {

// ---------- Shared by the three direct-edit types ----------
namespace directedit {

using json::Value;

std::string aKind(kernel::SubKind kind)
{
    return std::string(kind == kernel::SubKind::Edge ? "an " : "a ") + subKindName(kind);
}

std::string facesSchema(std::string_view description)
{
    return R"({"type":"array","minItems":1,"items":)" + topoRefSchema() + R"(,"description":")" +
           std::string(description) + R"("})";
}

Result<naming::TopoRef> readFaceRef(const PropsReader& r, std::string_view key)
{
    const Value* v = r.find(key);
    const std::string field(key);
    if (v == nullptr) {
        return r.error(field, "is required");
    }
    auto ref = topoRefFromJson(r, *v, field);
    if (!ref) {
        return tl::unexpected(ref.error());
    }
    if (ref->kind != kernel::SubKind::Face) {
        return r.error(field, "must be a face, not " + aKind(ref->kind));
    }
    return ref;
}

Result<std::vector<naming::TopoRef>> readFaceRefs(const PropsReader& r, std::string_view key)
{
    const Value* faces = r.find(key);
    const std::string name(key);
    if (faces == nullptr || !faces->isArray() || faces->asArray().empty()) {
        return r.error(name, "must list at least one face");
    }
    std::vector<naming::TopoRef> out;
    for (std::size_t i = 0; i < faces->asArray().size(); ++i) {
        const std::string field = name + "[" + std::to_string(i) + "]";
        auto ref = topoRefFromJson(r, faces->asArray()[i], field);
        if (!ref) {
            return tl::unexpected(ref.error());
        }
        if (ref->kind != kernel::SubKind::Face) {
            return r.error(field, "must be a face, not " + aKind(ref->kind));
        }
        if (!out.empty() && ref->body != out.front().body) {
            return r.error(field, "is on another body; one direct edit changes faces of one body only");
        }
        out.push_back(std::move(*ref));
    }
    return out;
}

Status checkBody(const DocState& state, const BodyId& body, const std::string& where, std::string_view field)
{
    if (std::any_of(state.bodies.begin(), state.bodies.end(), [&](const Body& b) { return b.id == body; })) {
        return {};
    }
    return invalid(where + ": '" + std::string(field) + "' is on body " + body.value.toString() +
                   ", which is not in the document");
}

std::string faceWord(std::size_t i, std::size_t n)
{
    return n == 1 ? "its face" : "face " + std::to_string(i + 1) + " of " + std::to_string(n);
}

Result<ResolvedFaces> resolveFaces(const RebuildContext& ctx, std::span<const naming::TopoRef> refs,
                                   const std::string& where, std::string_view done)
{
    ResolvedFaces out;
    std::set<kernel::SubId> faces;
    std::vector<std::string> broken;
    const std::size_t n = refs.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::string which = faceWord(i, n);
        const naming::Resolved r = ctx.resolver.resolve(refs[i]);
        out.perRef.emplace_back();
        if (!r.usable()) {
            if (r.state == naming::ResolveState::Repaired) {
                broken.push_back(which + " could not be found exactly; a likely match was found (" + r.reason +
                                 "): confirm it in the reference review, or pick the face again");
            } else {
                broken.push_back(which + " no longer exists" +
                                 (r.reason.empty() ? std::string() : " (" + r.reason + ")") +
                                 ": pick it again or remove it");
            }
            continue;
        }
        for (const kernel::SubId& e : r.elements) {
            if (e.kind != kernel::SubKind::Face) {
                return failed(ErrorCode::ReferenceBroken,
                              where + ": " + which + " now resolves to something that is not a face.");
            }
            faces.insert(e);
            out.perRef.back().push_back(e);
        }
        if (r.state == naming::ResolveState::Split) {
            out.warnings.push_back(where + ": " + which + " was split by an earlier change; all " +
                                   std::to_string(r.elements.size()) + " pieces are " + std::string(done) + ".");
        } else if (r.state == naming::ResolveState::Merged) {
            out.warnings.push_back(where + ": " + which + " is now part of a larger face; the whole face is " +
                                   std::string(done) + ".");
        } else if (r.state == naming::ResolveState::Repaired) {
            out.warnings.push_back(where + ": " + which + " uses a repair the user confirmed.");
        }
    }
    if (!broken.empty()) {
        std::string message = where + ": ";
        for (std::size_t i = 0; i < broken.size(); ++i) {
            message += (i ? "; " : "") + broken[i];
        }
        return failed(ErrorCode::ReferenceBroken, message + ".");
    }
    out.faces.assign(faces.begin(), faces.end());
    return out;
}

Result<kernel::Shape> bodyShape(const RebuildContext& ctx, const BodyId& body, const std::string& where)
{
    auto shape = ctx.shapeOf(body);
    if (!shape) {
        return failed(shape.error().code, where + ": " + shape.error().message);
    }
    if (shape->isNull()) {
        return failed(ErrorCode::ReferenceBroken, where + ": the body to edit is not built.");
    }
    return shape;
}

Result<FeatureOutput> finishEdit(const Feature& f, std::string_view type, const RebuildContext& ctx, const BodyId& body,
                                 const kernel::Shape& input, Result<kernel::OpResult> op,
                                 std::vector<std::string> warnings)
{
    const std::string where = featureLabel(f);
    if (!op) {
        if (op.error().code == ErrorCode::Cancelled) {
            return tl::unexpected(op.error());
        }
        return failed(op.error().code, where + ": " + op.error().message);
    }
    FeatureOutput out;
    out.warnings = std::move(warnings);
    for (const std::string& w : op->warnings) {
        out.warnings.push_back(where + ": " + w);
    }
    const naming::ElementMap& map = ctx.mapOf(body);
    const std::array<const naming::ElementMap*, 1> maps{&map};
    const std::array<const kernel::Shape*, 1> shapes{&input};
    const naming::NamingInput naming{f.id, std::string(type), maps, &op->history, &op->shape, {}, std::span(shapes)};
    auto named = naming::propagate(naming);
    if (!named) {
        return failed(ErrorCode::Internal, where + ": naming the result failed: " + named.error().message);
    }
    ctx.job.progress.set(1.0);
    out.bodies.push_back(BodyUpdate{body, op->shape, std::move(named->map), std::move(named->lineage)});
    return out;
}

} // namespace directedit

// ---------- delete_face ----------
namespace {

const std::string& schema()
{
    static const std::string text = objectSchema(
        R"("faces")", R"("faces":)" + directedit::facesSchema(
                                          "The faces to remove, all on one body; the faces around them are extended to "
                                          "close the gap. Any face of a hole stands for the whole hole (with its "
                                          "counterbore, countersink or chamfer); a fillet that joins other blends "
                                          "needs its whole blend run (model.blendRun gives it)"));
    return text;
}

struct Props {
    std::vector<naming::TopoRef> faces;
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
        if (auto ok = r.onlyKeys({"faces"}); !ok) {
            return tl::unexpected(ok.error());
        }
    }
    auto faces = directedit::readFaceRefs(r, "faces");
    if (!faces) {
        return tl::unexpected(faces.error());
    }
    return Props{std::move(*faces)};
}

class DeleteFaceType final : public IFeatureType,
                             public ParametricType,
                             public detail::BodyDeclaration,
                             public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kDeleteFaceType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Delete Face"; }

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
        return directedit::checkBody(state, props->faces.front().body, featureLabel(f), "faces");
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
        const BodyId body = props->faces.front().body;
        auto shape = directedit::bodyShape(ctx, body, where);
        if (!shape) {
            return tl::unexpected(shape.error());
        }
        auto picked = directedit::resolveFaces(ctx, props->faces, where, "deleted");
        if (!picked) {
            return tl::unexpected(picked.error());
        }
        JobContext job{ctx.job.cancel, ctx.job.progress.sub(0.9)};
        auto op = kernel::deleteFaces(*shape, picked->faces, job);
        return directedit::finishEdit(f, kDeleteFaceType, ctx, body, *shape, std::move(op),
                                      std::move(picked->warnings));
    }
};

} // namespace

std::unique_ptr<IFeatureType> makeDeleteFaceFeatureType()
{
    return std::make_unique<DeleteFaceType>();
}

std::string_view deleteFacePropsSchema()
{
    return schema();
}

} // namespace ray::doc::features
