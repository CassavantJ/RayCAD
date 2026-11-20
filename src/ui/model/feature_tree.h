// The `FeatureTree` QML singleton (task T-098): the document's features for the model tree (qml/panels/ModelTree.qml),
// the actions on them, the rollback bar, and the sketches shown in the 3D view outside the sketch mode.
//
//   Rows        one per feature in history order: name, type (with its icon), build status and message. A Failed
//               feature carries the rebuild's message; a NotBuilt one names the earlier feature it waits on.
//   Actions     rename (feature.edit), suppress (feature.suppress), delete (feature.remove), edit (the tool's panel
//               on the feature, FeatureTool.edit, or the sketch mode for a sketch), each one action and one undo step.
//               moveTo (feature.reorder) is checked first: the reorder is applied to a copy of the history and every
//               moved-past feature is checked as its add command checks it (the features and bodies it names must come
//               before it); then the reorder is rebuilt off to the side (Document::beginPreview) and refused with the
//               message of the first feature it would break. Only an order that builds as before is executed.
//   Rollback    the document's rollback bar (DocState::rollback): rollTo(i) moves it after feature i through the
//               feature.rollback action (one undo step, saved with the document), and the rebuild then builds the
//               history only up to i, so the scene shows the model as it was there. Later features are greyed
//               ("Rolled back") and new features go in right after the bar, which moves past them (the add
//               commands do this). -1 is the end of the history (roll forward).
//   Sketches    every visible sketch is drawn in the view in the sketch colours (construction dashed). A sketch is
//               visible unless a later feature uses it (an extrude, revolve or hole) or it was hidden here; the
//               sketch a tool works on is always shown. The sketch being edited in the sketch mode is left to it.
//   Datums      every datum plane built (not suppressed, not past the rollback bar) is drawn in the view as a square
//               frame sized to the model, a dashed inner frame and a short normal, unless hidden with its eye in the
//               tree. They are guides: they never take a viewport pick. Where a plane is asked for (the sketch mode's
//               plane picker, the datum plane tool's base) they are drawn in the accent colour and a click on one
//               (setView's view) picks it: the sketch goes on it, or the new plane is offset from it. The datum the
//               datum plane tool edits is left to its preview.
#pragma once

#include "preview.h"

#include <ray/api.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace ray::ui {

class AppContext;
class FeatureToolModel;
class SceneController;
class Session;
class SketchModel;

class FeatureTreeModel : public QAbstractListModel {
    Q_OBJECT
    QML_NAMED_ELEMENT(FeatureTree)
    QML_SINGLETON

    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
    // The last feature built (the document's rollback bar), or -1 for the whole history. Writing it runs rollTo.
    Q_PROPERTY(int rollbackIndex READ rollbackIndex WRITE setRollback NOTIFY rollbackChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentChanged)
    // Why the last tree action was refused (a blocked reorder, a failed rename, ...); "" when none.
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    // A reorder is being checked (its side rebuild runs).
    Q_PROPERTY(bool checking READ checking NOTIFY messageChanged)
    // The ViewportItem: clicks on datum planes where a plane is asked for.
    Q_PROPERTY(QObject* view READ view WRITE setView NOTIFY viewChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        TypeTitleRole,
        IconRole,
        StatusRole,     // "ok", "warning", "failed", "suppressed", "notBuilt", "building", "rolledBack"
        StatusTextRole, // short: "Failed", "Waits on Extrude 1", ...
        MessageRole,    // the rebuild's message (failed, warning) or what it waits on
        SuppressedRole,
        RolledBackRole, // after the rollback bar
        IsSketchRole,
        SketchShownRole,
        IsDatumRole,
        DatumShownRole,
    };

