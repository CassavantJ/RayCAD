// The `Replacement` QML singleton (task T-084): the replace dialog and its change summary
// (qml/model/ReplaceImport.qml). Replacing an imported part's file with a new revision is the action
// file.replaceImport (ray_api/replace_import.h); this model only drives it and shows its result.
//
//   Dialog     open() picks the import to replace: the feature selected in the model tree when it is an import, else
//   the
//              document's only import (with several and none selected, the dialog lists them to choose). chooseFile()
//              asks the QML for a file dialog; replace() runs the action on the job system. A refusal (another number
//              of bodies, not a solid, the same file, a file that cannot be read) is shown with its message and nothing
//              changes.
//   Summary    after a replace: one line with the face counts ("12 faces changed, 3 new, 1 removed"; or "Every face is
//              unchanged"), one with edges and corners, one for units when the files declare different ones, one for
//              the toolpaths that must be generated again, and a row per changed, new and removed face, in that order.
//   Viewport   while the summary is shown: changed faces tinted in the warning colour (amber), new ones in the success
//              colour (green), each removed face's place a dashed box in the error colour; unchanged faces as they are.
//              Tints are meshes over the scene's own (never picked). The current row is drawn with the selection
//              highlight (its dashed box thicker, for a removed face). Faces are found in the document's build by their
//              names (a later feature may have changed or removed them: such a row says "not in the model now").
//   Hand-over  this model never reviews references: the dialog opens the reference review panel (References), which
//              lists what the replace left for review, by itself once the replaced model is listed (when there is
//              something to review) and from its Review button; stale toolpaths are regenerated from there or the CAM
//              tree. replaced() and `built` tell the QML when.
//   Undo       undo() undoes the replace (edit.undo); the summary then closes. Any other change of the document clears
//              the tints (the summary text stays until the dialog closes).
#pragma once

#include "preview.h"

#include <ray/api.h>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QVariantList>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class FeatureTreeModel;
class SceneController;
class SelectionModel;
class Session;

class ReplaceImportModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Replacement)
    QML_SINGLETON

    Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY shownChanged)
    // The imports of the document that can be replaced: [{id, name, file}].
    Q_PROPERTY(QVariantList imports READ imports NOTIFY importsChanged)
    Q_PROPERTY(QString feature READ feature WRITE setFeature NOTIFY featureChanged)
    Q_PROPERTY(QString featureName READ featureName NOTIFY featureChanged)
    Q_PROPERTY(QString currentFile READ currentFile NOTIFY featureChanged)
    // The new revision chosen (a local path); "" none.
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY pathChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canReplace READ canReplace NOTIFY stateChanged)
    // The summary of the last replace.
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY resultChanged)
    Q_PROPERTY(QStringList notes READ notes NOTIFY resultChanged)
    Q_PROPERTY(int changedCount READ changedCount NOTIFY resultChanged)
    Q_PROPERTY(int newCount READ newCount NOTIFY resultChanged)
    Q_PROPERTY(int removedCount READ removedCount NOTIFY resultChanged)
    Q_PROPERTY(int toolpaths READ toolpaths NOTIFY resultChanged)
    // Rows: {fate ("changed" | "new" | "removed"), title, detail, located}.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(int current READ current WRITE setCurrent NOTIFY currentChanged)
    // The tints are on the view (the build of the replaced state is shown).
    Q_PROPERTY(bool tinted READ tinted NOTIFY rowsChanged)
    // The document's build is of the replaced state (what the reference review lists then is the replace's doing).
    Q_PROPERTY(bool built READ built NOTIFY builtChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)

