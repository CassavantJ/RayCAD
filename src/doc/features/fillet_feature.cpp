// The "fillet" feature type and the blend code it shares with "chamfer" (fillet_feature.h).
#include "fillet_feature.h"

#include "feature_props.h"

#include <ray/naming.h>

#include <algorithm>
#include <array>
#include <exception>
#include <new>
#include <set>
#include <utility>

namespace ray::doc::features {
namespace {

using json::Value;

const std::string& filletSchema()
{
    static const std::string text =
        objectSchema(R"("edges","radius")", R"("edges":{"type":"array","minItems":1,"items":)" + topoRefSchema() +
                                                R"(,"description":"The edges to round, all on one body"},"radius":)" +
                                                lengthSchema("Fillet radius"));
    return text;
}

struct BlendProps {
    std::vector<naming::TopoRef> edges;
    Value size;
};

std::string ordinal(std::size_t i, std::size_t n)
{
    return n == 1 ? "its edge" : "edge " + std::to_string(i + 1) + " of " + std::to_string(n);
}

class BlendType final : public IFeatureType,
                        public ParametricType,
                        public detail::BodyDeclaration,
                        public detail::ParametersFromProps {
public:
    explicit BlendType(const BlendKind& kind) : kind_(kind) {}

    std::string_view typeName() const override { return kind_.type; }
    std::string_view propsSchemaJson() const override { return kind_.schema; }
    std::string_view title() const override { return kind_.title; }

    std::vector<naming::TopoRef> references(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? props->edges : std::vector<naming::TopoRef>{};
    }

    std::vector<BodyId> bodiesWritten(const Feature& f) const override
    {
        auto props = parse(f, false);
        return props ? std::vector<BodyId>{props->edges.front().body} : std::vector<BodyId>{};
    }
    bool writesOnlyDeclared() const override { return true; }

    Status validate(const DocState& state, std::size_t, const Feature& f) const override
    {
        auto props = parse(f, true);
        if (!props) {
            return tl::unexpected(props.error());
        }
        return checkNumeric(props->size, documentParams(state), Parameter::Kind::Length, featureLabel(f),
                            std::string(kind_.size));
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
    Result<BlendProps> parse(const Feature& f, bool strict) const
    {
        const std::string where = featureLabel(f);
        auto root = PropsReader::parse(where, f.propsJson);
        if (!root) {
            return tl::unexpected(root.error());
        }
        const PropsReader r(where, *root);
        if (strict) {
            if (auto ok = r.onlyKeys({"edges", kind_.size}); !ok) {
                return tl::unexpected(ok.error());
            }
        }
        BlendProps out;
        const Value* edges = r.find("edges");
        if (edges == nullptr || !edges->isArray() || edges->asArray().empty()) {
            return r.error("edges", "must list at least one edge");
        }
        for (std::size_t i = 0; i < edges->asArray().size(); ++i) {
            const std::string field = "edges[" + std::to_string(i) + "]";
            auto ref = topoRefFromJson(r, edges->asArray()[i], field);
            if (!ref) {
                return tl::unexpected(ref.error());
            }
            if (ref->kind != kernel::SubKind::Edge) {
                return r.error(field, std::string("must be an edge; a ") + subKindName(ref->kind) + " cannot be " +
                                          (kind_.fillet ? "filleted" : "chamfered"));
            }
            if (!out.edges.empty() && ref->body != out.edges.front().body) {
                return r.error(field,
                               "is on another body; one " + std::string(kind_.type) + " blends edges of one body only");
            }
            out.edges.push_back(std::move(*ref));
        }
        auto size = r.numeric(kind_.size, kind_.size);
        if (!size) {
            return tl::unexpected(size.error());
        }
        out.size = std::move(*size);
        return out;
    }

    Result<FeatureOutput> build(const Feature& f, RebuildContext& ctx) const
    {
        const std::string where = featureLabel(f);
        auto props = parse(f, false);
        if (!props) {
            return tl::unexpected(props.error());
        }
        auto size = evaluateNumeric(props->size, ctx.paramValues, Parameter::Kind::Length, where, kind_.size);
        if (!size) {
            return tl::unexpected(size.error());
        }
        if (!(*size > 0)) {
            return invalid(where + ": the " + std::string(kind_.size) + " must be greater than 0.");
        }
        const BodyId body = props->edges.front().body;
        auto shape = ctx.shapeOf(body);
        if (!shape) {
            return failed(shape.error().code, where + ": " + shape.error().message);
        }
        const naming::ElementMap& map = ctx.mapOf(body);
        FeatureOutput out;
        std::set<std::uint32_t> edges;
        std::vector<std::string> broken;
        const std::size_t n = props->edges.size();
        for (std::size_t i = 0; i < n; ++i) {
            const naming::Resolved r = ctx.resolver.resolve(props->edges[i]);
            if (!consumable(r, props->edges[i])) {
                if (r.state == naming::ResolveState::Repaired) {
                    broken.push_back(ordinal(i, n) + " could not be found exactly; a likely match was found (" +
                                     r.reason + "): confirm it in the reference review, or pick the edge again");
                } else {
                    broken.push_back(ordinal(i, n) + " no longer exists" +
                                     (r.reason.empty() ? std::string() : " (" + r.reason + ")") +
                                     ": pick it again or remove it");
                }
                continue;
            }
            for (const kernel::SubId& e : r.elements) {
                if (e.kind != kernel::SubKind::Edge) {
                    return failed(ErrorCode::ReferenceBroken,
                                  where + ": " + ordinal(i, n) + " now resolves to something that is not an edge.");
                }
                edges.insert(e.index);
            }
            if (r.state == naming::ResolveState::Split) {
                out.warnings.push_back(where + ": " + ordinal(i, n) + " was split by an earlier change; all " +
                                       std::to_string(r.elements.size()) + " pieces are blended.");
            } else if (r.state == naming::ResolveState::Merged) {
                out.warnings.push_back(where + ": " + ordinal(i, n) +
                                       " is now part of a longer edge; the whole edge is blended.");
            } else if (r.state == naming::ResolveState::Repaired) {
                out.warnings.push_back(where + ": " + ordinal(i, n) + " uses a repair the user confirmed.");
            }
        }
        if (!broken.empty()) {
            std::string message = where + ": ";
            for (std::size_t i = 0; i < broken.size(); ++i) {
                message += (i ? "; " : "") + broken[i];
            }
            return failed(ErrorCode::ReferenceBroken, message + ".");
        }
        std::vector<kernel::SubId> picked;
        for (const std::uint32_t e : edges) {
            picked.push_back(kernel::SubId{kernel::SubKind::Edge, e});
        }
        JobContext job{ctx.job.cancel, ctx.job.progress.sub(0.9)};
        auto op =
            kind_.fillet ? kernel::fillet(*shape, picked, *size, job) : kernel::chamfer(*shape, picked, *size, job);
        if (!op) {
            if (op.error().code == ErrorCode::Cancelled) {
                return tl::unexpected(op.error());
            }
            return failed(op.error().code, where + ": " + op.error().message);
        }
        for (const std::string& w : op->warnings) {
            out.warnings.push_back(where + ": " + w);
        }
        const std::array<const naming::ElementMap*, 1> maps{&map};
        const naming::NamingInput input{f.id, std::string(kind_.type), maps, &op->history, &op->shape, {}};
        auto named = naming::propagate(input);
        if (!named) {
            return failed(ErrorCode::Internal, where + ": naming the result failed: " + named.error().message);
        }
        ctx.job.progress.set(1.0);
        out.bodies.push_back(BodyUpdate{body, op->shape, std::move(named->map), std::move(named->lineage)});
        return out;
    }

    BlendKind kind_;
};

} // namespace

std::unique_ptr<IFeatureType> makeBlendFeatureType(const BlendKind& kind)
{
    return std::make_unique<BlendType>(kind);
}

std::unique_ptr<IFeatureType> makeFilletFeatureType()
{
    return makeBlendFeatureType(BlendKind{kFilletType, "Fillet", "radius", filletSchema(), true});
}

std::string_view filletPropsSchema()
{
    return filletSchema();
}

} // namespace ray::doc::features
