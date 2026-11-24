// What the fixture panel (task T-077, cam/fixtures_tool.h) works out without a view or a document: the props of the
// "transform_body" feature that places a fixture from what the panel shows, where a new one goes in the history, how a
// stored value is shown again, and the fixture models of the libraries. Display and input only (src/ui is not
// deterministic code).
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ray::ui::camui {

// What the placement editor holds. Lengths are text as typed (a plain number in the display unit, a number with a
// unit, an expression); the angle is in degrees when it is a plain number. Empty text is 0. face/target: TopoRef JSON
// (model.reference), both or neither.
struct PlacementInput {
    QString body;
    QJsonObject face, target;
    QString gap, x, y, z, angle;
    QString unitLabel = QStringLiteral("mm"); // the display unit a plain length is in: "mm" or "in"
};

// Checks an expression: nullopt and *error set when it does not evaluate to the kind asked (a length or an angle).
using CheckFn = std::function<bool(const QString& expression, bool angle, QString* error)>;

struct PlacementArgs {
    QJsonObject props; // the feature's props, every key it uses; omitted keys are 0
    QString error;     // why there are none
    QString field;     // the field the error is about ("gap", "x", ...; "" for none)
};
// The props of feature.transform_body for `in`: {"body", "mate"?: {"face", "target"}, "gap"?, "angle"?, "x"?, "y"?,
// "z"?}, each number as an expression with its unit ("12.7 mm", "0.5 in", "90 deg"); a field that is empty or 0 is left
// out (gap also without a mate).
PlacementArgs placementProps(const PlacementInput& in, const CheckFn& check);

// The props patch of feature.transform_body.edit that makes the stored props `props`: every key of the feature, those
// not in `props` as null (the edit removes them).
QJsonObject editPatch(const QJsonObject& props);

// The text of a number or expression as the panel shows it: a JSON number is mm (shown in `unitLabel`, "mm" or "in",
// at up to 10 significant digits) or radians (shown in degrees); a string is shown as it is.
QString valueText(const QJsonValue& value, bool angle, const QString& unitLabel);

// Where a new transform_body feature that moves `body` goes: before the first transform_body feature whose mate target
// is on `body` (so that one is resolved against the body where this one puts it), else nullopt (the default place).
// `features` lists each feature's type and props JSON in history order.
std::optional<int> placementIndex(const std::vector<std::pair<std::string, std::string>>& features,
                                  const QString& body);

// The fixture models of the libraries: every .step and .stp file in the "fixtures" folder of each root, by root then
// file name. `name` is the file name without its extension, words split at '-' and '_' and the first capitalised
// ("generic-vise-6in.step": "Generic vise 6in").
struct LibraryFixture {
    QString name;
    QString path; // absolute, with '/' separators
    QString root; // the library's name
};
std::vector<LibraryFixture> libraryFixtures(const std::vector<std::pair<std::string, std::string>>& roots);

} // namespace ray::ui::camui
