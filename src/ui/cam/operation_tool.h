// The `CamOperation` QML singleton (task T-045): the operation panel of the CAM workspace (qml/cam/OperationPanel.qml),
// creating and editing an operation of any registered type with a live toolpath preview. Nothing here knows a type:
// the type's schema (IOperationType::paramsSchemaJson) gives the parameter form (SchemaFormModel, operation_form.h)
// and, through its annotations, what the other sections show (docs/operation_types.md).
//
//   Open         start() / startFor(setup) a new operation (the type chosen from the registry's list; the CAM
//                tree's "New operation" on a setup row, the cam.opTool.new command); edit(op) an existing one (the
//                tree's double-click, operationEditRequested).
//   Sections     Tool (a searchable list: the document's tools first, then the types x-toolTypes offers, every tool
//                behind "Show every tool"; holder, stick-out, tool number); Geometry (x-geometry: hole picks a hole by
//                any of its faces, face and edge pick those; a list with remove; a wrong pick is refused with the
//                reason in the status line); Heights (x-heightsUsed); Parameters (the schema form); Feeds and speeds
//                (x-cuttingUsed; computeFeeds for the tool, the setup's material and machine, with the limit that
//                applied; "Use calculated" writes the values and records them, Operation::calculated; a typed value is
//                never replaced without that click; when the tool or material changes and the values are still the
//                calculated ones, an update is offered, not made).
//   Numbers      as the feature panels take them: numbers in the display unit, numbers with units, expressions and
//                parameter names (api::cam_operation::evaluate); feeds in mm/min or in/min.
//   Preview      every change previews the operation command through Document::beginPreview (completed as the action
//                completes it, api::cam_operation::completeCommand), then, on the preview's build, checks it
//                (ICamEngine::validate: errors block OK, warnings do not) and generates its toolpath
//                (ICamEngine::preview) on workers; a superseded check or generation is cancelled, and Esc or Cancel
//                cancels them. The toolpath's backplot is built on a worker and shown in the operation's place
//                (ToolpathDisplay); the generation's progress is shown.
//   OK / Cancel  commit() runs cam.operation.add / edit as Caller::User (one undo step, no confirmation for the
//                user's own click), then asks the engine for the committed operation's toolpath (found at once in the
//                cache when the preview made it). cancel() (Esc) ends the preview: the document is as it was.
#pragma once

#include "operation_form.h"
#include "operation_logic.h"

#include <ray/api.h>
#include <ray_api/cam_operation.h>

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class SceneController;
class SelectionModel;
class Session;
class SetupTreeModel;
class SetupToolModel;
class ToolpathDisplay;
class UnitsObject;

class OperationToolModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamOperation)
    QML_SINGLETON

    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool editing READ editing NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title NOTIFY activeChanged)
    Q_PROPERTY(QString operationId READ operationId NOTIFY activeChanged)
    // The registered types [{name, title}] and the one chosen.
    Q_PROPERTY(QVariantList types READ types NOTIFY activeChanged)
    Q_PROPERTY(QString type READ type NOTIFY typeChanged)
    Q_PROPERTY(QString typeTitle READ typeTitle NOTIFY typeChanged)
    Q_PROPERTY(SchemaFormModel* form READ form CONSTANT)
    // Field text by key: name, clearance, retract, feedPlane, top, bottom, rpm, feedCut, feedPlunge, feedRamp,
    // feedLeadIn, feedLeadOut (text), coolant (string), clockwise (bool), stickOut, toolNumber (text).
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    // Messages beside those fields, by key.
    Q_PROPERTY(QVariantMap fieldMessages READ fieldMessages NOTIFY statusChanged)
    // The heights the type uses [{key, label, hint}] and the cutting members it uses.
    Q_PROPERTY(QVariantList heights READ heights NOTIFY typeChanged)
    Q_PROPERTY(QStringList cutting READ cutting NOTIFY typeChanged)
    // Tool: the chosen one ("" none), its text, the list as filtered [{id, name, detail, embedded, suits}].
    Q_PROPERTY(QString toolId READ toolId NOTIFY toolChanged)
    Q_PROPERTY(QString toolText READ toolText NOTIFY toolChanged)
    Q_PROPERTY(QVariantList tools READ tools NOTIFY toolsChanged)
    Q_PROPERTY(QString toolSearch READ toolSearch WRITE setToolSearch NOTIFY toolsChanged)
    Q_PROPERTY(bool showAllTools READ showAllTools WRITE setShowAllTools NOTIFY toolsChanged)
    Q_PROPERTY(QVariantList holders READ holders NOTIFY toolsChanged) // [{id, name}], "" first (none)
    Q_PROPERTY(QString holderId READ holderId NOTIFY toolChanged)
    // Geometry: x-geometry's kind ("hole", "face", "edge", "none") and prompt, the picks [{index, text}], picking.
    Q_PROPERTY(QString geometryKind READ geometryKind NOTIFY typeChanged)
    Q_PROPERTY(QString geometryPrompt READ geometryPrompt NOTIFY typeChanged)
    Q_PROPERTY(QVariantList picks READ picks NOTIFY picksChanged)
    Q_PROPERTY(bool picking READ picking NOTIFY picksChanged)
    // Feeds: the materials [{id, name}], the one used, where it comes from ("setup", "document", "chosen", "").
    Q_PROPERTY(QVariantList materials READ materials NOTIFY toolsChanged)
    Q_PROPERTY(QString materialId READ materialId NOTIFY feedsChanged)
    Q_PROPERTY(QString materialSource READ materialSource NOTIFY feedsChanged)
    // computeFeeds: {available, error, rpm, feed, chipLoad, surfaceSpeed, actualSurfaceSpeed, power, torque, opClass,
    // mode, limits [{name, state, machine, requested}], limited (the names of the limits that applied), notes}.
    Q_PROPERTY(QVariantMap calculated READ calculated NOTIFY feedsChanged)
    // "none" (nothing typed or calculated), "calculated" (the values are the record's), "offer" (they are, but the
    // tool or material changed since: an update is offered), "user" (typed by the user).
    Q_PROPERTY(QString feedsState READ feedsState NOTIFY feedsChanged)
    Q_PROPERTY(QString feedsNote READ feedsNote NOTIFY feedsChanged)
    // Status.
    Q_PROPERTY(QString message READ message NOTIFY statusChanged)          // why the values cannot be used, or a pick
    Q_PROPERTY(QString messageField READ messageField NOTIFY statusChanged)
    Q_PROPERTY(QString previewStatus READ previewStatus NOTIFY statusChanged) // "", "incomplete", "building", "ok", "failed"
    Q_PROPERTY(QStringList errors READ errors NOTIFY statusChanged)      // validation: block OK
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY statusChanged)  // validation: do not
    Q_PROPERTY(double progress READ progress NOTIFY statusChanged)       // the preview generation, 0..1, or -1
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY statusChanged)  // the next step, or "Ready: ..."
    Q_PROPERTY(bool committing READ committing NOTIFY statusChanged)
    Q_PROPERTY(bool canCommit READ canCommit NOTIFY statusChanged)

