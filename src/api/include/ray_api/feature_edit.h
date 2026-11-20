// What the desktop feature tools and feature tree (src/ui/model, task T-098) need from doc beyond the actions,
// without Qt: the closed regions of a sketch as the extrude and revolve features pick them, numeric fields as the
// rebuild reads them, and a side rebuilder.
//
// src/ui may link only api and render (PLAN.md 1.1), so it never calls doc's private helpers itself. Documents change
// only through the actions (feature.<type>.add / .edit, feature.*, param.*); these helpers only read. Not one of the
// contract headers in include/ray/.
#pragma once

#include <ray/api.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api::feature_edit {

// One closed region of a sketch: what an extrude or revolve "regions" entry names (the entities bounding its outer
// loop, sorted) and its outline on the sketch plane (mm, polylines that close on themselves; arcs and circles are
// tessellated, splines follow their control polygon).
struct Region {
    std::vector<SketchEntId> entities;
    std::vector<Vec2> outer;
    std::vector<std::vector<Vec2>> holes;
    double area = 0; // mm², the outer loop's
};
// The regions of sketch feature `sketch` in `state` (its stored, last solved model), as the sketch feature's rebuild
// finds them. InvalidArgument when the feature is not a sketch, NotFound when there is none; a sketch without a
// closed outline gives an empty list.
Result<std::vector<Region>> regionsOf(const doc::DocState& state, const FeatureId& sketch);

// The value of a numeric field as the feature's rebuild would read it: a length in mm or an angle in radians, the
// expression evaluated over the document's parameters. Errors say what is wrong with the expression.
Result<double> evaluate(const doc::DocState& state, std::string_view expression, bool angle);

// A rebuilder that reads the same objects as the document's own (imported BREP objects, in memory or in its file),
// with its own cache: for building a snapshot that is not the document's state. (Rolling back no longer needs it:
// the rollback bar is document state, DocState::rollback, built by the document's own rebuild.) Null when the
// document is unknown to `app`.
std::shared_ptr<doc::IRebuilder> makeSideRebuilder(IApp& app, const OpenDocument& document);

// ---------- The model as it was before a feature (the feature tools' edit mode) ----------
// `state` with the rollback bar on the feature before `feature` (DocState::rollback), so a rebuild stops there and
// builds the model as it was just before it; when it is the first feature, `state` without features (nothing is
// built). Null when `state` has no such feature.
doc::DocStatePtr stateBefore(const doc::DocState& state, const FeatureId& feature);
// Builds `snapshot` with the rebuilder of open document `documentId` (OpenDocument::id), so every feature unchanged
// since its rebuilds comes from its cache (stateBefore() of the document's state builds nothing new). For snapshots
// that are not the document's state. Runs on the calling thread: call it on a worker (any thread may call it).
// NotFound when no such document is open.
Result<doc::BuildResultPtr> buildSnapshot(IApp& app, const std::string& documentId, doc::DocStatePtr snapshot,
                                          JobContext& ctx);
// Where a stored reference (TopoRef JSON, as feature props hold it) is in `build`: its body and the elements it
// resolves to (all pieces of a split element). nullopt when it does not parse or its resolution is not usable
// (naming::Resolved::usable). Runs the resolver: call it on a worker.
struct Located {
    BodyId body;
    std::vector<kernel::SubId> elements;
};
std::optional<Located> locate(const doc::BuildResult& build, std::string_view refJson);

} // namespace ray::api::feature_edit
