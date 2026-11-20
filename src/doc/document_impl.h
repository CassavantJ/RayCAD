// Private to src/doc and its tests. The document (Document, makeDocument), the command registry and the JSON form of
// commands were declared here by T-009; T-010 moved them into the public <ray/document.h>, which documents them.
#pragma once

#include <ray/document.h>

#include <cstddef>
#include <optional>

namespace ray::doc::detail {

// ---------- The rollback bar (DocState::rollback; implemented in state.cpp) ----------
// The index of the rollback feature in state.features; nullopt for the whole history.
std::optional<std::size_t> rollbackIndex(const DocState& state);
// Drops a rollback that names no feature of the state, or its last feature (both mean the whole history).
void normalizeRollback(DocState& state);
// Where a feature added without an index goes: right after the rollback feature, else at the end.
std::size_t defaultInsertIndex(const DocState& state);
// Inserts `feature` at `at` (at most the feature count). A feature that goes in right below the rollback bar moves
// the bar past it, so the new feature is built and shown.
void insertFeature(DocState& state, std::size_t at, Feature feature);
// Removes feature `at`. When it held the rollback bar, the bar moves to the feature before it.
void eraseFeature(DocState& state, std::size_t at);

// ---------- Previews never solve a sketch (PLAN.md 7; implemented in document.cpp) ----------
// Document::beginPreview applies its command on the UI thread, which is right only because no command a preview is
// made of solves a sketch: the feature tools preview feature.<type>.add / .edit of the non-sketch types (extrude,
// revolve, fillet, chamfer, hole, datum_plane), whose stored props are the props as given, and the feature tree
// previews feature.reorder, which stores nothing. The scope marks the thread while beginPreview applies a command;
// features::storedSketch, the one place a command solves, asserts that it is not inside one (and logs an error in a
// release build). A preview that has to solve must apply its command with Document::prepare on a worker instead.
class PreviewApplyScope {
public:
    PreviewApplyScope();
    ~PreviewApplyScope();
    PreviewApplyScope(const PreviewApplyScope&) = delete;
    PreviewApplyScope& operator=(const PreviewApplyScope&) = delete;

private:
    bool outer_;
};
// True on a thread inside a PreviewApplyScope.
bool applyingPreview();

} // namespace ray::doc::detail
