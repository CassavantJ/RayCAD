// The "extrude" feature type, and what it shares with "revolve" (task T-096). Private to src/doc and its tests.
//
// Props: {"sketch": "<sketch feature id>",
//         "regions"?: [["<entity id>", ...], ...],   regions to sweep; all closed regions of the sketch when absent
//         "extent"?: "distance" | "through_all",     "distance" when absent (every feature made before it existed)
//         "distance": <length> (not with through_all), "direction"?: "normal" | "reverse", "symmetric"?: false,
//         "draft"?: <angle, 0>, "operation": "new" | "join" | "cut" | "intersect", "target": "<body id>"}
//
// THROUGH ALL. The extent is stored, not a length: every rebuild works out the length from the target body as it is
// just before the feature (its bounding box along the sketch normal, plus a margin), so the cut or join still goes
// through after the body grows upstream. One side ("direction"), or both sides of the sketch when "symmetric".
// It needs a target body, so it is refused for "new". A RayCAD from before "extent" ignores the key and fails the
// feature for its missing distance, rather than building it blind.
//
// REGIONS. A region is picked by the sketch entities that bound its outer loop (the UI stores all of them). On rebuild
// each stored list picks the region whose outer loop shares the most entities with it; a tie or no shared entity fails
// with a message ("pick the region again"). The cap faces of a region are named from the stored list, not from the
// curves the region has now, so adding a line to the outline (a corner cut) or trimming one keeps the caps' names;
// each side face is named from its own sketch entity (PLAN.md 3.3 rule 1). Without "regions" every region that does
// not fill a hole of another region is swept (a circle inside a rectangle is a hole; an island inside it is swept
// again), and its caps are named from its current outline.
//
// BUILD (one region at a time, as the T-081a harness does): kernel::extrude of the region on the sketch plane, named
// by naming::propagate from its SweepFaceOrigins; then for "new" the first region is the body and later ones are
// fused into it; "join" / "cut" fuse into / cut from the target body, each named by propagate over the body's and the
// tool's maps; "intersect" fuses the regions first and then intersects the target with them. The feature writes
// `target` (a new body's id for "new").
#pragma once

#include "feature_props.h"
#include "sketch_feature.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::doc::features {

inline constexpr std::string_view kExtrudeType = "extrude";

std::unique_ptr<IFeatureType> makeExtrudeFeatureType();
std::string_view extrudePropsSchema();

// ---------- Shared by extrude and revolve ----------
struct SweepCommon {
    FeatureId sketch;
    std::optional<std::vector<std::vector<SketchEntId>>> regions;
    std::string operation; // new, join, cut, intersect
    BodyId target;
};
Result<SweepCommon> readSweepCommon(const PropsReader& r);

// The loop tag a region's caps are named from: a hash of the (sorted) entities that pick it.
std::uint64_t regionTag(std::vector<SketchEntId> entities);

// The regions to sweep, each as profile loops (outer first, its tag set by regionTag).
Result<std::vector<std::vector<kernel::ProfileLoop>>> selectRegions(const SketchArtifact& sketch,
                                                                    const SweepCommon& props, const std::string& where,
                                                                    const std::string& sketchName);

// The sketch artifact a sweep uses (checks the sketch comes earlier, is a sketch and is not suppressed).
Result<std::shared_ptr<const SketchArtifact>> sketchFor(const Feature& self, RebuildContext& ctx,
                                                        const FeatureId& sketch, const Feature** sketchFeature);

// Sweeps every region with `sweepOne` and combines the tools with the target as the operation says, naming each
// step. `role` is the naming role ("extrude", "revolve").
using SweepOne = std::function<Result<kernel::SweepResult>(const kernel::Profile&, JobContext&)>;
Result<FeatureOutput> buildSweep(const Feature& self, RebuildContext& ctx, const SweepCommon& props,
                                 const kernel::Plane& plane,
                                 const std::vector<std::vector<kernel::ProfileLoop>>& regions, std::string_view role,
                                 const SweepOne& sweepOne);

// Schema fragments shared by both.
std::string regionsSchema();
std::string operationSchema();

// Command checks shared by both: the sketch, the operation's body.
Status validateSweepCommon(const DocState& state, std::size_t index, const Feature& self, const SweepCommon& props);

} // namespace ray::doc::features
