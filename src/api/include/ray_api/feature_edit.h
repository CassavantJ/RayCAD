// What the desktop feature tools and feature tree (src/ui/model, task T-098) need from doc beyond the actions,
// without Qt: the closed regions of a sketch as the extrude and revolve features pick them, and a rebuilder for
// showing the model rolled back to an earlier feature.
//
// src/ui may link only api and render (PLAN.md 1.1), so it never calls doc's private helpers itself. Documents change
// only through the actions (feature.<type>.add / .edit, feature.*, param.*); these helpers only read. Not one of the
// contract headers in include/ray/.
#pragma once

#include <ray/api.h>

#include <memory>
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
// with its own cache: for building a snapshot that is not the document's state, such as the history rolled back to an
// earlier feature. Null when the document is unknown to `app`.
std::shared_ptr<doc::IRebuilder> makeSideRebuilder(IApp& app, const OpenDocument& document);

} // namespace ray::api::feature_edit
