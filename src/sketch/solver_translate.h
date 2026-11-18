// Translation of a ray::sketch::Sketch into PlaneGCS parameters, geometry and constraint objects (task T-090).
// Private to src/sketch.
//
// Determinism, adapter rule 1 of PLAN.md section 8: PlaneGCS sorts parameters by address and keys several containers
// by double*, so every unknown lives in GcsModel::params, one contiguous array in canonical entity order (entities
// sorted by id; per entity: a point's x and y, a circle's radius, an arc's radius, start angle and end angle). Lines,
// splines and slots own no unknowns: their geometry is that of the Point entities they refer to.
//
// Tags: a constraint object's tag is the index of its sketch constraint in canonical order plus 1, so the int tags
// PlaneGCS reports in its diagnosis map back to Uuids (tagId). Tag 0 marks the arc rules the adapter adds for every
// arc (PlaneGCS never reports tag 0 as conflicting or redundant). Negative tags are the temporary drag constraints.
#pragma once

#include "model.h"

// PlaneGCS (third_party/planegcs), unmodified.
#include "GCS.h"

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace ray::sketch::detail {

struct EntityGeo {
    EntityKind kind = EntityKind::Point;
    int firstParam = -1; // index of the entity's first own unknown in GcsModel::params, or -1
    GCS::Point point;    // Point
    GCS::Line line;      // Line
    GCS::Circle circle;  // Circle
    GCS::Arc arc;        // Arc; start/end are swapped for a clockwise arc (PlaneGCS arcs run counter-clockwise)
};

struct GcsModel {
    Sketch sketch;                 // canonical copy of the input
    std::vector<double> params;    // every unknown; never resized after buildModel
    std::vector<double*> unknowns; // &params[i], in order
    std::deque<double> constants;  // dimension values and fixed coordinates; a deque keeps their addresses stable
    std::vector<EntityGeo> geo;    // one per entity of `sketch`, same order; never resized after buildModel
    int pointIndex(SketchEntId) const;
};

// Ownership of each new constraint object passes to the sink.
using ConstraintSink = std::function<void(GCS::Constraint*)>;

// Parameters and geometry for a canonical copy of `sketch` (validated). No constraint objects yet.
Result<std::unique_ptr<GcsModel>> buildModel(const Sketch& sketch);

// Creates the PlaneGCS constraint objects of the model, always in the same order for the same model state: the arc
// rules of every arc (tag 0) in entity order, then every enforced constraint in canonical order (tag = index + 1).
// Driven constraints get no object. The objects reproduce what GCS::System's addConstraint* helpers create for the
// same request (GCS.cpp, FreeCAD 1.1.4). A combination of references the solver cannot translate is
// ErrorCode::InvalidArgument; objects already passed to the sink stay with it.
Status emitConstraints(GcsModel& model, const ConstraintSink& sink);

// The Uuid of the sketch constraint behind tag `tag` (tag >= 1).
Uuid tagId(const GcsModel& model, int tag);
// Tags to Uuids, skipping tag 0 and negative tags, in tag order without duplicates.
std::vector<Uuid> tagIds(const GcsModel& model, const std::vector<int>& tags);

// The canonical sketch with the current parameter values written back (point coordinates and circle radii), and with
// the measured value of every driven dimension.
Sketch writeBack(const GcsModel& model);

} // namespace ray::sketch::detail
