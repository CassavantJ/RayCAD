// The sketch editing commands (task T-096), private to src/doc and its tests. Each edits the model of one sketch
// feature ("id" is the sketch feature's id) and is one undo step. Ids of new entities may be given by the caller; when
// they are not, they are derived from the sketch feature, the entity and the sketch's size, so a replay gives the
// same ids. Coordinates are mm in the sketch plane. Each stores the sketch with its coordinates solved, in the same
// undo step (sketch_feature.h, storedSketch): one pass over the constraints when they hold already (a drag's result,
// a caller's "solved"), else a full solve on the calling thread; a failed or conflicting solve keeps the coordinates
// the command produced and the rebuild reports it. Trim, extend, offset, split and fillet therefore compute on the
// shape that is shown.
//
//   sketch.set               {"id", "sketch", "plane"?}         replaces the whole model (and the plane)
//   sketch.entity.add        {"id", "entities": [...], "constraints"?: [...]}
//                                                               adds entities (sketch_json.h form), and with them the
//                                                               constraints a drawing tool inferred (one undo step)
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
// Added for the drawing tools (task T-092):
//   sketch.construction      {"id", "entities": ["<id>", ...], "construction": true | false}
//                            marks entities as construction geometry (or as profile geometry again)
//   sketch.extend            {"id", "entity", "at": {"x", "y"}, "newPoint"?}
//                            grows the end of a line or arc nearest `at` along its carrier to the first crossing with
//                            another curve (not construction). The end point moves; when it is shared with another
//                            curve or named by a constraint the entity gets a new end point instead, and a crossing at
//                            an end point of the other curve joins it there
//   sketch.offset            {"id", "entities": ["<id>", ...], "distance": mm, "side": {"x", "y"}}
//                            copies lines, arcs and circles at `distance` on the side of `side`. Entities that share
//                            end points form chains, offset as a whole: neighbours meet again at the crossing of their
//                            offset carriers. New lines are parallel to their originals (constraint); new arcs and
//                            circles share the original centre point
//   sketch.fillet            {"id", "point", "radius", "newEntity"?}
//                            rounds the corner where exactly two lines share `point` with a tangent arc: the lines
//                            end at the tangent points (tangent constraints added), the corner point is removed
//                            unless a constraint still names it
// Added for constraints and dimensions (task T-093):
//   sketch.constraint.add / .remove / .set   also take "solved"?: the caller's solved form of the same model (a sketch
//                            model, same entities): its point coordinates, circle radii and slot widths are where the
//                            change is solved from (the shape that is shown); stored as given when they satisfy the
//                            changed constraints already
//   sketch.constraint.add    also takes "labels"?: {"<dimension id>": {"x", "y"}}: where the new dimensions' value
//                            labels go (same step)
//   sketch.dimension.place   {"id", "constraint", "offset": {"x", "y"} | null}
//                            moves a dimension's value label: the offset (mm in the sketch plane) from the anchor the
//                            UI measures it from; null forgets it. Stored in the feature's props as
//                            "labels": {"<dimension id>": {"x", "y"}}; every sketch command drops the labels of
//                            dimensions that are gone. Labels change no geometry.
// Keeping ids through trims and splits keeps the names of the faces swept from the entity (PLAN.md 3.3 rule 1).
#pragma once

#include <ray/document.h>

#include <memory>
#include <vector>

namespace ray::doc::features {

std::vector<std::unique_ptr<ICommandHandler>> makeSketchCommands();

} // namespace ray::doc::features
