// The "extrude" feature type and the sweep code it shares with "revolve" (extrude_feature.h).
#include "extrude_feature.h"

#include "sketch_json.h"

#include <ray/naming.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iterator>
#include <new>
#include <set>
#include <span>
#include <utility>

namespace ray::doc::features {

using json::Value;

namespace {

const std::string& schema()
{
    static const std::string text = [] {
        std::string s = objectSchema(
            R"("sketch","operation","target")",
            R"("sketch":)" + uuidSchema() + R"(,"regions":)" + regionsSchema() +
                R"(,"extent":{"enum":["distance",)"
                R"("through_all"],"default":"distance","description":"distance: as long as 'distance'; through_all: )"
                R"(through the whole target body, worked out again at every rebuild; cut, join and intersect only"},)"
                R"("distance":)" +
                lengthSchema("Extrusion length, for the extent distance") +
                R"(,"direction":{"enum":["normal","reverse"],"default":"normal","description":"Along the sketch )"
                R"(normal or against it"},"symmetric":{"type":"boolean","default":false,"description":"Half the )"
                R"(distance to each side of the sketch; through_all: through the body on both sides"},"draft":)" +
                angleSchema("Draft angle; positive tapers inwards; default 0") + R"(,"operation":)" +
                operationSchema() + R"(,"target":)" + uuidSchema());
        // 'distance' is required unless the extent is through_all.
        s.pop_back();
        s += R"(,"anyOf":[{"required":["distance"]},{"required":["extent"],"properties":{"extent":)"
             R"({"const":"through_all"}}}]})";
        return s;
    }();
    return text;
}

struct ExtrudeProps {
    SweepCommon common;
    bool throughAll = false; // "extent": "through_all"
    Value distance;          // unused when throughAll
    bool reverse = false;
    bool symmetric = false;
    Value draft;
};

