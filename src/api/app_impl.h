// The application session behind IApp / HeadlessApp, private to src/api. app.cpp implements the session (documents,
// jobs, events, open, save, import, export); registry.cpp the action registry; builtin_actions.cpp the actions.
#pragma once

#include "files.h"
#include "json.h"
#include "ui_thread.h"

#include <ray/api.h>
#include <ray_api/settings.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ray::api::detail {

// Reads the objects of the document's .raycad file for the rebuild. The file behind it is swapped by save-as and
// detached when the document closes, under a lock, so a rebuild still running never reads a destroyed file.
class FileSource final : public doc::IObjectSource {
public:
    explicit FileSource(store::IObjectStore* file) : file_(file) {}
    Result<std::vector<std::byte>> object(const Hash256& hash) const override;
    void set(store::IObjectStore* file);
    void detach();

private:
    mutable std::mutex mutex_;
    store::IObjectStore* file_ = nullptr;
    bool detached_ = false;
};

// One open document and what the session keeps beside it.
struct DocEntry {
    OpenDocument od;
    std::string key; // pathKey(od.path), "" while unsaved
    std::shared_ptr<FileSource> fileSource;
    std::shared_ptr<doc::IObjectSource> objects; // od.objects, then the file
    std::shared_ptr<doc::IRebuilder> rebuilder;  // the document's own (its cache serves feature_edit::buildSnapshot)
    std::atomic<int> busy{0};                    // app jobs using the document (close refuses meanwhile)
    std::mutex saveMutex;                        // one save at a time; an import into the document holds it too
    // Recovery journal (od.journal): made under journalMutex, from the UI thread or a worker.
    std::mutex journalMutex;
    bool journalOff = false;                       // a journal write failed: no journal until a save-as
    std::unique_ptr<store::IDocumentFile> backing; // never-saved document: the file its journal belongs to
    std::string backingPath;
};

// How an action is ordered against the actions invoked before it (registry.cpp). The document commands run their
// work on workers (a sketch solve: PLAN.md 7), so one invoked later could otherwise overtake one still solving.
enum class ActionOrder {
    Free,    // runs as soon as its job runs
    Ordered, // starts after every Ordered action invoked before it has ended (document commands, undo, redo)
    After,   // the same, but later Ordered actions do not wait for it (save, close: they see the edits before them)
};

// A journal made on a worker before (or for) a document: for a never-saved document with its backing file.
struct JournalSlot {
    std::unique_ptr<store::IRecoveryJournal> journal;
    std::unique_ptr<store::IDocumentFile> backing;
    std::string backingPath;
    std::optional<Error> error; // why there is no journal (reported as a warning)
};

// What a document is built from: its object pool, the chain the rebuilder reads (pool, then the file), the rebuilder.
struct DocParts {
    std::shared_ptr<doc::ObjectPool> pool;
    std::shared_ptr<FileSource> fileSource;
    std::shared_ptr<doc::IObjectSource> chain;
    std::shared_ptr<doc::IRebuilder> rebuilder;
};

// Keeps busy > 0 while a job uses a document.
class BusyGuard {
public:
    BusyGuard() = default;
    explicit BusyGuard(DocEntry* e) : e_(e)
    {
        if (e_) {
            ++e_->busy;
        }
    }
    BusyGuard(BusyGuard&& o) noexcept : e_(o.e_) { o.e_ = nullptr; }
    BusyGuard& operator=(BusyGuard&&) = delete;
    BusyGuard(const BusyGuard&) = delete;
    ~BusyGuard()
    {
        if (e_) {
            --e_->busy;
        }
    }
    DocEntry* get() const { return e_; }

private:
    DocEntry* e_ = nullptr;
};

enum class ExportFormat { Step, Iges, Stl };

// Deletes a .raycad file with its SQLite journal and lock file (PLAN.md 4.1, 4.3), ignoring errors. Only for backing
// files of never-saved documents, which no user ever sees, and for a file a save-as created but could not finish.
void removeDocumentFiles(const std::string& path);

class AppImpl;
std::unique_ptr<IActionRegistry> makeActionRegistry(AppImpl& app);
Status registerBuiltinActions(AppImpl& app);
// settings.get, settings.set and settings.reset over `store` (settings.cpp, T-024).
Status registerSettingsActions(IActionRegistry& actions, std::shared_ptr<SettingsStore> store);

