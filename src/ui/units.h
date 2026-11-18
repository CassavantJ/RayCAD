// The `Units` QML singleton (task T-024): the ONE source of the display unit format. Everything that shows a length,
// an area or a volume formats it here (or takes format() and listens to changed()), so a unit switch reaches every
// read-out at once.
//
//   system     the active document's unit system (DocSettings::units), unless the quick toggle (toggle(), the
//              status bar's unit button) switched the display of that document; with no document open, the
//              unit system new documents get (settings units.newDocument). The toggle is per open document and
//              lasts for the session; it does not change the document (that would be a document command).
//   decimals   from the user settings: units.decimalsInch or units.decimalsMetric, by system.
//
// QML: `text: Units.revision >= 0 ? Units.formatLength(mm) : ""` re-evaluates when the format changes.
// C++ (the measure tools, T-022): connect to changed() and read format().
#pragma once

#include <ray/api.h> // UnitSystem, UnitFormat (core.h through the api: src/ui may name only api and render)

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <map>
#include <string>

namespace ray::ui {

class Session;

class UnitsObject : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Units)
    QML_SINGLETON

    Q_PROPERTY(QString system READ systemName NOTIFY changed)   // "inch" or "metric"
    Q_PROPERTY(QString unitLabel READ unitLabel NOTIFY changed) // "in" or "mm"
    Q_PROPERTY(int decimals READ decimals NOTIFY changed)
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY changed)
    Q_PROPERTY(QString documentSystem READ documentSystemName NOTIFY changed) // the document's own; "" without one
    Q_PROPERTY(bool overridden READ overridden NOTIFY changed) // the quick toggle differs from the document
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    // `session` gives the active document (may be null: no document). It must outlive this object.
    explicit UnitsObject(Session* session, QObject* parent = nullptr);
    ~UnitsObject() override;

    static UnitsObject* create(QQmlEngine* qml, QJSEngine* js);
    static void provideFor(QQmlEngine* engine, UnitsObject* units);

    // ---- what is in effect ----
    UnitFormat format() const { return UnitFormat{system_, decimalsFor(system_)}; }
    UnitSystem system() const { return system_; }
    QString systemName() const;
    QString unitLabel() const;
    int decimals() const { return decimalsFor(system_); }
    bool hasDocument() const { return !docId_.empty(); }
    QString documentSystemName() const;
    bool overridden() const;
    int revision() const { return revision_; }

    // ---- inputs ----
    // From the user settings (settings.h applies them).
    void setDecimals(int inch, int metric);
    void setNewDocumentSystem(UnitSystem system);
    // The active document changed (Session::documentChanged); re-reads its unit system.
    void refreshDocument();

    // ---- formatting (all values in mm, mm², mm³) ----
    Q_INVOKABLE QString formatLength(double mm) const;                   // "0.5000 in", "12.700 mm"
    Q_INVOKABLE QString formatArea(double mm2) const;                    // "0.2500 in²"
    Q_INVOKABLE QString formatVolume(double mm3) const;                  // "1.0000 in³"
    Q_INVOKABLE QString formatPoint(double x, double y, double z) const; // "1.000, 2.000, 3.000 mm"
    // A selection read-out row (selection_model.cpp): {label, value, quantity, mm}: quantity "length", "area",
    // "volume" or "point" (mm is a number, or [x, y, z] for a point) is formatted here; anything else shows value.
    Q_INVOKABLE QString formatRow(const QVariantMap& row) const;
    // Parses what a person typed ("0.5", "1/2 in", "12.7mm") in the current system; NaN if it does not read.
    Q_INVOKABLE double parseLength(const QString& text) const;
    // The quick toggle: switches the active document's display between inch and mm (no document: the default).
    Q_INVOKABLE void toggle();
    // "inch" or "metric" for the active document's display; "" goes back to the document's own units.
    Q_INVOKABLE void setDisplaySystem(const QString& system);

signals:
    void changed();
    // The quick toggle was used without a document open: the settings store the new default (settings.h).
    void newDocumentSystemRequested(ray::UnitSystem system);

private:
    int decimalsFor(UnitSystem s) const { return s == UnitSystem::Inch ? decimalsInch_ : decimalsMetric_; }
    void update();
    QString number(double value) const; // formatted in the current decimals, no unit

    QPointer<Session> session_;
    std::string docId_;
    UnitSystem docSystem_ = UnitSystem::Inch;
    UnitSystem newDocSystem_ = UnitSystem::Inch;
    std::map<std::string, UnitSystem> toggled_; // per open document id
    UnitSystem system_ = UnitSystem::Inch;
    int decimalsInch_ = 4;
    int decimalsMetric_ = 3;
    int revision_ = 0;
};

} // namespace ray::ui
