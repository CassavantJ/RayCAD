// The `CamSetup` QML singleton (task T-044): the setup panel of the CAM workspace (qml/cam/SetupPanel.qml), creating
// and editing a setup with a live preview.
//
//   Open         start() opens the panel for a new setup (the New setup command, cam.tool.newSetup); edit(setupId)
//                on an existing one (the CAM tree's double-click). The fastest path is four interactions on an
//                imported part: New setup, click the top face, click an origin marker on the previewed stock, OK (the
//                default stock is accepted as it is: a box with no offsets around the part bodies).
//   Fields       values() holds what the panel shows. Lengths are text in the display unit, numbers with units,
//                expressions and parameter names, as the feature panels take them; other fields are strings, numbers
//                and booleans. Part bodies default to every built body; fixture bodies are optional (setBodyRole).
//                The machine defaults to the last used one (this session's last setup, else the document's last
//                setup, else the first of the library), the post to the machine's default.
//   Picking      pickMode: "face" (the Z face: planar faces only), "edge" (the X edge: straight edges only), "vertex"
//                (the origin vertex of a model point origin) or "". A click in the view selects as usual (Selection,
//                with its filter set to the kind picked); the selected element is checked on a worker and taken
//                through model.reference, or refused with the reason in the status line. A new setup starts in
//                "face". The origin markers on the previewed stock (corners 0..7, top centre; cam.h numbering) are
//                clicked to choose the origin (pickOrigin), as is the origin control.
//   Preview      every change previews the setup command through Document::beginPreview (the command is checked as
//                OK would run it; it never enters the document), then resolves it on the job system
//                (api::cam_setup::previewSetup, cancelling a superseded resolve): the stock is shown translucent in the
//                scene's one preview slot (BodyOverlay) and the WCS triad, in the axis colours with X, Y and Z labels,
//                at the origin (drawTriad: line items now, the renderer's line primitive behind one function). A
//                failure shows in the status line and the last good preview stays.
//   OK / Cancel  commit() runs cam.setup.add or cam.setup.edit through the action registry as Caller::User (one undo
//                step; the user's own click needs no confirmation, the API's confirm hook is for automation only).
//                cancel() (Esc) ends the preview: the document is exactly as it was. Leaving the CAM workspace or
//                closing the document cancels too.
#pragma once

#include "../model/preview.h"
#include "setup_logic.h"

#include <ray/api.h>
#include <ray_api/cam_setup.h>

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

namespace ray::ui {

class AppContext;
class CommandCatalog;
class SceneController;
class SelectionModel;
class Session;
class UnitsObject;
class ViewportItem;

class SetupToolModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamSetup)
    QML_SINGLETON

    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool editing READ editing NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title NOTIFY activeChanged)
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    // The message about the current values or the last pick ("" when there is none); messageField is the field it is
    // about ("" when none is named).
    Q_PROPERTY(QString message READ message NOTIFY statusChanged)
    Q_PROPERTY(QString messageField READ messageField NOTIFY statusChanged)
    // "", "building", "ok", "failed".
    Q_PROPERTY(QString previewStatus READ previewStatus NOTIFY statusChanged)
    Q_PROPERTY(bool committing READ committing NOTIFY statusChanged)
    Q_PROPERTY(bool canCommit READ canCommit NOTIFY statusChanged)
    // "face", "edge", "vertex" or "".
    Q_PROPERTY(QString pickMode READ pickMode NOTIFY picksChanged)
    // What is picked, as text ("Face of Part 1", "None: model +Z", ...).
    Q_PROPERTY(QString zFaceText READ zFaceText NOTIFY picksChanged)
    Q_PROPERTY(QString xEdgeText READ xEdgeText NOTIFY picksChanged)
    Q_PROPERTY(QString vertexText READ vertexText NOTIFY picksChanged)
    Q_PROPERTY(bool hasZFace READ hasZFace NOTIFY picksChanged)
    Q_PROPERTY(bool hasXEdge READ hasXEdge NOTIFY picksChanged)
    Q_PROPERTY(bool hasVertex READ hasVertex NOTIFY picksChanged)
    // [{id, name, role ("part" | "fixture" | "none")}] every built body; [{id, name}] the machines; posts by stem.
    Q_PROPERTY(QVariantList bodies READ bodies NOTIFY optionsChanged)
    Q_PROPERTY(QVariantList machines READ machines NOTIFY optionsChanged)
    Q_PROPERTY(QStringList posts READ posts NOTIFY optionsChanged)
    // In-view items, in view coordinates: the origin markers [{key, x, y, current}] and the triad labels
    // [{axis ("X" | "Y" | "Z"), x, y}]. Empty while nothing is previewed.
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
    Q_PROPERTY(QVariantList triadLabels READ triadLabels NOTIFY markersChanged)
    Q_PROPERTY(QObject* view READ view WRITE setView NOTIFY viewChanged)

