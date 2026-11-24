// The `CamSheet` QML singleton (task T-076): the setup sheet dialog of the CAM workspace
// (qml/cam/SetupSheetDialog.qml).
//
//   Open        open(setupId): the CAM tree's setup row (its Setup sheet button, beside Post), the post dialog's Setup
//               sheet button, and the command cam.setupSheet.open (Ctrl+Shift+P in the CAM workspace) on the tree's
//               current row's setup.
//   Choices     which sheet: one setup, or every setup of the document in one document (each setup on its own
//               pages); the paper (Letter or A4; the default follows the system's locale); the folder (at first the
//               folder the setup's program was last posted to) and the file name (at first the program file's name,
//               cam_setupsheet::fileNameFor), saved as .pdf or .html.
//   Gate        as for posting (cam_post::blockers through cam_setupsheet::blockers): while an operation is stale,
//               failed, generating or has no toolpath, the dialog lists it with its reason (Generate all where that is
//               the fix) and nothing is made or saved, so a sheet never describes another program than the one that
//               would be posted.
//   Picture     for each setup the view is turned, for a moment, to look at the part as it sits on the machine (from
//               the front right, above, Z up, the stock framed), with the setup's stock outlined and its work offset's
//               axes drawn, captured (IViewport::captureImage) and turned back. Retake picture does it again.
//   Preview     the sheet's pages as they print (camui::toImages, the same layout as the PDF), on a worker.
//   Saving      Save PDF (camui::toPdf) or Save HTML (the sheet itself), on a worker, atomically
//               (cam_setupsheet::write, as programs are written); an existing file is asked about first (replace, keep
//               both with a numbered name, or cancel). A file problem (a folder that is missing, unreachable or not
//               writable, a locked file) is listed in full and nothing is written, as in the post dialog.
#pragma once

#include "setupsheet_print.h"

#include <ray/api.h>
#include <ray/render.h>
#include <ray_api/cam_post.h>
#include <ray_api/cam_setupsheet.h>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlEngine>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ray::ui {

class AppContext;
class CommandCatalog;
class LineOverlay;
class SceneController;
class Session;
class SetupToolModel;
class SetupTreeModel;
class ViewportItem;

// The preview's pages, shared with the image provider (which may be asked from another thread).
struct SheetPages {
    std::mutex mutex;
    std::vector<QImage> images;
};

class SetupSheetModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamSheet)
    QML_SINGLETON

    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title NOTIFY choiceChanged)
    // The ViewportItem the pictures are taken in (the shell's Viewport.qml `view`).
    Q_PROPERTY(QObject* view READ view WRITE setView NOTIFY viewChanged)
    // [{value, text}]: each setup (value: its id) and, with two or more, "all" (every setup in one document).
    Q_PROPERTY(QVariantList choices READ choices NOTIFY choiceChanged)
    Q_PROPERTY(QString choice READ choice NOTIFY choiceChanged)
    Q_PROPERTY(QString paper READ paper NOTIFY paperChanged) // "letter" | "a4"
    // "idle", "checking", "blocked", "picturing", "making", "ready", "saving", "asking", "done", "failed", "cancelled".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // What blocks the sheet: [{opId, name, reasonText, message, canGenerate}].
    Q_PROPERTY(QVariantList blockers READ blockers NOTIFY stateChanged)
    // Errors and notes: [{kind ("error" | "note"), text}].
    Q_PROPERTY(QVariantList messages READ messages NOTIFY stateChanged)
    // The preview: the number of pages and a revision that changes with every new set (image://setupsheet/<rev>/<n>).
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pagesChanged)
    Q_PROPERTY(int revision READ revision NOTIFY pagesChanged)
    Q_PROPERTY(double pageAspect READ pageAspect NOTIFY pagesChanged) // height / width of a page
    // Field values by key: directory, stem.
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    Q_PROPERTY(QString directoryShown READ directoryShown NOTIFY valuesChanged)
    Q_PROPERTY(QString pdfName READ pdfName NOTIFY valuesChanged)
    Q_PROPERTY(QString htmlName READ htmlName NOTIFY valuesChanged)
    Q_PROPERTY(QString nameNote READ nameNote NOTIFY valuesChanged)
    Q_PROPERTY(bool canSave READ canSave NOTIFY stateChanged)
    Q_PROPERTY(QString saveBlockedReason READ saveBlockedReason NOTIFY stateChanged)
    Q_PROPERTY(QString resultPath READ resultPath NOTIFY stateChanged)
    Q_PROPERTY(QString existingName READ existingName NOTIFY stateChanged)

