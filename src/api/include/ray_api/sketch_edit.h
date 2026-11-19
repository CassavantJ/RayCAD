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
Result<kernel::Plane> resolvePlane(const doc::DocState& state, const doc::BuildResult* build, std::string_view planeJson,
                                   JobContext& ctx);

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

// ---------- Solving ----------
// The sketch solver (PlaneGCS): full solves and drag sessions. Both run on workers (sketch.h).
std::unique_ptr<sketch::ISketchSolver> makeSolver();

} // namespace ray::api::sketch_edit
