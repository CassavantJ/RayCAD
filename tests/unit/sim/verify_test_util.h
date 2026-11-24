// Helpers of the safety-check tests (task T-071): soups, tools, hand-written toolpaths and a runner.
#pragma once

#include "tridexel_test_util.h"
#include "verify.h"

#include <ray/cam.h>
#include <ray/core.h>
#include <ray/libraries.h>
#include <ray/sim.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ray::sim::test {

// A box as a closed soup of 12 outward triangles.
inline kernel::TriSoup boxSoup(const Box3& b)
{
    kernel::TriSoup s;
    for (int i = 0; i < 8; ++i) {
        s.positions.push_back((i & 1) != 0 ? b.max.x : b.min.x);
        s.positions.push_back((i & 2) != 0 ? b.max.y : b.min.y);
        s.positions.push_back((i & 4) != 0 ? b.max.z : b.min.z);
    }
    s.indices = {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                 2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};
    return s;
}
inline Box3 box(double x0, double y0, double z0, double x1, double y1, double z1)
{
    return {{x0, y0, z0}, {x1, y1, z1}};
}

// A cylinder holder: one segment of `diameter` and `height` (the gauge length).
inline lib::Holder holder(double diameter, double height)
{
    lib::Holder h;
    h.segments = {{height, diameter, diameter}};
    h.gaugeLength = height;
    return h;
}
inline lib::Tool endMill(double diameter, double flute, double overall, lib::ToolType type = lib::ToolType::FlatEndMill)
{
    lib::Tool t;
    t.type = type;
    t.diameter = diameter;
    t.shankDiameter = diameter;
    t.fluteLength = flute;
    t.overallLength = overall;
    if (type == lib::ToolType::Drill) {
        t.tipAngleDeg = 118;
    }
    return t;
}

// A toolpath written move by move, in mm.
class Path {
public:
    explicit Path(double x, double y, double z)
    {
        tp_.startX = toNm(x);
        tp_.startY = toNm(y);
        tp_.startZ = toNm(z);
    }
    Path& rapid(double x, double y, double z) { return add(cam::MoveKind::Rapid, cam::MoveRole::Link, 0, x, y, z); }
    Path& feed(double x, double y, double z, cam::MoveRole role = cam::MoveRole::Cut)
    {
        return add(cam::MoveKind::Linear, role, 1000000, x, y, z);
    }
    // An XY arc to (x, y, z) about (cx, cy).
    Path& arc(bool cw, double x, double y, double z, double cx, double cy)
    {
        add(cw ? cam::MoveKind::ArcCW : cam::MoveKind::ArcCCW, cam::MoveRole::Cut, 1000000, x, y, z);
        tp_.moves.back().i = toNm(cx);
        tp_.moves.back().j = toNm(cy);
        tp_.moves.back().k = tp_.moves.back().z;
        return *this;
    }
    // A canned cycle at (x, y) down to `bottom`, R plane `r`, initial level `clear`, G98.
    Path& cycle(cam::MoveKind kind, double x, double y, double bottom, double top, double r, double clear,
                double peck = 0)
    {
        add(kind, cam::MoveRole::Plunge, 100000, x, y, bottom);
        cam::Move& m = tp_.moves.back();
        m.cycleTop = toNm(top);
        m.cycleRetract = toNm(r);
        m.cycleClearance = toNm(clear);
        m.cyclePeck = toNm(peck);
        m.cycleReturn = cam::CycleReturn::Initial;
        if (kind == cam::MoveKind::CycleTap) {
            m.threadPitchNm = 1000000;
        }
        return *this;
    }
    Path& cancel() { return add(cam::MoveKind::CycleCancel, cam::MoveRole::Link, 0, 0, 0, 0); }
    std::shared_ptr<const cam::Toolpath> done() const { return std::make_shared<const cam::Toolpath>(tp_); }

private:
    Path& add(cam::MoveKind kind, cam::MoveRole role, std::int32_t feed, double x, double y, double z)
    {
        cam::Move m{};
        m.kind = kind;
        m.role = role;
        m.feedMicronPerMin = feed;
        m.x = toNm(x);
        m.y = toNm(y);
        m.z = toNm(z);
        tp_.moves.push_back(m);
        return *this;
    }
    cam::Toolpath tp_;
};

