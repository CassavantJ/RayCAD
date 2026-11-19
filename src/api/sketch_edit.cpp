// The sketch mode's helpers (ray_api/sketch_edit.h). They reuse doc's own readers and writers of the sketch props
// (src/doc/features, private to doc and its friends, as viewer.cpp uses naming's resolver), so the UI writes exactly
// the form the commands read.
#include <ray_api/sketch_edit.h>

#include "../doc/features/feature_props.h"
#include "../doc/features/sketch_feature.h"
#include "../doc/features/sketch_json.h"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace ray::api::sketch_edit {
namespace {

namespace json = ray::doc::json;
namespace features = ray::doc::features;

tl::unexpected<Error> fail(ErrorCode code, std::string message)
{
    return tl::unexpected<Error>(Error{code, std::move(message), {}});
}

const doc::Feature* findFeature(const doc::DocState& state, const FeatureId& id)
{
    const auto it =
        std::find_if(state.features.begin(), state.features.end(), [&](const doc::Feature& f) { return f.id == id; });
    return it == state.features.end() ? nullptr : &*it;
}

// A rebuild context over a finished build: the bodies as they are now, for resolving a face reference.
class LatestBuild final : public doc::RebuildContext {
public:
    LatestBuild(const doc::DocState& state, const std::map<std::string, double>& params,
                const naming::IResolver& resolver, JobContext& job, const doc::BuildResult& build)
        : RebuildContext(state, params, resolver, job), build_(build)
    {
    }
    Result<kernel::Shape> shapeOf(BodyId body) const override
    {
        const auto it = build_.bodies.find(body);
        if (it == build_.bodies.end() || it->second.shape.isNull()) {
            return fail(ErrorCode::NotFound, "The body " + body.value.toString() + " is not in the latest build");
        }
        return it->second.shape;
    }
    const naming::ElementMap& mapOf(BodyId body) const override
    {
        static const naming::ElementMap empty{};
        const auto it = build_.bodies.find(body);
        return it == build_.bodies.end() ? empty : it->second.map;
    }
    Result<std::vector<std::byte>> blob(const Hash256&) const override
    {
        return fail(ErrorCode::NotFound, "No stored objects here");
    }

private:
    const doc::BuildResult& build_;
};

struct PlaneContext {
    const doc::DocState& state;
    const doc::BuildResult* build;
    std::map<std::string, double> params;
    JobContext& job;
    std::unique_ptr<naming::IResolver> resolver;
};

Result<kernel::Plane> planeOfRef(PlaneContext& pc, const features::PlaneRef& ref, int depth)
{
    if (!ref.principal.empty()) {
        if (auto p = features::principalPlane(ref.principal)) {
            return *p;
        }
        return fail(ErrorCode::InvalidArgument, "Unknown principal plane '" + ref.principal + "'");
    }
    if (ref.face) {
        if (pc.build == nullptr) {
            return fail(ErrorCode::NotFound, "The document has not been built yet, so the face cannot be found");
        }
        if (!pc.resolver) {
            std::vector<naming::ResolverBody> bodies;
            for (const auto& [id, body] : pc.build->bodies) {
                if (!body.shape.isNull()) {
                    bodies.push_back(naming::ResolverBody{id, body.map, body.shape});
                }
            }
            pc.resolver = naming::makeResolver(std::move(bodies));
            if (!pc.resolver) {
                return fail(ErrorCode::Internal, "Out of memory");
            }
        }
        LatestBuild ctx(pc.state, pc.params, *pc.resolver, pc.job, *pc.build);
        return features::facePlane(ctx, *ref.face, "The sketch plane");
    }
    if (!ref.datum) {
        return fail(ErrorCode::InvalidArgument, "The plane reference is empty");
    }
    if (depth > 64) {
        return fail(ErrorCode::InvalidArgument, "The datum planes are based on each other in a loop");
    }
    const doc::Feature* datum = findFeature(pc.state, *ref.datum);
    if (datum == nullptr || datum->type != "datum_plane") {
        return fail(ErrorCode::ReferenceBroken,
                    "The datum plane " + ref.datum->value.toString() + " is not in the document any more");
    }
    const std::string where = features::featureLabel(*datum);
    auto root = features::PropsReader::parse(where, datum->propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    const features::PropsReader r(where, *root);
    auto base = features::readPlaneRef(r, "base", "base");
    if (!base) {
        return tl::unexpected(base.error());
    }
    auto offsetValue = r.numeric("offset", "offset", json::Value::number(0.0));
    if (!offsetValue) {
        return tl::unexpected(offsetValue.error());
    }
    auto offset = features::evaluateNumeric(*offsetValue, pc.params, doc::Parameter::Kind::Length, where, "offset");
    if (!offset) {
        return tl::unexpected(offset.error());
    }
    auto flip = r.optBool("flip", "flip", false);
    if (!flip) {
        return tl::unexpected(flip.error());
    }
    auto plane = planeOfRef(pc, *base, depth + 1);
    if (!plane) {
        return plane;
    }
    kernel::Plane out = *plane;
    out.origin = features::vadd(plane->origin, features::vmul(plane->normal, *offset));
    if (*flip) {
        out.normal = features::vmul(plane->normal, -1.0);
        out.xDir = features::vmul(plane->xDir, -1.0);
    }
    return out;
}

// The "entities" and "constraints" arrays of a model in sketch_json.h form.
std::pair<json::Value, json::Value> arraysOf(const sketch::Sketch& model)
{
    json::Value all = features::sketchToJson(model);
    const json::Value* e = all.find("entities");
    const json::Value* c = all.find("constraints");
    return {e != nullptr ? *e : json::Value::array({}), c != nullptr ? *c : json::Value::array({})};
}

json::Value parsedOrNull(std::string_view text)
{
    auto v = json::parse(text);
    return v ? std::move(*v) : json::Value();
}

} // namespace

Result<sketch::Sketch> modelOf(const doc::DocState& state, const FeatureId& id)
{
    const doc::Feature* f = findFeature(state, id);
    if (f == nullptr) {
        return fail(ErrorCode::NotFound, "The document has no feature " + id.value.toString());
    }
    if (f->type != features::kSketchType) {
        return fail(ErrorCode::InvalidArgument,
                    "'" + features::featureLabel(*f) + "' is a " + f->type + ", not a sketch");
    }
    try {
        return features::sketchOfFeature(*f, false);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::Internal, "Out of memory");
    }
}

