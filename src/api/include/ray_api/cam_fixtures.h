// A library vise placed on a setup's stock (the fixtures panel, after task T-077): what the panel (src/ui/cam
// fixtures_tool.h) needs from cam, doc and naming to stand the stock in the vise the way a machinist holds it, without
// Qt. Placement geometry only, worked out once from the stock as it is: the result is ordinary "transform_body"
// features (src/doc/features/transform_body_feature.h) the user can edit by hand afterwards; nothing follows the stock
// when it changes (the action is simply run again). Not one of the contract headers in include/ray/.
//
// THE FIT (fitVise, pure arithmetic in the setup's WCS; the stock box is cam::ResolvedSetup::stockBounds, axis-aligned
// in the WCS; for a stock carried from an earlier setup, its box):
//   Grip      the jaws close on two opposite side faces of the stock: across WCS X or across WCS Y (Grip::Shorter, the
//             default: across the shorter of the two; across Y when they are equal).
//   Height    the stock's bottom rests on the vise's floor (z = 0 of the vise), or on the parallels' top when the setup
//             has parallels: the floor then lies the parallels' height below the stock. How deep the jaws hold the
//             stock is what results (gripDepth); jawAboveStock says how far the jaws stand above the stock's top.
//   Along     the stock is centred along the jaws (the vise's x = 0 on the middle of the stock).
//   Side      the vise stands square to the WCS with its Z the WCS Z. Across Y the fixed jaw is against the stock's
//             +Y face (the back of a vertical mill) and the moving jaw opens towards -Y (the operator); across X the
//             fixed jaw is against the stock's -X face and the moving jaw opens towards +X.
//   Opening   the moving jaw closes on the stock: opening = the stock across the jaws, clamped to 0..maxOpening; a
//             stock wider than the vise opens is tooWide (the jaw is shown fully open, not touching it).
//   Parallels the pair stands on the floor, one against each jaw, centred along the jaws; a stock narrower than the two
//             side by side is refused.
//
// THE FEATURES (planViseOnStock): the vise and each parallel get a transform_body WITHOUT a mate: tiltX, tiltY and
// angle (the turn that puts the vise square to the WCS, tilts only when the WCS Z is not model +Z) and x, y, z (where
// its frame goes), as numbers. The moving jaw gets a transform_body WITH a mate, as T-077 models the jaw opening: its
// clamping face against the fixed jaw's, the gap the opening, and the angle that keeps it the same way up as the vise.
// A body that has a placement already has it edited (every key replaced); one that has none gets a new one; the vise's
// new placement goes before the first feature that mates another body to the vise or moves its jaw or parallels, so
// they follow it. Everything is one cam.batch command: one undo step.
#pragma once

#include <ray/api.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::api::cam_fixtures {

// ---------- The library's fixture descriptions ----------
// A fixture model in a library's "fixtures" folder may have a description beside it: the same file name with ".json"
// in place of ".step" or ".stp" (generic-vise-6in.json beside generic-vise-6in.step). It names the bodies the file
// imports as and gives what placing them on a stock needs, in the vise frame of resources/library/fixtures/README.md
// (mm; X along the jaws, Y along the screw with the moving jaw opening towards +Y, Z up; the floor between the jaws
// z = 0; the fixed jaw's clamping face the plane y = 0 looking +Y, centred on x = 0):
//   {"schema": 1, "kind": "vise", "body": "<base and fixed jaw>", "movingJaw": "<moving jaw>",
//    "jawLength": <mm>, "jawHeight": <mm>, "maxOpening": <mm>}
//   {"schema": 1, "kind": "parallels", "bodies": ["<one>", "<the other>"], "height": <mm>, "thickness": <mm>}
// A vise's moving jaw is a body of its own with its clamping face looking -Y, centred on x = 0 and z = jawHeight / 2;
// parallels stand on the floor along the jaws.
struct ViseModel {
    std::string body, movingJaw;
    double jawLength = 0, jawHeight = 0, maxOpening = 0;
};
struct ParallelsModel {
    std::vector<std::string> bodies; // two
    double height = 0, thickness = 0;
};
struct FixtureModel {
    enum class Kind { Vise, Parallels } kind = Kind::Vise;
    ViseModel vise;
    ParallelsModel parallels;
};
// InvalidArgument naming what is wrong (a missing or non-positive size, a kind it does not know, schema not 1).
Result<FixtureModel> parseFixtureModel(std::string_view json);

