// The non-visual part of the operation panel (task T-045) besides the parameter form (operation_form.h): what the
// panel reads from a type's schema annotations (x-geometry, x-heightsUsed, x-cuttingUsed, x-toolTypes;
// docs/operation_types.md), the feeds read-out in the display units, and the cam.operation.add
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
// x-heightsUsed, in heightKeys() order (all five when absent): its keys (the object form) or its names (the array
// form).
QStringList heightsUsed(const QJsonObject& schema);
// What x-heightsUsed says height `key` is measured from ("stockTop", "partTop", "workZero", "eachPick",
// "highestPick"), "" when it does not say.
QString heightFrom(const QJsonObject& schema, const QString& key);
// The hint for a reference, worded for the geometry kind the type picks: "from stock top", "from each hole's top",
// "from the picked edges", ...; "" for an unknown or empty reference.
QString heightFromText(const QString& from, const QString& geometryKind);
// The registered type names in the panel's order: drill, face, contour2d, pocket2d, slot, chamfer2d, engrave,
// threadmill, then any others by name.
QStringList orderedTypeNames(const QStringList& names);
// The type a new operation starts with: `lastUsed` when it is one of `names`, else drill, else the first of
// orderedTypeNames; "" when there are none.
QString defaultTypeName(const QStringList& names, const QString& lastUsed);
// The cutting members (rpm, clockwise, feedCut, feedPlunge, feedRamp, feedLeadIn, feedLeadOut, coolant).
const QStringList& cuttingKeys();
// x-cuttingUsed, in cuttingKeys() order (all when absent); rpm and feedCut are always in it.
QStringList cuttingUsed(const QJsonObject& schema);
// x-toolTypes for the current params: the tool type names, empty for any.
QStringList toolTypesFor(const QJsonObject& schema, const QJsonObject& params);
// All five heights from the used ones: an unused height takes the nearest used one before it, else after it.
std::map<QString, double> completeHeights(const std::map<QString, double>& used);
// The first pair of heights out of the order every operation of the type needs (api::cam_operation::heightsOutOfOrder:
// two heights the type reads, measured from one reference, the earlier of clearance, retract, feedPlane, top, bottom
// below the later), as the keys of the lower and the higher; nullopt otherwise (any other order is the type's own
// validation's to judge, and the panel shows its message). Heights missing from the map count as 0.
std::optional<std::pair<QString, QString>> heightsOutOfOrder(const QJsonObject& schema,
                                                             const std::map<QString, double>& heights);

// ---------- Feeds ----------
// "Use calculated" has no rule for these (cam.h computeFeeds): fractions of the cut feed, offered as editable defaults.
inline constexpr double kPlungeFraction = 0.5;
inline constexpr double kRampFraction = 0.75;
QString rpmText(double rpm);                      // "1200 rpm"
QString feedText(double mmMin, bool inch);        // "320 mm/min", "12.6 in/min"
QString surfaceSpeedText(double mMin, bool inch); // "300 m/min", "984 sfm"
QString chipLoadText(double mm, bool inch);       // "0.05 mm/tooth", "0.0020 in/tooth"
QString feedPerRevText(double mm, bool inch);     // a tap's pitch: "1.25 mm/rev", "0.0492 in/rev"
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
// The tool number of a new operation whose number the user left empty: the number the document already gives `tool`
// (its first operation in program order that uses it), else `toolDefault` (lib::Tool::defaultToolNumber) when it is
// above 0 and no operation uses it, else one more than the highest number any operation uses (1 when there is none).
// A new tool therefore never takes a number another tool has, which the posts rely on: they change tools only where
// the number changes. Applying a template follows the same rule (cam_template_actions.cpp).
int newToolNumber(const cam::CamState* cam, const ToolId& tool, int toolDefault);

} // namespace ray::ui::camui