public:
    SetupToolModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                   QObject* parent = nullptr);
    ~SetupToolModel() override;
    static SetupToolModel* create(QQmlEngine* qml, QJSEngine* js);

    void setCommands(CommandCatalog* commands);
    void setUnits(UnitsObject* units);

    bool active() const { return id_.has_value(); }
    bool editing() const { return editing_; }
    QString title() const;
    QVariantMap values() const { return values_; }
    QString message() const { return message_; }
    QString messageField() const { return messageField_; }
    QString previewStatus() const { return previewStatus_; }
    bool committing() const { return committing_; }
    bool canCommit() const;
    QString pickMode() const { return pickMode_; }
    QString zFaceText() const;
    QString xEdgeText() const;
    QString vertexText() const;
    bool hasZFace() const { return !zFace_.isEmpty(); }
    bool hasXEdge() const { return !xEdge_.isEmpty(); }
    bool hasVertex() const { return !vertex_.isEmpty(); }
    QVariantList bodies() const;
    QVariantList machines() const { return machines_; }
    QStringList posts() const { return posts_; }
    QVariantList markers() const;
    QVariantList triadLabels() const;
    QObject* view() const;
    void setView(QObject* view);

    // ---- QML ----
    Q_INVOKABLE bool start();
    Q_INVOKABLE bool edit(const QString& setupId);
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void setBodyRole(const QString& bodyId, const QString& role);
    Q_INVOKABLE void setPickMode(const QString& mode);
    // "face", "edge", "vertex": forgets that pick (the Z face goes back to model +Z, the X edge to its rule).
    Q_INVOKABLE void clearPick(const QString& which);
    // An origin marker clicked (corner0..corner7, topCenter).
    Q_INVOKABLE void pickOrigin(const QString& key);
    Q_INVOKABLE bool commit();
    Q_INVOKABLE void cancel();

    // ---- the command catalog's hooks (cam.tool.*, cam.setup.add) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    std::optional<SetupId> setupId() const { return id_; }
    // Until what the panel shows is final: a pick being checked, a preview scheduled, building, resolving or meshing.
    bool previewBusy() const;
    int previewsShown() const { return previewsShown_; }
    int resolvesCancelled() const { return resolvesCancelled_; }
    const std::optional<api::cam_setup::Preview>& lastPreview() const { return preview_; }
    const LineOverlay& triadLines() const { return *lines_; }
    // The arguments OK would run now, or why it cannot.
    camui::ArgsResult arguments() const;

signals:
    void activeChanged();
    void valuesChanged();
    void statusChanged();
    void picksChanged();
    void optionsChanged();
    void markersChanged();
    void viewChanged();
    void committed(const QString& setupId);

private:
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void onDocumentChanged();
    void open(const std::optional<SetupId>& existing);
    void close();
    void defaults(const doc::DocState& state);
    void loadSetup(const doc::DocState& state, const cam::Setup& setup);
    void loadOptions();
    void applyPickMode();
    void onSelectionChanged();
    void takePick(kernel::SubKind kind, int body, kernel::SubId sub);
    void setStatus(const QString& status, const QString& message, const QString& field = QString());
    void schedulePreview();
    void startPreview();
    void onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build);
    void showPreview(const api::cam_setup::Preview& preview);
    void clearPreview();
    void drawTriad(const api::cam_setup::Preview& preview);
    std::optional<QPointF> toView(Vec3 world) const;
    QString exprOf(const QString& text) const;
    QString bodyName(const QString& id) const;
    camui::SetupInput input() const;

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;
    QPointer<ViewportItem> view_;
    QList<QMetaObject::Connection> viewConnections_;
    std::unique_ptr<LineOverlay> lines_;

    // The document observed.
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    int previewListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    // The open panel.
    std::optional<SetupId> id_;
    bool editing_ = false;
    QVariantMap values_;
    std::map<QString, QString> roles_;           // body id -> "part" | "fixture" | "none"
    QJsonObject zFace_, xEdge_, vertex_;         // TopoRef JSON
    QString zFaceBody_, xEdgeBody_, vertexBody_; // the body each is on (for the text)
    QString pickMode_;
    QString message_, messageField_, previewStatus_;
    bool committing_ = false;
    bool rejected_ = false; // the document refuses the command
    std::uint8_t savedFilter_ = 0;
    bool filterSaved_ = false;
    std::uint64_t pickSerial_ = 0;
    bool pickPending_ = false;

    // Options.
    QVariantList machines_;                      // [{id, name}]
    std::map<QString, std::string> machineJson_; // library machines by id (library file format)
    std::map<QString, QString> machinePost_;     // default post by machine id
    QStringList posts_;
    std::uint64_t postsSerial_ = 0;

    // Preview.
    QTimer previewTimer_;
    bool previewBusy_ = false;    // a document preview is building
    bool previewPending_ = false; // values changed while it built
    doc::DocStatePtr previewBase_;
    std::uint64_t previewSerial_ = 0;
    JobHandle<std::shared_ptr<const api::cam_setup::Preview>> resolveJob_;
    std::optional<api::cam_setup::Preview> preview_;
    int previewsShown_ = 0;
    int resolvesCancelled_ = 0;

    // This session's last machine (the default of the next new setup).
    QString lastMachine_;
};

} // namespace ray::ui
