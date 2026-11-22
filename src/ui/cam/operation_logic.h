// The non-visual part of the operation panel (task T-045) besides the parameter form (operation_form.h): what the
// panel reads from a type's schema annotations (x-geometry, x-heightsUsed, x-cuttingUsed, x-toolTypes;
// docs/operation_types.md), the feeds read-out in the display units, the tool list's order, and the cam.operation.add
// / edit arguments. Display and input only (src/ui is not deterministic code, PLAN.md 8).
#pragma once

#include "operation_form.h"

#include <ray/api.h>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <map>
#include <optional>
#include <vector>

namespace ray::ui::camui {

// ---------- Schema annotations ----------
struct GeometrySpec {
    QString kind = QStringLiteral("none"); // "hole", "face", "edge", "none"
    QString count = QStringLiteral("any"); // "one", "oneOrMore", "any"
    QString description;
    bool needsOne() const { return count == QLatin1String("one") || count == QLatin1String("oneOrMore"); }
    bool atMostOne() const { return count == QLatin1String("one"); }
};
GeometrySpec geometrySpec(const QJsonObject& schema);
// The five heights in their order.
const QStringList& heightKeys();
// x-heightsUsed, in heightKeys() order (all five when absent).
QStringList heightsUsed(const QJsonObject& schema);
// The cutting members (rpm, clockwise, feedCut, feedPlunge, feedRamp, feedLeadIn, feedLeadOut, coolant).
const QStringList& cuttingKeys();
// x-cuttingUsed, in cuttingKeys() order (all when absent); rpm and feedCut are always in it.
QStringList cuttingUsed(const QJsonObject& schema);
// x-toolTypes for the current params: the tool type names, empty for any.
QStringList toolTypesFor(const QJsonObject& schema, const QJsonObject& params);
// All five heights from the used ones: an unused height takes the nearest used one before it, else after it.
std::map<QString, double> completeHeights(const std::map<QString, double>& used);
// The first pair of the five out of order (clearance >= retract >= feedPlane >= top >= bottom), as the keys of the
// lower and the higher; nullopt when ordered.
std::optional<std::pair<QString, QString>> heightsOutOfOrder(const std::map<QString, double>& heights);

// ---------- Feeds ----------
// "Use calculated" has no rule for these (cam.h computeFeeds): fractions of the cut feed, offered as editable defaults.
inline constexpr double kPlungeFraction = 0.5;
inline constexpr double kRampFraction = 0.75;
QString rpmText(double rpm);                      // "1200 rpm"
QString feedText(double mmMin, bool inch);        // "320 mm/min", "12.6 in/min"
QString surfaceSpeedText(double mMin, bool inch); // "300 m/min", "984 sfm"
QString chipLoadText(double mm, bool inch);       // "0.05 mm/tooth", "0.0020 in/tooth"
QString powerText(double kw);                     // "1.2 kW"
// A feed field as mm/min: a bare number in the display unit, a trailing mm/min, in/min or ipm, an expression before
// it. nullopt with the reason.
std::optional<double> readFeed(const QString& text, bool inch, const ExprFn& expr, QString* error);
// A spindle speed field (a trailing rpm is allowed).
std::optional<double> readRpm(const QString& text, const ExprFn& expr, QString* error);
// "Not reached", "Applied", "Exceeded", "Not known".
QString limitStateText(cam::LimitState state);
// Whether the values are those a record wrote (the fields the type uses; within a rounding of the display).
bool matchesRecord(const cam::CalculatedFeeds& record, double rpm, double feedCut, double feedPlunge, double feedRamp,
                   const QStringList& used);
QJsonObject calculatedJson(const cam::CalculatedFeeds& record);

// ---------- Tools ----------
// The order of the tool list: those of the document first, then those of the types wanted, then by diameter and name.
struct ToolRow {
    QString id, name, type;
    double diameter = 0;
    int flutes = 0;
    bool embedded = false; // in the document already
    bool suits = true;     // of a type x-toolTypes offers (or any)
};
void sortTools(std::vector<ToolRow>& rows);
// Whether a row matches the search text: every word in its name, type or diameter text.
bool toolMatches(const ToolRow& row, const QString& search, bool inch);
QString toolDetail(const ToolRow& row, bool inch); // "⌀6 mm, 2 flutes, drill"

// ---------- Arguments ----------
struct OperationInput {
    QString id, setup, type, name;
    QString tool, holder; // holder "" for none
    std::optional<int> toolNumber;
    std::optional<double> stickOut;
    double rpm = 0, feedCut = 0, feedPlunge = 0, feedRamp = 0, feedLeadIn = 0, feedLeadOut = 0;
    bool clockwise = true;
    QString coolant = QStringLiteral("off");
    std::map<QString, double> heights; // all five
    QJsonArray geometry;               // TopoRef JSON
    QJsonObject params;
    QJsonValue calculated = QJsonValue(QJsonValue::Undefined); // an object, null (forget it) or undefined (as it is)
    QJsonArray libraryTools, libraryHolders;                   // items to embed in the same step
};
QJsonObject operationArgs(const OperationInput& input, bool adding);

} // namespace ray::ui::camui
