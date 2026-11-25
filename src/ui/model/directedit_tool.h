// The `DirectEdit` QML singleton (task T-083): the direct-edit tools of the Design workspace on any body, an imported
// customer part as much as a modelled one, and their panel (qml/model/DirectEditPanel.qml) and drag arrow
// (qml/model/DirectEditLayer.qml).
//
//   Tools        Delete faces (feature delete_face), Push/Pull (offset_face) and Resize hole (resize_hole), each a
//                command (directEdit.*: in the palette, Delete for Delete faces and Shift+P for Push/Pull in the Design
//                workspace while no sketch is edited; never while a text field has the focus) fronting the registry's
//                feature.<type>.add. start(tool) opens the tool on the faces selected in the view; while it is open the
//                selection filter picks faces only and every click changes the faces (Shift adds, Ctrl toggles), with
//                the preview shown. One tool at a time, and never beside an open feature tool or sketch.
//   Faces        the selected faces become persistent references through model.reference (the model the view
//                shows). Push/pull takes flat faces of one body (a curved one is refused by the kernel, in its words);
//                resize the first face (any face of the hole's bore stands for the hole).
//   Blend runs   Delete faces: when a picked face is a fillet that joins other blends (model.blendRun), the panel
//   offers
//                "Select the whole blend run" (selectBlendRun), which adds the run's faces to the selection.
//   Values       Push/Pull: "distance", signed (positive adds material), a number in the display unit, a number with
//                a unit, an expression or a parameter, as every dimension field takes; the arrow drag sets it in
//                steps (directedit_logic.h). Resize: "diameter", the hole's present diameter shown at the start; it
//                previews once it is changed.
//   Preview      every change rebuilds the feature off to the side (Document::beginPreview, on the job system; one
//                build at a time, changes meanwhile merged into the next, the newest values win), shown in the scene's
//                preview slot in place of the body it changes. The kernel's warning (push/pull's step, a counterbore
//                left in place) is shown in amber before OK; a refusal shows its message in red and OK stays blocked:
//                the document is never changed by a refused edit. A slow preview (a fillet removal can take
//                seconds) shows how long it has run; Esc (cancel) ends it at once (Document::cancelPreview: the
//                kernel abandons its work).
//   OK / Cancel  commit() runs feature.<type>.add through the action registry: one undo step. cancel() leaves the
//                document exactly as it was.
// Kernel work never runs on the UI thread: references, blend runs, the hole's diameter and the preview are jobs.
#pragma once

#include "directedit_logic.h"
#include "preview.h"

#include <ray/api.h>

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class FeatureToolModel;
class SceneController;
class SelectionModel;
class Session;
class SketchModel;
class UnitsObject;
class ViewportItem;

class DirectEditModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(DirectEdit)
    QML_SINGLETON

    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // "delete", "pushPull", "resize"; "" when none is open.
    Q_PROPERTY(QString tool READ tool NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title NOTIFY activeChanged)
    // Editing an existing push/pull or hole resize (the feature tree's double-click): its value only, its faces kept.
    Q_PROPERTY(bool editing READ editing NOTIFY activeChanged)
    // The fields as text: "distance" (push/pull), "diameter" (resize).
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    // The document's or the kernel's message about the current picks and values ("" when they build without one).
    Q_PROPERTY(QString message READ message NOTIFY statusChanged)
    // "", "building", "ok", "warning", "failed", "committing" (directedit::phaseKey).
    Q_PROPERTY(QString previewStatus READ previewStatus NOTIFY statusChanged)
    Q_PROPERTY(bool canCommit READ canCommit NOTIFY statusChanged)
    // What the panel says besides the message: "The hole is 6 mm across now", "Pick faces in the view".
    Q_PROPERTY(QString hint READ hint NOTIFY statusChanged)
    // Whole seconds the preview has been building (0 when it is not, or just started).
    Q_PROPERTY(int busySeconds READ busySeconds NOTIFY busyChanged)
    Q_PROPERTY(int pickCount READ pickCount NOTIFY picksChanged)
    Q_PROPERTY(QString pickText READ pickText NOTIFY picksChanged)
    Q_PROPERTY(QString pickHint READ pickHint NOTIFY activeChanged)
    // Delete faces: the faces of the blend run a picked fillet face belongs to, when more than are picked; 0 otherwise.
    Q_PROPERTY(int blendRunCount READ blendRunCount NOTIFY offerChanged)
    // Push/pull: the arrow, [{key: "distance", x, y, tipX, tipY}] in view coordinates (none until a face is picked).
    Q_PROPERTY(QVariantList handles READ handles NOTIFY handlesChanged)
    Q_PROPERTY(bool handleDragging READ handleDragging NOTIFY handlesChanged)
    Q_PROPERTY(QObject* view READ view WRITE setView NOTIFY viewChanged)

