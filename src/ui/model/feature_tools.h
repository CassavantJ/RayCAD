// The `FeatureTool` QML singleton (task T-098): the feature tools of the model environment and their property panel
// (qml/model/FeaturePanel.qml).
//
//   Tools        extrude, revolve, fillet, chamfer, hole and datum_plane. start(type) opens a tool for a new feature,
//                pre-filled from what is selected (a sketch in the feature tree, edges or a planar face in the view);
//                edit(featureId) opens it on an existing feature (the feature tree's double-click). Every tool is a
//                command (model.tool.*, and the registry's feature.<type>.add), so it is in the palette with a key.
//   Fields       values() holds what the panel shows, as text: numeric fields take numbers (in the display unit),
//                numbers with units, expressions and parameter names ("12 mm", "width / 2", "0.5 in"); a plain number
//                gets the display unit (lengths) or degrees (angles). Choices and switches are strings and booleans.
//   Picking      extrude and revolve: clicking a closed region of the sketch in the view toggles it (none picked: all
//                regions); hole: clicking a sketch point toggles it (none: every free point); fillet and chamfer: the
//                edges selected in the view (persistent references through model.reference), picked and unpicked
//                while the preview is shown (the preview never answers picks; the model under it does, preview.h);
//                when editing, the stored edges stay and the selected ones are added; datum plane on a face: the
//                selected planar face.
//   Preview      every change rebuilds the feature off to the side through Document::beginPreview on the job
//                system, never on the UI thread. While the history is rolled back, the add command puts the new
//                feature right after the rollback bar, so the preview is the rolled-back model with it. One preview
//                build runs at a time; changes made meanwhile are merged into the next one, so the newest values
//                always win. The result bodies are meshed on a worker and shown in the scene's preview slot in place
//                of the bodies they change (SceneController::preview()). A command that the document refuses, or a
//                feature that fails to build, shows its message in the panel and the last good preview stays.
//   Handles      handles() lists the in-view drag handles in view coordinates: an arrow for the extrude distance and
//                the hole depth, an angle handle for the revolve, a radius handle for fillet and chamfer.
//                dragHandle() turns a pointer position into the field's value (and a new preview).
//   OK / Cancel  commit() runs feature.<type>.add (or .edit) through the action registry: one undo step. cancel()
//                ends the preview: the document is exactly as it was.
//   After a sketch  when the sketch mode closes on a sketch with closed regions, offerSketch names it and the panel
//                offers Extrude for it.
#pragma once

#include "preview.h"

#include <ray/api.h>
#include <ray_api/feature_edit.h>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class FeatureTreeModel;
class SceneController;
class SelectionModel;
class Session;
class SketchModel;
class UnitsObject;
class ViewportItem;

class FeatureToolModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(FeatureTool)
    QML_SINGLETON

    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // "extrude", "revolve", "fillet", "chamfer", "hole", "datum_plane"; "" when no tool is open.
    Q_PROPERTY(QString type READ type NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title NOTIFY activeChanged)
    Q_PROPERTY(bool editing READ editing NOTIFY activeChanged)
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    // The document's or the feature's message about the current values ("" when they build); messageField is the
    // field it is about ("" when none is named).
    Q_PROPERTY(QString message READ message NOTIFY statusChanged)
    Q_PROPERTY(QString messageField READ messageField NOTIFY statusChanged)
    // "", "building", "ok", "warning", "failed".
    Q_PROPERTY(QString previewStatus READ previewStatus NOTIFY statusChanged)
    Q_PROPERTY(bool committing READ committing NOTIFY statusChanged)
    Q_PROPERTY(bool canCommit READ canCommit NOTIFY statusChanged)
    // Choices: [{id, name}] sketches before the feature, bodies, revolve axes [{key, title}], thread series
    // [{id, name}] and the sizes of the chosen series [{designation, tapDrill}].
    Q_PROPERTY(QVariantList sketches READ sketches NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList bodies READ bodies NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList axes READ axes NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList planes READ planes NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList threadSeries READ threadSeries NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList threadSizes READ threadSizes NOTIFY optionsChanged)
    // What is picked: "2 of 3 regions", "All 4 points", "3 edges"; pickHint says what a click does.
    Q_PROPERTY(QString pickText READ pickText NOTIFY picksChanged)
    Q_PROPERTY(QString pickHint READ pickHint NOTIFY picksChanged)
    Q_PROPERTY(int pickCount READ pickCount NOTIFY picksChanged)
    // Fillet and chamfer: the view's selection feeds the edges (with the preview shown).
    Q_PROPERTY(bool picking READ picking NOTIFY activeChanged)
    // In-view handles: [{key, kind ("arrow" | "angle" | "radius"), x, y, tipX, tipY}] in view coordinates.
    Q_PROPERTY(QVariantList handles READ handles NOTIFY handlesChanged)
    Q_PROPERTY(bool handleDragging READ handleDragging NOTIFY handlesChanged)
    // The sketch just finished in the sketch mode, offered for an extrude: {id, name} or empty.
    Q_PROPERTY(QVariantMap offerSketch READ offerSketch NOTIFY offerChanged)
    Q_PROPERTY(QObject* view READ view WRITE setView NOTIFY viewChanged)

