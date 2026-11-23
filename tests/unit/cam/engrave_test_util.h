// Helpers of the engraving and thread milling tests (task T-056): parts built through the kernel, tools, operations,
// running a type's validate and generate, the full toolpath invariant checker (tests/unit/cam/invariants) and the
// toolpath goldens (tests/golden/toolpaths, as drill_golden_test.cpp keeps them).
#pragma once

#include "drill_test_util.h" // box, cut, revolved, holesOf, holeFace, tool, machine, geometryOn, readText

#include "invariants/checker.h"

#include "cam/ops/engrave.h"
#include "cam/ops/engrave_font.h"
#include "cam/ops/threadmill.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ray::cam::test {

// ---------- Golden files ----------

inline bool updatingGoldens()
{
    const char* v = std::getenv("RAY_UPDATE_TOOLPATH_GOLDENS");
    return v != nullptr && std::string(v) == "1";
}

inline std::string toolpathGoldenPath(const std::string& file)
{
    return std::string(RAY_TOOLPATH_GOLDEN_DIR) + "/" + file;
}

inline void writeBytes(const std::string& path, const std::string& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

// The toolpath is the golden <name>: both files match it and each other (drill_golden_test.cpp's rule). To make them
// again after a reviewed change: RAY_UPDATE_TOOLPATH_GOLDENS=1, run the "[golden]" cases, read the .txt diff.
inline void matchesToolpathGolden(const std::string& name, const Toolpath& p)
{
    const std::vector<std::byte> raw = p.serialize();
    const std::string bytes(reinterpret_cast<const char*>(raw.data()), raw.size());
    const std::string text = dumpToolpath(p);
    if (updatingGoldens()) {
        writeBytes(toolpathGoldenPath(name + ".rctp"), bytes);
        writeBytes(toolpathGoldenPath(name + ".txt"), text);
    }
    const std::string storedBytes = readText(toolpathGoldenPath(name + ".rctp"));
    const std::string storedText = readText(toolpathGoldenPath(name + ".txt"));
    REQUIRE_FALSE(storedBytes.empty());
    INFO(name);
    CHECK(storedText == text);
    CHECK(storedBytes == bytes);
    auto back = Toolpath::deserialize(std::as_bytes(std::span<const char>(storedBytes.data(), storedBytes.size())));
    REQUIRE(back);
    CHECK(dumpToolpath(*back) == storedText);
}

// ---------- Tools and operations ----------

inline lib::Tool engraver(double diameter = 3, double tipAngle = 60, double tipDiameter = 0.1)
{
    lib::Tool t = tool(lib::ToolType::EngraveCutter, diameter, tipAngle, 8);
    t.tipDiameter = tipDiameter;
    return t;
}

// A thread mill: single-form (pitch 0) or multi-form (its pitch), with its teeth over fluteLength, then a neck 0.6 of its
// diameter up to `reach` above the tip. libraries.h has no neck diameter, so the neck is described as the shank (both
// the generator's shank check and the invariant checker's tool profile read it so).
inline lib::Tool threadMill(double diameter, double pitch = 0, double fluteLength = 1.5, double reach = 30)
{
    lib::Tool t = tool(lib::ToolType::ThreadMill, diameter, 0, fluteLength);
    t.flutes = 3;
    t.threadPitch = pitch;
    t.shankDiameter = 0.6 * diameter;
    t.shoulderLength = reach;
    t.overallLength = reach + 40;
    return t;
}

inline Operation millOp(const std::string& type, const std::string& params, double rpm, double feed)
{
    Operation op;
    op.type = type;
    op.name = type;
    op.paramsJson = params;
    op.cutting.spindleRpm = rpm;
    op.cutting.feedCut = feed;
    op.heights.clearance = 15;
    op.heights.retract = 5;
    op.heights.feedPlane = 2;
    op.heights.top = 0;
    op.heights.bottom = 0;
    op.tool.stickOut = 40;
    return op;
}

// One run of an operation type.
struct OpCase {
    Operation op;
    lib::Tool tool;
    lib::MachineDef machine = test::machine();
    ResolvedGeometry geom;
    std::function<std::unique_ptr<IOperationType>()> make;

    GenerateInput input() const { return GenerateInput{op, tool, nullptr, machine, geom}; }
    Status validate() const { return make()->validate(input()); }
    Result<std::vector<std::string>> check() const { return make()->check(input()); }
    Result<Toolpath> generate() const
    {
        JobContext ctx;
        return make()->generate(input(), ctx);
    }
    // generate(), which must succeed and keep every invariant of the checker with these claims.
    Toolpath path(const std::function<void(invariants::Claims&)>& claims = {}) const
    {
        auto p = generate();
        if (!p) {
            FAIL("generate failed: " << p.error().message);
        }
        invariants::Claims k = invariants::claimsFor(op, geom);
        k.holderClearance = 0;
        k.wcsInMachine = Vec3{100, 100, -400};
        if (claims) {
            claims(k);
        }
        const auto found = invariants::check(invariants::CheckInput{*p, geom, tool, nullptr, &machine, k});
        INFO(invariants::describe(found));
        CHECK(found.empty());
        return *p;
    }
};

inline OpCase engraveCase(const kernel::Shape& part, const std::vector<kernel::SubId>& edges, const std::string& params,
                          lib::Tool t = engraver())
{
    OpCase c;
    c.op = millOp("engrave", params, 8000, 300);
    c.op.cutting.feedPlunge = 100;
    c.tool = std::move(t);
    c.geom = geometryOn(part, edges);
    c.make = [] { return ops::makeEngraveOperationType(); };
    return c;
}

inline OpCase threadCase(const kernel::Shape& part, const std::vector<kernel::SubId>& faces, const std::string& params,
                         lib::Tool t)
{
    OpCase c;
    c.op = millOp("threadmill", params, 3000, 200);
    c.op.cutting.feedPlunge = 500;
    c.op.heights.retract = 2;
    c.op.heights.feedPlane = 2;
    c.tool = std::move(t);
    c.geom = geometryOn(part, faces);
    c.make = [] { return ops::makeThreadmillOperationType(); };
    return c;
}

// ---------- Parts ----------

// A cylinder standing on a plate: the plate [0, 60] x [0, 40] x [0, 10] with a ring pocket 6 deep around a pin of
// diameter d at (30, 20) (the pin's top is the plate's top, Z 10).
inline kernel::Shape pinPlate(double d, double ringOuter = 30)
{
    kernel::Shape s = box(Vec3{0, 0, 0}, Vec3{60, 40, 10});
    const kernel::Shape ring = revolved(30, 20, {{d / 2, 11}, {ringOuter / 2, 11}, {ringOuter / 2, 4}, {d / 2, 4}});
    return cut(s, ring);
}

// The faces of a shape whose surface is a cylinder of radius r about the vertical line through (x, y).
inline std::vector<kernel::SubId> cylinderFaces(const kernel::Shape& s, double x, double y, double r)
{
    std::vector<kernel::SubId> out;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        auto info = kernel::surfaceInfo(s, f);
        if (!info) {
            continue;
        }
        if (info->type == kernel::SurfaceType::Cylinder && std::abs(std::abs(info->axis.z) - 1) < 1e-9 &&
            std::abs(info->origin.x - x) < 1e-6 && std::abs(info->origin.y - y) < 1e-6 &&
            std::abs(info->radius - r) < 1e-6) {
            out.push_back(f);
        }
    }
    return out;
}