Result<ExtrudeProps> parse(const Feature& f, bool strict)
{
    const std::string where = featureLabel(f);
    auto root = PropsReader::parse(where, f.propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    const PropsReader r(where, *root);
    if (strict) {
        if (auto ok = r.onlyKeys(
                {"direction", "distance", "draft", "extent", "operation", "regions", "sketch", "symmetric", "target"});
            !ok) {
            return tl::unexpected(ok.error());
        }
    }
    ExtrudeProps out;
    auto common = readSweepCommon(r);
    if (!common) {
        return tl::unexpected(common.error());
    }
    out.common = std::move(*common);
    // A feature without "extent" (every one made before it existed) is a distance extrude.
    auto extent = r.choice("extent", "extent", {"distance", "through_all"}, "distance");
    if (!extent) {
        return tl::unexpected(extent.error());
    }
    out.throughAll = *extent == "through_all";
    auto distance =
        out.throughAll ? r.numeric("distance", "distance", Value::number(0.0)) : r.numeric("distance", "distance");
    auto direction = r.choice("direction", "direction", {"normal", "reverse"}, "normal");
    auto symmetric = r.optBool("symmetric", "symmetric", false);
    auto draft = r.numeric("draft", "draft", Value::number(0.0));
    if (!distance || !direction || !symmetric || !draft) {
        return tl::unexpected(!distance    ? distance.error()
                              : !direction ? direction.error()
                              : !symmetric ? symmetric.error()
                                           : draft.error());
    }
    out.distance = std::move(*distance);
    out.reverse = *direction == "reverse";
    out.symmetric = *symmetric;
    out.draft = std::move(*draft);
    return out;
}

std::string kernelMessage(const std::string& where, const Error& e)
{
    return where + ": " + e.message;
}

// A through-all extrude: where the profile starts along the sketch normal (an offset from the sketch plane) and the
// signed length that takes it through every part of the target body on the chosen side (both sides when
// symmetric), from the body's bounding box as it is before this feature. A cut or intersect goes a margin past it so
// it leaves no skin; a join ends at the box, so it adds nothing beyond the body.
Result<std::pair<double, double>> throughAllSpan(const Feature& f, RebuildContext& ctx, const BodyId& target,
                                                 const kernel::Plane& plane, bool bothSides, bool reverse, bool join)
{
    const std::string where = featureLabel(f);
    auto shape = ctx.shapeOf(target);
    if (!shape) {
        return failed(shape.error().code,
                      where + ": the body it goes through is not available: " + shape.error().message);
    }
    const std::optional<Vec3> normal = vunit(plane.normal);
    if (!normal) {
        return failed(ErrorCode::Internal, where + ": the sketch plane has no normal.");
    }
    const Box3 box = kernel::bounds(*shape);
    double lo = 0, hi = 0;
    bool first = true;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 p{(corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y,
                     (corner & 4) ? box.max.z : box.min.z};
        const double d = vdot(vsub(p, plane.origin), *normal);
        lo = first ? d : std::min(lo, d);
        hi = first ? d : std::max(hi, d);
        first = false;
    }
    const double size = vnorm(vsub(box.max, box.min));
    if (!(size > 0) || !std::isfinite(size)) {
        return invalid(where + ": the body it goes through is empty.");
    }
    const double margin = join ? 0.0 : 1.0 + 0.01 * size; // mm past the body
    constexpr double kTiny = 1e-7;
    if (bothSides) {
        const double from = std::min(lo, 0.0) - margin;
        const double to = std::max(hi, 0.0) + margin;
        return std::pair<double, double>(from, to - from);
    }
    if (!reverse) {
        if (!(hi > kTiny)) {
            return invalid(where + ": no part of the body lies on that side of the sketch; reverse the direction.");
        }
        return std::pair<double, double>(0.0, std::max(hi + margin, kTiny));
    }
    if (!(lo < -kTiny)) {
        return invalid(where + ": no part of the body lies on that side of the sketch; reverse the direction.");
    }
    return std::pair<double, double>(0.0, lo - margin);
}

class ExtrudeType final : public IFeatureType,
                          public ParametricType,
                          public detail::BodyDeclaration,
                          public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kExtrudeType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Extrude"; }
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
        const DocumentParams params = documentParams(state);
        const std::string where = featureLabel(f);
        if (props->throughAll && props->common.operation == "new") {
            return invalid(where + ": 'extent' through_all goes through a body: cut, join or intersect one (a new "
                                   "body needs a distance).");
        }
        if (!props->throughAll) {
            if (auto ok = checkNumeric(props->distance, params, Parameter::Kind::Length, where, "distance"); !ok) {
                return ok;
            }
        }
        if (auto ok = checkNumeric(props->draft, params, Parameter::Kind::Angle, where, "draft"); !ok) {
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
            auto distance = props->throughAll ? Result<double>(0.0)
                                              : evaluateNumeric(props->distance, ctx.paramValues,
                                                                Parameter::Kind::Length, where, "distance");
            // (through_all: the length comes from the body below)
            if (!distance) {
                return tl::unexpected(distance.error());
            }
            auto draft = evaluateNumeric(props->draft, ctx.paramValues, Parameter::Kind::Angle, where, "draft");
            if (!draft) {
                return tl::unexpected(draft.error());
            }
            if (!props->throughAll && !(std::abs(*distance) > 1e-7)) {
                return invalid(where + ": the distance must not be zero.");
            }
            const Feature* sketchFeature = nullptr;
            auto artifact = sketchFor(f, ctx, props->common.sketch, &sketchFeature);
            if (!artifact) {
                return tl::unexpected(artifact.error());
            }
            auto regions = selectRegions(**artifact, props->common, where, featureLabel(*sketchFeature));
            if (!regions) {
                return tl::unexpected(regions.error());
            }
            kernel::Plane plane = (*artifact)->plane;
            // The sketch's plane, before through-all or symmetric shift it along the normal: the frame the profile
            // is drawn in.
            const Xform placement = sweepPlacement(plane);
            double signedDistance = props->reverse ? -*distance : *distance;
            if (props->throughAll) {
                // Through the whole target as it is now, so it stays through-all after edits upstream.
                auto span = throughAllSpan(f, ctx, props->common.target, plane, props->symmetric, props->reverse,
                                           props->common.operation == "join");
                if (!span) {
                    return tl::unexpected(span.error());
                }
                plane.origin = vadd(plane.origin, vmul(plane.normal, span->first));
                signedDistance = span->second;
            } else if (props->symmetric) {
                const double full = std::abs(*distance);
                plane.origin = vsub(plane.origin, vmul(plane.normal, full / 2));
                signedDistance = full;
            }
            const double draftAngle = *draft;
            return buildSweep(
                f, ctx, props->common, plane, *regions, "extrude",
                [&](const kernel::Profile& profile, JobContext& job) {
                    return kernel::extrude(profile, signedDistance, draftAngle, job);
                },
                placement);
        } catch (const std::bad_alloc&) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": out of memory");
        } catch (...) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": unexpected failure");
        }
    }
};

