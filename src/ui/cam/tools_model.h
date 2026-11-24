// The tool library manager (task T-046): the `CamTools` QML singleton behind qml/cam/ToolLibrary.qml, and the
// `ToolList` list model that the manager and the tool picker (qml/cam/ToolPicker.qml; the operation panel's tool
// chooser) each make for their own filter.
//
//   Browse     every tool and holder of the library stack (IApp::libraries: the built-in library, read-only, then the
//              shared folders, then the user's own library, "My library") with the root each comes from, and the open
//              document's embedded copies as their own group ("In this document") first. A ToolList filters by words,
//              tool type, diameter range (in the display unit) and a material that has a cutting preset, sorted by
//              diameter, name or type (ties by name, then id): deterministic. The catalog is made on the UI thread from
//              the stack's merged lists when they change (LibraryStack::subscribe) and the document when it changes;
//              the list views are virtualised (only the visible rows are created). Refresh rescans the folders on a
//              worker (LibraryStack::refreshInBackground); so does opening the manager.
//   Edit       selecting an item opens it in the editor: a library item of a writable root is edited in place; a
//              built-in item and the document's copy are read-only, with "Duplicate to my library" (and, for the
//              document's copy, "Update from library"). New, duplicate, delete; presets (tools) and segments (holders)
//              as rows. Values with units are text in the display unit (Units, as the setup panel shows them); every
//              change checks the item against its schema in src/lib (api::cam_tools) and shows each problem beside its
//              field in the schema's words. The selection does not move away from unsaved changes (the reason shows).
//   Preview    the side view of the item (toolsui::toolProfile), to scale, in a holder of the library or the
//              document when one is chosen; remade only when the item's geometry (Tool::geometryHash and the holder's)
//              or the holder chosen changes: profileRevision counts the times.
//   Save       on a worker, into the root the item comes from (a new item or a copy: My library), through
//              LibraryStack::sharedLibrary, checking that the library still holds what the editor started from
//              (api::cam_tools::saveTool). A conflict (ErrorCode::Locked: the file changed underneath or another
//              program has it open; or any other failure) keeps the edits and shows what happened, the library and the
//              file, and the choices: retry (writes over the other version), save a copy to my library, or discard the
//              edits (the library's version is shown). Delete is checked the same way.
//   Document   picking a library tool for an operation embeds a copy in the same undo step, as the operation panel
//              does (the "library" member of cam.operation.add / edit): libraryArgsFor gives that member,
//              assignTool runs cam.operation.edit with it. A document copy whose library original now has another
//              geometry is marked; "Update from library" replaces the copy (cam.library.embed).
//   Command    cam.toolLibrary.open (Cam context, Shift+T) shows the manager; it needs a session, not a document.
#pragma once

#include "tools_logic.h"

#include <ray/api.h>

#include <QtCore/QAbstractListModel>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlParserStatus>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class Session;
class UnitsObject;

class ToolLibraryModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamTools)
    QML_SINGLETON

    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)
    // The catalog changed (the stack, the document or the units): ToolLists filter again.
    Q_PROPERTY(int revision READ revision NOTIFY catalogChanged)
    // [{index, name, dir, kind ("builtin" | "shared" | "user"), writable, open, loading}] lowest first.
    Q_PROPERTY(QVariantList roots READ roots NOTIFY catalogChanged)
    Q_PROPERTY(QString myLibrary READ myLibraryName NOTIFY catalogChanged)    // "" when there is no writable root
    Q_PROPERTY(QVariantList materials READ materials NOTIFY catalogChanged)   // [{id, name}] by name
    Q_PROPERTY(QVariantList holders READ holderChoices NOTIFY catalogChanged) // [{id, name}] the preview's choices
    Q_PROPERTY(QVariantList toolTypes READ toolTypes CONSTANT)                // [{value, text}]
    Q_PROPERTY(QVariantList opClasses READ opClasses CONSTANT)                // [{value, text}]
    Q_PROPERTY(int issueCount READ issueCount NOTIFY catalogChanged)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshingChanged)
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY catalogChanged)

    // ---- the editor ----
    // "tool", "holder" or "" (nothing selected).
    Q_PROPERTY(QString editorKind READ editorKind NOTIFY editorChanged)
    Q_PROPERTY(QString selectedId READ selectedId NOTIFY editorChanged)
    // "library", "document" or "new" (a new item or a copy not saved yet).
    Q_PROPERTY(QString selectedOrigin READ selectedOrigin NOTIFY editorChanged)
    Q_PROPERTY(QString title READ title NOTIFY valuesChanged)
    // Where the item is: "My library", "Shop library (read-only)", "In this document", "New in My library".
    Q_PROPERTY(QString whereText READ whereText NOTIFY editorChanged)
    Q_PROPERTY(bool readOnly READ readOnly NOTIFY editorChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY valuesChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY statusChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY valuesChanged)
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY valuesChanged)        // presets or segments
    Q_PROPERTY(QVariantMap problems READ problems NOTIFY valuesChanged) // field key -> message
    // [{value, text}] of a tool's centre-cutting field: the default of its type and tip as now edited, Yes, No.
    Q_PROPERTY(QVariantList centreCuttingChoices READ centreCuttingChoices NOTIFY valuesChanged)
    // [{key, label, kind}]; announced only when the list itself changes (a form is not made again for nothing).
    Q_PROPERTY(QVariantList fields READ fields NOTIFY fieldsChanged)
    Q_PROPERTY(QVariantList rowFields READ rowFields NOTIFY fieldsChanged)
    // The last result ("Saved to My library") or why the selection did not move; "" when none.
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusChanged)
    // A save or delete that did not happen: {kind ("changed" | "locked" | "failed"), action ("save" | "delete"),
    // title, message, library, file, canCopy}; empty when there is none.
    Q_PROPERTY(QVariantMap conflict READ conflict NOTIFY statusChanged)
    Q_PROPERTY(bool deleteAsked READ deleteAsked NOTIFY statusChanged)
    // The document's copy: the library's original now has another geometry / differs at all.
    Q_PROPERTY(bool libraryChanged READ libraryChanged NOTIFY editorChanged)
    Q_PROPERTY(bool canUpdateFromLibrary READ canUpdateFromLibrary NOTIFY editorChanged)

    // ---- the preview ----
    // {outline, cutter, holder: [point], halfWidth, height, stickOut, diameter, length} in mm (y up from the tip);
    // empty when nothing is shown.
    Q_PROPERTY(QVariantMap profile READ profile NOTIFY profileChanged)
    Q_PROPERTY(int profileRevision READ profileRevision NOTIFY profileChanged)
    Q_PROPERTY(QString previewHolder READ previewHolder WRITE setPreviewHolder NOTIFY profileChanged)

