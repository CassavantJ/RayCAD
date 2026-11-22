// The non-visual part of the CAM workspace (task T-044): what the setup panel and the CAM tree compute without the
// view or the document, so it can be tested on its own. Display and input only (src/ui is not deterministic code,
// PLAN.md 8): the toolpath frame itself is cam::resolveSetup's.
//
//   Badges     the status of an operation (cam::OpStatus) as the tree shows it: a key for QML, short text.
//   Origin     the origin choices of the panel ("corner0".."corner7", "topCenter", "modelPoint") and cam::Wcs.
//   Triad      the WCS triad as line segments (a shaft and an arrow head per axis) and the tips the labels go at.
//   Arguments  the cam.setup.add / cam.setup.edit arguments from what the panel holds.
#pragma once

#include <ray/api.h>

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ray::ui::camui {

// ---------- Badges ----------
// "upToDate", "stale", "generating", "failed", "broken", "suppressed".
QString statusKey(cam::OpStatus status);
// "Up to date", "Stale", "Generating" (with a percentage when progress is 0..1), "Failed", "Broken reference",
// "Suppressed".
QString statusText(cam::OpStatus status, double progress = -1);
// A setup row's badge: its own resolution first (a broken reference or a failure of its WCS or stock), then the worst
// of its operations that are not suppressed (broken > failed > generating > stale > up to date). nullopt when there is
// nothing to show (no operations and the setup resolves, or nothing known yet).
struct Badge {
    cam::OpStatus status = cam::OpStatus::Stale;
    QString message;
};
std::optional<Badge> setupBadge(const std::optional<Status>& own, const std::vector<const cam::OpState*>& operations);

// ---------- Origin ----------
QString originKey(const cam::Wcs& wcs);
// False for an unknown key.
bool originFromKey(const QString& key, cam::Wcs::OriginMode* mode, int* corner);
// The marker keys in Preview::markers order: corner0..corner7, topCenter.
QString markerKey(int index);

// ---------- Triad ----------
struct Triad {
    std::array<std::vector<Vec3>, 3> segments; // per axis X, Y, Z: pairs of points (shaft, then the two head strokes)
    std::array<Vec3, 3> tips;                  // where each axis ends (the labels go just past them)
};
// Axes of `length` from `origin` along the unit vectors x, y, z; the head strokes go back along the axis by a fifth
// of the length and out by a twelfth, in the plane of the axis and the next one.
Triad triadGeometry(Vec3 origin, Vec3 x, Vec3 y, Vec3 z, double length);
// The triad length for a stock box: a third of its largest extent, at least 5 mm and at most 200 mm.
double triadLength(const Box3& stock);

// ---------- Setup arguments ----------
struct SetupInput {
    QString id; // the setup id
    QString name;
    QStringList partBodies, fixtureBodies;
    QString stock;                      // "box", "cylinder", "solid"
    std::map<QString, QString> lengths; // field text by key: xNeg xPos yNeg yPos zNeg zPos diameter length endOffset
    QString axis;                       // cylinder: "x", "y", "z"
    QString solidBody;                  // solid: the body id
    QString origin;                     // originKey
    int workOffset = 1;
    QString programNumber;                  // text
    QJsonObject zFace, xEdge, originVertex; // TopoRef JSON, empty when not picked
    bool flipX = false, flipZ = false;
    QString machine; // id, empty for none
    QString post;
    QJsonObject libraryMachine;  // the machine to embed in the same step (library file format), empty if embedded
    QString material;            // the stock material's id, empty for none
    QJsonObject libraryMaterial; // the material to embed in the same step, as the machine
};
// A field's text to millimetres, or nullopt with the reason in *error.
using LengthFn = std::function<std::optional<double>(const QString& text, QString* error)>;
struct ArgsResult {
    QJsonObject args; // empty when `error` is set
    QString error;    // why the values cannot make a setup ("" when they can)
    QString field;    // the key of the field it is about ("" when none)
};
// The arguments of cam.setup.add (adding) or cam.setup.edit (every member given, so the edit replaces them all).
ArgsResult setupArgs(const SetupInput& input, const LengthFn& length, bool adding);

} // namespace ray::ui::camui
