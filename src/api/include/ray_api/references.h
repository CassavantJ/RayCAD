// What the reference review panel (src/ui, task T-083) needs from doc, cam and naming, without Qt: every persistent
// reference the document holds (feature props, CAM operation geometry, setup WCS references), each resolved where its
// owner resolves it, and the commands that confirm a repaired one or put new picks in its place.
//
// Where a reference is resolved:
//   - a feature's references where the feature stands: in the model as it was just before it
//     (feature_edit::stateBefore, built by the document's own rebuilder: its cache), exactly as the rebuild resolves
//     them (PLAN.md 3.3). Suppressed features, features rolled back and features that were not built (an earlier
//     feature of their body failed) are left out: what their references would resolve to is not known.
//   - a CAM operation's geometry and a setup's WCS references in the document's build (the build the CAM engine
//     reads, doc::camSnapshot).
// Where the references are found: in a feature's props, every JSON object that reads as a TopoRef (naming.h's JSON
// form: "body", "kind" and "names"), at any depth (fillet "edges", direct-edit "faces" and "face", a sketch's or datum
// plane's "face", ...); in a CAM operation, `geometry`; in a setup, wcs.zFace, wcs.xEdge and wcs.originVertex.
//
// Changing them: through the owner's own edit action, one undo step each (feature.<type>.edit, or feature.edit for a
// type without one; cam.operation.edit; cam.setup.edit). Confirming a Repaired reference sets its repairConfirmed
// (naming.h: the confirmation is the review), nothing else. Split and Merged have no confirmation of their own: a
// feature uses them already (with a warning), a CAM operation needs a new pick to be posted (cam.h
// operationInputHash), so only Repaired is confirmable.
//
// src/ui may link only api and render (PLAN.md 1.1); these helpers only read. Not one of the contract headers in
// include/ray/. Added by T-083.
#pragma once

#include <ray/api.h>

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ray::api::references {

enum class Owner : std::uint8_t { Feature, Operation, Setup };

struct Reference {
    Owner owner = Owner::Feature;
    std::string ownerId;   // the feature's, operation's or setup's id
    std::string ownerName; // its name
    std::string ownerType; // feature type ("delete_face"), operation type ("drill"), or "setup"
    // Order of the owners: features by history index, then setups and operations in program order (a setup, then its
    // operations), counting on after the last feature.
    std::size_t ownerOrder = 0;
    std::string setupName; // an operation's setup, or the setup itself
    // Where in the owner, as JSON path segments joined by '/': "faces/1", "face", "base/face", "geometry/0",
    // "wcs/zFace". `index` is the reference's place among its owner's references (0-based) and `of` their number.
    std::string path;
    std::size_t index = 0, of = 1;
    bool inList = false; // an entry of a list: a new pick may put several references in its place
    kernel::SubKind kind = kernel::SubKind::Face;
    naming::ResolveState state = naming::ResolveState::Broken;
    std::string reason;           // the resolver's (naming::Resolved::reason), may be empty
    bool repairConfirmed = false; // the stored reference's
    BodyId body;
    // Features: the elements are those of the model just before this feature (Listing::before holds its build);
    // nullopt: of the document's build.
    std::optional<FeatureId> before;
    std::vector<kernel::SubId> elements; // what it resolves to now (all pieces of a split element)
    std::string refJson;                 // the stored reference (naming::toJson)
    // Where the element lay when it was picked (its pick-time evidence, naming.h Territory), in mm; empty when the
    // reference has none.
    std::optional<Box3> pickedBox;
    std::vector<Vec3> pickedPoints;

    // A Repaired reference the user has not confirmed yet.
    bool confirmable() const { return state == naming::ResolveState::Repaired && !repairConfirmed; }
    // Needs the user: Split, Merged, Broken, or Repaired and not confirmed. (A confirmed repair is the user's answer.)
    bool needsReview() const
    {
        return state == naming::ResolveState::Split || state == naming::ResolveState::Merged ||
               state == naming::ResolveState::Broken || confirmable();
    }
};

struct Listing {
    // In owner order, then path order within an owner.
    std::vector<Reference> references;
    // The model before each feature listed (Reference::before), for highlighting its elements.
    std::map<FeatureId, doc::BuildResultPtr> before;
};

// Every reference of `state` that needs review (Reference::needsReview), or every one with `all`, resolved as above
// against `build` (the document's build of `state`; null: nothing of CAM is resolved and features are built by the
// rebuilder). `documentId` is the open document whose rebuilder builds the models before features
// (feature_edit::buildSnapshot). Runs the rebuilder and the resolver: call it on a worker. Cancelled stops it.
Result<Listing> list(IApp& app, const std::string& documentId, const doc::DocStatePtr& state,
                     const doc::BuildResultPtr& build, JobContext& ctx, bool all = false);

// The command that puts `refs` (TopoRef JSON objects, as model.reference gives them) in the place of `ref` in
// `state`, through its owner's edit action: one reference for a reference that is not in a list, one or more in a
// list (they take its place, in that order; one already in the list is not added twice). InvalidArgument for a wrong
// count or a reference whose kind differs; NotFound when the owner or the reference is no longer in `state`.
Result<doc::Command> replaceCommand(const doc::DocState& state, const Reference& ref,
                                    std::span<const std::string> refs);
// The command that confirms a Repaired reference (repairConfirmed set), likewise.
Result<doc::Command> confirmCommand(const doc::DocState& state, const Reference& ref);

} // namespace ray::api::references
