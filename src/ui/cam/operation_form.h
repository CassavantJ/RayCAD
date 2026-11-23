// The generic parameter form of the operation panel (task T-045): an operation type's JSON Schema
// (IOperationType::paramsSchemaJson) turned into fields, with no knowledge of any type. qml/cam/SchemaForm.qml draws
// it; docs/operation_types.md ("The schema subset the form edits") is the contract it implements:
//
//   Fields     number (with x-unit and limits), integer, boolean, choice (enum with x-enumTitles, and
//              x-enumDescriptions as a second line in the open list and the tooltip), text, a list of points (a text
//              with x-format "pointsXY": shown and typed in the display unit, stored in mm); in
//              x-propertyOrder, then key order; descriptions as hints; a reset to the default per field. A property
//              or top-level keyword the form cannot edit is a row saying so (FormField::Kind::Unsupported, or
//              FormSchema::problems), never a crash or a missing field; such a property's stored value is kept.
//   Relevance  x-relevantWhen, transitive: a field that is not relevant is hidden and not validated, and keeps its
//   value. Numbers    text as the feature panels take it: a bare number in the display unit, a number with a unit, an
//              expression, parameter names, read through an ExprFn (the document's expressions; ray_api
//              cam_operation::evaluate in the app). The value is in the schema's units (mm, degrees, seconds).
//              Text left as loaded keeps the stored value exactly (no round trip through the display unit).
//   Messages   beside each field: what does not read or is out of range, and messages set from outside (an API error
//              naming /params/<key>).
// Display and input only (src/ui is not deterministic code, PLAN.md 8).
#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ray::ui::camui {

// x-unit. Percent: a number in percent (of what, the parameter's description says), shown "45 %".
enum class Unit { None, Length, Angle, Time, Count, Percent };

struct FormField {
    enum class Kind { Number, Integer, Boolean, Enum, String, Unsupported };
    QString key, title, description;
    Kind kind = Kind::Unsupported;
    Unit unit = Unit::None;
    std::optional<double> minimum, maximum, exclusiveMinimum, exclusiveMaximum;
    QStringList enumValues, enumTitles; // titles: x-enumTitles, else the values
    QStringList enumDescriptions;       // x-enumDescriptions, empty when the schema has none
    std::optional<int> minLength, maxLength;
    QString pattern;
    QString format;         // a text's x-format: "pointsXY" (a list of points on the setup's XY plane), "" plain text
    bool multiline = false; // a text's x-multiline: edited on several lines (Enter is a line break)
    QJsonValue defaultValue = QJsonValue(QJsonValue::Undefined);
    std::vector<std::pair<QString, QJsonArray>> relevantWhen; // x-relevantWhen, in key order
    QString reason;                                           // Unsupported: why
};

struct FormSchema {
    QString title, description;
    std::vector<FormField> fields; // in form order
    QStringList problems;          // what the form cannot honour (each is shown as a row)
    QJsonObject top;               // the schema object (x-geometry, x-heightsUsed, ... are read from it)
};
// Parses a schema. A text that is not a JSON object gives no fields and one problem saying so.
FormSchema parseFormSchema(const QString& schemaJson);

// The display text of a stored value: "10 mm" / "0.3937 in" (lengths in the display unit), "118 deg", "0.5 s", "45 %",
// "2".
QString numberText(double value, Unit unit, const QString& lengthUnit);
// The unit label of a field ("mm", "in", "deg", "s", "%", "").
QString unitLabel(Unit unit, const QString& lengthUnit);

// An expression read in the document: kind 0 Length (mm), 1 Angle (degrees), 2 Unitless. nullopt with the reason.
using ExprFn = std::function<std::optional<double>(const QString& expression, int kind, QString* error)>;
// A plain reader for use without a document: numbers, optionally followed by mm, in, deg or rad.
ExprFn plainExprFn();
// A field's text as a value in the schema's units, or nullopt with the reason in *error.
std::optional<double> readNumber(const QString& text, Unit unit, const QString& lengthUnit, const ExprFn& expr,
                                 QString* error);

// x-format "pointsXY": points on the setup's XY plane (WCS), stored as text in mm whatever the display unit, "x y"
// pairs separated by ';' (a comma between x and y reads too): "20 15; 60 15". A point keeps the tokens it was stored
// with, so the points not edited are written back exactly as they were.
struct PointTokens {
    QString x, y; // numbers in mm, as stored
};
// The points of a stored text (empty text: none), or nullopt with the reason ("point 2 has no second number").
std::optional<std::vector<PointTokens>> parsePoints(const QString& text, QString* error);
QString pointsText(const std::vector<PointTokens>& points);
// A length in mm as a stored token: fixed, at most 6 decimals (a nanometre), no trailing zeros ("20", "2.54").
QString mmToken(double mm);

} // namespace ray::ui::camui