public:
    OperationToolModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                       QObject* parent = nullptr);
    ~OperationToolModel() override;
    static OperationToolModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    void setUnits(UnitsObject* units);
    void setTree(SetupTreeModel* tree);
    void setSetupTool(SetupToolModel* tool) { setupTool_ = tool; }
    void setDisplay(ToolpathDisplay* display) { display_ = display; }

    bool active() const { return id_.has_value(); }
    bool editing() const { return editing_; }
    QString title() const;
    QString operationId() const;
    QVariantList types() const;
    QString type() const { return type_; }
    QString typeTitle() const;
    SchemaFormModel* form() const { return form_; }
    QVariantMap values() const { return values_; }
    QVariantMap fieldMessages() const;
    QVariantList heights() const;
    QStringList cutting() const { return cutting_; }
    QString toolId() const { return toolId_; }
    QString toolText() const;
    QVariantList tools() const;
    QString toolSearch() const { return toolSearch_; }
    void setToolSearch(const QString& text);
    bool showAllTools() const { return showAllTools_; }
    void setShowAllTools(bool all);
    QVariantList holders() const;
    QString holderId() const { return holderId_; }
    QString geometryKind() const { return geometry_.kind; }
    QString geometryPrompt() const;
    QVariantList picks() const;
    bool picking() const { return picking_; }
    QVariantList materials() const;
    QString materialId() const { return materialId_; }
    QString materialSource() const { return materialSource_; }
    QVariantMap calculated() const { return calculated_; }
    QString feedsState() const { return feedsState_; }
    QString feedsNote() const;
    QString message() const { return message_; }
    QString messageField() const { return messageField_; }
    QString previewStatus() const { return previewStatus_; }
    QStringList errors() const { return errors_; }
    QStringList warnings() const { return warnings_; }
    double progress() const { return progress_; }
    QString statusLine() const;
    bool committing() const { return committing_; }
    bool canCommit() const;

    // ---- QML ----
    Q_INVOKABLE bool start();
    Q_INVOKABLE bool startFor(const QString& setupId);
    Q_INVOKABLE bool edit(const QString& operationId);
    Q_INVOKABLE void setType(const QString& type);
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void chooseTool(const QString& toolId);
    Q_INVOKABLE void chooseHolder(const QString& holderId);
    Q_INVOKABLE void setMaterial(const QString& materialId);
    Q_INVOKABLE void setPicking(bool on);
    Q_INVOKABLE void removePick(int index);
    Q_INVOKABLE void clearPicks();
    Q_INVOKABLE void useCalculated();
    Q_INVOKABLE bool commit();
    Q_INVOKABLE void cancel();

    // ---- the command catalog's hooks (cam.opTool.*, cam.operation.add) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    std::optional<OperationId> id() const { return id_; }
    std::optional<SetupId> setupId() const { return setup_; }
    // Until what the panel shows is final: a pick or the holes being worked out, a preview scheduled or building, a
    // check, a generation or a backplot running, a commit.
    bool previewBusy() const;
    int previewsShown() const { return previewsShown_; }
    int previewsCancelled() const { return previewsCancelled_; }
    std::shared_ptr<const cam::Toolpath> previewToolpath() const { return previewPath_; }
    std::shared_ptr<const sim::Backplot> previewBackplot() const { return previewBackplot_; }
    std::size_t holeCount() const { return holes_.size(); }
    const std::vector<api::cam_operation::Hole>& holes() const { return holes_; }
    // The arguments OK would run now, or why it cannot (the field it is about in *field).
    std::optional<QJsonObject> arguments(QString* error, QString* field) const;

signals:
    void activeChanged();
    void typeChanged();
    void valuesChanged();
    void toolChanged();
    void toolsChanged();
    void picksChanged();
    void feedsChanged();
    void statusChanged();
    void committed(const QString& operationId);

