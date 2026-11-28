// The direct-edit feature types (PLAN.md 3.2, task T-082), private to src/doc and its tests:
//   "delete_face"  {"faces": [<TopoRef of a face>, ...]}                     remove the faces and heal the gap
//   "offset_face"  {"faces": [<TopoRef of a planar face>, ...], "distance": <length>}   push/pull
//   (offset_face_feature.h) "resize_hole"  {"face": <TopoRef of a face of a hole>, "diameter": <length>}
//   (resize_hole_feature.h)
// Lengths are numbers in mm or expression strings over the document parameters, as every parametric feature takes them
// (feature_props.h). All faces of one feature are on one body, which the feature writes. They run the kernel's direct
// edits (kernel.h "Direct editing": deleteFaces, offsetFaces, resizeHole) on the body as it is just before the feature,
// so they work on an imported body (a customer's STEP file) as on a modelled one, at any point of the history, and an
// edit of a feature before them replays them.
//
// REFERENCES. Each face reference is resolved where the feature stands (naming::IResolver), exactly as the fillet
// resolves its edges: Exact is used; Split uses every piece and Merged the whole face that now holds the picked one,
// each with a warning naming the face so the change can be reviewed; a Repaired reference the user confirmed is used
// with a warning. A Broken reference, or a Repaired one not confirmed, fails the feature with ReferenceBroken and a
// message naming which face: nothing is ever edited on a guess (PLAN.md 3.3). A reference that resolves to something
// that is not a face fails too.
//
// KERNEL. What the kernel says is passed on unchanged after the feature's name: its warnings become the feature's
// warnings ("<name>: <kernel warning>", e.g. push/pull's step where a neighbour is not square, or the counterbore of a
// hole left in place), and its refusal becomes the feature's failure with the kernel's error code and message
// ("<name>: <kernel message>", which ends "Nothing was changed."). A failed feature changes no body: the rebuilder
// keeps the body as the features before it left it (document.h, "Failure"). Cancellation (the kernel abandons its OCCT
// work and returns Cancelled at once) is passed through as Cancelled, so the rebuild ends and nothing is cached.
//
// NAMING. The result is named by naming::propagate from the kernel's ShapeHistory (role: the type name), with the input
// shape given so a face that took in a neighbour carries its names; later features and CAM operations follow their
// references through that history like through any other feature. Nothing is unified after an edit (no
// unifySameDomain): the kernel's booleans already unify what the edit itself makes coplanar (a face pushed flush with
// another is Merged), and a separate unify would also merge faces the edit never touched (the half faces many exporters
// write for a hole), changing references to them.
#pragma once

#include "feature_props.h"

#include <ray/document.h>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ray::doc::features {

inline constexpr std::string_view kDeleteFaceType = "delete_face";

std::unique_ptr<IFeatureType> makeDeleteFaceFeatureType();
std::string_view deleteFacePropsSchema();

// ---------- Shared by the three direct-edit types (delete_face_feature.cpp) ----------
namespace directedit {

// The schema of a non-empty array of face references, all on one body.
std::string facesSchema(std::string_view description);

// Reads `key`: a non-empty array of face references, all on one body (errors name the field: "'faces[1]' ...").
Result<std::vector<naming::TopoRef>> readFaceRefs(const PropsReader& r, std::string_view key);
// Reads `key`: one face reference.
Result<naming::TopoRef> readFaceRef(const PropsReader& r, std::string_view key);
// InvalidArgument "<where>: '<field>' is on body <id>, which is not in the document" unless `body` is in `state`.
Status checkBody(const DocState& state, const BodyId& body, const std::string& where, std::string_view field);

// The faces the references stand for where the feature stands. `faceWord(i)` names reference i in messages ("face 2
// of 3", "the hole face"); `done` says what happens to them in warnings ("deleted", "moved", "resized"). Every piece of
// a Split reference and the whole face of a Merged one are included, with a warning each; a reference that is not
// usable fails the whole call with ReferenceBroken naming every such reference.
struct ResolvedFaces {
    std::vector<kernel::SubId> faces;               // sorted, unique
    std::vector<std::vector<kernel::SubId>> perRef; // what each reference resolved to
    std::vector<std::string> warnings;              // "<where>: ..."
};
Result<ResolvedFaces> resolveFaces(const RebuildContext& ctx, std::span<const naming::TopoRef> refs,
                                   const std::string& where, std::string_view done);
// "its face" for one reference, "face i+1 of n" for several.
std::string faceWord(std::size_t i, std::size_t n);

// The body to edit as it is just before the feature: its shape, or ReferenceBroken if it is not built.
Result<kernel::Shape> bodyShape(const RebuildContext& ctx, const BodyId& body, const std::string& where);

// The feature's output from the kernel's answer `op` on `input` (the body `body` before the feature): Cancelled passed
// through; a failure as "<where>: <kernel message>" with the kernel's code; the kernel's warnings after `warnings`,
// each as "<where>: <warning>"; the result named by naming::propagate with the role `type`. `moves`: how the edit moved
// the faces it was given (naming.h NamingInput::faceMoves; offset_face's, by naming::offsetFaceMove), so that what was
// picked on them follows them.
Result<FeatureOutput> finishEdit(const Feature& f, std::string_view type, const RebuildContext& ctx, const BodyId& body,
                                 const kernel::Shape& input, Result<kernel::OpResult> op,
                                 std::vector<std::string> warnings, std::span<const naming::FaceMove> moves = {});

} // namespace directedit
} // namespace ray::doc::features
