// The `MeasureTool` QML singleton (task T-022): the Measure panel's model (panels/Measure.qml), its witness geometry in
// the viewport and its labels over it (panels/MeasureOverlay.qml), and the measure.* commands of the palette, the
// context menu and the M key.
//
//   Measuring    The current selection (Selection singleton) is measured with ray_api/measure.h on the job system;
//                the UI thread never runs a kernel query. One element: its own measures; two: the distance and the
//                angle between them; a palette command (measure.distance, measure.angle, measure.radius,
//                measure.massProperties, measure.boundingBox, measure.selection) asks for that measurement of the
//                selection (mass properties and the box: of the selected bodies, else of every body). A newer request
//                supersedes an older one; a late result is dropped.
//   Units        Values are kept in mm / radians and formatted in `unitFormat`, which is ALWAYS the Units singleton's
//                format (units.h, T-024: the document's units, the session toggle, decimals from the user settings),
//                live: a change re-formats, nothing is measured again. setUnits() / setUnitFormat() / `metric` (the
//                panel's in/mm buttons) switch the display units in Units, so the panel, its overlay labels, the
//                selection read-out and the status bar always agree. Mass follows the system too (lb or g / kg).
//   Pinned       pin() keeps the current result (values and witness) while other things are selected; unpin(id) and
//                clearPinned() remove them. They are cleared when another document becomes active.
//   Viewport     The witness lines of the current result (while `active`, i.e. the Measure panel is shown) and of
//                every pinned one go into the viewport with IViewport::addLines in the AccentThinLine theme colour
//                (construction lines dashed); `labels`, `points` and the x-ray `currentPaths` / `pinnedPaths` are the
//                same geometry projected into the viewport item's coordinates for the QML overlay, updated with the
//                camera.
//   Clipboard    copyValue(row), copyPinnedValue(id, row) and copyAll() put the formatted text on the clipboard.
#pragma once

#include <ray/api.h>
#include <ray/render.h>
#include <ray_api/measure.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <optional>
#include <vector>

namespace ray::ui {

class CommandCatalog;
class SceneController;
class SelectionModel;
class Session;
class ShortcutManager;
class UnitsObject;
class ViewportItem;

// A list of maps with fixed role names for QML views. setItems() with as many items as before only reports changed
// data, so delegates are updated in place (new units, a moved camera), not made again.
class MeasureListModel : public QAbstractListModel {
    Q_OBJECT
public:
    MeasureListModel(QList<QByteArray> roles, QObject* parent);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setItems(const QVariantList& items);
    const QVariantList& items() const { return items_; }

private:
    QList<QByteArray> roles_;
    QVariantList items_;
};

class MeasureModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(MeasureTool)
    QML_SINGLETON

    // ---- units ----
    Q_PROPERTY(QString unitSystem READ unitSystem NOTIFY unitsChanged) // "inch" or "mm"
    Q_PROPERTY(int decimals READ decimals NOTIFY unitsChanged)
    Q_PROPERTY(bool metric READ metric WRITE setMetric NOTIFY unitsChanged)
    // Density for the mass, in g/cm³; densityText in the current units.
    Q_PROPERTY(double density READ density WRITE setDensity NOTIFY densityChanged)
    Q_PROPERTY(QString densityText READ densityText NOTIFY densityTextChanged)

    // ---- the current measurement ----
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY currentChanged)
    Q_PROPERTY(QString title READ title NOTIFY currentChanged)
    // { key, label, value (formatted text), primary } in the order the measurement gives them.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY currentChanged)
    // The same rows as a list model (roles key, label, value, primary) for the panel: new units update it in place.
    Q_PROPERTY(QAbstractItemModel* rowsModel READ rowsModel CONSTANT)
    Q_PROPERTY(QStringList notes READ notes NOTIFY currentChanged)
    // What to do (nothing selected), or why it could not be measured.
    Q_PROPERTY(QString hint READ hint NOTIFY currentChanged)
    Q_PROPERTY(bool hintIsError READ hintIsError NOTIFY currentChanged)

    // ---- pinned ----
    // { id, title, rows, notes } each, oldest first.
    Q_PROPERTY(QVariantList pinned READ pinned NOTIFY pinnedChanged)
    Q_PROPERTY(int pinnedCount READ pinnedCount NOTIFY pinnedChanged)

    // ---- the viewport ----
    // The Measure panel is shown: the current result's witness is drawn (pinned ones always are).
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // The ViewportItem the witness goes into and the overlay projects with.
    Q_PROPERTY(QObject* view READ view WRITE setView NOTIFY viewChanged)
    // In the view item's coordinates: { px, py, text, pinned, primary } / { px, py, pinned } / lists of point pairs.
    Q_PROPERTY(QVariantList labels READ labels NOTIFY overlayChanged)
    Q_PROPERTY(QVariantList points READ points NOTIFY overlayChanged)
    Q_PROPERTY(QVariantList currentPaths READ currentPaths NOTIFY overlayChanged)
    Q_PROPERTY(QVariantList pinnedPaths READ pinnedPaths NOTIFY overlayChanged)
    // labels and points as list models (the overlay's delegates follow the camera in place).
    Q_PROPERTY(QAbstractItemModel* labelsModel READ labelsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* pointsModel READ pointsModel CONSTANT)