public:
    DirectEditModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                    QObject* parent = nullptr);
    ~DirectEditModel() override;
    static DirectEditModel* create(QQmlEngine* qml, QJSEngine* js);

    void setCommands(CommandCatalog* commands);
    void setUnits(UnitsObject* units);
    void setFeatureTool(FeatureToolModel* tool);
    void setSketchTool(SketchModel* sketch);

    bool active() const { return flow_.active(); }
    QString tool() const { return directedit::toolKey(tool_); }
    QString title() const;
    bool editing() const { return editId_.has_value(); }
    QVariantMap values() const { return values_; }
    QString message() const { return message_; }
    QString previewStatus() const { return directedit::phaseKey(flow_.phase()); }
    bool canCommit() const;
    QString hint() const { return hint_; }
    int busySeconds() const { return busySeconds_; }
    int pickCount() const { return static_cast<int>(picks_.size()); }
    QString pickText() const;
    QString pickHint() const;
    int blendRunCount() const { return offerRun_.size() > 1 ? static_cast<int>(offerRun_.size()) : 0; }
    QVariantList handles() const;
    bool handleDragging() const { return !dragKey_.isEmpty(); }
    QObject* view() const;
    void setView(QObject* view);

    // ---- QML ----
    Q_INVOKABLE bool start(const QString& tool);
    // Opens an existing push/pull or hole resize to change its distance or diameter (OK: feature.<type>.edit).
    Q_INVOKABLE bool edit(const QString& featureId);
    Q_INVOKABLE void setValue(const QString& key, const QString& text);
    Q_INVOKABLE bool commit();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void selectBlendRun();
    // The arrow dragged to (x, y) in view coordinates; `finished` on release; `fine` (Shift) for the small step.
    Q_INVOKABLE void dragHandle(const QString& key, qreal x, qreal y, bool finished, bool fine);

    // ---- the command catalog's hooks (directEdit.*, feature.<type>.add of the three types) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    directedit::Phase phase() const { return flow_.phase(); }
    directedit::Tool toolKind() const { return tool_; }
    // Busy until what the panel shows is final: references on their way, a preview scheduled, building or meshing.
    bool previewBusy() const;
    int previewBuilds() const { return previewBuilds_; }
    const BodyOverlay& overlay() const;
    std::optional<FeatureId> featureId() const { return id_; }
    // The face references picked (TopoRef JSON objects), in pick order.
    QJsonArray faceRefs() const;
    // The props the panel would commit now, or why it cannot.
    Result<std::string> propsJson() const;
    // Push/pull: the arrow's base and outward direction (from the display mesh of the first face), when known.
    std::optional<std::pair<Vec3, Vec3>> arrow() const;
    // Resize: the picked hole's diameter (mm) when its face is a cylinder.
    std::optional<double> currentDiameter() const { return currentDiameter_; }

signals:
    void activeChanged();
    void valuesChanged();
    void statusChanged();
    void picksChanged();
    void offerChanged();
    void handlesChanged();
    void viewChanged();
    void busyChanged();
    // A direct edit was added (its feature id): the reference review looks at what it left.
    void committed(const QString& featureId);

private:
    struct Pick {
        BodyId body;
        std::uint32_t index = 0;
        Vec3 point{};
        QJsonObject ref; // empty until model.reference answered
    };

    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void onDocumentChanged();
    void open(directedit::Tool tool, std::optional<FeatureId> editId = std::nullopt);
    void close();
    void applySelection(); // the view's selection becomes the picks
    void refsArrived(std::uint64_t serial);
    void updateBlendOffer();
    void loadDiameter();
    void updateArrow();
    void schedulePreview();
    void startPreview();
    void onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build);
    void showPreview(const doc::DocState& state, const doc::BuildResult& build);
    void setMessage(const QString& message);
    void setBusy(bool busy);
    Result<doc::Command> command() const;
    QString exprOf(const QString& key) const;
    std::optional<double> numberOf(const QString& key) const;
    bool inch() const;
    int sceneIndexOf(const BodyId& body) const;
    std::optional<QPointF> toView(Vec3 world) const;
    std::optional<render::Ray> rayAt(QPointF viewPos) const;

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;
    QPointer<FeatureToolModel> featureTool_;
    QPointer<SketchModel> sketchTool_;
    QPointer<ViewportItem> view_;
    QList<QMetaObject::Connection> viewConnections_;

    // The document observed.
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    int previewListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    // The open tool.
    directedit::Tool tool_ = directedit::Tool::None;
    directedit::Flow flow_;
    std::optional<FeatureId> id_;
    std::optional<FeatureId> editId_; // editing this feature (its faces are kept: editRefs_)
    QJsonArray editRefs_;
    QString editName_;
    QVariantMap values_;
    QString message_, hint_;
    bool diameterTouched_ = false;
    std::uint8_t savedFilter_ = 0;
    bool filterSaved_ = false;

    // Picks, their references (model.reference, cached per element of the build shown).
    std::vector<Pick> picks_;
    std::map<std::pair<BodyId, std::uint32_t>, QJsonObject> refCache_;
    const void* refCacheFor_ = nullptr; // the scene snapshot the cache is for
    std::uint64_t pickSerial_ = 0;
    int refsPending_ = 0;

    // Delete faces: the blend run offered.
    std::vector<std::uint32_t> offerRun_;
    BodyId offerBody_;
    std::uint64_t offerSerial_ = 0;
    std::map<std::pair<BodyId, std::uint32_t>, std::vector<std::uint32_t>> runCache_;

    // Resize: the picked hole's diameter.
    std::optional<double> currentDiameter_;
    std::uint64_t diameterSerial_ = 0;

    // Push/pull: the arrow.
    std::optional<Vec3> arrowBase_, arrowDir_;
    QString dragKey_;
    double dragT0_ = 0, dragV0_ = 0;

    // Preview.
    QTimer previewTimer_;
    bool previewBusy_ = false;        // a beginPreview build is on its way
    bool previewPending_ = false;     // changes came in meanwhile
    std::uint64_t previewSerial_ = 0; // the flow serial of the build on its way
    doc::DocStatePtr previewBase_;
    int previewBuilds_ = 0;
    QElapsedTimer busyClock_;
    QTimer busyTick_;
    int busySeconds_ = 0;
    bool committing_ = false;
};

} // namespace ray::ui
