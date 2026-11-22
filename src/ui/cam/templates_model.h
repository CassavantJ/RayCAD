// The `CamTemplates` QML singleton (task T-057): the operation template panel of the CAM workspace
// (qml/cam/Templates.qml). Everything it changes goes through the cam.template.* actions (src/api), so the panel, the
// command palette, scripts and agents do the same thing.
//
//   Browse     every template of the library stack (IApp::libraries), grouped by set and material: a set row (the
//              set's templates for one material, applied together) and its templates under it, in sequence order;
//              filtered by words (name, set, kind, material, library), by feature kind and by material (the setup's,
//              any, or one). Each row says where it comes from; a built-in one is read-only.
//   Preview    for the selected set or template, the chosen setup and the holes chosen (all the setup's recognised
//              holes, one group of alike holes, or the holes of the faces selected in the view):
//              cam.template.preview, on every change, coalesced; the operations it would make (name, tool and its
//              number, how many holes) or why it would make none (the missing tools and their holes, in the action's
//              words), BEFORE anything is applied.
//   Apply      cam.template.apply with the same arguments (the templates by id, in the set's order): one undo step.
//   Save       openSave(operationId) (the CAM tree's operation row): the name, set, sequence, feature kind, material,
//              tool rule (a rule or this exact tool), feeds rule (calculated or fixed) and heights rule, then
//              cam.template.saveFrom into the user's library.
//   Manage     rename and delete (with a question) of templates in a writable library: cam.template.rename / remove,
//              checked against what the panel showed (the content hash), so someone else's change is not overwritten.
//   Re-apply   reapply(operationId): cam.template.reapply (the CAM tree's operation row).
//   Command    cam.templates.open (Cam context) opens the panel on the CAM tree's current setup.
#pragma once

#include <ray/api.h>

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class SelectionModel;
class Session;
class SetupTreeModel;

class TemplatesModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamTemplates)
    QML_SINGLETON

    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)
    // "apply" (browse and apply) or "save" (save an operation as a template).
    Q_PROPERTY(QString mode READ mode NOTIFY shownChanged)
    // ---- browse ----
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY filterChanged)
    Q_PROPERTY(QString kindFilter READ kindFilter WRITE setKindFilter NOTIFY filterChanged)
    // "setup" (the setup's material, the default), "" (any) or a material id.
    Q_PROPERTY(QString materialFilter READ materialFilter WRITE setMaterialFilter NOTIFY filterChanged)
    Q_PROPERTY(QVariantList kinds READ kinds CONSTANT)                      // [{value, text}]
    Q_PROPERTY(QVariantList materials READ materials NOTIFY catalogChanged) // [{value, text}]
    // [{rowKind ("set" | "template"), key, title, detail, readOnly, library, depth}]
    Q_PROPERTY(QVariantList rows READ rows NOTIFY catalogChanged)
    Q_PROPERTY(int templateCount READ templateCount NOTIFY catalogChanged)
    // The selected row's key ("set:<set>|<material id>" or a template id), "" for none.
    Q_PROPERTY(QString selected READ selected WRITE select NOTIFY selectionChanged)
    // {kind, title, subtitle, lines: [{label, value}], members: [names], readOnly, canRename, id, library}
    Q_PROPERTY(QVariantMap selectedInfo READ selectedInfo NOTIFY selectionChanged)
    // ---- the target ----
    Q_PROPERTY(QVariantList setups READ setups NOTIFY setupsChanged) // [{value, text}]
    Q_PROPERTY(QString setupId READ setupIdText WRITE setSetup NOTIFY setupsChanged)
    // [{value ("all" | "selection" | a group key), text}]
    Q_PROPERTY(QVariantList targets READ targets NOTIFY previewChanged)
    Q_PROPERTY(QString target READ target WRITE setTarget NOTIFY previewChanged)
    Q_PROPERTY(int selectedFaces READ selectedFaces NOTIFY previewChanged)
    // ---- the preview ----
    // "" (nothing to preview), "working", "ok", "refused" (missing tools or nothing to make), "failed".
    Q_PROPERTY(QString previewState READ previewState NOTIFY previewChanged)
    // [{name, tool, toolNumber, holes, template}]
    Q_PROPERTY(QVariantList previewOps READ previewOps NOTIFY previewChanged)
    Q_PROPERTY(QString previewMessage READ previewMessage NOTIFY previewChanged)
    Q_PROPERTY(QString previewSummary READ previewSummary NOTIFY previewChanged)
    Q_PROPERTY(bool canApply READ canApply NOTIFY previewChanged)
    Q_PROPERTY(QString applyBlockedReason READ applyBlockedReason NOTIFY previewChanged)
    // ---- save ----
    Q_PROPERTY(QString saveOperationName READ saveOperationName NOTIFY shownChanged)
    // name, set, sequence (text), featureKind, material ("setup" | "any"), tool ("rule" | "exact"), feeds
    // ("calculated" | "fixed"), heights ("operation" | "defaults")
    Q_PROPERTY(QVariantMap saveValues READ saveValues NOTIFY saveValuesChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY saveValuesChanged)
    // ---- results ----
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusChanged)
    Q_PROPERTY(QString deleteAsked READ deleteAsked NOTIFY statusChanged) // the template id asked about, or ""

