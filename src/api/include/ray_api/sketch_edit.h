// What the desktop sketch mode (src/ui/sketch, task T-092) needs from doc and sketch, without Qt: a sketch feature's
// model, where its plane lies in the world, the JSON arguments of the sketch commands, and the solver for background
// solves and drags.
//
// src/ui may link only api and render (PLAN.md 1.1), so it never calls doc's private helpers or the sketch module
// itself; everything goes through here. Documents change only through the actions (feature.sketch.add,
// sketch.entity.add, sketch.set, ...); these helpers only read and format. Not one of the contract headers in
// include/ray/.
#pragma once

#include <ray/api.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api::sketch_edit {

// The stored model of sketch feature `id` (its "sketch" props: the last solved coordinates). InvalidArgument when the
// feature is not a sketch, NotFound when the state has no such feature.
Result<sketch::Sketch> modelOf(const doc::DocState& state, const FeatureId& id);
// The feature's "plane" props as canonical JSON: {"principal": "xy"}, {"datum": "<id>"} or {"face": <TopoRef>}.
Result<std::string> planeJsonOf(const doc::DocState& state, const FeatureId& id);

// Where a plane reference lies in the world, as the rebuild places a sketch on it (origin, normal, x direction;
// sketch y is normal x xDir): a principal plane as doc defines it; a datum plane through its base and offset (and
// that base's own, recursively); a face as it is in `build` (the latest build: the face as it is now), with the
// outward normal, the origin nearest the world origin and x the world X projected onto the plane. A face needs kernel
// queries: run it on a worker. Errors name what is wrong (ReferenceBroken, InvalidArgument).
Result<kernel::Plane> resolvePlane(const doc::DocState& state, const doc::BuildResult* build,
                                   std::string_view planeJson, JobContext& ctx);

// ---------- Arguments of the commands (canonical JSON objects) ----------
// sketch.entity.add {"id", "entities", "constraints"?}.
std::string entityAddArgs(const FeatureId& sketch, const std::vector<sketch::Entity>& entities,
                          const std::vector<sketch::Constraint>& constraints);
// sketch.set {"id", "sketch"}.
std::string sketchSetArgs(const FeatureId& sketch, const sketch::Sketch& model);
// feature.sketch.add {"id", "name", "props": {"plane", "sketch"}}; `planeJson` as planeJsonOf gives it.
std::string sketchAddArgs(const FeatureId& id, std::string_view name, std::string_view planeJson,
                          const sketch::Sketch& model);
// feature.datum_plane.add {"id", "name", "props": {"base", "offset"}}.
std::string datumAddArgs(const FeatureId& id, std::string_view name, std::string_view basePlaneJson, double offsetMm);

// ---------- Dimensions (task T-093) ----------
// `text` evaluated as a dimension's expression over the document's parameters, in the feature props' expression
// language (numbers, unit suffixes, parameter names, functions): mm, or radians when `angle`. The error says why not.
Result<double> evaluateExpression(const doc::DocState& state, std::string_view text, bool angle);
// The model with each driving dimension that has an expression set to the expression's value now, as the rebuild
// solves it. An expression that does not evaluate keeps the stored value.
sketch::Sketch withEvaluatedDimensions(const doc::DocState& state, sketch::Sketch model);
// Where the value labels of a sketch feature's dimensions sit (its "labels" props): dimension id -> offset in mm
// from the dimension's anchor. Empty when there are none.
std::map<Uuid, Vec2> labelsOf(const doc::DocState& state, const FeatureId& id);
// sketch.constraint.add {"id", "constraints", "labels"?, "solved"?}.
std::string constraintAddArgs(const FeatureId& sketch, const std::vector<sketch::Constraint>& constraints,
                              const std::map<Uuid, Vec2>& labels = {}, const sketch::Sketch* solved = nullptr);
// A model as the sketch commands read it (sketch_json form), e.g. the "solved" argument.
std::string sketchJson(const sketch::Sketch& model);

// ---------- Solving ----------
// The sketch solver (PlaneGCS): full solves and drag sessions. Both run on workers (sketch.h).
std::unique_ptr<sketch::ISketchSolver> makeSolver();

} // namespace ray::api::sketch_edit
