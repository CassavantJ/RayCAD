// RayCAD interface contract: application API (the one API everything goes through)
// Module: src/api   Depends on: core, doc, cam, lib, sim, post, store
// Status: PLAN DRAFT, revised by T-017 (asynchronous open/save, several open documents, app events and jobs,
// AppOptions/makeApp for the GUI, HeadlessApp; CAM, libraries and post are null until M2); crash recovery wired in
// (a recovery journal per document, pendingRecoveries/recover/discardRecovery, Warning events).
//
// The UI, the command palette, keyboard shortcuts, scripts (v2) and the MCP server (v2) all call this
// and nothing else. Built-in features and operations are registered through the same registries a
// script would use, so the API is never second-class (differentiator 7).
//
// Threads (PLAN.md 7): "the UI thread" is the thread that created the app; with AppOptions::dispatcher it is the
// thread that runs that dispatcher. Everything in IApp and OpenDocument is called on the UI thread unless noted.
// Nothing here throws.
#pragma once
#include "core.h"
#include "document.h"
#include "cam.h"
#include "libraries.h"
#include "sim.h"
#include "post.h"
#include "store.h"
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>

namespace ray::api {

// ---------- Actions: anything a user can invoke ----------
// Document-changing actions wrap a doc::Command. Others (view, file, simulate, post) are app actions.
struct ActionInfo {
    std::string id;                 // "cam.operation.add", "view.fit", "file.save"
    std::string title, category, description;
    std::string argsSchemaJson;     // JSON Schema; empty args allowed when every property has a default
    std::string defaultShortcut;    // "Ctrl+S"; remappable
    bool changesDocument = false;
    bool changesToolpathsOrGcode = false;   // automation callers must obtain user confirmation first
    bool longRunning = false;               // returns a job; shows progress; Esc cancels
};
enum class Caller { User, Script, Agent };
struct ActionResult { std::string resultJson; std::vector<std::string> warnings; };

// The function behind an action. It receives the arguments validated against the schema, with the defaults of
// missing top-level properties filled in, as canonical JSON. It runs inside the job invoke() returns: on a worker
// when ActionInfo::longRunning, otherwise on the UI thread (posted through the app's dispatcher).
using ActionFn = std::function<Result<ActionResult>(std::string_view argsJson, Caller, JobContext&)>;

class IActionRegistry {
public:
    virtual ~IActionRegistry() = default;
    // InvalidArgument for an empty id, an id already registered, a null function or a schema that is not a JSON
    // object (an empty schema means "no arguments"). Thread-safe; actions are never removed.
    virtual Status add(ActionInfo, ActionFn) = 0;
    virtual std::vector<ActionInfo> all() const = 0;                             // sorted by id; feeds the palette and MCP tool list
    virtual std::optional<ActionInfo> find(std::string_view id) const = 0;
    // Fuzzy match on id, title, category and description. The ranking is a pure function of the query and the
    // registered actions (ties broken by id), so it is identical in every run. An empty query lists all(); a
    // limit of 0 or less means no limit.
    virtual std::vector<ActionInfo> search(std::string_view query, int limit) const = 0;
    // Validates args against the schema, enforces the confirmation rule for Caller::Script/Agent, runs it.
    // Never blocks the calling thread (any thread). Errors come back through the handle: NotFound for an unknown
    // id; InvalidArgument for arguments that fail the schema, with the JSON Pointer of the bad field in the
    // message (e.g. `/path`); Cancelled when the confirm hook declined (the action then did not run).
    // userVisible jobs (long-running ones) are listed in IApp::activeJobs while they run.
    virtual JobHandle<ActionResult> invoke(std::string_view id, std::string_view argsJson, Caller) = 0;
};

// Confirmation hook: for Caller != User and changesToolpathsOrGcode, the app shows what will change
// (a diff of operations and parameters) and waits. Headless test runs install an auto-approve hook.
// Called on the UI thread. Without a hook such invocations are declined.
using ConfirmHook = std::function<bool(const ActionInfo&, std::string_view argsJson, std::string_view humanSummary)>;

// ---------- Application session ----------
// One open document. The app owns it; the pointer stays valid until IApp::close (or the app's destruction).
// Fields are written by the app on the UI thread only (path, title and file change on save-as).
struct OpenDocument {
    std::string id;                                     // session id (a UUID), the "document" argument of actions
    std::string path;                                   // UTF-8 .raycad path; empty until saved for the first time
    std::string title;                                  // file name without folder, or "Untitled N"
    bool readOnly = false;                              // a verified snapshot copy: someone else holds the lock
    std::optional<store::LockInfo> lock;                // who holds the lock, when readOnly
    std::unique_ptr<store::IDocumentFile> file;         // null until saved
    std::shared_ptr<doc::ObjectPool> objects;           // imported BREP objects (written into `file` by save)
    std::unique_ptr<doc::Document> document;
    // The crash-recovery journal (PLAN.md 4.2): every executed command, undo, redo and imported object since the
    // last save. Null for a read-only document, when nothing is persisted (AppOptions::userDataDir ""), and for a
    // never-saved document until its first change (it then journals into a backing file, see IApp::newDocument).
    // After a journal write failed it is switched off (a Warning event says so) and stays so until a save-as.
    std::unique_ptr<store::IRecoveryJournal> journal;
    // Set after a recovery whose state is not the file's head although document->dirty() is false (a journalled
    // undo past the last save): show the document as modified while `modified || document->dirty()`. Cleared by save.
    bool modified = false;
    std::vector<std::string> warnings;                  // the messages of the Warning events about this document
};

// A journal left by a session that ended without saving or closing (a crash, a kill, a power cut).
struct RecoveryInfo {
    std::string id;                                     // the journal's path: the key for recover/discardRecovery
    std::string filePath;                               // the .raycad document (for `unsaved`, its backing file)
    bool unsaved = false;                               // never saved by the user: recovers as an untitled document
    std::string title;                                  // the file name without folder, or "Untitled"
    std::size_t steps = 0;                              // journalled changes (commands, undos, redos, snapshots)
    std::int64_t lastWriteMs = 0;                       // ms since 1970
    bool damagedTail = false;                           // the last record was torn: the changes before it recover
};

// Delivered on the UI thread, posted through the dispatcher (never inside the call that caused it).
struct AppEvent {
    enum class Kind { DocumentOpened, DocumentClosed, ActiveDocumentChanged, DocumentSaved, JobsChanged, RecentFilesChanged,
                      Warning };
    Kind kind;
    std::string documentId;                             // empty for JobsChanged and RecentFilesChanged
    std::string message;                                // Warning: user-presentable text (also in OpenDocument::warnings)
};
using AppListener = std::function<void(const AppEvent&)>;

// A user-visible job the app runs (open, save, import, export, any long-running action).
struct JobStatus {
    std::uint64_t id;
    std::string title;
    double progress;                                    // 0..1, or -1 before the first progress report
    std::string stage;
};

class IApp {
public:
    virtual ~IApp() = default;   // cancels and waits for running jobs, then closes every document without saving
    virtual IActionRegistry& actions() = 0;
    // Feature types and commands: add custom types only before the first document is created or opened.
    virtual doc::FeatureRegistry& featureTypes() = 0;
    virtual const doc::CommandRegistry& commandTypes() const = 0;
    virtual IJobSystem& jobs() = 0;
    virtual IUiDispatcher& dispatcher() = 0;
    // CAM, tool libraries, simulation and post processing arrive in M2 (PLAN.md 13): until then these are null.
    virtual cam::OperationRegistry* operationTypes() = 0;
    virtual lib::LibraryStack* libraries() = 0;
    virtual cam::ICamEngine* cam() = 0;
    virtual post::IPostEngine* posts() = 0;

