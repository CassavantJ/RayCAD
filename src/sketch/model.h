// Sketch model helpers (task T-090): canonical order, lookup, validation and measurement of the types declared in
// <ray/sketch.h>. Private to src/sketch; the solver, loops, inference and DXF code of this module build on it.
#pragma once

#include <ray/sketch.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ray::sketch {

// The alternatives of Entity::geom, in declaration order.
enum class EntityKind { Point, Line, Arc, Circle, Spline, Slot };
EntityKind kindOf(const Entity&);
const char* kindName(EntityKind);
const char* constraintTypeName(ConstraintType);

// Entities sorted by id and constraints sorted by id: the canonical order PLAN.md section 3.4 stores. Two sketches
// that differ only in the order of these lists have the same canonical form and solve to the same bits.
Sketch canonicalSketch(Sketch);

// Lookup of entities by id in a canonical sketch (binary search; the sketch must outlive the index).
class EntityIndex {
public:
    explicit EntityIndex(const Sketch& canonical);
    // Position in sketch.entities, or -1.
    int indexOf(SketchEntId) const;
    const Entity* find(SketchEntId) const;
    const Sketch& sketch() const { return *sketch_; }

private:
    const Sketch* sketch_;
};

// Structural checks: unique entity and constraint ids; every reference resolves; line, arc, circle, slot and spline
// references name Point entities; coordinates and values are finite; sizes are positive; each constraint has 1 to 3
// references. Whether a constraint type accepts the kinds it references is checked by the solver, which knows what
// it can translate. Returns ErrorCode::InvalidArgument naming the offending id.
Status validateSketch(const Sketch&);

// Coordinates of a Point entity.
std::optional<Vec2> pointOf(const EntityIndex&, SketchEntId);

// The current value of a dimensional constraint (Distance, DistanceX, DistanceY, Radius, Diameter, Angle) measured
// from the geometry, in the convention the solver enforces it with (see solver_translate.cpp). Used to report driven
// (reference) dimensions. nullopt for a non-dimensional type or a combination of references the solver does not
// support.
std::optional<double> measureConstraint(const EntityIndex&, const Constraint&);

// A Uuid in canonical text form for messages.
std::string idText(const Uuid&);

} // namespace ray::sketch