public:
    ToolLibraryModel(Session* session, AppContext* context, QObject* parent = nullptr);
    ~ToolLibraryModel() override;
    static ToolLibraryModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    void setUnits(UnitsObject* units);

    bool shown() const { return shown_; }
    void setShown(bool shown);
    int revision() const { return revision_; }
    QVariantList roots() const { return roots_; }
    QString myLibraryName() const;
    QVariantList materials() const { return materials_; }
    QVariantList holderChoices() const { return holderChoices_; }
    QVariantList toolTypes() const;
    QVariantList opClasses() const;
    int issueCount() const { return issueCount_; }
    bool refreshing() const { return refreshing_; }
    bool hasDocument() const;

    QString editorKind() const;
    QString selectedId() const;
    QString selectedOrigin() const;
    QString title() const;
    QString whereText() const;
    bool readOnly() const;
    bool dirty() const;
    bool saving() const { return saving_; }
    bool canSave() const;
    QVariantMap values() const;
    QVariantList rows() const;
    QVariantMap problems() const;
    QVariantList centreCuttingChoices() const;
    QVariantList fields() const { return fields_; }
    QVariantList rowFields() const { return rowFields_; }
    QString status() const { return status_; }
    bool statusIsError() const { return statusIsError_; }
    QVariantMap conflict() const { return conflict_; }
    bool deleteAsked() const { return deleteAsked_; }
    bool libraryChanged() const;
    bool canUpdateFromLibrary() const;
    QVariantMap profile() const { return profile_; }
    int profileRevision() const { return profileRevision_; }
    QString previewHolder() const { return previewHolder_; }
    void setPreviewHolder(const QString& id);

    // ---- QML ----
    Q_INVOKABLE void show();
    Q_INVOKABLE void hide(); // false-free: refused (with the reason in status) while there are unsaved changes
    Q_INVOKABLE void refresh();
    // Selects a row (origin "library" or "document"; kind "tool" or "holder"). False when unsaved changes hold the
    // selection (status says so).
    Q_INVOKABLE bool select(const QString& origin, const QString& kind, const QString& id);
    // A new tool of the type (a file format name) or a new holder (type "holder"), in My library, not saved yet.
    Q_INVOKABLE bool newItem(const QString& type);
    // A copy of the selected item in My library, not saved yet ("Duplicate to my library").
    Q_INVOKABLE bool duplicate();
    Q_INVOKABLE void setValue(const QString& key, const QString& text);
    Q_INVOKABLE void setRowValue(int row, const QString& key, const QString& text);
    Q_INVOKABLE void addRow();
    Q_INVOKABLE void duplicateRow(int row);
    Q_INVOKABLE void removeRow(int row);
    Q_INVOKABLE bool save();
    Q_INVOKABLE void revert(); // back to what was opened (a new item that was never saved goes)
    Q_INVOKABLE void askDelete();
    Q_INVOKABLE void cancelDelete();
    Q_INVOKABLE bool confirmDelete();
    // "retry", "copy", "discard" or "cancel" (close the message, keep editing).
    Q_INVOKABLE bool resolveConflict(const QString& choice);
    Q_INVOKABLE bool updateFromLibrary();
    Q_INVOKABLE void dismissStatus();

    // ---- the tool picker's API (qml/cam/ToolPicker.qml; the operation panel, T-045) ----
    // The library shown for a picker (its Library button): while `choosing`, the panel offers "Use this tool" for the
    // selected tool, which ends the choice with toolChosen(id, origin) and closes the panel. Closing it otherwise ends
    // the choice without one (choiceEnded either way: the picker takes the focus back).
    Q_PROPERTY(bool choosing READ choosing NOTIFY choosingChanged)
    bool choosing() const { return choosing_; }
    Q_INVOKABLE void showToChoose();
    // "Use this tool": false (nothing happens) when no saved tool is selected or there are unsaved edits.
    Q_INVOKABLE bool useSelected();
    // The "library" member a cam.operation.add / edit needs so the tool (and holder, "" for none) is embedded in the
    // same step: {tools: [item], holders: [item]} with only what the document does not have yet; empty when nothing.
    Q_INVOKABLE QVariantMap libraryArgsFor(const QString& toolId, const QString& holderId = QString()) const;
    // Sets the operation's tool (and holder) through cam.operation.edit, embedding a copy on first use: one undo step.
    Q_INVOKABLE bool assignTool(const QString& operationId, const QString& toolId, const QString& holderId = QString());

    // ---- the command catalog's hooks (cam.toolLibrary.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- ToolList and tests ----
    const std::vector<toolsui::Entry>& entries(bool holders);
    // The catalog as it is (never made here): what a ToolList's rows index while it shows them.
    const std::vector<toolsui::Entry>& cachedEntries(bool holders) const { return holders ? holders_ : tools_; }
    void useCatalog(); // a ToolList is shown: keep the catalog made
    QString rootLabel(int root) const;
    QString typeTitle(const QString& type) const;
    QString formatLength(double mm) const;
    double parseLength(const QString& text) const;
    bool busy() const; // a save, delete, refresh or command is running
    int catalogBuilds() const { return catalogBuilds_; }
    double lastCatalogMs() const { return lastCatalogMs_; }

signals:
    void shownChanged();
    void catalogChanged();
    void refreshingChanged();
    void editorChanged();
    void fieldsChanged();
    void valuesChanged();
    void statusChanged();
    void profileChanged();
    // After a save: the item now in the library (the list selects it).
    void saved(const QString& kind, const QString& id);
    void choosingChanged();
    void toolChosen(const QString& toolId, const QString& origin);
    void choiceEnded();

private:
    struct Draft {
        bool holder = false;
        QString origin; // "library", "document", "new"
        QString id;
        int root = -1;               // where it is saved (-1: nowhere: read-only)
        std::optional<Hash256> base; // as opened (library); nullopt: a new item
        QJsonObject baseJson;        // the file format as opened
        toolsui::Values values;
        bool readOnly = false;
        int sourceRoot = -1; // the root it came from (library items)
    };
    api::IApp* app() const;
    lib::LibraryStack* stack() const;
    doc::DocStatePtr docState() const;
    void subscribeSession();
    void subscribeDocument();
    void unsubscribeDocument();
    void catalogStale();
    void buildCatalog();
    int myRoot() const;
    toolsui::Units units() const;
    bool hold(); // unsaved changes: set the status and return true
    void open(Draft draft);
    void clearEditor();
    void recheck();
    void updateFields();
    void updateProfile();
    void setStatus(const QString& text, bool error = false);
    void setConflict(const Error& error, const QString& action);
    bool startSave(bool overwrite, bool asCopy);
    std::optional<lib::Tool> editedTool() const;
    std::optional<lib::Holder> editedHolder() const;
    const lib::Tool* libraryTool(const QString& id) const;
    const lib::Holder* libraryHolder(const QString& id) const;
    const lib::Tool* documentTool(const QString& id) const;
    const lib::Holder* documentHolder(const QString& id) const;

    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<UnitsObject> units_;

    bool shown_ = false;
    bool choosing_ = false; // shown for a picker (showToChoose)
    int revision_ = 0;
    bool catalogValid_ = false;
    bool catalogWanted_ = false;
    int catalogBuilds_ = 0;
    double lastCatalogMs_ = 0;
    std::vector<toolsui::Entry> tools_, holders_;
    QVariantList roots_, materials_, holderChoices_;
    int issueCount_ = 0;
    bool refreshing_ = false;
    lib::LibraryStack* stackSubscribed_ = nullptr;
    int stackListener_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;

    std::optional<Draft> draft_;
    toolsui::Checked checked_;
    QVariantList fields_, rowFields_;
    QString status_;
    bool statusIsError_ = false;
    QVariantMap conflict_;
    bool deleteAsked_ = false;
    bool saving_ = false;
    int jobsRunning_ = 0;

    QVariantMap profile_;
    int profileRevision_ = 0;
    QString profileKey_;
    QString previewHolder_;
};