namespace ray::ui {

class SchemaFormModel : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString schemaJson READ schemaJson WRITE setSchemaJson NOTIFY schemaChanged)
    Q_PROPERTY(QString title READ title NOTIFY schemaChanged)
    // "mm" or "in": the display unit of lengths.
    Q_PROPERTY(QString lengthUnit READ lengthUnit WRITE setLengthUnit NOTIFY changed)
    // [{key, title, description, kind ("number", "integer", "boolean", "enum", "string", "points", "unsupported"),
    //   unit (label), value (text, or bool), fullValue (the text to edit while it shows a stored value, else ""),
    //   choices [{value, text}], relevant, message, isDefault, defaultText, reason}]
    // A "string" field has multiline (x-multiline: true, a text area where Enter is a line break).
    // A "points" field (a text with x-format "pointsXY") also has points [{x, y, xFull, yFull, xBad, yBad}] (each
    // coordinate in the display unit as a length field shows it, the text being typed where it does not read) and
    // pointsRead (false: the stored text does not read as points, and is edited as text, `value`).
    Q_PROPERTY(QVariantList fields READ fields NOTIFY changed)
    Q_PROPERTY(int fieldCount READ fieldCount NOTIFY schemaChanged)
    Q_PROPERTY(QStringList problems READ problems NOTIFY schemaChanged)
    // Every relevant field reads and is in range.
    Q_PROPERTY(bool valid READ valid NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    explicit SchemaFormModel(QObject* parent = nullptr);

    QString schemaJson() const { return schemaJson_; }
    void setSchemaJson(const QString& json);
    QString title() const { return schema_.title; }
    QString lengthUnit() const { return lengthUnit_; }
    void setLengthUnit(const QString& unit);
    QVariantList fields() const;
    int fieldCount() const { return static_cast<int>(schema_.fields.size()); }
    QStringList problems() const { return schema_.problems; }
    bool valid() const;
    int revision() const { return revision_; }

    // A choice's [{value, text, description}] (stable while the schema stays, so a list bound to it is not made again
    // on each edit); description "" without x-enumDescriptions.
    Q_INVOKABLE QVariantList choicesOf(const QString& key) const;
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void resetField(const QString& key);
    Q_INVOKABLE void resetAll();
    // A "points" field: a coordinate (axis 0 X, 1 Y) of point `index` typed in the display unit (what length fields
    // take); a point added (WCS mm, from a pick in the view or (0, 0) to type over); a point removed.
    Q_INVOKABLE void setPointCoordinate(const QString& key, int index, int axis, const QString& text);
    Q_INVOKABLE void addPoint(const QString& key, double xMm, double yMm);
    Q_INVOKABLE void removePoint(const QString& key, int index);

    // ---- C++ ----
    void setExprFn(camui::ExprFn fn);
    // How a length shows at rest (the app's display format, UnitsObject::formatLength); unset: numberText in full.
    // A field being edited shows the value in full (fields(): fullValue). The stored value stays exact either way.
    void setLengthFormat(std::function<QString(double mm)> format);
    const camui::FormSchema& schema() const { return schema_; }
    // Shows stored params (as the document holds them; members left out show their default).
    void load(const QJsonObject& params);
    // The params OK stores: every field the form edits (its value, or its default where the text does not read and the
    // field is not relevant), and what the form cannot edit kept as loaded.
    QJsonObject params() const;
    // The value of a field now (its default while it does not read); for a field's own type.
    QJsonValue valueOf(const QString& key) const;
    // The "points" fields (x-format "pointsXY"), in form order, and the points of one as they read now (WCS mm; the
    // points of its stored text, those whose coordinates are being typed included as they were).
    QStringList pointKeys() const;
    std::vector<std::pair<double, double>> pointsOf(const QString& key) const;
    bool isRelevant(const QString& key) const;
    // The first relevant field whose text does not read or is out of range, and why.
    std::optional<std::pair<QString, QString>> firstError() const;
    // A message from outside (the API's check), shown beside `key` until the field changes or clearExternal().
    void setExternalMessage(const QString& key, const QString& message);
    void clearExternal();
    QString messageOf(const QString& key) const;

signals:
    void schemaChanged();
    void changed();
    // The user changed a field (setValue, resetField): the owner previews the new values.
    void edited(const QString& key);

private:
    struct Entry {
        QVariant text;                    // what the field shows: QString, or bool for a boolean
        std::optional<QJsonValue> loaded; // the stored value, while `text` is what it was loaded as
        QVariant loadedText;
        QVariant fullText; // the stored value's text in full precision (it reads as the stored value too)
    };
    const camui::FormField* field(const QString& key) const;
    QVariant textOf(const camui::FormField& f, const QJsonValue& v) const;
    QVariant fullTextOf(const camui::FormField& f, const QJsonValue& v) const;
    // The value of the field's text, or nullopt with the reason.
    std::optional<QJsonValue> read(const camui::FormField& f, QString* error) const;
    bool relevant(const camui::FormField& f, int depth = 0) const;
    void bump();
    // The first coordinate of a points field typed so that it does not read: "Point 2, Y: ...", "" when none.
    QString pointEditError(const camui::FormField& f) const;
    // A points field's stored points changed: its text is written from them and the field shows it.
    void setPoints(const QString& key, const std::vector<camui::PointTokens>& points);

    QString schemaJson_;
    camui::FormSchema schema_;
    QString lengthUnit_ = QStringLiteral("mm");
    camui::ExprFn expr_;
    std::function<QString(double mm)> lengthFormat_;
    std::map<QString, Entry> entries_;
    QJsonObject kept_; // loaded values of properties the form cannot edit
    std::map<QString, QString> external_;
    // A points field's coordinates typed that do not read, by key and index * 2 + axis (the stored point keeps its
    // value until they do).
    std::map<QString, std::map<int, QString>> pointEdits_;
    int revision_ = 0;
    mutable QVariantList fieldsCache_; // fields() as of fieldsRevision_
    mutable int fieldsRevision_ = -1;
};

} // namespace ray::ui
