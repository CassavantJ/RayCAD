// The `CamPost` QML singleton (task T-047): the post dialog of the CAM workspace (qml/cam/PostDialog.qml).
//
//   Open        open(setupId) for a setup: the CAM tree's setup row (its Post button) and the command cam.post.open
//               (Ctrl+P in the CAM workspace) on the tree's current row's setup.
//   Choices     the post (the library's posts; the default is what was last used for the setup, else the setup's post,
//               else its machine's defaultPost), the post's declared properties (inspected on a worker; shown with
//               SchemaForm.qml through a SchemaFormModel made from ray_api/cam_post.h propertiesSchemaJson, so the
//               machine definition's values are the defaults and Reset goes back to them), the program number and
//               comment (the setup's; changed ones are written back with cam.setup.edit, one undo step), the output
//               units (the document's), the output folder, the file name pattern with a preview of the name, the
//               overwrite rule (ask, replace, new numbered name, refuse) and whether the name is cut to the machine's
//               maxProgramNameLength. Everything but the setup's own members is remembered per setup
//               (cam_post::remember, in the user data folder).
//   Notice      while the machine definition is not verified, or the post is a shipped one with no recorded machine
//               verification (its declaration's verifiedOn is empty, post_api.d.ts; none has one yet, see
//               docs/post_review), a short notice says so and to dry-run the program first. A post from the user's own
//               library is not labelled either way. It does not block posting, cannot be dismissed for good and changes
//               nothing in the file.
//   Gate        the setup's operations are checked whenever the document or the engine changes
//               (ICamEngine::statusAsync, cam_setup::setupStates, cam_post::blockers): each blocking operation is
//               listed with its reason, with Generate where generating is the fix, and Post is disabled. post() is
//               refused while anything blocks, and cam_post::run and stillCurrent apply the same gate again, so no
//               click, command or race writes a program from an operation that is not up to date.
//   Posting     on workers: the post (cam_post::run, with progress), then on the UI thread the statuses again, then
//               on a worker the re-check (stillCurrent) and the file (cam_post::write: atomic; for "ask" an existing
//               file is asked about first: replace, keep both with a numbered name, or cancel). Cancel stops it at any
//               step before the file is in place; nothing is written then. The post's errors and warnings are listed
//               in full, each with the operation it names ("section N") or the script line of a script error; an
//               error writes no file. The program then goes to the G-code viewer (CamGcode).
#pragma once

#include "operation_form.h"
#include "post_view.h"

#include <ray/api.h>
#include <ray_api/cam_post.h>

#include <QtCore/QObject>
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
class Session;
class SetupTreeModel;
struct PostRun; // post_tool.cpp: a post run's program and its viewer index

