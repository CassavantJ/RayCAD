// Plane geometry of the sketch mode (task T-092), without Qt: a display form of a sketch model (points, curves and
// their polylines in mm on the sketch plane), hit tests, and the constructions the drawing tools need (arc through
// three points, tangent arc, carriers crossing horizontal and vertical lines).
//
// Display only: nothing here decides geometry that is stored, except the coordinates the tools give new entities,
// which the solver then owns. The math library is used freely (src/ui is not deterministic code, PLAN.md 8).
#pragma once

#include <ray/api.h>

#include <optional>
#include <set>
#include <vector>

namespace ray::ui::sketchui {

// ---------- vectors ----------
inline Vec2 add(Vec2 a, Vec2 b)
{
    return {a.x + b.x, a.y + b.y};
}
inline Vec2 sub(Vec2 a, Vec2 b)
{
    return {a.x - b.x, a.y - b.y};
}
inline Vec2 mul(Vec2 a, double s)
{
    return {a.x * s, a.y * s};
}
inline double dot(Vec2 a, Vec2 b)
{
    return a.x * b.x + a.y * b.y;
}
inline double cross(Vec2 a, Vec2 b)
{
    return a.x * b.y - a.y * b.x;
}
double length(Vec2 a);
double distance(Vec2 a, Vec2 b);
Vec2 unit(Vec2 a); // (0, 0) for a zero vector
Vec2 perpLeft(Vec2 a);
Vec2 polar(Vec2 center, double radius, double angle);
double angleOf(Vec2 v);     // atan2, radians
double normAngle(double a); // to [0, 2 pi)

constexpr double kPi = 3.14159265358979323846;

// ---------- the display form of a model ----------
struct ViewPoint {
    SketchEntId id;
    Vec2 p;
    bool construction = false;
    bool center = false; // the centre of an arc or circle
    bool used = false;   // some curve uses it (a lone point is not)
};

struct ViewCurve {
    enum class Kind { Line, Arc, Circle, Spline, Slot } kind = Kind::Line;
    SketchEntId id;
    bool construction = false;
    Vec2 a, b, c;                    // line ends; arc start, end; arc / circle centre; slot centres a, b
    double r = 0;                    // arc / circle radius; slot half width
    bool ccw = true;                 // arc direction
    double start = 0;                // arc: angle of a
    double sweep = 0;                // arc: angle from a to b in its direction (> 0)
    std::vector<SketchEntId> points; // the point entities it uses
    std::vector<Vec2> poly;          // the curve as a polyline (closed curves repeat their first point)
};

class SketchView {
public:
    SketchView() = default;
    explicit SketchView(const sketch::Sketch& model);

    const std::vector<ViewPoint>& points() const { return points_; }
    const std::vector<ViewCurve>& curves() const { return curves_; }
    std::optional<Vec2> point(const SketchEntId& id) const;
    const ViewPoint* pointEntry(const SketchEntId& id) const;
    const ViewCurve* curve(const SketchEntId& id) const;
    // The curves that use a point.
    std::vector<const ViewCurve*> curvesAt(const SketchEntId& point) const;
    bool empty() const { return points_.empty() && curves_.empty(); }
    // Bounds of everything (nullopt when empty).
    std::optional<std::pair<Vec2, Vec2>> bounds() const;

    struct Hit {
        enum class Kind { None, Point, Curve } kind = Kind::None;
        SketchEntId id;
        Vec2 at;      // the point, or the nearest point on the curve
        double d = 0; // distance from the query, mm
    };
    // The point within `tol` nearest `q` (points win over curves), else the nearest curve within `tol`. Entities in
    // `exclude` (and curves using an excluded point, when `excludeUsers`) are skipped.
    Hit hit(Vec2 q, double tol, const std::set<SketchEntId>& exclude = {}) const;
    Hit hitPoint(Vec2 q, double tol, const std::set<SketchEntId>& exclude = {}) const;
    Hit hitCurve(Vec2 q, double tol, const std::set<SketchEntId>& exclude = {}) const;

private:
    std::vector<ViewPoint> points_; // sorted by id
    std::vector<ViewCurve> curves_; // in model order
};

// ---------- curves ----------
// The polyline of a circle arc from angle `start` sweeping `sweep` (signed: positive counter-clockwise).
std::vector<Vec2> arcPolyline(Vec2 center, double r, double start, double sweep);
std::vector<Vec2> splinePolyline(const std::vector<Vec2>& control, int degree, bool closed);
// A slot's outline: two sides and two half-circle caps around the centres a and b, half width r.
std::vector<Vec2> slotPolyline(Vec2 a, Vec2 b, double r);
// The nearest point to q on a curve (analytic for lines, arcs and circles; the polyline otherwise).
Vec2 nearestOn(const ViewCurve& c, Vec2 q);
// Tangent direction (unit) of a line or arc at one of its ends, pointing away from the curve's body:
// the direction a curve continuing tangentially from that end leaves in.
std::optional<Vec2> tangentOut(const ViewCurve& c, const SketchEntId& endPoint);

// The centre of the circle through three points; nullopt when they are (nearly) in line.
std::optional<Vec2> circleThrough(Vec2 a, Vec2 b, Vec2 c);
// An arc from `start`, leaving in direction `dir` (unit), ending at `end`: its centre and direction (ccw).
struct ArcFit {
    Vec2 center;
    bool ccw = true;
    double r = 0;
};
std::optional<ArcFit> tangentArc(Vec2 start, Vec2 dir, Vec2 end);
// Where the carrier of a line or circle (arcs: their circle) meets the line y = value (horizontal) or x = value.
std::vector<Vec2> crossAxisLine(const ViewCurve& c, bool horizontal, double value);

} // namespace ray::ui::sketchui