    // ---- Documents ----
    virtual OpenDocument* active() = 0;                                // null when none is open
    virtual void setActive(OpenDocument*) = 0;                         // null or one of documents()
    virtual std::vector<OpenDocument*> documents() = 0;                // in the order they were opened
    virtual OpenDocument* findDocument(std::string_view id) = 0;
    // A new, empty, unsaved document that becomes active. templateId "" is the default (inch); others NotFound.
    // Never-saved documents (this one, and STEP/IGES opens) are protected by the journal too: at their first change
    // the app creates a backing .raycad file in <user data dir>/unsaved/ that holds no version and exists only for the
    // journal (OpenDocument::path stays empty; save still asks for a name, and save-as deletes the backing file).
    virtual Result<OpenDocument*> newDocument(std::string_view templateId) = 0;
    // .raycad: opens the file (read-only with OpenDocument::lock set when another session holds it) and rebuilds
    // from the stored objects; a file this app has open already is just made active. STEP/IGES (.step .stp .p21
    // .iges .igs): a new unsaved document with one import feature. The result becomes active. Errors: NotFound
    // (no such file), FormatError/FormatTooNew (not a RayCAD file, damaged, newer), InvalidArgument (unknown
    // file type), the kernel's import errors, Cancelled.
    // A .raycad file with changes from a session that ended unexpectedly is NOT opened: open fails with Locked
    // ("... recover or discard them first") and pendingRecoveries() lists the journal (match RecoveryInfo::filePath);
    // open succeeds once recover() or discardRecovery() dealt with it. Opened read-write, a document starts its
    // journal at once; read-only documents have none.
    virtual JobHandle<OpenDocument*> open(const std::string& path) = 0;
    // Imports a STEP/IGES file into the document as one undoable import feature.
    virtual JobHandle<std::monostate> importFile(OpenDocument&, const std::string& path) = 0;
    // Saves a commit with every imported BREP object the state needs. With saveAsPath (or for a document never
    // saved) writes a new .raycad file there and the document continues in it; an existing file there is
    // InvalidArgument unless `overwrite` (then it must be a RayCAD file nobody has open). A read-only document can
    // only be saved as a new file (Locked otherwise). Edits made while the save runs stay dirty.
    virtual JobHandle<std::monostate> save(OpenDocument&, std::optional<std::string> saveAsPath = {}, bool overwrite = false) = 0;
    // Closes without saving (asking about unsaved changes is the caller's business) and deletes its recovery
    // journal: call it for a clean close and for "don't save", not when the user cancelled closing. Locked while a
    // job of the app is using the document (a save, an import, an export). The app's destructor, by contrast, keeps
    // the journals of documents with unsaved changes, so they are offered at the next start.
    virtual Status close(OpenDocument&) = 0;

