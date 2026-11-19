// The "sketch" feature type (task T-096), private to src/doc and its tests.
//
// Props: {"plane": {"principal": "xy" | "yz" | "xz"} | {"datum": "<datum_plane id>"} | {"face": <TopoRef>},
//         "sketch": <sketch model, sketch_json.h>,
//         "labels"?: {"<dimension id>": {"x", "y"}}}   (T-093: dimension label offsets, display only)
// The model is stored in the feature itself (not as a separate object): the feature object is content-addressed
// already, and a sketch is small. Its point coordinates are the last solved ones, so a file opens without a solve.
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

} // namespace ray::doc::features