public:
    FeatureToolModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                     QObject* parent = nullptr);
    ~FeatureToolModel() override;
    static FeatureToolModel* create(QQmlEngine* qml, QJSEngine* js);

    void setCommands(CommandCatalog* commands);
    void setUnits(UnitsObject* units);
    void setTree(FeatureTreeModel* tree) { tree_ = tree; }
    void setSketchTool(SketchModel* sketch);

    bool active() const { return !type_.isEmpty(); }
    QString type() const { return type_; }
    QString title() const;
    bool editing() const { return editId_.has_value(); }
    QVariantMap values() const { return values_; }
    QString message() const { return message_; }
    QString messageField() const { return messageField_; }
    QString previewStatus() const { return previewStatus_; }
    bool committing() const { return committing_; }
    bool canCommit() const;
    QVariantList sketches() const;
    QVariantList bodies() const;
    QVariantList axes() const;
    QVariantList planes() const;
    QVariantList threadSeries() const;
    QVariantList threadSizes() const;
    QString pickText() const;
    QString pickHint() const;
    int pickCount() const;
    bool picking() const;
    QVariantList handles() const;
    bool handleDragging() const { return !dragKey_.isEmpty(); }
    QVariantMap offerSketch() const { return offer_; }
    QObject* view() const;
    void setView(QObject* view);

    // ---- QML ----
    Q_INVOKABLE bool start(const QString& type);
    Q_INVOKABLE bool edit(const QString& featureId);
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE bool commit();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearPicks();
    Q_INVOKABLE void acceptOffer(); // Extrude the offered sketch
    Q_INVOKABLE void dismissOffer();
    // A handle dragged to (x, y) in view coordinates; `finished` on release.
    Q_INVOKABLE void dragHandle(const QString& key, qreal x, qreal y, bool finished);
    // A click in the view (item coordinates): toggles a region or a point. True if it picked something.
    Q_INVOKABLE bool pickAt(qreal x, qreal y);

    // ---- the command catalog's hooks (model.tool.*, feature.<type>.add) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    std::optional<FeatureId> featureId() const { return id_; }
    bool previewBusy() const;
    // The scene's preview slot (shared; this tool's preview while it holds it).
    const BodyOverlay& overlay() const;
    int previewBuilds() const { return previewBuilds_; }
    std::optional<kernel::Plane> sketchPlane() const { return plane_; }
    const std::vector<api::feature_edit::Region>& regions() const { return regions_; }
    // The props the panel would commit now (canonical-free JSON object text), or the reason it cannot.
    Result<std::string> propsJson() const;