    // ---- Crash recovery (PLAN.md 4.2) ----
    // Journals no running session has open, newest first; empty when nothing is persisted. Call at start-up.
    virtual std::vector<RecoveryInfo> pendingRecoveries() const = 0;
    // Opens the document read-write (breaking its lock if the owner is provably dead or the lock is stale: the user
    // asked for the recovery), restores the journalled objects and replays the journal; the document becomes active
    // and keeps journalling. If replay stopped early, a Warning ("N of M changes recovered: ...") is raised.
    // An `unsaved` document recovers untitled (path empty). Errors: NotFound (journal or file gone), Locked (the file
    // is open in a live session, or in this app), InvalidArgument (the file was saved again after the journal began).
    virtual JobHandle<OpenDocument*> recover(const RecoveryInfo&) = 0;
    // Deletes the journal (and an `unsaved` document's backing file): the user declined. Locked if it is in use.
    virtual Status discardRecovery(const RecoveryInfo&) = 0;

    virtual void setConfirmHook(ConfirmHook) = 0;
    // Most recent first, at most 20; persisted per user in <user data dir>/recent-files.json. Thumbnails are empty
    // in M1. Updated by open and save.
    struct RecentFile { std::string path; std::int64_t openedMs; std::vector<std::byte> thumbnailPng; };
    virtual std::vector<RecentFile> recentFiles() const = 0;
    // The per-user data folder (UTF-8), or "" when nothing is persisted (AppOptions::userDataDir).
    virtual std::string userDataDir() const = 0;