public:
    // Any may be null (a bare engine); they must outlive this object, except the view (watched).
    MeasureModel(SceneController* scene, SelectionModel* selection, Session* session, QObject* parent = nullptr);
    ~MeasureModel() override;

    // Made by the QML engine on first use with the engine's Scene, Selection, Session, Commands and Shortcuts.
    static MeasureModel* create(QQmlEngine* qml, QJSEngine* js);

    // ---- units (the Units singleton decides, T-024) ----
    UnitFormat unitFormat() const { return format_; }
    // Where the format comes from (create() passes the engine's Units). Null: setUnitFormat applies directly.
    void setUnitsSource(UnitsObject* units);
    // Switches the display unit system (in Units); with a units source the decimals come from the user settings.
    void setUnitFormat(UnitFormat format);
    // "inch" / "in" or "mm" / "metric"; decimals < 0: the default for the system (only without a units source).
    Q_INVOKABLE void setUnits(const QString& system, int decimals = -1);
    QString unitSystem() const;
    int decimals() const { return format_.decimals; }
    bool metric() const { return format_.system == UnitSystem::Metric; }
    void setMetric(bool on) { setUnits(on ? QStringLiteral("mm") : QStringLiteral("inch")); }
    double density() const { return density_; }
    void setDensity(double gPerCm3);
    QString densityText() const;

    bool busy() const { return job_ != 0; }
    bool hasResult() const { return current_.has_value(); }
    QString title() const;
    QVariantList rows() const;
    QStringList notes() const;
    QString hint() const { return hint_; }
    bool hintIsError() const { return hintIsError_; }
    QVariantList pinned() const;
    int pinnedCount() const { return static_cast<int>(pinned_.size()); }
    bool active() const { return active_; }
    void setActive(bool on);
    QObject* view() const;
    void setView(QObject* view);
    QVariantList labels() const { return labels_; }
    QVariantList points() const { return points_; }
    QVariantList currentPaths() const { return currentPaths_; }
    QVariantList pinnedPaths() const { return pinnedPaths_; }
    QAbstractItemModel* rowsModel() const { return rowsModel_; }
    QAbstractItemModel* labelsModel() const { return labelsModel_; }
    QAbstractItemModel* pointsModel() const { return pointsModel_; }

    // The current measurement in mm / radians (tests, the clipboard); null when there is none.
    const api::measure::Measurement* current() const { return current_ ? &*current_ : nullptr; }
    // The formatted value of `key` in the current measurement ("" if absent).
    Q_INVOKABLE QString valueText(const QString& key) const;

    // ---- actions ----
    // Measures the selection: "auto" (measure.selection), "distance", "angle", "radius", "massProperties" or
    // "boundingBox". Shows the panel (showRequested).
    Q_INVOKABLE void measure(const QString& kind = QStringLiteral("auto"));
    Q_INVOKABLE bool pin();
    Q_INVOKABLE void unpin(int id);
    Q_INVOKABLE void clearPinned();
    Q_INVOKABLE void copyValue(int row);
    Q_INVOKABLE void copyPinnedValue(int id, int row);
    Q_INVOKABLE void copyAll();

    // ---- the command catalog's hooks (measure.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

signals:
    void unitsChanged();
    void densityChanged();
    void densityTextChanged();
    void currentChanged();
    void busyChanged();
    void pinnedChanged();
    void activeChanged();
    void viewChanged();
    void overlayChanged();
    // The Measure panel should be shown (the M key, a measure command).
    void showRequested();
    // Text went to the clipboard.
    void copied(const QString& text);

private:
    struct Pinned {
        int id = 0;
        api::measure::Measurement m;
    };
    struct Request {
        QString kind;
        std::vector<std::pair<int, kernel::SubId>> elements; // (scene body index, element), selection order
        bool wholeScene = false;                             // mass properties / box of every body
    };

    void attachCommands(CommandCatalog* commands, ShortcutManager* shortcuts);
    void selectionChanged();
    void documentChanged();
    void applyFormat(UnitFormat format);
    void start(const QString& kind);
    std::optional<Request> requestFor(const QString& kind, QString* why) const;
    void setHint(const QString& text, bool error);
    void redrawWitness();
    void clearItems(std::vector<render::ItemId>& items);
    void addWitness(const api::measure::Witness& witness, bool pinned, std::vector<render::ItemId>& items);
    void updateOverlay();
    QString format(const api::measure::Value& v) const;
    QVariantList rowsOf(const api::measure::Measurement& m) const;
    QString textOf(const api::measure::Measurement& m) const;
    void toClipboard(const QString& text);

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<ViewportItem> view_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;
    QList<QMetaObject::Connection> viewConnections_;
    UnitFormat format_{UnitSystem::Inch, 4}; // Units' format once a units source is set
    std::string documentId_;
    double density_ = api::measure::kDefaultDensity;
    bool active_ = false;
    bool dirty_ = true; // the selection changed while the panel was hidden
    QString mode_ = QStringLiteral("auto");
    std::uint64_t job_ = 0; // serial of the job in flight, 0 if none
    std::uint64_t nextSerial_ = 1;
    std::optional<api::measure::Measurement> current_;
    QString hint_;
    bool hintIsError_ = false;
    std::vector<Pinned> pinned_;
    int nextPinId_ = 1;
    std::vector<render::ItemId> currentItems_, pinnedItems_;
    QVariantList labels_, points_, currentPaths_, pinnedPaths_;
    MeasureListModel* rowsModel_ = nullptr;
    MeasureListModel* labelsModel_ = nullptr;
    MeasureListModel* pointsModel_ = nullptr;
    bool fromShortcut_ = false;
};

} // namespace ray::ui
