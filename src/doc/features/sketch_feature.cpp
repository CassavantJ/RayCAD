// The "sketch" feature type (sketch_feature.h).
#include "sketch_feature.h"

#include "sketch_json.h"

#include <algorithm>
#include <exception>
#include <new>
#include <set>
#include <utility>

namespace ray::doc::features {
namespace {

using json::Member;
using json::Value;

const std::string& schema()
{
    static const std::string text = [] {
        const std::string u = uuidSchema();
        return objectSchema(
            R"("plane","sketch")",
            R"("plane":)" + planeSchema() +
                R"(,"sketch":{"type":"object","additionalProperties":false,"properties":{"schema":{"const":1},)"
                R"("entities":{"type":"array","items":{"type":"object","required":["id","type"],"properties":{"id":)" +
                u + R"(,"type":{"enum":["point","line","arc","circle","spline","slot"]},)" +
                R"("construction":{"type":"boolean"},"x":{"type":"number"},"y":{"type":"number"},"a":)" + u +
                R"(,"b":)" + u + R"(,"center":)" + u + R"(,"start":)" + u + R"(,"end":)" + u +
                R"(,"ccw":{"type":"boolean"},"radius":{"type":"number","exclusiveMinimum":0},"points":)"
                R"({"type":"array","items":)" +
                u + R"(},"degree":{"type":"integer","minimum":1},"closed":{"type":"boolean"},)" +
                R"("width":{"type":"number","exclusiveMinimum":0}}}},"constraints":{"type":"array","items":)"
                R"({"type":"object","additionalProperties":false,"required":["id","type","refs"],"properties":{"id":)" +
                u +
                R"(,"type":{"enum":["coincident","horizontal","vertical","parallel","perpendicular","tangent",)"
                R"("equal","symmetric","concentric","midpoint","pointOnCurve","fix","distance","distanceX",)"
                R"("distanceY","radius","diameter","angle"]},"refs":{"type":"array","minItems":1,"maxItems":3,)"
                R"("items":)" +
                u +
                R"(},"expression":{"type":"string","minLength":1,"description":"A dimension's value as an )"
                R"(expression over the document parameters"},"value":{"type":"number"},"driven":{"type":"boolean"})"
                R"(}}}}})");
    }();
    return text;
}

struct SketchProps {
    PlaneRef plane;
    sketch::Sketch model;
};