Result<std::vector<SketchEntId>> readIds(const PropsReader& r, const Value& list, const std::string& field)
{
    std::vector<SketchEntId> out;
    if (!list.isArray() || list.asArray().empty()) {
        return r.error(field, "must list the sketch entities of the region's outline");
    }
    for (const Value& v : list.asArray()) {
        const Uuid id = v.isString() ? Uuid::fromString(v.asString()) : Uuid{};
        if (!v.isString() || id.toString() != v.asString() || id == Uuid{}) {
            return r.error(field, "must list sketch entity ids (lower-case UUIDs)");
        }
        out.push_back(SketchEntId{id});
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::size_t shared(const std::vector<SketchEntId>& a, const std::vector<SketchEntId>& b)
{
    std::vector<SketchEntId> both;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(both));
    return both.size();
}

} // namespace

Result<SweepCommon> readSweepCommon(const PropsReader& r)
{
    SweepCommon out;
    auto sketch = r.uuid("sketch", "sketch");
    if (!sketch) {
        return tl::unexpected(sketch.error());
    }
    out.sketch = FeatureId{*sketch};
    if (const Value* regions = r.find("regions")) {
        if (!regions->isArray() || regions->asArray().empty()) {
            return r.error("regions", "must list at least one region (or be left out for all regions)");
        }
        std::vector<std::vector<SketchEntId>> list;
        for (std::size_t i = 0; i < regions->asArray().size(); ++i) {
            auto ids = readIds(r, regions->asArray()[i], "regions[" + std::to_string(i) + "]");
            if (!ids) {
                return tl::unexpected(ids.error());
            }
            list.push_back(std::move(*ids));
        }
        out.regions = std::move(list);
    }
    auto operation = r.choice("operation", "operation", {"new", "join", "cut", "intersect"}, std::nullopt);
    if (!operation) {
        return tl::unexpected(operation.error());
    }
    out.operation = *operation;
    auto target = r.uuid("target", "target");
    if (!target) {
        return tl::unexpected(target.error());
    }
    out.target = BodyId{*target};
    return out;
}

std::uint64_t regionTag(std::vector<SketchEntId> entities)
{
    std::sort(entities.begin(), entities.end());
    Hasher h;
    h.str("ray.doc.sweep.region/1");
    for (const SketchEntId& e : entities) {
        h.bytes(std::as_bytes(std::span<const std::uint8_t>(e.value.bytes)));
    }
    const Hash256 d = h.finish();
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | d.bytes[static_cast<std::size_t>(i)];
    }
    return v;
}

