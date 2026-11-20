// The drawing tools of the sketch mode (task T-092), without Qt: snapping with inferred constraints, and one state
// machine per tool that turns pointer positions (mm on the sketch plane) and typed values into what to commit.
//
// A tool never edits anything. click() returns an Action: entities with the constraints inferred for them (one
// sketch.entity.add, one undo step), or a sketch command with its arguments (trim, extend, offset, fillet). The
// sketch mode (sketch_mode.h) runs it through the action registry.
//
// The point tool places one free point per click (hole positions, construction references): on a curve it gets
// PointOnCurve, at a line's middle Midpoint, lined up with another point Horizontal / Vertical; on an existing
// point it places nothing (that point is there already). Typed "x" and "y" place it at those coordinates.
//
// Snaps, strongest first: an existing point (end point, centre: the new geometry uses that point, which is the
// coincidence), the midpoint of a line (Midpoint), horizontal / vertical from the tool's anchor (the line gets
// Horizontal / Vertical), a curve (PointOnCurve; with an axis snap, where the axis line crosses the curve), and
// alignment with another point (Horizontal / Vertical between the two points). The line tool also snaps a segment
// leaving an arc or line end along its tangent (Tangent at the joint). Typed values (fields) override the pointer:
// a typed length or diameter also adds the dimension.
#pragma once

#include "sketch_geom.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ray::ui::sketchui {

enum class ToolKind {
    Select,
    Point,
    Line,
    Rectangle,
    Circle,
    Arc3,
    ArcCenter,
    ArcTangent,
    Slot,
    Spline,
    Trim,
    Extend,
    Offset,
    Fillet
};

// "line", "rectangle", ...; ToolKind::Select for an unknown name.
ToolKind toolFromName(const std::string& name);
std::string toolName(ToolKind kind);

struct Glyph {
    enum class Kind { Coincident, Center, Midpoint, OnCurve, Horizontal, Vertical, Tangent, Dimension } kind;
    Vec2 at;
};
const char* glyphName(Glyph::Kind kind);

struct Snap {
    enum class Kind { Free, Point, Midpoint, OnCurve } kind = Kind::Free;
    Vec2 p{};
    SketchEntId ref;             // Point: the point; Midpoint, OnCurve: the curve
    bool center = false;         // Point: an arc or circle centre
    bool curveConstrains = true; // OnCurve: the curve takes a PointOnCurve (lines, arcs, circles)
    enum class Axis { None, Horizontal, Vertical } axis = Axis::None; // from the anchor
    std::optional<SketchEntId> alignH, alignV;                        // the point it lines up with (same y / same x)
    std::vector<std::pair<Vec2, Vec2>> guides;                        // dashed inference lines
};

struct SnapOptions {
    std::optional<Vec2> anchor; // the tool's previous point: horizontal / vertical inference
    std::set<SketchEntId> exclude;
    bool enabled = true; // false: the pointer as it is (Alt held)
};
Snap snapAt(const SketchView& view, Vec2 q, double tol, const SnapOptions& options);

// What a tool hands to the sketch mode.
struct Batch {
    std::vector<sketch::Entity> entities;
    std::vector<sketch::Constraint> constraints;
    bool empty() const { return entities.empty() && constraints.empty(); }
};
struct Action {
    enum class Kind { None, Add, Command } kind = Kind::None;
    Batch batch;
    std::string command;  // "sketch.trim", ...
    std::string argsJson; // its arguments without "id" (the sketch mode adds it): a JSON object
    std::string label;    // for the status line
};

// What a tool shows now, in mm on the sketch plane.
struct Draft {
    std::vector<std::vector<Vec2>> curves;     // the geometry it would make
    std::vector<std::pair<Vec2, Vec2>> guides; // dashed inference lines
    std::vector<Vec2> points;                  // the points it would make or use
    std::vector<Glyph> glyphs;                 // the constraints it would add
    std::vector<SketchEntId> highlight;        // existing entities it acts on (trim, extend, offset, fillet)
    std::optional<Vec2> labelAt;               // where the value chips go
};