Result<SketchProps> parse(const Feature& f, bool strict)
{
    const std::string where = featureLabel(f);
    auto root = PropsReader::parse(where, f.propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    const PropsReader r(where, *root);
    if (strict) {
        if (auto ok = r.onlyKeys({"plane", "sketch"}); !ok) {
            return tl::unexpected(ok.error());
        }
    }
    SketchProps out;
    auto plane = readPlaneRef(r, "plane", "plane");
    if (!plane) {
        return tl::unexpected(plane.error());
    }
    out.plane = std::move(*plane);
    const Value* model = r.find("sketch");
    if (model == nullptr) {
        return r.error("sketch", "is required");
    }
    auto sketch = sketchFromJson(r, *model, "sketch");
    if (!sketch) {
        return tl::unexpected(sketch.error());
    }
    out.model = std::move(*sketch);
    return out;
}

// Evaluates the dimension expressions of `model` (driven dimensions keep their value).
Status evaluateDimensions(sketch::Sketch& model, const std::map<std::string, double>& params, const std::string& where)
{
    for (sketch::Constraint& c : model.constraints) {
        if (!c.expression || c.driven) {
            continue;
        }
        const Parameter::Kind kind =
            c.type == sketch::ConstraintType::Angle ? Parameter::Kind::Angle : Parameter::Kind::Length;
        auto v = evaluateNumeric(Value::string(*c.expression), params, kind, where,
                                 std::string("dimension ") + c.id.toString());
        if (!v) {
            return tl::unexpected(v.error());
        }
        c.value = *v;
    }
    return {};
}

std::string idList(const std::vector<Uuid>& ids)
{
    std::string out;
    for (std::size_t i = 0; i < ids.size() && i < 8; ++i) {
        out += (i ? ", " : "") + ids[i].toString();
    }
    if (ids.size() > 8) {
        out += " and " + std::to_string(ids.size() - 8) + " more";
    }
    return out;
}

// Curve tag -> the entity it comes from (slots: their four parts).
std::map<std::uint64_t, SketchEntId> curveTags(const sketch::Sketch& s)
{
    std::map<std::uint64_t, SketchEntId> out;
    for (const sketch::Entity& e : s.entities) {
        if (e.construction || std::holds_alternative<sketch::Point>(e.geom)) {
            continue;
        }
        if (std::holds_alternative<sketch::Slot>(e.geom)) {
            for (int part = 0; part < 4; ++part) {
                out.emplace(slotPartCurveTag(e.id, part), e.id);
            }
        } else {
            out.emplace(entityCurveTag(e.id), e.id);
        }
    }
    return out;
}

class SketchType final : public IFeatureType,
                         public ParametricType,
                         public detail::BodyDeclaration,
                         public detail::ParametersFromProps {
public:
    std::string_view typeName() const override { return kSketchType; }
    std::string_view propsSchemaJson() const override { return schema(); }
    std::string_view title() const override { return "Sketch"; }

    std::vector<naming::TopoRef> references(const Feature& f) const override
    {
        auto props = parse(f, false);
        if (props && props->plane.face) {
            return {*props->plane.face};
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
        if (props->plane.datum) {
            auto datum = earlierFeature(state, f, *props->plane.datum, "datum_plane", "datum plane");
            if (!datum) {
                return tl::unexpected(datum.error());
            }
        }
        const DocumentParams params = documentParams(state);
        for (const sketch::Constraint& c : props->model.constraints) {
            if (!c.expression || c.driven) {
                continue;
            }
            const Parameter::Kind kind =
                c.type == sketch::ConstraintType::Angle ? Parameter::Kind::Angle : Parameter::Kind::Length;
            if (auto ok = checkNumeric(Value::string(*c.expression), params, kind, featureLabel(f),
                                       "dimension " + c.id.toString());
                !ok) {
                return ok;
            }
        }
        return {};
    }

    Result<FeatureOutput> rebuild(const Feature& f, RebuildContext& ctx) const override
    {
        try {
            const std::string where = featureLabel(f);
            auto props = parse(f, false);
            if (!props) {
                return tl::unexpected(props.error());
            }
            auto plane = resolvePlaneRef(props->plane, ctx, f, where + " (its plane)");
            if (!plane) {
                return tl::unexpected(plane.error());
            }
            if (auto ok = evaluateDimensions(props->model, ctx.paramValues, where); !ok) {
                return tl::unexpected(ok.error());
            }
            FeatureOutput out;
            auto artifact = std::make_shared<SketchArtifact>();
            artifact->plane = *plane;
            JobContext solveCtx{ctx.job.cancel, ctx.job.progress.sub(0.8)};
            auto solved = sketch::makePlaneGcsSolver()->solve(props->model, solveCtx);
            if (!solved) {
                if (solved.error().code == ErrorCode::Cancelled) {
                    return tl::unexpected(solved.error());
                }
                return failed(ErrorCode::SolverFailed,
                              where + ": the sketch cannot be solved: " + solved.error().message);
            }
            switch (solved->status) {
            case sketch::SolveStatus::Conflicting:
                return failed(ErrorCode::Overconstrained, where + ": the constraints conflict (" +
                                                              idList(solved->conflicting) +
                                                              "); remove one of them or change a dimension.");
            case sketch::SolveStatus::Failed:
                return failed(ErrorCode::SolverFailed,
                              where + ": the constraints cannot all be met with these dimensions (the solver did not "
                                      "converge); check the dimensions you changed last.");
            case sketch::SolveStatus::OverConstrained: {
                std::vector<Uuid> redundant = solved->redundant;
                redundant.insert(redundant.end(), solved->partiallyRedundant.begin(), solved->partiallyRedundant.end());
                out.warnings.push_back(where + ": redundant constraints (" + idList(redundant) +
                                       "); the sketch solves, but removing them makes edits more predictable.");
                break;
            }
            default:
                break;
            }
            artifact->solved = std::move(solved->solved);
            artifact->status = solved->status;
            artifact->freeDof = solved->freeDof;
            artifact->regions = sketchRegions(artifact->solved, &artifact->regionError);
            if (artifact->regionError) {
                out.warnings.push_back(where + ": " + *artifact->regionError);
            } else {
                std::set<std::uint64_t> used;
                for (const SketchRegion& region : artifact->regions) {
                    for (const kernel::ProfileCurve& c : region.outer.curves) {
                        used.insert(c.tag);
                    }
                    for (const kernel::ProfileLoop& hole : region.holes) {
                        for (const kernel::ProfileCurve& c : hole.curves) {
                            used.insert(c.tag);
                        }
                    }
                }
                std::set<SketchEntId> unused;
                for (const auto& [tag, entity] : curveTags(artifact->solved)) {
                    if (used.count(tag) == 0) {
                        unused.insert(entity);
                    }
                }
                if (!unused.empty()) {
                    out.warnings.push_back(where + ": " + std::to_string(unused.size()) +
                                           (unused.size() == 1 ? " curve is" : " curves are") +
                                           " not part of a closed outline (an open end, an overlap or a line that only "
                                           "joins two outlines); extrudes and revolves leave it out. Mark it as "
                                           "construction if that is intended.");
                }
            }
            ctx.job.progress.set(1.0);
            detail::publishArtifact(ctx, std::move(artifact));
            return out;
        } catch (const std::bad_alloc&) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": out of memory");
        } catch (...) {
            return failed(ErrorCode::Internal, featureLabel(f) + ": unexpected failure");
        }
    }
};

} // namespace