    // ---- Jobs and events ----
    virtual std::vector<JobStatus> activeJobs() const = 0;            // sorted by id (start order)
    virtual void cancelJob(std::uint64_t id) = 0;                     // Esc; unknown ids are ignored
    virtual int subscribe(AppListener) = 0;                           // 0 if the listener is empty
    virtual void unsubscribe(int) = 0;
};

struct AppOptions {
    // The UI thread's dispatcher (the GUI passes one that posts into the Qt event loop). It must outlive the app.
    // Null for a headless app: the app then owns a queue that HeadlessApp::runUntil drains.
    IUiDispatcher* dispatcher = nullptr;
    // A job system to use instead of making one (it must outlive the app and dispatch to `dispatcher`).
    IJobSystem* jobs = nullptr;
    int workerThreads = 0;                              // for the job system the app makes; 0 = one per hardware thread
    std::vector<std::string> libraryRoots;              // lowest first; unused until M2
    // nullopt: $RAYCAD_USER_DATA_DIR if set, else the platform's per-user application data folder
    // (%LOCALAPPDATA%\RayCAD, ~/Library/Application Support/RayCAD, $XDG_DATA_HOME/raycad or ~/.local/share/raycad),
    // resolved at run time. "": nothing is persisted.
    std::optional<std::string> userDataDir;
};

// The app driven by a UI thread with its own dispatcher (AppOptions::dispatcher must not be null). Null when it
// is null or the job system cannot be made.
std::unique_ptr<IApp> makeApp(AppOptions);

// Headless: the app owns its dispatcher; the thread that made it plays the UI thread and drains the queue.
class HeadlessApp : public IApp {
public:
    // Runs posted UI work on the calling thread until done() returns true or `timeout` passes; returns done().
    virtual bool runUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout) = 0;
    virtual std::size_t runPending() = 0;                // runs what is queued now; returns how many ran
    // Drives the queue until the job is done, then returns its result (Internal on timeout; the job keeps running).
    template <class T>
    Result<T> wait(JobHandle<T> job, std::chrono::milliseconds timeout = std::chrono::minutes(10)) {
        if (!runUntil([&job] { return job.done(); }, timeout)) {
            return tl::unexpected<Error>(Error{ErrorCode::Internal, "Timed out waiting for a job", {}});
        }
        return job.wait();
    }
    // Drives the queue until the document has no rebuild pending; returns its buildError() if the rebuild did not
    // finish, Internal on timeout. Failed features are reported in build(), not here.
    virtual Status waitForBuild(OpenDocument&, std::chrono::milliseconds timeout = std::chrono::minutes(10)) = 0;
};

// Headless entry point used by tests, golden-file tools and (v2) the MCP server: no Qt, no window.
std::unique_ptr<HeadlessApp> makeHeadlessApp(std::vector<std::string> libraryRoots, int workerThreads);
std::unique_ptr<HeadlessApp> makeHeadlessApp(AppOptions);   // AppOptions::dispatcher must be null

// ---------- Queries (read-only, safe from any thread on a snapshot) ----------
// Canonical JSON: {"bodies":[...],"features":[...],"parameters":[...],"rollback"?:"<feature id>","settings":{...},
// "stateHash":"<hex>"} ("rollback" only while the history is rolled back); every feature with its build status, every body with its built shape's content hash and sub-shape counts. It holds
// nothing that depends on timing, so it is identical for identical inputs.
std::string describeDocumentJson(const doc::DocState&, const doc::BuildResult&);     // feature tree, bodies, params
std::string describeCamJson(const doc::DocState&, const std::map<OperationId, cam::OpState>&);

} // namespace ray::api