signals:
    void activeChanged();
    void valuesChanged();
    void statusChanged();
    void optionsChanged();
    void picksChanged();
    void handlesChanged();
    void offerChanged();
    void viewChanged();
    void committed(const QString& featureId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct SketchInfo {
        FeatureId id;
        std::optional<kernel::Plane> plane;
        std::vector<api::feature_edit::Region> regions;
        std::vector<std::pair<SketchEntId, Vec2>> freePoints; // free-standing points and circle / arc centres
        std::set<SketchEntId> centres;
        std::vector<std::pair<SketchEntId, std::pair<Vec2, Vec2>>> lines;
        QString error;
    };

    api::IApp* app() const;
    api::OpenDocument* document() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void onDocumentChanged();
    void open(const QString& type, std::optional<FeatureId> editId);
    void close();
    void defaults();
    void prefillFromSelection();
    void loadFeature(const doc::Feature& feature);
    void loadSketch(); // plane, regions, points, lines of values["sketch"] (on a worker)
    void loadThreads();
    void applyEdges();
    void schedulePreview();
    void startPreview();
    void onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build);
    void showPreview(const doc::DocState& state, const doc::BuildResult& build);
    void showDatumPlane(const doc::DocState& state, const doc::BuildResult& build);
    void setStatus(const QString& status, const QString& message, const QString& field = QString());
    void updateHighlights();
    void updateHandles();
    std::optional<QPointF> toView(Vec3 world) const;
    std::optional<render::Ray> rayAt(QPointF viewPos) const;
    Result<doc::Command> command() const;
    std::optional<std::size_t> insertIndex() const;
    QString lengthText(double mm) const;
    QString exprOf(const QString& key, bool angle) const;
    std::optional<double> numberOf(const QString& key, bool angle) const;
    std::optional<Vec3> handleBase(const QString& key, Vec3* dir) const;
    QString fieldOfMessage(const QString& message) const;

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;
    QPointer<FeatureTreeModel> tree_;
    QPointer<SketchModel> sketchTool_;
    QPointer<ViewportItem> view_;
    QList<QMetaObject::Connection> viewConnections_;
    std::unique_ptr<LineOverlay> lines_;

    // The document observed.
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    int previewListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    // The open tool.
    QString type_;
    std::optional<FeatureId> id_;     // the feature (new: made at start)
    std::optional<FeatureId> editId_; // set when editing
    std::string oldPropsJson_;        // editing: the props it had
    BodyId newBody_;                  // the body an operation "new" creates
    QVariantMap values_;
    bool flipTouched_ = false;
    QString message_, messageField_, previewStatus_;
    bool committing_ = false;
    bool rejected_ = false; // the document refuses the command (not just a feature that fails to build)
    std::uint8_t savedFilter_ = 0;
    bool filterSaved_ = false;

    // Picks.
    std::vector<std::vector<SketchEntId>> regionPicks_;
    std::vector<SketchEntId> pointPicks_;
    QJsonArray edges_;             // TopoRef JSON objects: storedEdges_, then the selected ones
    QJsonArray storedEdges_;       // editing: the edges the feature has (they may not exist in the model shown)
    std::vector<Vec3> edgePoints_; // where each selected edge was picked (for the radius handle)
    QJsonObject faceRef_;          // datum plane on a face
    std::uint64_t edgeSerial_ = 0;

    // The chosen sketch.
    std::optional<kernel::Plane> plane_;
    std::vector<api::feature_edit::Region> regions_;
    std::vector<std::pair<SketchEntId, Vec2>> freePoints_;
    std::set<SketchEntId> centres_;
    std::vector<std::pair<SketchEntId, std::pair<Vec2, Vec2>>> sketchLines_;
    std::uint64_t sketchSerial_ = 0;

    // Threads (standards.threads.list), read once.
    QJsonArray threadTable_;
    bool threadsLoading_ = false;

    // Preview.
    QTimer previewTimer_;
    bool previewBusy_ = false;
    bool previewPending_ = false;
    doc::DocStatePtr previewBase_; // the state the last preview was built on
    std::uint64_t previewSerial_ = 0;
    int previewBuilds_ = 0;

    // Handles and clicks in the view.
    QString dragKey_;
    QPointF pressAt_;
    bool pressed_ = false;

    // Offer after a sketch.
    QVariantMap offer_;
    std::optional<FeatureId> lastSketch_;
    std::optional<FeatureId> preferredSketch_; // the sketch to pre-fill (from the offer)
};

} // namespace ray::ui
