// The sketch editing commands (task T-096), private to src/doc and its tests. Each edits the model of one sketch
// feature ("id" is the sketch feature's id) and is one undo step. Ids of new entities may be given by the caller; when
// they are not, they are derived from the sketch feature, the entity and the sketch's size, so a replay gives the
// same ids. Coordinates are mm in the sketch plane. None of them solves: the rebuild does.
//
//   sketch.set               {"id", "sketch", "plane"?}         replaces the whole model (and the plane)
//   sketch.entity.add        {"id", "entities": [...]}          adds entities (sketch_json.h form)
//   sketch.entity.remove     {"id", "entities": ["<id>", ...]}  removes them, the curves that use a removed point,
//                                                               end points no curve uses any more, and every
//                                                               constraint on something removed
//   sketch.constraint.add    {"id", "constraints": [...]}
//   sketch.constraint.remove {"id", "constraints": ["<id>", ...]}
//   sketch.constraint.set    {"id", "constraint", "expression"?, "value"?, "driven"?}   edits a dimension; an
//                                                               expression of "" removes it (the value stays)
//   sketch.split             {"id", "entity", "at": {"x", "y"}, "newEntity"?, "newPoint"?}
//                            splits a line or arc at the point of it nearest `at`: the entity keeps its id, its
//                            start and every constraint on it; the new entity runs from the new point to the old end
//   sketch.trim              {"id", "entity", "at": {"x", "y"}, "newEntity"?, "newPoints"?: ["<id>", "<id>"]}
//                            removes the piece of a line, arc or circle around `at` between its nearest crossings with
//                            other curves (lines, arcs and circles that are not construction). A piece at an end
//                            shortens the entity, which keeps its id; a piece in the middle splits it (the entity
//                            keeps its id and its start, the new entity has the far part); a circle becomes an arc
//                            with the same id. With no crossing the entity is removed.
// Keeping ids through trims and splits keeps the names of the faces swept from the entity (PLAN.md 3.3 rule 1).
#pragma once

#include <ray/document.h>

#include <memory>
#include <vector>

namespace ray::doc::features {

std::vector<std::unique_ptr<ICommandHandler>> makeSketchCommands();

} // namespace ray::doc::features