class PostToolModel : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(CamPost)
    QML_SINGLETON

    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(QString setupId READ setupIdText NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title NOTIFY activeChanged)
    // The posts on offer (file stems) and the one chosen, with what its script declares.
    Q_PROPERTY(QStringList posts READ posts NOTIFY optionsChanged)
    Q_PROPERTY(QString postId READ postId NOTIFY postChanged)
    Q_PROPERTY(QString postTitle READ postTitle NOTIFY postChanged)
    // "" (none chosen), "loading", "ok", "failed" (postMessage says why).
    Q_PROPERTY(QString postState READ postState NOTIFY postChanged)
    Q_PROPERTY(QString postMessage READ postMessage NOTIFY postChanged)
    Q_PROPERTY(SchemaFormModel* form READ form CONSTANT)
    Q_PROPERTY(QString machineName READ machineName NOTIFY activeChanged)
    Q_PROPERTY(bool machineVerified READ machineVerified NOTIFY activeChanged)
    Q_PROPERTY(bool postProven READ postProven NOTIFY postChanged)
    // "unproven": a post shipped with RayCAD that declares no control it has run on; "proven": one that declares one or
    // more (PostInfo::verifiedOn); "": a post from the user's own library (not labelled), or not read yet.
    Q_PROPERTY(QString postProof READ postProof NOTIFY postChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY postChanged) // "" when nothing is unverified
    // Field values by key: programNumber, programComment (text), units ("inch" | "metric"), directory, pattern,
    // overwrite ("ask" | "replace" | "newName" | "refuse"), limitNameLength (bool).
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY valuesChanged) // the name the pattern makes, or ""
    // The output folder as the dialog shows it at rest: under the user's home folder as "~" and the rest
    // ("~\Documents\nc"); the folder itself (values.directory) is what is written to, unchanged.
    Q_PROPERTY(QString directoryShown READ directoryShown NOTIFY valuesChanged)
    Q_PROPERTY(QString fileNameNote READ fileNameNote NOTIFY valuesChanged) // why it differs, or the pattern's error
    Q_PROPERTY(int maxNameLength READ maxNameLength NOTIFY activeChanged)
    // The gate: [{opId, name, reason (key), message, canGenerate}]; checking while it is worked out.
    Q_PROPERTY(QVariantList blockers READ blockers NOTIFY gateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY gateChanged)
    // "idle", "posting", "checking", "writing", "asking", "done", "failed", "cancelled".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    // What the post said: [{kind ("error" | "warning" | "note"), text, opId, opName, scriptLine (-1)}].
    Q_PROPERTY(QVariantList messages READ messages NOTIFY stateChanged)
    Q_PROPERTY(QString resultPath READ resultPath NOTIFY stateChanged)
    // For "asking": the file that exists.
    Q_PROPERTY(QString existingName READ existingName NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canPost READ canPost NOTIFY canPostChanged)
    Q_PROPERTY(QString postBlockedReason READ postBlockedReason NOTIFY canPostChanged)
    // T-059: the setup's cycle-time estimate as the CAM tree worked it out (camui::cycleTimeDetails: h:mm:ss, marked as
    // an estimate, with what it assumed); "" while there is none.
    Q_PROPERTY(QString cycleTime READ cycleTime NOTIFY cycleTimeChanged)

public:
    PostToolModel(SceneController* scene, Session* session, AppContext* context, QObject* parent = nullptr);
    ~PostToolModel() override;
    static PostToolModel* create(QQmlEngine* qml, QJSEngine* js);
    void setCommands(CommandCatalog* commands);
    void setTree(SetupTreeModel* tree);
    void setViewer(GcodeModel* viewer) { viewer_ = viewer; }

    bool active() const { return setup_.has_value(); }
    QString setupIdText() const;
    QString title() const;
    QStringList posts() const { return posts_; }
    QString postId() const { return postId_; }
    QString postTitle() const { return postTitle_; }
    QString postState() const { return postState_; }
    QString postMessage() const { return postMessage_; }
    SchemaFormModel* form() const { return form_; }
    QString machineName() const { return machineName_; }
    bool machineVerified() const { return machineVerified_; }
    bool postProven() const { return postProof_ == QLatin1String("proven"); }
    QString postProof() const { return postProof_; }
    // postProof for a post read from the library shipped with RayCAD (`shipped`) or from another root.
    static QString proofOf(bool shipped, const post::PostInfo& info);
    QString notice() const;
    QVariantMap values() const { return values_; }
    QString fileName() const { return fileName_; }
    QString directoryShown() const;
    // `path` with the home folder `home` shown as "~" (native separators); as it is when it is not under it.
    static QString homeRelative(const QString& path, const QString& home);
    QString fileNameNote() const { return fileNameNote_; }
    int maxNameLength() const { return maxNameLength_; }
    QVariantList blockers() const;
    bool checking() const { return checking_; }
    QString state() const { return state_; }
    double progress() const { return progress_; }
    QString statusText() const { return statusText_; }
    QVariantList messages() const { return messages_; }
    QString resultPath() const { return resultPath_; }
    QString existingName() const { return existingName_; }
    bool busy() const;
    bool canPost() const { return postBlockedReason().isEmpty(); }
    QString postBlockedReason() const;
    QString cycleTime() const;

    // ---- QML ----
    Q_INVOKABLE bool open(const QString& setupId);
    Q_INVOKABLE void close();
    Q_INVOKABLE void setPost(const QString& postId);
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    // A folder chosen in a folder dialog (a file:// URL or a path).
    Q_INVOKABLE void setFolder(const QVariant& urlOrPath);
    Q_INVOKABLE bool post();
    // Stops a post run or write (nothing is written), or closes the dialog when idle.
    Q_INVOKABLE void cancel();
    // The answer to "the file exists": "replace", "newName" or "cancel".
    Q_INVOKABLE void answerOverwrite(const QString& answer);
    Q_INVOKABLE bool generate(const QString& opId);
    Q_INVOKABLE bool generateAll();
    Q_INVOKABLE void showGcode();

    // ---- the command catalog's hooks (cam.post.*, cam.post) ----
    QString commandReason(const QString& id) const;
    bool runCommand(const QString& id);

    // ---- C++ (tests) ----
    std::optional<SetupId> setupId() const { return setup_; }
    const std::vector<api::cam_post::Blocker>& blockerList() const { return blockers_; }
    // The settings post() would use now.
    api::cam_post::Settings settings() const;
    // Post runs started, and files written, since the start.
    int runs() const { return runs_; }
    int writes() const { return writes_; }
    // The file system the writes go through (tests inject a fake; null: the real one).
    void setFileSystemForTest(post::IOutputFs* fs) { fs_ = fs; }