public:
    SetupSheetModel(SceneController* scene, Session* session, AppContext* context, QObject* parent = nullptr);
    ~SetupSheetModel() override;
    static SetupSheetModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    void setTree(SetupTreeModel* tree);
    void setSetupTool(SetupToolModel* tool);

    bool active() const { return setup_.has_value(); }
    QString title() const;
    QObject* view() const;
    void setView(QObject* view);
    QVariantList choices() const { return choices_; }
    QString choice() const { return choice_; }
    QString paper() const { return camui::paperKey(paper_); }
    QString state() const { return state_; }
    QString statusText() const { return statusText_; }
    bool busy() const;
    QVariantList blockers() const;
    QVariantList messages() const { return messages_; }
    int pageCount() const { return pageCount_; }
    int revision() const { return revision_; }
    double pageAspect() const;
    QVariantMap values() const { return values_; }
    QString directoryShown() const;
    QString pdfName() const;
    QString htmlName() const;
    QString nameNote() const { return nameNote_; }
    bool canSave() const { return saveBlockedReason().isEmpty(); }
    QString saveBlockedReason() const;
    QString resultPath() const { return resultPath_; }
    QString existingName() const { return existingName_; }

    // ---- QML ----
    Q_INVOKABLE bool open(const QString& setupId);
    Q_INVOKABLE void close();
    Q_INVOKABLE void setChoice(const QString& value);
    Q_INVOKABLE void setPaper(const QString& key);
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE void setFolder(const QVariant& urlOrPath);
    Q_INVOKABLE bool savePdf();
    Q_INVOKABLE bool saveHtml();
    Q_INVOKABLE void retakePictures();
    // "replace", "newName" or "cancel".
    Q_INVOKABLE void answerOverwrite(const QString& answer);
    Q_INVOKABLE bool generateAll();
    // Stops a save (nothing is written), or closes the dialog when idle.
    Q_INVOKABLE void cancel();

    // ---- the command catalog's hooks (cam.setupSheet.*) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    std::optional<SetupId> setupId() const { return setup_; }
    const std::string& html() const { return html_; }
    // The setups of the current choice, in program order.
    std::vector<SetupId> chosenSetups() const;
    // The pictures taken, by setup (PNG).
    const std::map<SetupId, std::vector<std::byte>>& pictures() const { return pictures_; }
    std::shared_ptr<SheetPages> pages() const { return pages_; }
    int saves() const { return saves_; }
    void setFileSystemForTest(post::IOutputFs* fs) { fs_ = fs; }

signals:
    void activeChanged();
    void viewChanged();
    void choiceChanged();
    void paperChanged();
    void stateChanged();
    void pagesChanged();
    void valuesChanged();
    void saved(const QString& path);

private:
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void loadChoices();
    void updateName(bool resetStem);
    void setState(const QString& state, const QString& status = QString());
    void scheduleRefresh(bool pictures);
    void refresh();
    void afterGate(doc::DocStatePtr state, doc::BuildResultPtr build,
                   std::shared_ptr<const std::map<OperationId, cam::OpState>> statuses);
    void takePictures(std::vector<api::cam_setupsheet::View> views, std::uint64_t serial);
    void nextPicture(std::uint64_t serial);
    void endPictures();
    void make(std::uint64_t serial);
    void layOut(std::uint64_t serial);
    bool save(bool pdf);
    void write(std::optional<api::cam_post::Overwrite> rule);
    void fail(const Error& error, const QString& status);
    QString sheetTitle() const;
    QString documentName() const;
    std::vector<std::string> libraryRoots(bool shippedOnly) const;

    QPointer<SceneController> scene_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<SetupTreeModel> tree_;
    QPointer<SetupToolModel> setupTool_;
    QPointer<ViewportItem> view_;
    post::IOutputFs* fs_ = nullptr;

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;
    cam::ICamEngine* engine_ = nullptr;
    int engineListener_ = 0;

    std::optional<SetupId> setup_;
    QVariantList choices_;
    QString choice_;
    camui::Paper paper_ = camui::Paper::A4;
    QString state_ = QStringLiteral("idle");
    QString statusText_;
    std::vector<api::cam_post::Blocker> blockers_;
    QVariantList messages_;
    QVariantMap values_;
    bool stemEdited_ = false;
    QString nameNote_;
    QString lastFolder_; // the folder a sheet was last saved to in this session
    QString resultPath_, existingName_;

    // The refresh: the gate, then the pictures (when wanted), then the HTML, then the pages.
    QTimer refreshTimer_;
    bool wantPictures_ = false;
    std::uint64_t serial_ = 0;
    JobHandle<std::map<OperationId, cam::OpState>> statusJob_;
    JobHandle<std::shared_ptr<const std::map<SetupId, Status>>> setupJob_;
    JobHandle<std::vector<api::cam_setupsheet::View>> viewsJob_;
    JobHandle<api::cam_setupsheet::Made> makeJob_;
    struct Laid {
        std::vector<QImage> images;
        int pages = 0;
    };
    JobHandle<std::shared_ptr<const Laid>> layoutJob_;
    doc::DocStatePtr madeState_;
    doc::BuildResultPtr madeBuild_;
    std::shared_ptr<const std::map<OperationId, cam::OpState>> madeStatuses_;

    // The pictures: the views still to take, the camera to turn back to, the lines drawn for the one being taken.
    std::map<SetupId, std::vector<std::byte>> pictures_;
    std::deque<api::cam_setupsheet::View> pendingViews_;
    std::optional<render::Camera> savedCamera_;
    std::unique_ptr<LineOverlay> overlay_;
    std::vector<render::ItemId> hiddenItems_;
    QTimer pictureTimeout_;
    std::uint64_t pictureSerial_ = 0;

    std::string html_;
    std::shared_ptr<SheetPages> pages_;
    int pageCount_ = 0;
    int revision_ = 0;

    // Saving.
    bool savingPdf_ = true;
    std::shared_ptr<const QByteArray> bytes_; // what is being saved
    QString savingName_;
    JobHandle<api::cam_post::Written> writeJob_;
    std::uint64_t saveSerial_ = 0;
    int saves_ = 0;
};

} // namespace ray::ui
