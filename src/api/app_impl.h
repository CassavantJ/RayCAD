// The application session behind IApp / HeadlessApp, private to src/api. app.cpp implements the session (documents,
// jobs, events, open, save, import, export); registry.cpp the action registry; builtin_actions.cpp the actions.
#pragma once

#include "files.h"
#include "json.h"
#include "ui_thread.h"

#include <ray/api.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    std::atomic<int> busy{0};                    // app jobs using the document (close refuses meanwhile)
    std::mutex saveMutex;                        // one save at a time; an import into the document holds it too
    // Recovery journal (od.journal): made under journalMutex, from the UI thread or a worker.
    std::mutex journalMutex;
    bool journalOff = false;                       // a journal write failed: no journal until a save-as
    std::unique_ptr<store::IDocumentFile> backing; // never-saved document: the file its journal belongs to
    std::string backingPath;
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
// files of never-saved documents, which no user ever sees.
void removeDocumentFiles(const std::string& path);

class AppImpl;
std::unique_ptr<IActionRegistry> makeActionRegistry(AppImpl& app);
Status registerBuiltinActions(AppImpl& app);

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
    cam::OperationRegistry* operationTypes() override { return nullptr; }
    lib::LibraryStack* libraries() override { return nullptr; }
    cam::ICamEngine* cam() override { return nullptr; }
    post::IPostEngine* posts() override { return nullptr; }
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
    void cancelJob(std::uint64_t id) override;
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

    // UI thread: executes a command on the document (recovery-journal seam), returning the new state.
    Result<doc::DocStatePtr> execute(DocEntry& entry, const doc::Command& command);
    Status undo(DocEntry& entry, bool redo);
    // UI thread: close (the busy check is the caller's). `keepJournal`: leave the journal (and backing file) for
    // recovery at the next start instead of deleting it.
    void closeEntry(DocEntry* entry, bool keepJournal = false);

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
        std::function<void()> cancel;
        std::function<void()> wait;
    };
    struct Alive {
        std::atomic<bool> ok{true};
    };

    AppImpl() = default;
    Status init(AppOptions options, std::unique_ptr<QueueDispatcher> owned);
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
    std::string userDataDir_;
    std::unique_ptr<RecentFiles> recent_;

    mutable std::mutex hookMutex_;
    ConfirmHook hook_;

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
    // Every job is registered until its continuation runs on the UI thread, so ~AppImpl can cancel and wait for it
    // (also with a job system the app does not own). Only user-visible ones are listed in activeJobs().
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
    JobHandle<T> handle = jobs_->submit<T>(std::move(info), std::move(wrapped));
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        if (auto it = tracked_.find(id); it != tracked_.end()) {
            it->second.cancel = [handle]() mutable {
                handle.cancel();
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
