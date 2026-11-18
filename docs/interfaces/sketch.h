// RayCAD interface contract: sketch
// Module: src/sketch   Depends on: core, kernel (Profile types only), PlaneGCS (private)
// Status: PLAN DRAFT, revision 2 (after spike S7).
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
    std::vector<Uuid> partiallyRedundant;
    // OverConstrained: redundant constraints exist but the sketch still solves. Conflicting: no solution; `conflicting` is set.
    Sketch solved;                        // same ids, updated coordinates
};

class ISketchSolver {
public:
    virtual ~ISketchSolver() = default;
    // Full solve with diagnosis. Measured in spike S7: about 0.65 s for 200 curves in one coupled group,
    // so this ALWAYS runs on a worker, never on the UI thread.
    // Deterministic: same input gives bit-identical output on one build, PROVIDED the implementation keeps all
    // solver parameters in one contiguous array in canonical entity order, pins Eigen's cache sizes at start-up
    // and is built with one fixed set of floating-point flags (PLAN.md section 8).
    virtual Result<SolveResult> solve(const Sketch&, JobContext&) = 0;

    // Interactive drag is a SESSION that keeps the solver alive; a stateless call would rebuild and diagnose
    // on every step (22 to 42 ms even at 50 curves). Budget (PLAN.md section 9): one step under 16 ms for a
    // coupled group of up to 200 curves (spike S11: sparse re-run of PlaneGCS's drag iteration on a reduced
    // system, falling back to the full sparse step). The caller still runs the session on the drag worker and
    // always passes the newest pointer position.
    // move(target) solves from the beginDrag state, never from the previous move, so the result depends only
    // on `target` and skipped pointer positions cannot change the final geometry.
    // Drag results are bit-identical run to run and across core types on one build (proven on Windows
    // only). They are not bit-identical to PlaneGCS's own drag solve: they agree with it to 1e-9 mm on
    // the S7/S11 grid sketches and to about 1e-8 mm with tangent arcs, angles, point-on-curve and
    // symmetry; on an under-constrained shape dragged a long way the two can settle up to 1e-6 mm apart.
    // In every case all constraints hold to 1e-14 (T-090).
    // A failed move() returns status Failed with the drag-start geometry; end() returns the last
    // successful move. Sketches with partially redundant constraints drag on PlaneGCS's own slow path.
    // beginDrag builds and diagnoses the system (measured up to 0.6 s at 200 curves), so it takes a
    // JobContext and runs on a worker like solve().
    class IDragSession {
    public:
        virtual ~IDragSession() = default;
        virtual Result<SolveResult> move(Vec2 target) = 0;     // diagnosis fields are those from beginDrag
        virtual Sketch end() = 0;                              // final solved sketch
    };
    virtual Result<std::unique_ptr<IDragSession>> beginDrag(const Sketch&, SketchEntId point, JobContext&) = 0;
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