public:
    ReplaceImportModel(SceneController* scene, SelectionModel* selection, Session* session, AppContext* context,
                       QObject* parent = nullptr);
    ~ReplaceImportModel() override;
    static ReplaceImportModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    void setFeatureTree(FeatureTreeModel* tree);

    bool shown() const { return shown_; }
    void setShown(bool shown);
    QVariantList imports() const { return imports_; }
    QString feature() const { return feature_; }
    void setFeature(const QString& id);
    QString featureName() const;
    QString currentFile() const;
    QString path() const { return path_; }
    void setPath(const QString& pathOrUrl);
    QString fileName() const;
    bool busy() const { return busy_; }
    bool canReplace() const { return !busy_ && !feature_.isEmpty() && !path_.isEmpty(); }
    bool hasResult() const { return hasResult_; }
    QString summary() const { return summary_; }
    QStringList notes() const { return notes_; }
    int changedCount() const { return changed_; }
    int newCount() const { return added_; }
    int removedCount() const { return removed_; }
    int toolpaths() const { return toolpaths_; }
    QVariantList rows() const { return rowList_; }
    int current() const { return current_; }
    void setCurrent(int row);
    bool tinted() const { return !tintItems_.empty() || (lines_ && lines_->count() > 0); }
    QString message() const { return message_; }
    bool messageIsError() const { return messageIsError_; }
    bool built() const { return built_; }

    // ---- QML ----
    Q_INVOKABLE void open();
    Q_INVOKABLE void close();
    Q_INVOKABLE void chooseFile();
    Q_INVOKABLE bool replace();
    Q_INVOKABLE bool undo();

    // ---- the command catalog's hook (replaceImport.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    // The faces tinted: per fate ("changed", "new"), how many; and the removed boxes drawn.
    int tintedFaces(const QString& fate) const;
    int removedBoxes() const;
    const std::vector<render::PickHit>& highlighted() const { return highlighted_; }
    // The result JSON of the last replace ("" none).
    const std::string& resultJson() const { return resultJson_; }

signals:
    void shownChanged();
    void importsChanged();
    void featureChanged();
    void pathChanged();
    void busyChanged();
    void stateChanged();
    void resultChanged();
    void rowsChanged();
    void currentChanged();
    void messageChanged();
    void builtChanged();
    // The QML shows a file dialog for the new revision, opening in `folder`.
    void fileDialogRequested(const QUrl& folder);
    // A replace finished and left references to review: the QML hands over to the reference review.
    void replaced();

private:
    struct Row {
        QString fate;
        BodyId body;
        kernel::SubId sub;          // in the replaced geometry (removed: the old one)
        std::vector<Hash256> names; // to find it in the document's build
        std::optional<Box3> box;    // its box (for a removed face: where it was)
        QString title, detail;
        std::optional<kernel::SubId> located; // in the document's build
    };
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void refreshImports();
    void watch();
    void unwatch();
    void setBusy(bool busy);
    void setMessage(const QString& message, bool error);
    void clearResult();
    void takeResult(const std::string& json);
    void onBuild();
    void applyTints();
    void clearTints();
    void applyCurrent();
    int sceneIndexOf(const BodyId& body) const;

    QPointer<SceneController> scene_;
    QPointer<SelectionModel> selection_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<FeatureTreeModel> tree_;
    std::unique_ptr<LineOverlay> lines_;

    bool shown_ = false;
    QVariantList imports_;
    QString feature_, path_;
    bool busy_ = false;
    std::uint64_t serial_ = 0;

    // The result.
    bool hasResult_ = false;
    std::string resultJson_;
    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t generation_ = 0;
    doc::DocStatePtr resultState_; // the state the replace made
    QString summary_;
    QStringList notes_;
    int changed_ = 0, added_ = 0, removed_ = 0, toolpaths_ = 0;
    std::vector<Row> rows_;
    QVariantList rowList_;
    int current_ = -1;
    std::vector<render::PickHit> highlighted_;
    std::vector<render::ItemId> tintItems_;
    render::IRenderViewport* tintViewport_ = nullptr;
    int tintChanged_ = 0, tintNew_ = 0;

    QString message_;
    bool messageIsError_ = false;
    bool built_ = false;
};

} // namespace ray::ui