public:
    TemplatesModel(Session* session, AppContext* context, QObject* parent = nullptr);
    ~TemplatesModel() override;
    static TemplatesModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    void setTree(SetupTreeModel* tree) { tree_ = tree; }
    void setSelection(SelectionModel* selection);

    bool shown() const { return shown_; }
    QString mode() const { return mode_; }
    QString search() const { return search_; }
    void setSearch(const QString& text);
    QString kindFilter() const { return kindFilter_; }
    void setKindFilter(const QString& kind);
    QString materialFilter() const { return materialFilter_; }
    void setMaterialFilter(const QString& material);
    QVariantList kinds() const;
    QVariantList materials() const { return materials_; }
    QVariantList rows() const { return rows_; }
    int templateCount() const { return templateCount_; }
    QString selected() const { return selected_; }
    void select(const QString& key);
    QVariantMap selectedInfo() const;
    QVariantList setups() const { return setups_; }
    QString setupIdText() const;
    void setSetup(const QString& id);
    QVariantList targets() const;
    QString target() const { return target_; }
    void setTarget(const QString& target);
    int selectedFaces() const;
    QString previewState() const { return previewState_; }
    QVariantList previewOps() const { return previewOps_; }
    QString previewMessage() const { return previewMessage_; }
    QString previewSummary() const { return previewSummary_; }
    bool canApply() const { return applyBlockedReason().isEmpty(); }
    QString applyBlockedReason() const;
    QString saveOperationName() const { return saveOpName_; }
    QVariantMap saveValues() const { return saveValues_; }
    bool canSave() const;
    bool busy() const { return busy_; }
    QString status() const { return status_; }
    bool statusIsError() const { return statusIsError_; }
    QString deleteAsked() const { return deleteAsked_; }

    // ---- QML ----
    // Opens the panel to browse and apply, on setup `setupId` ("" the CAM tree's current setup, else the first).
    Q_INVOKABLE bool open(const QString& setupId = QString());
    // Opens the panel to save operation `operationId` as a template.
    Q_INVOKABLE bool openSave(const QString& operationId);
    Q_INVOKABLE void close();
    Q_INVOKABLE bool apply();
    Q_INVOKABLE void setSaveValue(const QString& key, const QVariant& value);
    Q_INVOKABLE bool save();
    Q_INVOKABLE bool rename(const QString& templateId, const QString& name);
    Q_INVOKABLE void askDelete(const QString& templateId);
    Q_INVOKABLE void cancelDelete();
    Q_INVOKABLE bool confirmDelete();
    Q_INVOKABLE bool reapply(const QString& operationId);
    Q_INVOKABLE void dismissStatus();
    // The template an operation came from, for the CAM tree: its name, "" when none (or not found).
    Q_INVOKABLE QString templateName(const QString& templateId) const;

    // ---- the command catalog's hooks (cam.templates.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    // Previews run since the start, and the arguments the last one (and an apply) used.
    int previews() const { return previews_; }
    std::string applyArgs() const;

signals:
    void shownChanged();
    void filterChanged();
    void catalogChanged();
    void selectionChanged();
    void setupsChanged();
    void previewChanged();
    void saveValuesChanged();
    void statusChanged();
    // An application made operations (their ids), and a save made a template (its id).
    void applied(const QStringList& operationIds);
    void saved(const QString& templateId);

private:
    struct Template {
        lib::OpTemplate item;
        QString set, library;
        int sequence = 0;
        bool readOnly = true;
        QString hash; // content hash, as the actions check it
    };
    api::IApp* app() const;
    lib::LibraryStack* stack() const;
    doc::DocStatePtr docState() const;
    void subscribeSession();
    void subscribeDocument();
    void unsubscribeDocument();
    void rebuildCatalog();
    void rebuildSetups();
    void schedulePreview();
    void runPreview();
    void setStatus(const QString& text, bool error);
    std::vector<const Template*> selectedTemplates() const;
    QString materialOfSetup() const;
    QString materialName(const QString& id) const;
    bool invoke(const char* action, const std::string& args, std::function<void(const QJsonObject&)> done,
                const QString& what);

    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<SetupTreeModel> tree_;
    QPointer<SelectionModel> selection_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    lib::LibraryStack* stackSubscribed_ = nullptr;
    int stackListener_ = 0;
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    bool shown_ = false;
    QString mode_ = QStringLiteral("apply");
    QString search_, kindFilter_, materialFilter_ = QStringLiteral("setup");
    std::vector<Template> templates_;
    QVariantList materials_, rows_;
    int templateCount_ = 0;
    QString selected_;
    QVariantList setups_;
    std::optional<SetupId> setup_;
    QString target_ = QStringLiteral("all");
    QVariantList groups_; // the setup's hole groups, from the last preview
    QString groupsSetup_; // the setup they are of
    QTimer previewTimer_;
    std::uint64_t previewSerial_ = 0;
    int previews_ = 0;
    QString previewState_;
    QVariantList previewOps_;
    QString previewMessage_, previewSummary_;
    std::string lastArgs_;
    QString saveOp_, saveOpName_;
    QVariantMap saveValues_;
    bool busy_ = false;
    QString status_;
    bool statusIsError_ = false;
    QString deleteAsked_;
};

} // namespace ray::ui
