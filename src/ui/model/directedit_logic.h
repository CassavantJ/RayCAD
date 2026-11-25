// The non-visual part of the direct-edit tools and the reference review (task T-083), testable without a window:
// which tool makes which feature, the preview state machine, drag snapping and length text, and the review list's
// grouping and wording.
//
//   Tools     Delete faces (delete_face), Push/Pull (offset_face), Resize hole (resize_hole).
//   Flow      the preview's phases: Picking (no faces, or no value yet), Building (a preview of the newest picks and
//             values is on its way), Ready / Warning (that preview built; the kernel's warning is shown before OK),
//             Refused (the document or the kernel said no: the message shows, OK is blocked), Committing. Every request
//             gets a serial; a result for an older one is ignored, so OK is never enabled by a preview of values that
//             are no longer the ones shown.
//   Snapping  a push/pull drag moves in steps of the display unit: 0.1 mm or 0.005 in, a tenth of that with Shift.
//             (The other drags of the app, the extrude arrow and the hole depth, do not snap: they show 3 decimals.)
//   Review    rows grouped by owner (features in history order, then setups and their operations in program order),
//   each
//             with its state in shop words.
// Text is translated in the "ray::ui::DirectEdit" context. Display and input only (src/ui is not deterministic code,
// PLAN.md 8).
#pragma once

#include <QtCore/QString>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ray::ui::directedit {

enum class Tool : std::uint8_t { None, Delete, PushPull, Resize };

// "delete", "pushPull", "resize" (QML's names), "" for None; and back (None for anything else).
QString toolKey(Tool tool);
Tool toolOfKey(const QString& key);
// The feature type each tool adds: "delete_face", "offset_face", "resize_hole" ("" for None); and back.
QString featureType(Tool tool);
Tool toolOfFeatureType(const QString& type);
// The panel's title and the name stem of the features it adds: "Delete Faces", "Push/Pull", "Resize Hole".
QString toolTitle(Tool tool);

// ---------- the preview state machine ----------
enum class Phase : std::uint8_t { Closed, Picking, Building, Ready, Warning, Refused, Committing };
enum class Outcome : std::uint8_t { Ok, Warning, Refused };

class Flow {
public:
    void open();
    void close();
    // The picks or values are not complete (no face, no number): nothing to preview, nothing to commit.
    void incomplete();
    // A preview of the current picks and values is asked for: its serial.
    std::uint64_t request();
    // The answer to request `serial`. False (and nothing changes) when a newer request was made since, or none is open.
    bool result(std::uint64_t serial, Outcome outcome);
    // The document refused the command before any build (bad props): Refused at once.
    void refuse();
    void commitStarted();
    void commitFailed(); // Refused: the message says why
    void committed();    // Closed

    Phase phase() const { return phase_; }
    bool active() const { return phase_ != Phase::Closed; }
    bool busy() const { return phase_ == Phase::Building || phase_ == Phase::Committing; }
    // OK: the newest preview built (with or without a warning) and nothing newer is on its way.
    bool canCommit() const { return phase_ == Phase::Ready || phase_ == Phase::Warning; }
    std::uint64_t serial() const { return serial_; }

private:
    Phase phase_ = Phase::Closed;
    std::uint64_t serial_ = 0;
};

// The panel's previewStatus: "" (closed or picking), "building", "ok", "warning", "failed", "committing".
QString phaseKey(Phase phase);

// ---------- lengths ----------
// The drag step in mm: 0.1 mm (metric) or 0.005 in, a tenth of that when fine.
double snapStepMm(bool inch, bool fine);
// `mm` rounded to the nearest step. Never 0 (a push/pull of 0 is refused): a value that rounds to 0 becomes one step
// in the direction of `mm` (positive for 0).
double snapLength(double mm, bool inch, bool fine);
// A length as the panel's fields show it, in the display unit with the step's decimals and no trailing zeros:
// "1.2 mm", "-0.055 in", "3 mm".
QString lengthText(double mm, bool inch, bool fine);

// ---------- the review list ----------
enum class OwnerKind : std::uint8_t { Feature, Operation, Setup };
enum class RefState : std::uint8_t { Exact, Split, Merged, Repaired, Broken };
enum class ElementKind : std::uint8_t { Face, Edge, Vertex };

struct ReviewItem {
    OwnerKind owner = OwnerKind::Feature;
    QString ownerId, ownerName, setupName;
    QString path; // where in the owner: "faces/1", "face", "geometry/0", "wcs/zFace"
    std::size_t ownerOrder = 0;
    std::size_t index = 0, of = 1; // the reference's place among its owner's
    RefState state = RefState::Broken;
    ElementKind kind = ElementKind::Face;
    std::size_t elements = 0; // what it resolves to now
    bool confirmable = false;
    QString reason;
};

struct ReviewGroup {
    OwnerKind owner = OwnerKind::Feature;
    QString ownerId;
    QString title;                  // "Delete Faces 1", "Drill 1 in Setup 1", "Setup 1: work offset"
    std::vector<std::size_t> items; // indices into the list given, in their order
};
// Groups by owner, in owner order (features before CAM), items in reference order within each.
std::vector<ReviewGroup> groupItems(const std::vector<ReviewItem>& items);

// The state as a short label: "Split", "Merged", "Check match", "Missing" ("" for Exact).
QString stateLabel(RefState state);
// "split", "merged", "repaired", "broken", "exact": QML's keys (colours).
QString stateKey(RefState state);
// What happened, in shop words: "This reference now points at two faces", "This face changed shape" (Merged: the
// element that carries its name now is not the one picked, larger, smaller or reshaped; the row cannot tell which),
// "This face changed; a likely match was found", "This face no longer exists".
QString stateWords(const ReviewItem& item);
// What it means for the owner: "The feature does not build until the face is picked again.", ...
QString consequence(const ReviewItem& item);
// Which reference of its owner: "Face 2 of 3", "The face", "Z face of the work offset", "Geometry 1 of 2".
QString placeWords(const ReviewItem& item);
// "one", "two", ... "twelve", then digits.
QString countWord(std::size_t n);
// "face" / "faces", "edge" / "edges", "point" / "points".
QString elementWord(ElementKind kind, std::size_t n = 1);

} // namespace ray::ui::directedit