// ---------- The fit ----------
enum class Grip { Shorter, AcrossX, AcrossY };
struct ViseFit {
    bool acrossX = false;     // the jaws close across WCS X (else across WCS Y)
    Xform wcsFromVise;        // the vise frame in the WCS
    double across = 0;        // the stock across the jaws
    double opening = 0;       // the moving jaw's opening: `across` clamped to 0..maxOpening
    bool tooWide = false;     // across > maxOpening
    double floorZ = 0;        // the WCS z of the vise's floor
    double height = 0;        // the stock's height
    double gripDepth = 0;     // how much of the stock's height is between the jaws (<= 0: none)
    double jawAboveStock = 0; // the jaws' top above the stock's top (negative: below it)
    double along = 0;         // the stock along the jaws
};
// `parallels` null: none. Pure arithmetic (see the header).
ViseFit fitVise(const Box3& stock, Grip grip, const ViseModel& vise, const ParallelsModel* parallels);

// The turn and offsets of a transform_body without a mate that make the rigid move `modelFromBody`: angles in radians
// with Rz(angle) Ry(tiltY) Rx(tiltX) its rotation (tilts 0 whenever its Z is model +Z), and its translation. Angles
// within 1e-9 degrees of a whole degree are that degree.
struct FreeMove {
    double tiltX = 0, tiltY = 0, angle = 0;
    Vec3 offset;
};
FreeMove freeMove(const Xform& modelFromBody);

// Text of a length (mm) in the display unit ("mm": up to 6 decimals; "in": up to 8) and of an angle (radians, written
// in degrees with up to 9 decimals), as the feature's expressions take them: "12.7 mm", "0.5 in", "-90 deg"; "0 mm" for
// 0.
std::string lengthText(double mm, std::string_view unit);
std::string angleText(double radians);

// ---------- The features ----------
struct PlaceRequest {
    doc::DocStatePtr state;                 // the document as built (Document::builtState)
    doc::BuildResultPtr build;              // its build
    Xform modelFromWcs = Xform::identity(); // the setup's frame (cam::ResolvedSetup::modelFromWcs)
    Box3 stock;                             // its stock box in the WCS (cam::ResolvedSetup::stockBounds)
    SetupId setup;
    ViseModel viseModel;
    BodyId vise, jaw;
    std::optional<ParallelsModel> parallelsModel; // with `parallels`: the two parallel bodies used
    std::vector<BodyId> parallels;
    Grip grip = Grip::Shorter;
    std::string unit = "mm";              // the display unit numbers are written in ("mm" or "in")
    std::string namePattern = "Place %1"; // a new feature's name; %1: the body's name
    // The build of the model just before a feature (feature_edit::stateBefore, built): where a body that has a
    // placement is found as its library file has it.
    std::function<Result<doc::BuildResultPtr>(const FeatureId&)> buildBefore;
    std::function<FeatureId()> newFeatureId; // default: random
};
struct PlacePlan {
    std::string batchArgsJson; // {"commands": [...]}: the args of cam.batch, without "document"
    ViseFit fit;
    std::vector<BodyId> addedToSetup; // bodies made fixtures of the setup by the batch
};
// The batch that places the vise (and its jaw and parallels) on the stock. Errors (InvalidArgument, saying what to do):
// a body is not in the state or not built; a body is moved by more than one transform_body; the moving jaw's or a
// parallel's placement comes before the vise's in the history while the vise has one; the vise or its jaw is not where
// its library model puts it before it is placed (its clamping face is not found); the stock is narrower than the
// parallels side by side; a reference cannot be made. Runs kernel queries and builds: call it on a worker.
Result<PlacePlan> planViseOnStock(const PlaceRequest& request);

// The transform_body features that move `body`, in history order.
std::vector<FeatureId> placementsOf(const doc::DocState& state, const BodyId& body);

} // namespace ray::api::cam_fixtures