private:
    struct Pick {
        QJsonObject ref;  // TopoRef JSON
        QString body;     // body id
        std::uint32_t element = 0;
        QString text;
    };
    struct ToolItem {
        camui::ToolRow row;
        lib::Tool tool;
        bool embedded = false;
    };
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void onDocumentChanged();
    void open(const SetupId& setup, const std::optional<OperationId>& existing, const QString& type);
    void close();
    void loadOperation(const doc::DocState& state, const cam::Operation& op);
    void applyType(const QString& type, bool keepParams);
    void defaults(const doc::DocState& state);
    void loadOptions();
    void loadHoles(std::vector<QJsonObject> refsToName);
    void updateFeeds();
    void applyPickMode();
    void onSelectionChanged();
    void takePick(int bodyIndex, kernel::SubId sub);
    void addPick(const QString& body, std::uint32_t element, const QString& text, kernel::SubKind kind);
    QString holeText(const api::cam_operation::Hole& hole) const;
    void setStatus(const QString& status, const QString& message, const QString& field = QString());
    void schedulePreview();
    void startPreview();
    void onPreviewBuilt(doc::DocStatePtr state, doc::BuildResultPtr build);
    void cancelJobs();
    void showBackplot(std::shared_ptr<const sim::Backplot> backplot);
    camui::ExprFn exprFn() const;
    bool inch() const;
    QString lengthText(double mm) const;
    std::optional<double> length(const QString& key, QString* error) const;
    const ToolItem* chosenTool() const;
    const cam::Setup* setupOf(const doc::DocState& state) const;
    void mapError(const QString& message);

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;
    QPointer<SetupTreeModel> tree_;
    QPointer<SetupToolModel> setupTool_;
    QPointer<ToolpathDisplay> display_;
    SchemaFormModel* form_ = nullptr;

    // The document observed.
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    int previewListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    // The open panel.
    std::optional<OperationId> id_;
    std::optional<SetupId> setup_;
    bool editing_ = false;
    QString type_;
    QJsonObject schema_; // the type's schema (annotations)
    camui::GeometrySpec geometry_;
    QStringList heights_, cutting_;
    QVariantMap values_;
    std::map<QString, QString> fieldErrors_; // from the API, by key
    QString toolId_, holderId_;
    std::vector<ToolItem> toolItems_;
    // A field's text as it was written from a stored or calculated value, and that value: while the text is unchanged
    // it reads as exactly that value (no round trip through the display unit's rounding).
    std::map<QString, std::pair<QString, double>> exact_;
    std::map<QString, std::string> holderJson_; // library holders by id
    QVariantList holderList_;
    QString toolSearch_;
    bool showAllTools_ = false;
    std::vector<Pick> picks_;
    bool picking_ = false;
    std::uint8_t savedFilter_ = 0;
    bool filterSaved_ = false;
    std::uint64_t pickSerial_ = 0;
    bool pickPending_ = false;
    std::vector<api::cam_operation::Hole> holes_;
    bool holesReady_ = false;
    std::optional<std::pair<int, kernel::SubId>> queuedPick_; // a click while the holes were being found
    JobHandle<std::shared_ptr<const std::pair<std::vector<api::cam_operation::Hole>, std::vector<int>>>> holesJob_;
    // Feeds.
    std::vector<lib::Material> materialItems_;
    QString materialId_, materialSource_;
    QVariantMap calculated_;
    std::optional<cam::FeedsResult> feeds_;
    std::optional<cam::CalculatedFeeds> record_;       // what "Use calculated" wrote (loaded, or this session's)
    std::optional<cam::CalculatedFeeds> loadedRecord_; // as the operation had it
    QString feedsState_ = QStringLiteral("none");
    // Status.
    QString message_, messageField_, previewStatus_;
    QStringList errors_, warnings_;
    double progress_ = -1;
    bool committing_ = false;
    bool rejected_ = false;
    bool checked_ = false; // the check of the values now has come back
    // Preview.
    QTimer previewTimer_;
    bool previewBusy_ = false, previewPending_ = false;
    doc::DocStatePtr previewBase_;
    std::uint64_t previewSerial_ = 0;
    JobHandle<cam::OpValidation> validateJob_;
    JobHandle<std::shared_ptr<const cam::Toolpath>> previewJob_;
    JobHandle<std::shared_ptr<const sim::Backplot>> backplotJob_;
    std::shared_ptr<const cam::Toolpath> previewPath_;
    std::shared_ptr<const sim::Backplot> previewBackplot_;
    int previewsShown_ = 0;
    int previewsCancelled_ = 0;
    int nameSerial_ = 0;
};

} // namespace ray::ui