// A program: operations with their tools, and the solids.
struct Program {
    struct Op {
        std::shared_ptr<const cam::Toolpath> path;
        lib::Tool tool;
        std::optional<lib::Holder> holder;
        double stickOut = 0;
        ToolShape shape;
        OperationId id;
    };
    std::vector<Op> ops;
    std::optional<kernel::TriSoup> stock, part;
    std::vector<kernel::TriSoup> fixtures;
    std::optional<lib::MachineDef> machine;
    std::optional<Vec3> wcsInMachine;

    Program& op(std::shared_ptr<const cam::Toolpath> path, const lib::Tool& tool,
                std::optional<lib::Holder> h = std::nullopt, double stickOut = 0)
    {
        Op o;
        o.path = std::move(path);
        o.tool = tool;
        o.holder = std::move(h);
        o.stickOut = stickOut;
        o.id.value.bytes[0] = static_cast<std::uint8_t>(ops.size() + 1);
        ops.push_back(std::move(o));
        return *this;
    }
    sim::detail::VerifyInput input()
    {
        sim::detail::VerifyInput in;
        for (Op& o : ops) {
            o.shape = makeToolShape(o.tool, o.holder ? &*o.holder : nullptr, o.stickOut);
        }
        for (const Op& o : ops) {
            in.ops.push_back({o.id, o.path.get(), &o.shape, &o.tool});
        }
        in.stock = stock ? &*stock : nullptr;
        in.part = part ? &*part : nullptr;
        for (const kernel::TriSoup& f : fixtures) {
            in.fixtures.push_back(&f);
        }
        in.machine = machine ? &*machine : nullptr;
        in.wcsInMachine = wcsInMachine;
        return in;
    }
};

inline std::vector<SimEvent> verify(Program& p, sim::detail::VerifyStats* stats = nullptr,
                                    const sim::detail::VerifyOptions& opts = {})
{
    JobContext ctx;
    const sim::detail::VerifyInput in = p.input();
    Result<std::vector<SimEvent>> r = sim::detail::verifyProgram(in, jobs(), ctx, opts, stats);
    REQUIRE(r.has_value());
    return *r;
}

inline std::string kindName(EventKind k)
{
    switch (k) {
    case EventKind::RapidIntoStock:
        return "RapidIntoStock";
    case EventKind::RapidIntoCutStock:
        return "RapidIntoCutStock";
    case EventKind::HolderHitsStock:
        return "HolderHitsStock";
    case EventKind::ShankHitsStock:
        return "ShankHitsStock";
    case EventKind::ToolHitsFixture:
        return "ToolHitsFixture";
    case EventKind::HolderHitsFixture:
        return "HolderHitsFixture";
    case EventKind::HolderHitsPart:
        return "HolderHitsPart";
    case EventKind::Gouge:
        return "Gouge";
    case EventKind::PlungeWithNonCentreCutting:
        return "PlungeWithNonCentreCutting";
    case EventKind::ExcessEngagement:
        return "ExcessEngagement";
    case EventKind::FeedIntoStockAtRapidHeight:
        return "FeedIntoStockAtRapidHeight";
    case EventKind::TravelLimit:
        return "TravelLimit";
    case EventKind::ToolTooShort:
        return "ToolTooShort";
    case EventKind::ShankHitsPart:
        return "ShankHitsPart";
    }
    return "?";
}

// "Kind@op.move" for each event, in order: what a known-bad program must raise exactly.
inline std::vector<std::string> summary(const std::vector<SimEvent>& events, const Program& p)
{
    std::vector<std::string> out;
    for (const SimEvent& e : events) {
        std::size_t op = 0;
        for (std::size_t k = 0; k < p.ops.size(); ++k) {
            if (p.ops[k].id == e.operation) {
                op = k;
            }
        }
        out.push_back(kindName(e.kind) + "@" + std::to_string(op) + "." + std::to_string(e.moveIndex));
    }
    return out;
}

inline std::string describe(const std::vector<SimEvent>& events)
{
    std::string s;
    for (const SimEvent& e : events) {
        s += kindName(e.kind) + " move " + std::to_string(e.moveIndex) + " at (" + std::to_string(e.position.x) + ", " +
             std::to_string(e.position.y) + ", " + std::to_string(e.position.z) + ") amount " +
             std::to_string(e.amountMm) + ": " + e.message + "\n";
    }
    return s;
}

} // namespace ray::sim::test