class AppImpl final : public HeadlessApp {
public:
    // `ownedDispatcher` is set for a headless app (then options.dispatcher is null).
    static Result<std::unique_ptr<AppImpl>> create(AppOptions options, std::unique_ptr<QueueDispatcher> owned);
    ~AppImpl() override;
    AppImpl(const AppImpl&) = delete;
    AppImpl& operator=(const AppImpl&) = delete;

    // ---- IApp ----
    IActionRegistry& actions() override { return *actions_; }
    doc::FeatureRegistry& featureTypes() override { return *featureTypes_; }
    const doc::CommandRegistry& commandTypes() const override { return *commands_; }
    IJobSystem& jobs() override { return *jobs_; }
    IUiDispatcher& dispatcher() override { return *dispatcher_; }
    cam::OperationRegistry* operationTypes() override { return operationTypes_.get(); }
    lib::LibraryStack* libraries() override { return libraries_.get(); }
    cam::ICamEngine* cam() override { return cam_.get(); }
    post::IPostEngine* posts() override { return posts_.get(); }
    OpenDocument* active() override;
    void setActive(OpenDocument*) override;
    std::vector<OpenDocument*> documents() override;
    OpenDocument* findDocument(std::string_view id) override;
    Result<OpenDocument*> newDocument(std::string_view templateId) override;
    JobHandle<OpenDocument*> open(const std::string& path) override;
    JobHandle<std::monostate> importFile(OpenDocument&, const std::string& path) override;
    JobHandle<std::monostate> save(OpenDocument&, std::optional<std::string> saveAsPath, bool overwrite) override;
    Status close(OpenDocument&) override;
    void setConfirmHook(ConfirmHook hook) override;
    std::vector<RecentFile> recentFiles() const override { return recent_->list(); }
    std::string userDataDir() const override { return userDataDir_; }
    std::vector<RecoveryInfo> pendingRecoveries() const override;
    JobHandle<OpenDocument*> recover(const RecoveryInfo&) override;
    Status discardRecovery(const RecoveryInfo&) override;
    std::vector<JobStatus> activeJobs() const override;
    bool cancelJob(std::uint64_t id) override;
    int subscribe(AppListener) override;
    void unsubscribe(int) override;

