// RayCAD interface contract: sketch
// Module: src/sketch   Depends on: core, kernel (Profile types only), PlaneGCS (private)
// Status: PLAN DRAFT.
#pragma once
#include "core.h"
#include "kernel.h"
#include <optional>
#include <variant>

namespace ray::sketch {

struct Point   { Vec2 p; };
struct Line    { SketchEntId a, b; };                       // endpoints are Point entities
struct Arc     { SketchEntId center, start, end; bool ccw; };
struct Circle  { SketchEntId center; double radius; };
struct Spline  { std::vector<SketchEntId> controlPoints; int degree; bool closed; };
struct Slot    { SketchEntId a, b; double width; };         // sugar: expands to 2 lines + 2 arcs on solve

struct Entity {
    SketchEntId id;
    std::variant<Point, Line, Arc, Circle, Spline, Slot> geom;
    bool construction = false;
};

enum class ConstraintType {
    Coincident, Horizontal, Vertical, Parallel, Perpendicular, Tangent, Equal, Symmetric,
    Concentric, Midpoint, PointOnCurve, Fix,
    Distance, DistanceX, DistanceY, Radius, Diameter, Angle
};
struct Constraint {
    Uuid id;
    ConstraintType type;
    std::vector<SketchEntId> refs;        // 1 to 3 entities depending on type
    // Dimensional constraints: literal value or an expression over document parameters.
    std::optional<std::string> expression;   // e.g. "width / 2 + 0.125 in"
    double value = 0;                        // evaluated value in mm or rad
    bool driven = false;                     // reference dimension: reported, not enforced
};

// Where the sketch lives: a datum plane or a planar face reference (resolved by the document).
struct Sketch {
    std::vector<Entity> entities;         // stored sorted by id for canonical serialisation
    std::vector<Constraint> constraints;
};

enum class SolveStatus { FullyConstrained, UnderConstrained, OverConstrained, Conflicting, Failed };
struct SolveResult {
    SolveStatus status;
    int freeDof;
    std::vector<Uuid> conflicting;        // constraint ids
    std::vector<Uuid> redundant;
    Sketch solved;                        // same ids, updated coordinates
};

class ISketchSolver {
public:
    virtual ~ISketchSolver() = default;
    // Deterministic: same input gives bit-identical output on one build. Single-threaded.
    virtual Result<SolveResult> solve(const Sketch&, JobContext&) = 0;
    // Interactive drag: solve with one point pulled toward `target`; must return in under 16 ms for 200 entities.
    virtual Result<SolveResult> drag(const Sketch&, SketchEntId point, Vec2 target) = 0;
};
std::unique_ptr<ISketchSolver> makePlaneGcsSolver();

// Auto-constraint suggestions while drawing (snap tolerance in screen-independent mm).
std::vector<Constraint> inferConstraints(const Sketch&, SketchEntId newEntity, double tolerance);

// Closed-region detection. Each ProfileCurve.tag is the low 64 bits of the SketchEntId hash,
// so kernel::SweepFaceOrigin can be mapped back to a sketch entity for naming.
Result<std::vector<kernel::ProfileLoop>> closedLoops(const Sketch&);

// DXF import into sketch entities (LINE, ARC, CIRCLE, LWPOLYLINE, SPLINE; R12 to 2018 ASCII).
Result<Sketch> importDxf(const std::string& path, UnitSystem assumeIfUnitless);
// DXF export of sketch geometry (R2000 ASCII; lines, arcs, circles, splines). Constraints are not exported.
Status exportDxf(const std::string& path, const Sketch&, UnitSystem);

} // namespace ray::sketch
