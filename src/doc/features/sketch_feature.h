// The "sketch" feature type (task T-096), private to src/doc and its tests.
//
// Props: {"plane": {"principal": "xy" | "yz" | "xz"} | {"datum": "<datum_plane id>"} | {"face": <TopoRef>},
//         "sketch": <sketch model, sketch_json.h>,
//         "labels"?: {"<dimension id>": {"x", "y"}}}   (T-093: dimension label offsets, display only)
// The model is stored in the feature itself (not as a separate object): the feature object is content-addressed
// already, and a sketch is small. Its point coordinates are the solved ones (storedSketch: every command that writes a
// sketch stores it solved), so a rebuild or a reload solves to the shape that was shown.
//
// Rebuild: resolves the plane at this point of the history (a face as it is just before the sketch), evaluates the
// dimension expressions, solves (sketch::ISketchSolver), finds the closed regions (sketch::closedLoops) and publishes
// all of it as a SketchArtifact; the rebuilder caches it with the feature, so an unchanged sketch is not solved again.
// It writes no body. Conflicting or unsolvable constraints fail the sketch (naming the constraints); redundant ones
// and curves outside any closed region are warnings.
#pragma once

#include "feature_props.h"

#include <ray/sketch.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::doc::features {

inline constexpr std::string_view kSketchType = "sketch";

// One closed region of a solved sketch: kernel::Profile loops (outer counter-clockwise, holes clockwise) and the
// entities whose curves bound the outer loop.
struct SketchRegion {
    kernel::ProfileLoop outer;
    std::vector<kernel::ProfileLoop> holes;
    std::vector<SketchEntId> outerEntities; // sorted, unique
    double area2 = 0;                       // twice the signed area of the outer loop
};

struct SketchArtifact : PlaneArtifact {
    sketch::Sketch solved;
    sketch::SolveStatus status = sketch::SolveStatus::UnderConstrained;
    int freeDof = 0;
    std::vector<SketchRegion> regions;
    std::optional<std::string> regionError; // why closedLoops failed
};

std::unique_ptr<IFeatureType> makeSketchFeatureType();
std::string_view sketchPropsSchema();

// Regions of a solved sketch (exposed for tests): closedLoops, split into regions, with the entities of each outer
// loop. `error` is set when closedLoops fails.
std::vector<SketchRegion> sketchRegions(const sketch::Sketch& solved, std::optional<std::string>* error);
// Twice the signed area enclosed by a profile loop given in chain order (positive counter-clockwise); B-splines by
// their control polygon (the sign is what matters).
double profileLoopArea2(const kernel::ProfileLoop& loop);

// Reads the sketch model out of a sketch feature's props (for commands that edit it).
Result<sketch::Sketch> sketchOfFeature(const Feature& feature, bool strict);
// The props of a sketch feature with its model replaced.
Result<std::string> withSketch(const Feature& feature, const sketch::Sketch& model);

// What storedSketch did with a model.
enum class StoredSketch {
    AsGiven,   // every constraint held at the given coordinates already (or there is nothing to solve): no solve
    Solved,    // the coordinates were replaced by a solve's
    NotSolved, // the solve failed, the constraints conflict, or an expression does not evaluate: kept as given
    Cancelled, // `job` was cancelled before or during the solve: kept as given; the command fails with Cancelled
};
// The model as a command stores it (every sketch.* command, feature.sketch.add and .edit, feature.add and .edit of a
// sketch, and param.set for the sketches it changes, storeDependentSketches): with the coordinates the
// rebuild's solve gives (points, circle radii), so the document holds the shape the person sees and every later solve
// (a new constraint, a dimension edit, a drag, a drawing step on this geometry, a reload) starts from it. A solve from
// other coordinates can land on another solution (a tangent arc the other way round, a point on the far side of a
// line), and the tools that compute on the stored geometry (trim, extend, offset, fillet) would act on a shape that
// is not shown. Dimension expressions are evaluated over `state`'s parameters for the solve only; the constraints are
// stored as given. A model whose constraints hold at its coordinates already is returned unchanged without a solve
// (sketch::constraintsHold), so a commit of an already solved shape (a drag, a caller's "solved") costs one pass
// over the constraints. A failed or conflicting solve never replaces the coordinates: they stay as given and the
// rebuild reports the status. The solve runs on the calling thread: a worker when the command runs in a job
// (Document::prepare), whose cancellation `job` carries (null: not cancellable, e.g. IDocument::execute).
sketch::Sketch storedSketch(const DocState& state, sketch::Sketch model, StoredSketch* outcome = nullptr,
                            JobContext* job = nullptr);

// After a change of the document parameters that is not a sketch command (param.set, param.rename): every sketch of
// `next` with a
// dimension whose value differs between `before`'s parameters and `next`'s (named directly or through another
// parameter; one that starts or stops evaluating counts) is stored again as the sketch commands store it: the shape
// shown before the change (storedSketch over `before`), solved on with the new values (storedSketch over `next`). So
// the document, a saved file and the geometry built from it hold the same shape, and undo of the change restores the
// shape before it. A sketch the change does not reach is not touched (no solve, no new props). A solve here runs on
// the calling thread, like the sketch commands' (sketch_commands.h); Cancelled when `job` is cancelled.
Status storeDependentSketches(const DocState& before, DocState& next, JobContext* job = nullptr);

} // namespace ray::doc::features