// The edges of the faces of `s` at height z (up-facing planes) that are circles of radius r about (x, y), or, with
// r 0, every edge of those faces.
inline std::vector<kernel::SubId> topEdges(const kernel::Shape& s, double z, double x = 0, double y = 0, double r = 0)
{
    std::vector<kernel::SubId> out;
    for (std::uint32_t i = 0; i < s.count(kernel::SubKind::Face); ++i) {
        const kernel::SubId f{kernel::SubKind::Face, i};
        auto info = kernel::surfaceInfo(s, f);
        if (!info) {
            continue;
        }
        if (info->type != kernel::SurfaceType::Plane || std::abs(info->bounds.max.z - z) > 1e-6 ||
            std::abs(info->bounds.min.z - z) > 1e-6) {
            continue;
        }
        for (const kernel::SubId e : kernel::adjacent(s, f, kernel::SubKind::Edge)) {
            auto ci = kernel::curveInfo(s, e);
            if (!ci) {
                continue;
            }
            const bool circle = ci->type == kernel::CurveType::Circle && std::abs(ci->center.x - x) < 1e-6 &&
                                std::abs(ci->center.y - y) < 1e-6 && std::abs(ci->radius - r) < 1e-6;
            if ((r == 0 || circle) && std::find(out.begin(), out.end(), e) == out.end()) {
                out.push_back(e);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace ray::cam::test
