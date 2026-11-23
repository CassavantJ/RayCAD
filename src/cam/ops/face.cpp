// The "face" operation type (face.h, task T-052): the IOperationType, and the toolpath of a plan.
#include "face.h"
#include "op_params.h"

#include <new>
#include <utility>

namespace ray::cam::ops {

namespace {

class FaceOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "face"; }
    std::string_view paramsSchemaJson() const override { return embeddedOpSchema("face"); }

    Status validate(const GenerateInput& in) const override
    {
        JobContext ctx; // validate has no job
        auto plan = planFace(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return {};
    }

    // The plan's warnings too (the same plan as validate and generate), for the operation panel.
    Result<std::vector<std::string>> check(const GenerateInput& in) const override
    {
        JobContext ctx;
        auto plan = planFace(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return std::move(plan->warnings);
    }

    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        auto plan = planFace(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        auto path = faceToolpath(*plan, ctx);
        if (!path) {
            return tl::unexpected(path.error());
        }
        path->operation = in.op.id;
        ctx.progress.set(1.0);
        return path;
    }
};

} // namespace

Result<Toolpath> faceToolpath(const FacePlan& plan, JobContext& ctx)
{
    try {
        // The flat bottoms of the last level cover the stock (face.h STROKES); a gap would be a bug of the plan.
        auto uncovered = faceUncovered(plan, ctx);
        if (!uncovered) {
            return tl::unexpected(uncovered.error());
        }
        if (*uncovered > kFaceCoverageSlackNm2) {
            return tl::unexpected(Error{ErrorCode::Internal,
                                        "The facing strokes would leave " +
                                            std::to_string(static_cast<long long>(*uncovered / 1e6)) +
                                            " square micrometres of the stock top uncut",
                                        {}});
        }
        std::vector<link::Pass> passes;
        passes.reserve(plan.strokes.size());
        for (const FaceStroke& s : plan.strokes) {
            if (auto st = ctx.cancel.check(); !st) {
                return tl::unexpected(st.error());
            }
            geom2d::Path line;
            line.start = s.from;
            line.spans.push_back(geom2d::Span{geom2d::SpanKind::Line, s.to, {}, 0});
            passes.push_back(link::passFromPath(line, s.z, plan.feed));
        }
        auto linked = link::linkPasses(passes, plan.link, plan.stock, ctx);
        if (!linked) {
            return tl::unexpected(linked.error());
        }
        Toolpath path;
        path.startX = linked->start.x;
        path.startY = linked->start.y;
        path.startZ = linked->start.z;
        path.moves = std::move(linked->moves);
        const link::Lengths lengths = link::lengthsOf(path.moves, linked->start);
        path.stats.cutLengthMm = lengths.feed / kNmPerMm;
        path.stats.rapidLengthMm = lengths.rapid / kNmPerMm;
        path.stats.minZ = toMm(plan.bottom);
        path.stats.maxZ = toMm(plan.link.clearance);
        path.warnings = plan.warnings;
        path.warnings.insert(path.warnings.end(), linked->warnings.begin(), linked->warnings.end());
        return path;
    } catch (const std::bad_alloc&) {
        return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while making the facing toolpath", {}});
    }
}

std::unique_ptr<IOperationType> makeFaceOperationType()
{
    return std::make_unique<FaceOperation>();
}

} // namespace ray::cam::ops