    // ---- HeadlessApp ----
    bool runUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout) override;
    std::size_t runPending() override;
    Status waitForBuild(OpenDocument&, std::chrono::milliseconds timeout) override;

    // ---- For the registry and the built-in actions ----
    UiBridge& bridge() { return *bridge_; }
    ConfirmHook confirmHook() const;
    // Submits `work` as a job; user-visible jobs are listed in activeJobs() with their progress until they end.
    template <class T>
    JobHandle<T> runJob(JobInfo info, std::function<Result<T>(JobContext&)> work);
    // The same for a job that runs only when its handle's runDeferred() is called (IJobSystem::defer): an entry of a
    // document's command queue (registry.cpp).
    template <class T>
    JobHandle<T> deferJob(JobInfo info, std::function<Result<T>(JobContext&)> work);
    // Any thread: registers a job started elsewhere (a toolpath of the CAM engine) like the app's own, so ~AppImpl
    // cancels and waits for it. Not listed in activeJobs().
    template <class T>
    void adoptJob(JobHandle<T> handle, std::string title);

    // Any thread: the id of the active document, "" when none is open.
    std::string activeDocumentId() const;
    // Any thread: the rebuilder of the open document `id` (null when there is none).
    std::shared_ptr<doc::IRebuilder> rebuilderOf(std::string_view id) const;
    // UI thread. The document named by `id` ("" = the active one), or NotFound.
    Result<DocEntry*> resolve(std::string_view id);
    DocEntry* entryOf(const OpenDocument&);
    // UI thread: resolve, then mark busy (for a job that continues on a worker).
    Result<BusyGuard> claim(std::string_view id);

    // The work behind open, save, import and export; they run inside a job on a worker.
    Result<OpenDocument*> openWork(const std::string& path, JobContext& ctx);
    // `documentId`: nullopt = a new document; "" = the active one (a new one if none is open); else that one.
    struct ImportOptions {
        std::optional<kernel::Heal> heal;
        doc::Body::Role role = doc::Body::Role::Part;
        std::string name; // feature name; "" = "Import <file name>"
    };
    Result<std::string> importWork(std::optional<std::string> documentId, const std::string& path,
                                   const ImportOptions& options, JobContext& ctx);
    Result<store::Commit> saveWork(DocEntry& entry, std::optional<std::string> saveAsPath, bool overwrite,
                                   JobContext& ctx);
    Status exportWork(DocEntry& entry, const std::string& path, ExportFormat format, double chordTolerance,
                      const std::vector<BodyId>* bodies, JobContext& ctx);

    // UI thread, inside the hop of a job that is about to change a document or the session (commit or execute a
    // command, add a document, run an action that is not long-running): the job's point of no return
    // (CancelToken::close on `cancel`, the job's ctx.cancel). False when the job was cancelled first: the hop must then
    // change nothing and return Error{Cancelled}. After true a cancel is too late: the job reports what the hop did.
    bool pointOfNoReturn(CancelToken& cancel);
    // Tests only: called on the UI thread just before (After = false) and just after (After = true) every
    // pointOfNoReturn() closes its token, so a test can cancel exactly there. Empty by default.
    void setNoReturnHookForTest(std::function<void(bool after)> hook);

    // UI thread: executes a command on the document (recovery-journal seam), returning the new state.
    Result<doc::DocStatePtr> execute(DocEntry& entry, const doc::Command& command);
    // UI thread: commits a command prepared on a worker (doc::Document::commit), with the same journal record as
    // execute(). InvalidArgument when the document changed after it was prepared.
    Result<doc::DocStatePtr> commit(DocEntry& entry, doc::PreparedCommand prepared);
    // Actions whose function runs on the worker of their job although they are not long-running (no progress, not
    // listed in activeJobs()): the document commands, whose sketch solves must not run on the UI thread (PLAN.md 7).
    // Marked while the built-in actions are registered, before any action runs; read-only after that.
    void runOnWorker(std::string id) { workerActions_.insert(std::move(id)); }
    bool runsOnWorker(std::string_view id) const { return workerActions_.count(id) != 0; }
    // The order of an action (ActionOrder; Free when never set). Set like runOnWorker.
    void setOrder(std::string id, ActionOrder order) { orders_[std::move(id)] = order; }
    ActionOrder orderOf(std::string_view id) const
    {
        const auto it = orders_.find(id);
        return it == orders_.end() ? ActionOrder::Free : it->second;
    }
    Status undo(DocEntry& entry, bool redo);
    // UI thread: close (the busy check is the caller's). `keepJournal`: leave the journal (and backing file) for
    // recovery at the next start instead of deleting it.
    void closeEntry(DocEntry* entry, bool keepJournal = false);
    // UI thread (ray_api/recent_files.h): drops a path from the recent-files list and posts RecentFilesChanged.
    bool forgetRecent(const std::string& path);
    // The user settings of userDataDir() (ray_api/settings.h, T-024); never null.
    std::shared_ptr<SettingsStore> settings() const { return settings_; }
    // Any thread: the state a new document starts from (the default unit system of the settings).
    doc::DocStatePtr newDocumentState() const;

    // ---- CAM, libraries and posts (cam_wiring.cpp) ----
    // Makes the operation registry, the library stack (and watches it), the CAM engine and the post engine.
    Status initCam(const AppOptions& options);
    // UI thread: the default library stack again from the settings (AppOptions::libraryRoots empty), else nothing.
    void configureLibraries();
    // The roots of the default stack now and how many of them are read-only (the built-in library first).
    std::pair<std::vector<std::string>, std::size_t> defaultLibraryRoots() const;
    // UI thread: cancels the toolpaths being generated for the operations of the state (a document closing).
    void cancelGenerations(const doc::DocStatePtr& state);
    // ~AppImpl, after the app's jobs ended: the engine, the settings listener and the library stack go.
    void shutdownCam();

    // ---- Recovery journal (recovery.cpp) ----
    Result<OpenDocument*> recoverWork(const RecoveryInfo& info, JobContext& ctx); // on a worker
    bool journaling() const { return !userDataDir_.empty(); }
    std::string unsavedDir() const;
    // Any thread: a backing file in unsavedDir() and its journal, starting from `base`.
    JournalSlot startUnsavedJournal(doc::DocStatePtr base);
    // Any thread: makes the journal of a never-saved document at its first change (base: its state() now).
    // Returns the error to warn about, if it could not.
    std::optional<Error> ensureJournal(DocEntry& entry);
    // UI thread: a journal write failed: recovery is off for the document (until a save-as), with a warning.
    void journalFailed(DocEntry& entry, const Error& error);
    void journalFailedLocked(DocEntry& entry, const Error& error); // the caller holds entry.journalMutex
    void warn(DocEntry& entry, std::string message);
    // UI thread: closes and deletes a never-saved document's backing file.
    void dropBacking(DocEntry& entry);
    // Any thread: the parts of a document over `file` (may be null), and a document from them.
    Result<DocParts> makeParts(store::IObjectStore* file);
    std::unique_ptr<doc::Document> makeDocumentFrom(const DocParts& parts, doc::DocStatePtr initial);