Result<std::string> planeJsonOf(const doc::DocState& state, const FeatureId& id)
{
    const doc::Feature* f = findFeature(state, id);
    if (f == nullptr) {
        return fail(ErrorCode::NotFound, "The document has no feature " + id.value.toString());
    }
    auto root = features::PropsReader::parse(features::featureLabel(*f), f->propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    const json::Value* plane = root->find("plane");
    if (plane == nullptr) {
        return fail(ErrorCode::InvalidArgument, "'" + features::featureLabel(*f) + "' has no plane");
    }
    return json::write(*plane);
}

Result<kernel::Plane> resolvePlane(const doc::DocState& state, const doc::BuildResult* build,
                                   std::string_view planeJson, JobContext& ctx)
{
    try {
        auto plane = json::parse(planeJson);
        if (!plane || !plane->isObject()) {
            return fail(ErrorCode::InvalidArgument, "The plane reference is not a JSON object");
        }
        const json::Value holder = json::Value::object({json::Member{"plane", std::move(*plane)}});
        const features::PropsReader r("The sketch plane", holder);
        auto ref = features::readPlaneRef(r, "plane", "plane");
        if (!ref) {
            return tl::unexpected(ref.error());
        }
        PlaneContext pc{state, build, {}, ctx, nullptr};
        if (auto params = doc::evaluateParameters(state.parameters)) {
            pc.params = std::move(*params);
        }
        return planeOfRef(pc, *ref, 0);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::Internal, "Out of memory");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, std::string("Unexpected failure: ") + e.what());
    }
}

std::string entityAddArgs(const FeatureId& sketchId, const std::vector<sketch::Entity>& entities,
                          const std::vector<sketch::Constraint>& constraints)
{
    sketch::Sketch part{entities, constraints};
    auto [e, c] = arraysOf(part);
    json::Value::Object members{{"id", json::Value::string(sketchId.value.toString())}, {"entities", std::move(e)}};
    if (!constraints.empty()) {
        members.push_back({"constraints", std::move(c)});
    }
    return json::write(json::Value::object(std::move(members)));
}

std::string sketchSetArgs(const FeatureId& sketchId, const sketch::Sketch& model)
{
    return json::write(json::Value::object(
        {{"id", json::Value::string(sketchId.value.toString())}, {"sketch", features::sketchToJson(model)}}));
}

std::string sketchAddArgs(const FeatureId& id, std::string_view name, std::string_view planeJson,
                          const sketch::Sketch& model)
{
    json::Value props =
        json::Value::object({{"plane", parsedOrNull(planeJson)}, {"sketch", features::sketchToJson(model)}});
    return json::write(json::Value::object({{"id", json::Value::string(id.value.toString())},
                                            {"name", json::Value::string(std::string(name))},
                                            {"props", std::move(props)}}));
}

std::string datumAddArgs(const FeatureId& id, std::string_view name, std::string_view basePlaneJson, double offsetMm)
{
    json::Value props =
        json::Value::object({{"base", parsedOrNull(basePlaneJson)}, {"offset", json::Value::number(offsetMm)}});
    return json::write(json::Value::object({{"id", json::Value::string(id.value.toString())},
                                            {"name", json::Value::string(std::string(name))},
                                            {"props", std::move(props)}}));
}

std::unique_ptr<sketch::ISketchSolver> makeSolver()
{
    return sketch::makePlaneGcsSolver();
}

} // namespace ray::api::sketch_edit