    FeatureTreeModel(SceneController* scene, Session* session, AppContext* context, QObject* parent = nullptr);
    ~FeatureTreeModel() override;
    static FeatureTreeModel* create(QQmlEngine* qml, QJSEngine* js);
    void setSketchTool(SketchModel* sketch);
    void setFeatureTool(FeatureToolModel* tool);
    QObject* view() const;
    void setView(QObject* view);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(rows_.size()); }
    int revision() const { return revision_; }
    int rollbackIndex() const { return rollback_; }
    void setRollback(int index) { rollTo(index); }
    int currentIndex() const { return current_; }
    void setCurrentIndex(int index);
    QString message() const { return message_; }
    bool checking() const { return checking_; }

    // ---- QML ----
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int rowOf(const QString& featureId) const;
    Q_INVOKABLE bool rename(int row, const QString& name);
    Q_INVOKABLE bool setSuppressed(int row, bool suppressed);
    Q_INVOKABLE bool remove(int row);
    Q_INVOKABLE bool edit(int row);
    // Moves row `from` to index `to` of the resulting history, after the checks above. False when refused at once
    // (message() says why); true when the check started (the move happens if it passes).
    Q_INVOKABLE bool moveTo(int from, int to);
    // Why moving `from` to `to` is refused, from the quick checks only ("" when they pass).
    Q_INVOKABLE QString moveProblem(int from, int to) const;
    Q_INVOKABLE void setSketchShown(int row, bool shown);
    Q_INVOKABLE void setDatumShown(int row, bool shown);
    // A click in the view (item coordinates) where a plane is asked for: picks the datum plane there, if one is drawn
    // there. True if it picked one.
    Q_INVOKABLE bool pickDatumAt(qreal x, qreal y);
    // Moves the rollback bar after row `index` (-1 or the last row: the whole history) as one undo step. False when
    // nothing was started (it is there already, or the document cannot change; message() says why).
    Q_INVOKABLE bool rollTo(int index);
    Q_INVOKABLE void dismissMessage();

    // ---- C++ ----
    std::optional<FeatureId> currentFeatureId() const;
    std::optional<FeatureId> featureAt(int row) const;
    // The sketch a tool works on: always drawn (nullopt: none).
    void setToolSketch(std::optional<FeatureId> sketch);
    const LineOverlay& sketchLines() const { return *sketchLines_; }
    bool sketchShown(const FeatureId& id) const;
    // Datum planes in the view (keys "datum:<id>:frame", ":inner", ":normal").
    const LineOverlay& datumLines() const { return *datumLines_; }
    bool datumShown(const FeatureId& id) const;
    // A plane is asked for: the sketch mode's plane picker or the datum plane tool is open.
    bool planeWanted() const;
    // The drawn datum plane a ray meets first inside its frame.
    std::optional<FeatureId> datumAt(const render::Ray& ray) const;
    // Sketch-line and datum jobs still running (tests).
    bool busy() const { return sketchJob_.valid() || datumJob_.valid(); }

signals:
    void changed();
    void rollbackChanged();
    void currentChanged();
    void messageChanged();
    // The Rename Feature command (F2): the tree opens the name of `row` for editing.
    void renameRequested(int row);
    // The Parameters command: the model panel shows the parameter table.
    void parametersRequested();
    void viewChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Row {
        FeatureId id;
        QString name, type, status, statusText, message;
        bool suppressed = false;
        bool sketch = false;
        bool datum = false;
    };
    api::IApp* app() const;
    api::OpenDocument* document() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void sync();
    void setMessage(const QString& message);
    bool run(const QString& actionId, const std::string& argsJson, const QString& label);
    void updateSketchLines();
    void updateDatumLines();
    void drawDatums(); // datums_ into datumLines_, in the colour of planeWanted()
    void finishReorderCheck(doc::DocStatePtr state, doc::BuildResultPtr build);
    QString quickMoveProblem(const doc::DocState& state, int from, int to) const;

    QPointer<SceneController> scene_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<SketchModel> sketchTool_;
    QPointer<FeatureToolModel> tool_;
    QQmlEngine* engine_ = nullptr;
    std::unique_ptr<LineOverlay> sketchLines_;
    std::unique_ptr<LineOverlay> datumLines_;
    QPointer<QQuickItem> view_;
    QPointF pressAt_;
    bool pressed_ = false;

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    int previewListener_ = 0;
    std::uint64_t docGeneration_ = 0;
    doc::DocStatePtr shownState_;
    doc::BuildResultPtr shownBuild_;

    std::vector<Row> rows_;
    int revision_ = 0;
    int rollback_ = -1;
    int current_ = -1;
    QString message_;

    // Reorder check.
    bool checking_ = false;
    FeatureId checkId_;
    std::size_t checkTo_ = 0;
    std::string checkArgs_;
    std::map<FeatureId, doc::FeatureStatus> checkBefore_;

    // Sketch lines.
    std::map<FeatureId, bool> sketchOverride_; // user's show / hide
    std::optional<FeatureId> toolSketch_;
    std::optional<FeatureId> editedSketch_; // open in the sketch mode
    struct SketchLines {
        FeatureId id;
        std::vector<Vec3> geometry, construction, points; // segments in world coordinates
    };
    JobHandle<std::shared_ptr<const std::vector<SketchLines>>> sketchJob_;
    std::uint64_t sketchSerial_ = 0;

    // Datum planes.
    std::map<FeatureId, bool> datumOverride_; // user's show / hide (shown by default)
    struct DatumPlane {
        FeatureId id;
        kernel::Plane plane;
    };
    std::vector<DatumPlane> datums_; // the ones drawn
    double datumHalf_ = 25;          // half the frame's side (mm), from the model's size
    JobHandle<std::shared_ptr<const std::vector<DatumPlane>>> datumJob_;
    std::uint64_t datumSerial_ = 0;
};

} // namespace ray::ui