signals:
    void activeChanged();
    void optionsChanged();
    void postChanged();
    void valuesChanged();
    void gateChanged();
    void stateChanged();
    void canPostChanged();
    void cycleTimeChanged();
    void written(const QString& path);

private:
    api::IApp* app() const;
    doc::Document* liveDoc() const;
    void subscribe();
    void unsubscribe();
    void loadPosts();
    void inspect();
    void scheduleCheck();
    void check();
    void updateFileName();
    void setState(const QString& state, const QString& status = QString());
    void fail(const Error& error);
    void addMessages(const std::vector<std::string>& texts, const QString& kind);
    void afterPost(std::shared_ptr<const api::cam_post::Posted> posted);
    void recheckThenWrite(std::optional<api::cam_post::Overwrite> rule);
    void finish(const api::cam_post::Written& written);
    void writeBack();
    void rememberNow();
    // The library roots, lowest first; `shippedOnly`: only the library shipped with RayCAD (the read-only root).
    std::vector<std::string> libraryRoots(bool shippedOnly = false) const;
    QString documentName() const;

    QPointer<SceneController> scene_;
    QPointer<Session> session_;
    QPointer<AppContext> context_;
    QPointer<CommandCatalog> commands_;
    QPointer<SetupTreeModel> tree_;
    QPointer<GcodeModel> viewer_;
    SchemaFormModel* form_ = nullptr;
    post::IOutputFs* fs_ = nullptr;

    std::string docId_;
    doc::Document* doc_ = nullptr;
    int docListener_ = 0;
    std::uint64_t docGeneration_ = 0;
    cam::ICamEngine* engine_ = nullptr;
    int engineListener_ = 0;

    std::optional<SetupId> setup_;
    QString setupName_, machineName_;
    bool machineVerified_ = false;
    int maxNameLength_ = 0;
    QStringList posts_;
    QString postId_, postTitle_, postState_, postMessage_, postProof_;
    std::optional<post::PostInfo> info_;
    std::optional<lib::MachineDef> machine_; // the setup's embedded machine (the property defaults)
    QString rememberedPost_;                 // what was last used for the setup
    std::map<std::string, std::string> rememberedProps_;
    // A superseded answer is dropped: the post list, the post's declaration, the gate (each has its own count).
    std::uint64_t postsSerial_ = 0, inspectSerial_ = 0, checkSerial_ = 0;
    QVariantMap values_;
    QString fileName_, fileNameNote_;
    // Gate.
    QTimer checkTimer_;
    bool checking_ = false;
    std::vector<api::cam_post::Blocker> blockers_;
    JobHandle<std::map<OperationId, cam::OpState>> statusJob_;
    JobHandle<std::shared_ptr<const std::map<SetupId, Status>>> setupJob_;
    // Posting.
    QString state_ = QStringLiteral("idle");
    double progress_ = -1;
    QString statusText_;
    QVariantList messages_;
    QString resultPath_, existingName_;
    std::shared_ptr<const api::cam_post::Posted> posted_;
    std::shared_ptr<const camui::GcodeIndex> viewerIndex_;
    JobHandle<std::shared_ptr<const PostRun>> postJob_;
    JobHandle<api::cam_post::Written> writeJob_;
    JobHandle<std::map<OperationId, cam::OpState>> recheckJob_;
    std::uint64_t runSerial_ = 0;
    int runs_ = 0, writes_ = 0;
};

} // namespace ray::ui
