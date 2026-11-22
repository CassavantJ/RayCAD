// The "drill" operation type (drill.h, task T-035): the IOperationType, and the toolpath of a plan.
#include "drill.h"
#include "op_params.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <new>
#include <utility>

namespace ray::cam::ops {

namespace {

// The motion of one cycle move as cam.h describes it, for the toolpath's statistics: feed and rapid length in nm.
// Pecks are counted with the move's peck clearance, else kDefaultPeckClearanceNm (the machine's is not known here).
void addCycleLengths(const Move& m, Nm fromZ, double& feed, double& rapid)
{
    const double r = static_cast<double>(m.cycleRetract);
    const double z = static_cast<double>(m.z);
    const double ret = m.cycleReturn == CycleReturn::Initial ? static_cast<double>(m.cycleClearance) : r;
    rapid += std::abs(static_cast<double>(fromZ) - r); // down (or up) to R
    switch (m.kind) {
    case MoveKind::CyclePeck:
    case MoveKind::CycleChipBreak: {
        const double q = static_cast<double>(m.cyclePeck);
        const double c = static_cast<double>(peckClearanceNm(m, nullptr));
        double depth = std::max(r - q, z); // the first peck
        feed += r - depth;
        while (depth > z) {
            const double next = std::max(depth - q, z);
            if (m.kind == MoveKind::CyclePeck) {
                rapid += (r - depth) + (r - std::min(r, depth + c)); // out to R, back to c above the last depth
                feed += std::min(r, depth + c) - next;
            } else {
                rapid += std::min(r, depth + c) - depth; // back off by c
                feed += std::min(r, depth + c) - next;
            }
            depth = next;
        }
        rapid += ret - z;
        break;
    }
    case MoveKind::CycleTap:
    case MoveKind::CycleBore:
    case MoveKind::CycleReam:
        feed += 2 * (r - z);
        rapid += ret - r;
        break;
    default:
        feed += r - z;
        rapid += ret - z;
        break;
    }
}

class DrillOperation final : public IOperationType {
public:
    std::string_view typeName() const override { return "drill"; }
    std::string_view paramsSchemaJson() const override { return embeddedOpSchema("drill"); }

    Status validate(const GenerateInput& in) const override
    {
        JobContext ctx; // validate has no job; recognition here is not cancellable
        auto plan = planDrill(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        return {};
    }

    Result<Toolpath> generate(const GenerateInput& in, JobContext& ctx) const override
    {
        auto plan = planDrill(in, ctx);
        if (!plan) {
            return tl::unexpected(plan.error());
        }
        try {
            Toolpath path = drillToolpath(*plan);
            path.operation = in.op.id;
            ctx.progress.set(1.0);
            return path;
        } catch (const std::bad_alloc&) {
            return tl::unexpected(Error{ErrorCode::Internal, "Out of memory while making the drilling toolpath", {}});
        }
    }
};

} // namespace

Toolpath drillToolpath(const DrillPlan& plan)
{
    Toolpath path;
    if (plan.holes.empty()) {
        return path;
    }
    const PlannedHole& first = plan.holes.front();
    path.startX = first.hole.x;
    path.startY = first.hole.y;
    path.startZ = plan.clearance;

    Move approach{};
    approach.kind = MoveKind::Rapid;
    approach.role = MoveRole::Approach;
    approach.x = first.hole.x;
    approach.y = first.hole.y;
    approach.z = plan.clearance;
    path.moves.push_back(approach);

    double feed = 0, rapid = 0;
    Nm x = approach.x, y = approach.y, z = approach.z;
    Nm minZ = plan.clearance;
    for (const PlannedHole& ph : plan.holes) {
        Move m{};
        m.kind = plan.kind;
        m.role = MoveRole::Plunge;
        m.cycleReturn = ph.ret;
        m.cycleLeftHand = plan.kind == MoveKind::CycleTap && ph.leftHand ? 1 : 0;
        m.feedMicronPerMin = plan.feed;
        m.cycleDwellMs = plan.dwellMs;
        m.threadPitchNm = plan.kind == MoveKind::CycleTap ? plan.pitchNm : 0;
        m.x = ph.hole.x;
        m.y = ph.hole.y;
        m.z = ph.tip;
        m.cycleRetract = ph.retract;
        m.cycleTop = ph.top;
        m.cycleClearance = plan.clearance;
        const bool pecks = plan.kind == MoveKind::CyclePeck || plan.kind == MoveKind::CycleChipBreak;
        m.cyclePeck = pecks ? plan.peck : 0;
        m.cyclePeckClearance = pecks ? plan.peckClearance : 0;
        path.moves.push_back(m);

        const double dx = static_cast<double>(m.x - x);
        const double dy = static_cast<double>(m.y - y);
        rapid += dmath::hypot(dx, dy);
        addCycleLengths(m, z, feed, rapid);
        x = m.x;
        y = m.y;
        z = m.cycleReturn == CycleReturn::Initial ? m.cycleClearance : m.cycleRetract;
        minZ = std::min(minZ, m.z);
    }
    Move cancel{};
    cancel.kind = MoveKind::CycleCancel;
    cancel.role = MoveRole::Retract;
    cancel.x = x;
    cancel.y = y;
    cancel.z = z;
    path.moves.push_back(cancel);

    path.stats.cutLengthMm = feed / kNmPerMm;
    path.stats.rapidLengthMm = rapid / kNmPerMm;
    path.stats.minZ = toMm(minZ);
    path.stats.maxZ = toMm(plan.clearance);
    path.warnings = plan.warnings;
    return path;
}

std::unique_ptr<IOperationType> makeDrillOperationType()
{
    return std::make_unique<DrillOperation>();
}

} // namespace ray::cam::ops