private:
    struct TrackedJob {
        std::uint64_t id;
        std::string title;
        double progress = -1;
        std::string stage;
        bool visible = true;
        std::function<bool()> cancel; // JobHandle::cancel
        std::function<void()> wait;
    };
    struct Alive {
        std::atomic<bool> ok{true};
    };

    AppImpl() = default;
    Status init(AppOptions options, std::unique_ptr<QueueDispatcher> owned);
    template <class T>
    JobHandle<T> trackJob(JobInfo info, std::function<Result<T>(JobContext&)> work, bool deferred);
    // UI thread. Takes ownership of `file` (may be null) and makes the document.
    Result<DocEntry*> addEntry(std::unique_ptr<store::IDocumentFile> file, doc::DocStatePtr initial, std::string path,
                               std::string title, bool readOnly, std::optional<store::LockInfo> lock);
    struct NewEntry {
        std::unique_ptr<store::IDocumentFile> file;
        doc::DocStatePtr initial;
        std::string path, title;
        bool readOnly = false;
        std::optional<store::LockInfo> lock;
        JournalSlot journal;
        std::optional<DocParts> parts;           // made already (recovery); else made over `file`
        std::unique_ptr<doc::Document> document; // made already (recovery); else made from `initial`
        bool modified = false;
        std::vector<std::string> warnings;
    };
    Result<DocEntry*> addEntry(NewEntry entry);
    void emit(AppEvent::Kind kind, std::string documentId, std::string message = {});
    void jobProgress(std::uint64_t id, double fraction, std::string_view stage);
    void jobEnded(std::uint64_t id);
    std::string untitledName();
    // UI thread: the journal record of a command the document has just made its state (execute, commit).
    void journalExecuted(DocEntry& entry, const doc::Command& command, const doc::DocStatePtr& state);

    // Members are destroyed in reverse order; ~AppImpl() first stops the jobs and closes the documents.
    std::unique_ptr<QueueDispatcher> ownedDispatcher_;
    IUiDispatcher* dispatcher_ = nullptr;
    std::unique_ptr<IJobSystem> ownedJobs_;
    IJobSystem* jobs_ = nullptr;
    std::thread::id uiThread_;
    std::unique_ptr<UiBridge> bridge_;
    std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();
    std::shared_ptr<doc::FeatureRegistry> featureTypes_;
    std::shared_ptr<const doc::CommandRegistry> commands_;
    std::unique_ptr<IActionRegistry> actions_;
    std::set<std::string, std::less<>> workerActions_;       // runOnWorker
    std::map<std::string, ActionOrder, std::less<>> orders_; // setOrder
    std::string userDataDir_;
    std::unique_ptr<RecentFiles> recent_;
    std::shared_ptr<SettingsStore> settings_;
    // CAM, libraries and posts (cam_wiring.cpp); destroyed before the job system and the dispatcher.
    std::shared_ptr<cam::OperationRegistry> operationTypes_;
    std::unique_ptr<lib::LibraryStack> libraries_;
    std::vector<std::string> explicitLibraryRoots_; // AppOptions::libraryRoots
    std::optional<std::string> builtinLibraryDir_;  // AppOptions::builtinLibraryDir
    int librarySettingsListener_ = 0;
    std::unique_ptr<cam::ICamEngine> cam_;
    std::unique_ptr<post::IPostEngine> posts_;

    mutable std::mutex hookMutex_;
    ConfirmHook hook_;
    std::function<void(bool after)> noReturnHook_; // guarded by hookMutex_

    mutable std::mutex docsMutex_; // documents_ and active_ (written on the UI thread)
    std::vector<std::unique_ptr<DocEntry>> documents_;
    DocEntry* active_ = nullptr;
    int untitledCount_ = 0;

    mutable std::mutex jobsMutex_;
    std::map<std::uint64_t, TrackedJob> tracked_;
    std::uint64_t nextJobId_ = 1;

    std::mutex listenersMutex_;
    std::map<int, AppListener> listeners_;
    int nextListener_ = 1;
};

