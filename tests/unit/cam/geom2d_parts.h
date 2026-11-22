// Reference parts for the geom2d tests (task T-050), built with the kernel: the same parts as the kernel slice tests
// (tests/unit/kernel/slice_test.cpp), plus a plate with n through holes for the timing test.
#pragma once
#include "geom2d_test_util.h"
#include "kernel_test_util.h"
#include "profile_test_util.h"

#include <ray/kernel.h>

namespace ray::cam::geom2d::test {

namespace kt = ray::kernel::test;

inline kernel::Shape extruded(const kernel::Profile& p, double distance, double draft = 0.0)
{
    JobContext ctx;
    return take(kernel::extrude(p, distance, draft, ctx)).op.shape;
}
inline kernel::Shape revolvedZ(const kernel::Profile& p)
{
    JobContext ctx;
    return take(kernel::revolve(p, Vec3{0, 0, 0}, Vec3{0, 0, 1}, 2 * kPiD, ctx)).op.shape;
}

inline kernel::ProfileLoop roundedRectLoop(double x0, double y0, double x1, double y1, double r, std::uint64_t tag)
{
    kernel::ProfileLoop l;
    l.tag = tag;
    l.curves.push_back(kt::line({x0 + r, y0}, {x1 - r, y0}, tag));
    l.curves.push_back(kt::arc({x1 - r, y0}, {x1, y0 + r}, {x1 - r, y0 + r}, true, tag + 1));
    l.curves.push_back(kt::line({x1, y0 + r}, {x1, y1 - r}, tag + 2));
    l.curves.push_back(kt::arc({x1, y1 - r}, {x1 - r, y1}, {x1 - r, y1 - r}, true, tag + 3));
    l.curves.push_back(kt::line({x1 - r, y1}, {x0 + r, y1}, tag + 4));
    l.curves.push_back(kt::arc({x0 + r, y1}, {x0, y1 - r}, {x0 + r, y1 - r}, true, tag + 5));
    l.curves.push_back(kt::line({x0, y1 - r}, {x0, y0 + r}, tag + 6));
    l.curves.push_back(kt::arc({x0, y0 + r}, {x0 + r, y0}, {x0 + r, y0 + r}, true, tag + 7));
    return l;
}

// Block 0..100 x 0..60 x 0..20 with through holes r 5 at (25, 30) and r 8 at (75, 30).
inline kernel::Shape blockWithHoles()
{
    return extruded(kt::profile(kt::xyPlane(), {kt::rectangle(0, 0, 100, 60, 1, 1), kt::circleLoop({25, 30}, 5, 10, 2),
                                                kt::circleLoop({75, 30}, 8, 11, 3)}),
                    20);
}

// Block 0..100 x 0..80 x 0..30 with a pocket 10..90 x 10..70 (corner radius 6) from z = 20 up, round an island
// 40..60 x 30..50 (corner radius 3) standing to the top.
inline kernel::Shape pocketPart()
{
    const kernel::Shape block = kt::box({0, 0, 0}, {100, 80, 30});
    const kernel::Shape tool = extruded(
        kt::profile(kt::xyPlane(20), {roundedRectLoop(10, 10, 90, 70, 6, 1), roundedRectLoop(40, 30, 60, 50, 3, 20)}),
        15);
    JobContext ctx;
    return take(kernel::boolean(kernel::BoolOp::Cut, block, tool, ctx)).shape;
}

inline kernel::Shape cone()
{
    return revolvedZ(kt::profile(kt::xzPlane(), {kt::polygon({{0, 0}, {20, 0}, {10, 30}, {0, 30}}, 1, 1)}));
}
inline kernel::Shape sphere()
{
    kernel::ProfileLoop half;
    half.curves.push_back(kt::arc({0, -10}, {0, 10}, {0, 0}, true, 1));
    half.curves.push_back(kt::line({0, 10}, {0, -10}, 2));
    return revolvedZ(kt::profile(kt::xzPlane(), {half}));
}
inline kernel::Shape torus()
{
    return revolvedZ(kt::profile(kt::xzPlane(), {kt::circleLoop({30, 0}, 5, 1, 1)}));
}
inline kernel::Shape draftedBlock()
{
    return extruded(kt::profile(kt::xyPlane(), {kt::rectangle(0, 0, 40, 20, 1, 1)}), 10, 5.0 * kPiD / 180.0);
}

// A plate (side x side mm, 10 thick) with `count` through holes of radius 2 on a grid (count <= rows * rows).
inline kernel::Shape plateWithHoles(int count)
{
    int rows = 1;
    while (rows * rows < count)
        ++rows;
    const double pitch = 8.0;
    const double side = pitch * rows + 8.0;
    std::vector<kernel::ProfileLoop> loops{kt::rectangle(0, 0, side, side, 1, 1)};
    for (int i = 0; i < count; ++i)
        loops.push_back(kt::circleLoop({8.0 + pitch * (i % rows), 8.0 + pitch * (i / rows)}, 2.0,
                                       static_cast<std::uint64_t>(100 + i), static_cast<std::uint64_t>(10 + i)));
    return extruded(kt::profile(kt::xyPlane(), std::move(loops)), 10);
}

} // namespace ray::cam::geom2d::test
