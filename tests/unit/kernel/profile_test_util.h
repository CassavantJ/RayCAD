// Profile builders shared by the sweep and boolean tests (task T-094). No OCCT header here (PLAN.md 0.4 rule 1).
// The ProfileCurve convention is described in src/kernel/sweep_internal.h.
#pragma once
#include <ray/kernel.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace ray::kernel::test {

inline constexpr double kPi = 3.141592653589793238462643383279502884;

inline Plane xyPlane(double z = 0.0)
{
    return Plane{Vec3{0, 0, z}, Vec3{0, 0, 1}, Vec3{1, 0, 0}};
}
// The plane z = 0 seen from below: normal -Z, so extrusions with positive distance go down.
inline Plane xyPlaneDown()
{
    return Plane{Vec3{0, 0, 0}, Vec3{0, 0, -1}, Vec3{1, 0, 0}};
}
// The XZ plane through the origin (x along X, y along Z, normal -Y): profiles to revolve about Z.
inline Plane xzPlane()
{
    return Plane{Vec3{0, 0, 0}, Vec3{0, -1, 0}, Vec3{1, 0, 0}};
}

inline ProfileCurve line(Vec2 a, Vec2 b, std::uint64_t tag)
{
    return ProfileCurve{ProfileCurve::Kind::Line, {a, b}, 0.0, false, tag};
}
inline ProfileCurve arc(Vec2 start, Vec2 end, Vec2 centre, bool ccw, std::uint64_t tag)
{
    return ProfileCurve{ProfileCurve::Kind::Arc, {start, end, centre}, 0.0, ccw, tag};
}
inline ProfileCurve circle(Vec2 centre, double radius, std::uint64_t tag)
{
    return ProfileCurve{ProfileCurve::Kind::Circle, {centre}, radius, true, tag};
}
inline ProfileCurve bspline(std::vector<Vec2> poles, std::uint64_t tag)
{
    return ProfileCurve{ProfileCurve::Kind::Bspline, std::move(poles), 0.0, false, tag};
}

// A closed polygon; curve i runs from corner i to corner i + 1 and has tag firstTag + i.
inline ProfileLoop polygon(std::initializer_list<Vec2> corners, std::uint64_t firstTag, std::uint64_t loopTag)
{
    const std::vector<Vec2> c(corners);
    ProfileLoop loop;
    loop.tag = loopTag;
    for (std::size_t i = 0; i < c.size(); ++i)
        loop.curves.push_back(line(c[i], c[(i + 1) % c.size()], firstTag + i));
    return loop;
}
inline ProfileLoop rectangle(double x0, double y0, double x1, double y1, std::uint64_t firstTag, std::uint64_t loopTag)
{
    return polygon({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, firstTag, loopTag);
}
inline ProfileLoop circleLoop(Vec2 centre, double radius, std::uint64_t tag, std::uint64_t loopTag)
{
    return ProfileLoop{{circle(centre, radius, tag)}, loopTag};
}
// An obround: centres (x0, y) and (x1, y), radius r. Tags: bottom line, right arc, top line, left arc.
inline ProfileLoop slot(double x0, double x1, double y, double r, std::uint64_t firstTag, std::uint64_t loopTag)
{
    ProfileLoop loop;
    loop.tag = loopTag;
    loop.curves.push_back(line({x0, y - r}, {x1, y - r}, firstTag));
    loop.curves.push_back(arc({x1, y - r}, {x1, y + r}, {x1, y}, true, firstTag + 1));
    loop.curves.push_back(line({x1, y + r}, {x0, y + r}, firstTag + 2));
    loop.curves.push_back(arc({x0, y + r}, {x0, y - r}, {x0, y}, true, firstTag + 3));
    return loop;
}

inline Profile profile(Plane plane, std::vector<ProfileLoop> loops)
{
    return Profile{plane, std::move(loops)};
}

// Every ProfileCurve tag of a profile, with the tag of its loop.
struct TagInfo {
    std::uint64_t curve;
    std::uint64_t loop;
    const ProfileCurve* source;
};
inline std::vector<TagInfo> tagsOf(const Profile& p)
{
    std::vector<TagInfo> out;
    for (const ProfileLoop& loop : p.loops)
        for (const ProfileCurve& c : loop.curves)
            out.push_back({c.tag, loop.tag, &c});
    return out;
}

} // namespace ray::kernel::test