// A value the user can type while drawing (Tab moves between them, Enter commits).
struct FieldSpec {
    std::string key;    // "length", "angle", "width", "height", "diameter", "radius", "distance"
    std::string label;  // English, translated by the caller
    bool angle = false; // degrees on screen, radians here
    double value = 0;   // live value from the pointer (mm or rad)
};

class ToolEngine {
public:
    ToolKind kind() const { return kind_; }
    void setKind(ToolKind kind); // resets the tool's state
    bool construction() const { return construction_; }
    void setConstruction(bool on) { construction_ = on; }
    // A drawing operation is under way (a point placed, a chain going): Esc first ends it.
    bool busy() const;
    // Esc: ends the chain / drops the points placed. False if there was nothing to end.
    bool cancel();
    // The model changed under the tool (undo, another edit): forgets references that are gone.
    void modelChanged(const SketchView& view);

    // The pointer moved to q (mm). `tol` is the snap distance in mm.
    void hover(const SketchView& view, Vec2 q, double tol, bool snapping);
    // A click at the last hover position.
    Action click(const SketchView& view);
    // Enter: commits with the typed values (or finishes a spline).
    Action enter(const SketchView& view);

    // ---- typed values ----
    std::vector<FieldSpec> fields() const;
    // Values typed so far (mm or rad); a typed value holds until the step commits or the tool resets.
    void setTyped(const std::string& key, std::optional<double> value);
    const std::map<std::string, double>& typed() const { return typed_; }
    // The fillet radius and offset distance carry over between clicks.
    double filletRadius() const { return filletRadius_; }

    const Draft& draft() const { return draft_; }
    const Snap& snap() const { return snap_; }
    // Offset: the chain picked, waiting for the distance.
    const std::vector<SketchEntId>& offsetChain() const { return offsetChain_; }

private:
    struct Placed {
        Snap snap;
        SketchEntId id; // committed already (an existing point) or to make
        bool committed = false;
    };
    SketchEntId pointFor(const Placed& p, Batch& b) const;
    SketchEntId addCurve(Batch& b, sketch::Entity e) const;
    void addConstraint(Batch& b, sketch::ConstraintType type, std::vector<SketchEntId> refs, double value = 0) const;
    void glyphsFor(const Snap& s, std::vector<Glyph>& out) const;
    Vec2 target() const; // the pointer with typed values applied
    void rebuildDraft(const SketchView& view);
    Action commitLine(const SketchView& view);
    Action commitStep(const SketchView& view);
    void resetTyped() { typed_.clear(); }

    ToolKind kind_ = ToolKind::Select;
    bool construction_ = false;
    std::vector<Placed> placed_;            // points placed in this operation
    std::optional<SketchEntId> chainStart_; // line chain: its first point (closing it ends the chain)
    std::optional<SketchEntId> lastCurve_;  // the last curve of a chain (tangent arc continues from it)
    Vec2 cursor_{};
    Vec2 raw_{};
    double tol_ = 1;
    Snap snap_;
    std::optional<SketchEntId> tangentFrom_; // line: the curve the segment leaves tangentially
    double arcWinding_ = 0;                  // centre-start-end arc: the angle the pointer turned through
    double lastAngle_ = 0;
    SketchView::Hit hovered_; // trim, extend, fillet, offset
    std::vector<SketchEntId> offsetChain_;
    double offsetDistance_ = 0;
    double filletRadius_ = 2;
    std::map<std::string, double> typed_;
    Draft draft_;
};

// The chain of curves joined end to end through `seed` (lines and arcs; a circle alone).
std::vector<SketchEntId> connectedChain(const SketchView& view, const SketchEntId& seed);

// JSON helpers for the command arguments.
std::string jsonNumber(double v);
std::string jsonPoint(Vec2 p);

} // namespace ray::ui::sketchui
