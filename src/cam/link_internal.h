// Private to src/cam/link*.cpp (task T-051): the small vector arithmetic, the reading of passes (their direction at
// the ends, their oriented form) and the error helpers the linking files share. Deterministic: plain IEEE arithmetic
// (src/cam is built without contraction or fast math), trigonometry through ray::dmath, rounding through roundNm.
#pragma once
#include "link.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::cam::link::detail {

inline constexpr double kPi = 3.141592653589793238462643383279502884;

struct D2 {
    double x = 0, y = 0;
};
inline D2 d2(Pt p)
{
    return D2{static_cast<double>(p.x), static_cast<double>(p.y)};
}
inline D2 operator+(D2 a, D2 b)
{
    return D2{a.x + b.x, a.y + b.y};
}
inline D2 operator-(D2 a, D2 b)
{
    return D2{a.x - b.x, a.y - b.y};
}
inline D2 operator*(D2 a, double s)
{
    return D2{a.x * s, a.y * s};
}
inline double norm(D2 a)
{
    return dmath::sqrt(a.x * a.x + a.y * a.y);
}
inline D2 unit(D2 a)
{
    const double n = norm(a);
    return n > 0.0 ? D2{a.x / n, a.y / n} : D2{0.0, 0.0};
}
inline D2 leftOf(D2 t)
{
    return D2{-t.y, t.x};
}
inline D2 rightOf(D2 t)
{
    return D2{t.y, -t.x};
}
inline Pt roundPt(D2 p)
{
    return Pt{geom2d::roundNm(p.x), geom2d::roundNm(p.y)};
}
inline Pt xyOf(const Pt3& p)
{
    return Pt{p.x, p.y};
}
inline Pt xyOf(const Move& m)
{
    return Pt{m.x, m.y};
}
inline double distXY(Pt a, Pt b)
{
    return norm(d2(a) - d2(b));
}
inline bool isArcMove(const Move& m)
{
    return m.kind == MoveKind::ArcCW || m.kind == MoveKind::ArcCCW;
}

// A pass as linking reads it, in the orientation it is cut.
struct Shape {
    Pt3 start, end;
    std::vector<Move> moves;
    std::optional<Path> path; // planar: the XY path at start.z
    bool hasTangent = false;  // some move moves in XY
    D2 t0, t1;                // unit directions at the start and at the end (XY)
    Nm zMax = 0, zMin = 0;
    std::uint8_t comp = 0; // the first move's compensation
};
// The pass as given, or reversed / started elsewhere along its path (Placement; uniform planar passes only).
Shape placedShape(const Pass& pass, const Placement& placement);
// The XY projection of the pass's moves as a path (lines and arcs; z ignored), for its swept area.
Path projection(Pt3 start, const std::vector<Move>& moves);
// The tool-position ends of a placed pass with its leads at their nominal size (no room test): where the tool enters
// (the lead-in's start) and where it leaves (the lead-out's end).
struct Ends {
    Pt3 a, e;
};
Ends nominalEnds(const Pass& pass, const Shape& shape);

// "12.5" from 12'500'000 nm: millimetres, rounded half to even to the micrometre, trailing zeros dropped.
std::string mm(Nm v);
std::string mm(double nm);
Error failure(std::string message); // ToolpathFailed
Error invalid(std::string message); // InvalidArgument
inline constexpr std::string_view kWhere = "cam::link";

} // namespace ray::cam::link::detail