Result<std::vector<std::vector<kernel::ProfileLoop>>> selectRegions(const SketchArtifact& sketch,
                                                                    const SweepCommon& props, const std::string& where,
                                                                    const std::string& sketchName)
{
    if (sketch.regions.empty()) {
        return invalid(where + ": the sketch '" + sketchName + "' has no closed outline to sweep" +
                       (sketch.regionError ? " (" + *sketch.regionError + ")." : "."));
    }
    auto loopsOf = [](const SketchRegion& region, std::uint64_t tag) {
        std::vector<kernel::ProfileLoop> loops;
        loops.push_back(region.outer);
        loops.front().tag = tag;
        loops.insert(loops.end(), region.holes.begin(), region.holes.end());
        return loops;
    };
    std::vector<std::vector<kernel::ProfileLoop>> out;
    if (!props.regions) {
        // Every region that does not fill a hole of another one, so a circle inside a rectangle makes a hole; a
        // region inside that hole (an island) again counts, one inside the island's hole does not, and so on.
        auto tagsOf = [](const kernel::ProfileLoop& loop) {
            std::set<std::uint64_t> tags;
            for (const kernel::ProfileCurve& c : loop.curves) {
                tags.insert(c.tag);
            }
            return tags;
        };
        const std::size_t n = sketch.regions.size();
        std::vector<int> parent(n, -1);
        for (std::size_t i = 0; i < n; ++i) {
            const std::set<std::uint64_t> outer = tagsOf(sketch.regions[i].outer);
            for (std::size_t j = 0; j < n && parent[i] < 0; ++j) {
                for (const kernel::ProfileLoop& hole : sketch.regions[j].holes) {
                    if (j != i && tagsOf(hole) == outer) {
                        parent[i] = static_cast<int>(j);
                        break;
                    }
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            int depth = 0;
            for (int p = parent[i]; p >= 0 && depth <= static_cast<int>(n); p = parent[static_cast<std::size_t>(p)]) {
                ++depth;
            }
            if (depth % 2 == 0) {
                out.push_back(loopsOf(sketch.regions[i], regionTag(sketch.regions[i].outerEntities)));
            }
        }
        return out;
    }
    std::set<std::size_t> taken;
    for (std::size_t k = 0; k < props.regions->size(); ++k) {
        const std::vector<SketchEntId>& wanted = (*props.regions)[k];
        std::size_t best = 0;
        std::size_t bestScore = 0;
        bool tie = false;
        for (std::size_t i = 0; i < sketch.regions.size(); ++i) {
            const std::size_t score = shared(wanted, sketch.regions[i].outerEntities);
            if (score > bestScore) {
                best = i;
                bestScore = score;
                tie = false;
            } else if (score == bestScore && score > 0) {
                tie = true;
            }
        }
        const std::string which = props.regions->size() == 1 ? "the region" : "region " + std::to_string(k + 1);
        if (bestScore == 0) {
            return invalid(where + ": " + which + " it sweeps is no longer in the sketch '" + sketchName +
                           "' (its outline curves were deleted); pick the region again.");
        }
        if (tie) {
            return invalid(where + ": " + which + " it sweeps can no longer be told apart in the sketch '" +
                           sketchName + "' (a new curve divides it); pick the region again.");
        }
        if (!taken.insert(best).second) {
            return invalid(where + ": two of its regions are now the same region of the sketch '" + sketchName +
                           "'; pick the regions again.");
        }
        out.push_back(loopsOf(sketch.regions[best], regionTag(wanted)));
    }
    return out;
}

Result<std::shared_ptr<const SketchArtifact>> sketchFor(const Feature& self, RebuildContext& ctx,
                                                        const FeatureId& sketch, const Feature** sketchFeature)
{
    auto feature = earlierFeature(ctx.state, self, sketch, kSketchType, "sketch");
    if (!feature) {
        return tl::unexpected(feature.error());
    }
    if (sketchFeature != nullptr) {
        *sketchFeature = *feature;
    }
    auto artifact = std::dynamic_pointer_cast<const SketchArtifact>(detail::artifactOf(ctx, sketch));
    if (!artifact) {
        return failed(ErrorCode::ReferenceBroken,
                      featureLabel(self) + ": its sketch '" + featureLabel(**feature) + "' did not build.");
    }
    return artifact;
}

Xform sweepPlacement(const kernel::Plane& plane)
{
    const Vec3 x = plane.xDir;
    const Vec3 y = planeYDir(plane);
    const Vec3 n = plane.normal;
    const Vec3 o = plane.origin;
    return Xform{{x.x, y.x, n.x, o.x, x.y, y.y, n.y, o.y, x.z, y.z, n.z, o.z}};
}

Result<FeatureOutput> buildSweep(const Feature& self, RebuildContext& ctx, const SweepCommon& props,
                                 const kernel::Plane& plane,
                                 const std::vector<std::vector<kernel::ProfileLoop>>& regions, std::string_view role,
                                 const SweepOne& sweepOne, const std::optional<Xform>& placement)
{
    const std::string where = featureLabel(self);
    const std::string featureRole(role);
    FeatureOutput out;
    std::vector<naming::LineageEvent> lineage;
    std::optional<kernel::Shape> shape;
    naming::ElementMap map;
    if (props.operation == "join" || props.operation == "cut" || props.operation == "intersect") {
        auto target = ctx.shapeOf(props.target);
        if (!target) {
            return failed(target.error().code, where + ": the body it " +
                                                   (props.operation == "join"  ? "joins"
                                                    : props.operation == "cut" ? "cuts"
                                                                               : "intersects") +
                                                   " is not available: " + target.error().message);
        }
        shape = *target;
        map = ctx.mapOf(props.target);
    }
    // For "intersect" the regions are fused into one tool first.
    std::optional<kernel::Shape> tool;
    naming::ElementMap toolMap;
    const double share = 1.0 / static_cast<double>(regions.size() + 1);
    for (const std::vector<kernel::ProfileLoop>& loops : regions) {
        if (auto ok = ctx.job.cancel.check(); !ok) {
            return tl::unexpected(ok.error());
        }
        JobContext job{ctx.job.cancel, ctx.job.progress.sub(share)};
        const kernel::Profile profile{plane, loops};
        auto swept = sweepOne(profile, job);
        if (!swept) {
            if (swept.error().code == ErrorCode::Cancelled) {
                return tl::unexpected(swept.error());
            }
            return failed(swept.error().code, kernelMessage(where, swept.error()));
        }
        out.warnings.insert(out.warnings.end(), swept->op.warnings.begin(), swept->op.warnings.end());
        naming::NamingInput toolInput{self.id, featureRole, {}, nullptr, &swept->op.shape, swept->origins};
        toolInput.placement = placement;
        auto named = naming::propagate(toolInput);
        if (!named) {
            return failed(ErrorCode::Internal, where + ": naming the swept faces failed: " + named.error().message);
        }
        // The shape and map this region goes into, and the operation.
        std::optional<kernel::Shape>& into = props.operation == "intersect" ? tool : shape;
        naming::ElementMap& intoMap = props.operation == "intersect" ? toolMap : map;
        if (!into) {
            into = swept->op.shape;
            intoMap = std::move(named->map);
            continue;
        }
        const kernel::BoolOp op = props.operation == "cut" ? kernel::BoolOp::Cut : kernel::BoolOp::Union;
        auto combined = kernel::boolean(op, *into, swept->op.shape, job);
        if (!combined) {
            if (combined.error().code == ErrorCode::Cancelled) {
                return tl::unexpected(combined.error());
            }
            return failed(combined.error().code, kernelMessage(where, combined.error()));
        }
        out.warnings.insert(out.warnings.end(), combined->warnings.begin(), combined->warnings.end());
        const std::array<const naming::ElementMap*, 2> maps{&intoMap, &named->map};
        const std::array<const kernel::Shape*, 2> shapes{&*into, &swept->op.shape};
        naming::NamingInput input{self.id, featureRole,      maps, &combined->history, &combined->shape,
                                  {},      std::span(shapes)};
        input.placement = placement;
        auto result = naming::propagate(input);
        if (!result) {
            return failed(ErrorCode::Internal, where + ": naming the result failed: " + result.error().message);
        }
        into = combined->shape;
        intoMap = std::move(result->map);
        lineage.insert(lineage.end(), result->lineage.begin(), result->lineage.end());
    }
    if (props.operation == "intersect") {
        JobContext job{ctx.job.cancel, ctx.job.progress.sub(share)};
        auto combined = kernel::boolean(kernel::BoolOp::Intersect, *shape, *tool, job);
        if (!combined) {
            if (combined.error().code == ErrorCode::Cancelled) {
                return tl::unexpected(combined.error());
            }
            return failed(combined.error().code, kernelMessage(where, combined.error()));
        }
        out.warnings.insert(out.warnings.end(), combined->warnings.begin(), combined->warnings.end());
        const std::array<const naming::ElementMap*, 2> maps{&map, &toolMap};
        const std::array<const kernel::Shape*, 2> shapes{&*shape, &*tool};
        naming::NamingInput input{self.id, featureRole,      maps, &combined->history, &combined->shape,
                                  {},      std::span(shapes)};
        input.placement = placement;
        auto result = naming::propagate(input);
        if (!result) {
            return failed(ErrorCode::Internal, where + ": naming the result failed: " + result.error().message);
        }
        shape = combined->shape;
        map = std::move(result->map);
        lineage.insert(lineage.end(), result->lineage.begin(), result->lineage.end());
    }
    for (std::string& w : out.warnings) {
        w = where + ": " + w;
    }
    ctx.job.progress.set(1.0);
    out.bodies.push_back(BodyUpdate{props.target, *shape, std::move(map), std::move(lineage)});
    return out;
}

Status validateSweepCommon(const DocState& state, std::size_t index, const Feature& self, const SweepCommon& props)
{
    auto sketch = earlierFeature(state, self, props.sketch, kSketchType, "sketch");
    if (!sketch) {
        return tl::unexpected(sketch.error());
    }
    if (props.operation == "new") {
        return {};
    }
    // The target must be a body the document has, or one an earlier feature makes.
    if (std::any_of(state.bodies.begin(), state.bodies.end(), [&](const Body& b) { return b.id == props.target; })) {
        return {};
    }
    const auto registry = builtinFeatureRegistry();
    for (std::size_t i = 0; i < index && i < state.features.size() && registry; ++i) {
        const IFeatureType* t = registry->find(state.features[i].type);
        std::vector<BodyId> made;
        if (const auto* d = dynamic_cast<const detail::BodyDeclaration*>(t)) {
            made = d->bodiesWritten(state.features[i]);
        }
        if (std::find(made.begin(), made.end(), props.target) != made.end()) {
            return {};
        }
    }
    return invalid(featureLabel(self) + ": the body to " + props.operation + " (" + props.target.value.toString() +
                   ") is not in the document; pick a body.");
}

std::unique_ptr<IFeatureType> makeExtrudeFeatureType()
{
    return std::make_unique<ExtrudeType>();
}

std::string_view extrudePropsSchema()
{
    return schema();
}

std::string regionsSchema()
{
    return R"({"type":"array","minItems":1,"items":{"type":"array","minItems":1,"items":)" + uuidSchema() +
           R"(},"description":"Each region is picked by the sketch entities of its outline; all regions when )"
           R"(omitted"})";
}

std::string operationSchema()
{
    return R"({"enum":["new","join","cut","intersect"],"description":"new: a new body with the id in target; join, )"
           R"(cut, intersect: change the body target"})";
}

} // namespace ray::doc::features