double profileLoopArea2(const kernel::ProfileLoop& loop)
{
    double sum = 0;
    const auto cross = [](Vec2 a, Vec2 b) {
        return a.x * b.y - a.y * b.x;
    };
    for (const kernel::ProfileCurve& c : loop.curves) {
        switch (c.kind) {
        case kernel::ProfileCurve::Kind::Line:
            if (c.pts.size() == 2) {
                sum += cross(c.pts[0], c.pts[1]);
            }
            break;
        case kernel::ProfileCurve::Kind::Arc:
            if (c.pts.size() == 3) {
                const Vec2 s = c.pts[0], e = c.pts[1], m = c.pts[2];
                const Vec2 a{s.x - m.x, s.y - m.y}, b{e.x - m.x, e.y - m.y};
                double theta = dmath::atan2(cross(a, b), a.x * b.x + a.y * b.y);
                constexpr double kTwoPi = 6.283185307179586476925286766559;
                if (c.ccw) {
                    if (theta <= 0) {
                        theta += kTwoPi;
                    }
                } else {
                    if (theta >= 0) {
                        theta -= kTwoPi;
                    }
                    theta = -theta;
                }
                const double r2 = a.x * a.x + a.y * a.y;
                sum += cross(s, e) + (c.ccw ? 1.0 : -1.0) * r2 * (theta - dmath::sin(theta));
            }
            break;
        case kernel::ProfileCurve::Kind::Circle: {
            constexpr double kTwoPi = 6.283185307179586476925286766559;
            sum += (c.ccw ? 1.0 : -1.0) * kTwoPi * c.radius * c.radius;
            break;
        }
        case kernel::ProfileCurve::Kind::Bspline:
            for (std::size_t i = 0; i + 1 < c.pts.size(); ++i) {
                sum += cross(c.pts[i], c.pts[i + 1]);
            }
            break;
        }
    }
    return sum;
}

std::vector<SketchRegion> sketchRegions(const sketch::Sketch& solved, std::optional<std::string>* error)
{
    std::vector<SketchRegion> out;
    auto loops = sketch::closedLoops(solved);
    if (!loops) {
        if (error != nullptr) {
            *error = loops.error().message;
        }
        return out;
    }
    const std::map<std::uint64_t, SketchEntId> tags = curveTags(solved);
    for (kernel::ProfileLoop& loop : *loops) {
        const double area2 = profileLoopArea2(loop);
        if (out.empty() || area2 > 0) {
            SketchRegion region;
            region.area2 = area2;
            std::set<SketchEntId> entities;
            for (const kernel::ProfileCurve& c : loop.curves) {
                const auto it = tags.find(c.tag);
                if (it != tags.end()) {
                    entities.insert(it->second);
                }
            }
            region.outerEntities.assign(entities.begin(), entities.end());
            region.outer = std::move(loop);
            out.push_back(std::move(region));
        } else {
            out.back().holes.push_back(std::move(loop));
        }
    }
    return out;
}

Result<sketch::Sketch> sketchOfFeature(const Feature& feature, bool strict)
{
    auto props = parse(feature, strict);
    if (!props) {
        return tl::unexpected(props.error());
    }
    return std::move(props->model);
}

Result<std::string> withSketch(const Feature& feature, const sketch::Sketch& model)
{
    auto root = PropsReader::parse(featureLabel(feature), feature.propsJson);
    if (!root) {
        return tl::unexpected(root.error());
    }
    Value::Object members;
    for (const Member& m : root->asObject()) {
        if (m.key != "sketch") {
            members.push_back(m);
        }
    }
    members.push_back({"sketch", sketchToJson(model)});
    return json::write(Value::object(std::move(members)));
}

std::unique_ptr<IFeatureType> makeSketchFeatureType()
{
    return std::make_unique<SketchType>();
}

std::string_view sketchPropsSchema()
{
    return schema();
}

} // namespace ray::doc::features