// One filtered, sorted view of the catalog (QML: ToolList). Made by the manager and by each tool picker.
class ToolListModel : public QAbstractListModel, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(ToolList)

    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY filterChanged) // "tools" (default) or "holders"
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY filterChanged)
    Q_PROPERTY(QString type READ type WRITE setType NOTIFY filterChanged)
    // Several types at once (an operation type's x-toolTypes); empty for any. Applies together with `type`.
    Q_PROPERTY(QStringList types READ types WRITE setTypes NOTIFY filterChanged)
    // Diameter bounds as typed, in the display unit ("" for none); a bound that does not read is ignored and named in
    // diameterProblem.
    Q_PROPERTY(QString minDiameter READ minDiameter WRITE setMinDiameter NOTIFY filterChanged)
    Q_PROPERTY(QString maxDiameter READ maxDiameter WRITE setMaxDiameter NOTIFY filterChanged)
    Q_PROPERTY(QString diameterProblem READ diameterProblem NOTIFY filterChanged)
    Q_PROPERTY(QString material READ material WRITE setMaterial NOTIFY filterChanged)
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY filterChanged) // "diameter", "name", "type"
    Q_PROPERTY(bool includeDocument READ includeDocument WRITE setIncludeDocument NOTIFY filterChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int total READ total NOTIFY countChanged) // before filtering

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        OriginRole, // "document" or "library"
        GroupRole,  // the section title: "In this document", "Library"
        NameRole,
        TypeRole,     // the file format's name
        TypeTextRole, // its title
        DiameterTextRole,
        FlutesRole,
        RootNameRole,
        ReadOnlyRole,
        LibraryChangedRole,
    };

    explicit ToolListModel(QObject* parent = nullptr);
    ~ToolListModel() override;
    void classBegin() override {}
    void componentComplete() override;
    void setSource(ToolLibraryModel* source); // C++ (tests); QML finds the CamTools singleton itself

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString kind() const { return holders_ ? QStringLiteral("holders") : QStringLiteral("tools"); }
    void setKind(const QString& kind);
    QString text() const { return text_; }
    void setText(const QString& text);
    QString type() const { return type_; }
    void setType(const QString& type);
    QStringList types() const { return types_; }
    void setTypes(const QStringList& types);
    QString minDiameter() const { return min_; }
    void setMinDiameter(const QString& text);
    QString maxDiameter() const { return max_; }
    void setMaxDiameter(const QString& text);
    QString diameterProblem() const { return diameterProblem_; }
    QString material() const { return material_; }
    void setMaterial(const QString& id);
    QString sort() const;
    void setSort(const QString& key);
    bool includeDocument() const { return includeDocument_; }
    void setIncludeDocument(bool on);
    int count() const { return static_cast<int>(rows_.size()); }
    int total() const { return total_; }

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int rowOf(const QString& origin, const QString& id) const;

    // Tests: the filter runs done and how long the last took.
    int filterRuns() const { return filterRuns_; }
    double lastFilterMs() const { return lastFilterMs_; }

signals:
    void filterChanged();
    void countChanged();

private:
    void refilter();
    const toolsui::Entry* entryAt(int row) const;

    QPointer<ToolLibraryModel> source_;
    QMetaObject::Connection sourceConnection_;
    bool holders_ = false;
    QString text_, type_, min_, max_, material_, diameterProblem_;
    QStringList types_;
    toolsui::SortKey sort_ = toolsui::SortKey::Diameter;
    bool includeDocument_ = true;
    std::vector<int> rows_;
    std::vector<int> order_; // the catalog sorted for sort_ (of revision orderRevision_)
    int orderRevision_ = -1;
    bool orderHolders_ = false;
    toolsui::SortKey orderSort_ = toolsui::SortKey::Diameter;
    bool refiltering_ = false;
    int total_ = 0;
    int filterRuns_ = 0;
    double lastFilterMs_ = 0;
};

} // namespace ray::ui