template <class T>
JobHandle<T> AppImpl::runJob(JobInfo info, std::function<Result<T>(JobContext&)> work)
{
    return trackJob<T>(std::move(info), std::move(work), false);
}

template <class T>
JobHandle<T> AppImpl::deferJob(JobInfo info, std::function<Result<T>(JobContext&)> work)
{
    return trackJob<T>(std::move(info), std::move(work), true);
}

template <class T>
void AppImpl::adoptJob(JobHandle<T> handle, std::string title)
{
    if (!handle.valid()) {
        return;
    }
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        id = nextJobId_++;
        TrackedJob job{id, std::move(title), -1, {}, false, {}, {}};
        job.cancel = [handle]() mutable {
            return handle.cancel();
        };
        job.wait = [handle]() mutable {
            (void)handle.wait();
        };
        tracked_[id] = std::move(job);
    }
    std::shared_ptr<Alive> alive = alive_;
    handle.then([this, alive, id](Result<T>) {
        if (alive->ok.load()) {
            jobEnded(id);
        }
    });
}

template <class T>
JobHandle<T> AppImpl::trackJob(JobInfo info, std::function<Result<T>(JobContext&)> work, bool deferred)
{
    // Every job is registered until its continuation runs on the UI thread, so ~AppImpl can cancel and wait for it
    // (also with a job system the app does not own; a deferred job cancelled there ends at once). Only user-visible
    // ones are listed in activeJobs().
    const bool visible = info.userVisible;
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        id = nextJobId_++;
        tracked_[id] = TrackedJob{id, info.title, -1, {}, visible, {}, {}};
    }
    if (visible) {
        emit(AppEvent::Kind::JobsChanged, {});
    }
    std::shared_ptr<Alive> alive = alive_;
    auto wrapped = [this, alive, id, visible, work = std::move(work)](JobContext& ctx) -> Result<T> {
        if (!visible) {
            return work(ctx);
        }
        // Progress goes to the job's own listener (JobHandle::onProgress) and to activeJobs().
        Progress outer = ctx.progress;
        Progress mine([this, alive, id, outer](double fraction, std::string_view stage) mutable {
            outer.set(fraction, stage);
            if (alive->ok.load()) {
                jobProgress(id, fraction, stage);
            }
        });
        JobContext inner{ctx.cancel, mine};
        return work(inner);
    };
    JobHandle<T> handle = deferred ? jobs_->defer<T>(std::move(info), std::move(wrapped))
                                   : jobs_->submit<T>(std::move(info), std::move(wrapped));
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        if (auto it = tracked_.find(id); it != tracked_.end()) {
            it->second.cancel = [handle]() mutable {
                return handle.cancel();
            };
            it->second.wait = [handle]() mutable {
                (void)handle.wait();
            };
        }
    }
    handle.then([this, alive, id](Result<T>) {
        if (alive->ok.load()) {
            jobEnded(id);
        }
    });
    return handle;
}

// ---- Small helpers shared by the sources ----
inline tl::unexpected<Error> fail(ErrorCode code, std::string message, std::string context = {})
{
    Error e{code, std::move(message), {}};
    if (!context.empty()) {
        e.context.push_back(std::move(context));
    }
    return tl::unexpected<Error>(std::move(e));
}
// Adds a context line (innermost first) to an error and passes it on.
inline tl::unexpected<Error> withContext(Error e, std::string context)
{
    e.context.push_back(std::move(context));
    return tl::unexpected<Error>(std::move(e));
}

} // namespace ray::api::detail
