// The document: current snapshot, unlimited undo and redo, previews, rebuild scheduling and change notifications
// (<ray/document.h>, "The document").
//
// Rebuild jobs: every change of state() cancels the running rebuild and submits one for the new snapshot, tagged
// with a generation number. Its continuation runs on the UI thread (JobHandle::then) and publishes the result only
// if no newer rebuild was scheduled since; a superseded result is dropped (its finished features stay in the
// rebuilder's cache). The preview rebuild works the same way with its own generation. Continuations hold the
// document's shared state weakly, so a job that finishes after the document is gone does nothing.
#include "document_impl.h"

#include <exception>
#include <mutex>
#include <new>
#include <utility>

namespace ray::doc {
namespace {

tl::unexpected<Error> failure(ErrorCode code, std::string message)
{
    return tl::unexpected(Error{code, std::move(message), {}});
}

struct UndoEntry {
    Command command; // arguments in canonical form
    DocStatePtr before;
    DocStatePtr after;
};

struct Preview {
    Command command;
    DocStatePtr state;          // null if the command failed
    std::optional<Error> error; // set if the command failed
    BuildResultPtr build;       // the finished rebuild of `state`
    JobHandle<BuildResultPtr> job;
    std::uint64_t generation = 0;
};

// Everything the document owns that a posted notification or a job continuation may still read after the document
// is gone.
struct Shared {
    mutable std::mutex mutex;
    DocStatePtr current;
    DocStatePtr clean;
    std::vector<UndoEntry> undo;
    std::vector<UndoEntry> redo;
    std::optional<Preview> preview;
    std::uint64_t previewGeneration = 0;
    std::map<int, std::shared_ptr<const IDocument::Listener>> listeners;
    std::map<int, std::shared_ptr<const Document::PreviewListener>> previewListeners;
    int nextListener = 1;

    BuildResultPtr build;            // latest published build
    DocStatePtr builtState;          // the snapshot `build` was made from
    std::optional<Error> buildError; // why the last rebuild of `current` did not finish
    DocStatePtr building;            // the snapshot being rebuilt; null when no rebuild is pending
    JobHandle<BuildResultPtr> buildJob;
    std::uint64_t buildGeneration = 0;
};

// Runs the handler on the canonical arguments (in the job `job`, when given: Document::prepare); never throws.
Result<DocStatePtr> applyCommand(const CommandRegistry& registry, const Command& command, const DocStatePtr& before,
                                 std::string& canonicalArgs, JobContext* job = nullptr)
{
    const ICommandHandler* handler = registry.find(command.name);
    if (handler == nullptr) {
        return failure(ErrorCode::NotFound, "Unknown command '" + command.name + "'");
    }
    canonicalArgs = canonicalJson(command.argsJson);
    if (canonicalArgs.empty()) {
        return failure(ErrorCode::InvalidArgument, command.name + ": the arguments are not valid JSON");
    }
    Result<DocStatePtr> after = [&]() -> Result<DocStatePtr> {
        try {
            return job != nullptr ? handler->applyWithJob(before, canonicalArgs, *job)
                                  : handler->apply(before, canonicalArgs);
        } catch (const std::bad_alloc&) {
            return failure(ErrorCode::Internal, command.name + ": out of memory");
        } catch (...) {
            return failure(ErrorCode::Internal, command.name + ": the command handler threw an exception");
        }
    }();
    if (after && !*after) {
        return failure(ErrorCode::Internal, command.name + ": the command handler returned no snapshot");
    }
    return after;
}

// Caller holds the mutex.
template <class Map>
std::vector<int> idsOf(const Map& listeners)
{
    std::vector<int> ids;
    ids.reserve(listeners.size());
    for (const auto& entry : listeners) {
        ids.push_back(entry.first);
    }
    return ids;
}

// Posts one notification. It calls, once each, the document listeners that were subscribed at the time of the
// change (`ids`) and still are when it runs, with `state` and the build published at that moment.
void notify(IUiDispatcher& dispatcher, const std::shared_ptr<Shared>& s, DocStatePtr state, std::vector<int> ids)
{
    if (ids.empty()) {
        return;
    }
    try {
        std::weak_ptr<Shared> weak = s;
        dispatcher.post([weak, state = std::move(state), ids = std::move(ids)]() {
            const std::shared_ptr<Shared> shared = weak.lock();
            if (!shared) {
                return; // the document is gone
            }
            for (const int id : ids) {
                std::shared_ptr<const IDocument::Listener> listener;
                BuildResultPtr build;
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    const auto it = shared->listeners.find(id);
                    if (it == shared->listeners.end()) {
                        continue; // unsubscribed in the meantime
                    }
                    listener = it->second;
                    build = shared->build;
                }
                try {
                    (*listener)(state, build);
                } catch (...) {
                    log(LogLevel::Error, "doc", "A document change listener threw an exception; it was ignored");
                }
            }
        });
    } catch (...) {
        log(LogLevel::Error, "doc", "Could not post a document change notification: out of memory");
    }
}

// The same for the preview listeners, with the preview snapshot and its build.
void notifyPreview(IUiDispatcher& dispatcher, const std::shared_ptr<Shared>& s, DocStatePtr state, BuildResultPtr build,
                   std::vector<int> ids)
{
    if (ids.empty()) {
        return;
    }
    try {
        std::weak_ptr<Shared> weak = s;
        dispatcher.post([weak, state = std::move(state), build = std::move(build), ids = std::move(ids)]() {
            const std::shared_ptr<Shared> shared = weak.lock();
            if (!shared) {
                return;
            }
            for (const int id : ids) {
                std::shared_ptr<const Document::PreviewListener> listener;
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    const auto it = shared->previewListeners.find(id);
                    if (it == shared->previewListeners.end()) {
                        continue;
                    }
                    listener = it->second;
                }
                try {
                    (*listener)(state, build);
                } catch (...) {
                    log(LogLevel::Error, "doc", "A preview listener threw an exception; it was ignored");
                }
            }
        });
    } catch (...) {
        log(LogLevel::Error, "doc", "Could not post a preview notification: out of memory");
    }
}

// UI thread: the rebuild of generation `generation` (of `state`) finished.
void finishBuild(IUiDispatcher& dispatcher, const std::weak_ptr<Shared>& weak, std::uint64_t generation,
                 const DocStatePtr& state, Result<BuildResultPtr> result)
{
    const std::shared_ptr<Shared> shared = weak.lock();
    if (!shared) {
        return;
    }
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (generation != shared->buildGeneration) {
            return; // a newer rebuild was scheduled; this one was cancelled or is stale
        }
        shared->building.reset();
        shared->buildJob = {};
        if (!result || !*result) {
            shared->buildError =
                result ? Error{ErrorCode::Internal, "The rebuild returned no result", {}} : std::move(result.error());
            if (shared->buildError->code != ErrorCode::Cancelled) {
                log(LogLevel::Error, "doc", "The rebuild failed: " + shared->buildError->message);
            }
            return;
        }
        shared->build = std::move(*result);
        shared->builtState = state;
        shared->buildError.reset();
        ids = idsOf(shared->listeners);
    }
    notify(dispatcher, shared, state, std::move(ids));
}

// UI thread: the preview rebuild of generation `generation` finished.
void finishPreview(IUiDispatcher& dispatcher, const std::weak_ptr<Shared>& weak, std::uint64_t generation,
                   Result<BuildResultPtr> result)
{
    const std::shared_ptr<Shared> shared = weak.lock();
    if (!shared) {
        return;
    }
    std::vector<int> ids;
    DocStatePtr state;
    BuildResultPtr build;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (!shared->preview || shared->preview->generation != generation) {
            return; // the preview was replaced or ended
        }
        shared->preview->job = {};
        if (!result || !*result) {
            if (!result && result.error().code != ErrorCode::Cancelled) {
                log(LogLevel::Warn, "doc", "The preview rebuild failed: " + result.error().message);
            }
            return;
        }
        shared->preview->build = std::move(*result);
        state = shared->preview->state;
        build = shared->preview->build;
        ids = idsOf(shared->previewListeners);
    }
    notifyPreview(dispatcher, shared, std::move(state), std::move(build), std::move(ids));
}

class DocumentImpl final : public Document {
public:
    DocumentImpl(IUiDispatcher& dispatcher, IJobSystem* jobs, std::shared_ptr<IRebuilder> rebuilder,
                 std::shared_ptr<const CommandRegistry> registry, DocStatePtr initial)
        : dispatcher_(dispatcher), jobs_(jobs), rebuilder_(jobs != nullptr ? std::move(rebuilder) : nullptr),
          registry_(std::move(registry)), s_(std::make_shared<Shared>())
    {
        s_->current = initial;
        s_->clean = std::move(initial);
        std::lock_guard<std::mutex> lock(s_->mutex);
        scheduleRebuildLocked();
    }

    ~DocumentImpl() override
    {
        try {
            std::lock_guard<std::mutex> lock(s_->mutex);
            s_->buildJob.cancel();
            dropPreviewLocked();
        } catch (...) {
        }
    }

    DocumentImpl(const DocumentImpl&) = delete;
    DocumentImpl& operator=(const DocumentImpl&) = delete;

    DocStatePtr state() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->current;
    }

    BuildResultPtr build() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->build;
    }

    Result<DocStatePtr> execute(const Command& command) override
    {
        try {
            DocStatePtr after;
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> lock(s_->mutex);
                std::string args;
                auto result = applyCommand(*registry_, command, s_->current, args);
                if (!result) {
                    return result;
                }
                after = std::move(*result);
                // Everything that can fail happens before the first change, so a failure leaves no trace.
                ids = installLocked(Command{command.name, std::move(args), command.label}, s_->current, after);
            }
            notify(dispatcher_, s_, after, std::move(ids));
            return after;
        } catch (const std::bad_alloc&) {
            return failure(ErrorCode::Internal, command.name + ": out of memory");
        } catch (...) {
            return failure(ErrorCode::Internal, command.name + ": unexpected failure");
        }
    }

    Result<PreparedCommand> prepare(const Command& command, JobContext& ctx) const override
    {
        try {
            const DocStatePtr before = state();
            // Outside the mutex: the snapshot is immutable, and the UI thread must not wait for the solve.
            std::string args;
            auto result = applyCommand(*registry_, command, before, args, &ctx);
            if (!result) {
                return tl::unexpected(std::move(result.error()));
            }
            return PreparedCommand{Command{command.name, std::move(args), command.label}, before, std::move(*result)};
        } catch (const std::bad_alloc&) {
            return failure(ErrorCode::Internal, command.name + ": out of memory");
        } catch (...) {
            return failure(ErrorCode::Internal, command.name + ": unexpected failure");
        }
    }

    Result<DocStatePtr> commit(PreparedCommand prepared) override
    {
        try {
            DocStatePtr after = prepared.after;
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> lock(s_->mutex);
                if (!prepared.before || !after || prepared.before != s_->current) {
                    return failure(ErrorCode::InvalidArgument,
                                   prepared.command.name +
                                       ": the document changed after the command was prepared; prepare it again");
                }
                ids = installLocked(std::move(prepared.command), s_->current, after);
            }
            notify(dispatcher_, s_, after, std::move(ids));
            return after;
        } catch (const std::bad_alloc&) {
            return failure(ErrorCode::Internal, prepared.command.name + ": out of memory");
        } catch (...) {
            return failure(ErrorCode::Internal, prepared.command.name + ": unexpected failure");
        }
    }

    bool canUndo() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return !s_->undo.empty();
    }

    bool canRedo() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return !s_->redo.empty();
    }

    Status undo() override { return step(true); }
    Status redo() override { return step(false); }

    void beginPreview(const Command& command) override
    {
        try {
            std::lock_guard<std::mutex> lock(s_->mutex);
            dropPreviewLocked();
            Preview preview{command, nullptr, std::nullopt, nullptr, {}, ++s_->previewGeneration};
            std::string args;
            auto result = applyCommand(*registry_, command, s_->current, args);
            preview.command.argsJson = std::move(args);
            if (result) {
                preview.state = std::move(*result);
            } else {
                preview.error = std::move(result.error());
            }
            s_->preview = std::move(preview);
            if (s_->preview->state && rebuilder_) {
                const std::uint64_t generation = s_->preview->generation;
                auto job = submitRebuild(s_->preview->state, "Preview");
                s_->preview->job = job;
                std::weak_ptr<Shared> weak = s_;
                IUiDispatcher* dispatcher = &dispatcher_;
                job.then([dispatcher, weak, generation](Result<BuildResultPtr> built) {
                    finishPreview(*dispatcher, weak, generation, std::move(built));
                });
            }
        } catch (...) {
            // Out of memory: no preview is shown. dropPreviewLocked() ran first, so the old one is not left behind.
            try {
                std::lock_guard<std::mutex> lock(s_->mutex);
                dropPreviewLocked();
            } catch (...) {
            }
            log(LogLevel::Warn, "doc", "Could not start a preview: out of memory");
        }
    }

    void cancelPreview() override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        dropPreviewLocked();
    }

    bool dirty() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->current != s_->clean;
    }

    int subscribe(Listener listener) override
    {
        if (!listener) {
            return 0;
        }
        try {
            auto shared = std::make_shared<const Listener>(std::move(listener));
            std::lock_guard<std::mutex> lock(s_->mutex);
            const int id = s_->nextListener++;
            s_->listeners.emplace(id, std::move(shared));
            return id;
        } catch (...) {
            return 0;
        }
    }

    void unsubscribe(int id) override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->listeners.erase(id);
    }

    DocStatePtr previewState() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->preview ? s_->preview->state : nullptr;
    }

    std::optional<Error> previewError() const override
    {
        try {
            std::lock_guard<std::mutex> lock(s_->mutex);
            return s_->preview ? s_->preview->error : std::nullopt;
        } catch (...) {
            return Error{ErrorCode::Internal, "Out of memory", {}};
        }
    }

    BuildResultPtr previewBuild() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->preview ? s_->preview->build : nullptr;
    }

    int subscribePreview(PreviewListener listener) override
    {
        if (!listener) {
            return 0;
        }
        try {
            auto shared = std::make_shared<const PreviewListener>(std::move(listener));
            std::lock_guard<std::mutex> lock(s_->mutex);
            const int id = s_->nextListener++;
            s_->previewListeners.emplace(id, std::move(shared));
            return id;
        } catch (...) {
            return 0;
        }
    }

    void unsubscribePreview(int id) override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->previewListeners.erase(id);
    }

    DocStatePtr builtState() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->builtState;
    }

    bool rebuildPending() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->building != nullptr;
    }

    std::optional<Error> buildError() const override
    {
        try {
            std::lock_guard<std::mutex> lock(s_->mutex);
            return s_->buildError;
        } catch (...) {
            return Error{ErrorCode::Internal, "Out of memory", {}};
        }
    }

    void cancelRebuild() override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        if (s_->building) {
            s_->buildJob.cancel(); // the continuation records Error{Cancelled}
        }
    }

    void scheduleRebuild() override
    {
        try {
            std::lock_guard<std::mutex> lock(s_->mutex);
            scheduleRebuildLocked();
        } catch (...) {
            log(LogLevel::Error, "doc", "Could not schedule a rebuild: out of memory");
        }
    }

    std::size_t undoCount() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->undo.size();
    }

    std::size_t redoCount() const override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        return s_->redo.size();
    }

    std::optional<Command> undoCommand() const override { return topCommand(true); }
    std::optional<Command> redoCommand() const override { return topCommand(false); }

    void markClean() override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->clean = s_->current;
    }

private:
    JobHandle<BuildResultPtr> submitRebuild(DocStatePtr state, const char* title)
    {
        std::shared_ptr<IRebuilder> rebuilder = rebuilder_;
        return jobs_->submit<BuildResultPtr>(
            JobInfo{title, JobPriority::Interactive, false},
            [rebuilder, state = std::move(state)](JobContext& ctx) { return rebuilder->rebuild(state, ctx); });
    }

    // Caller holds the mutex. Cancels the running rebuild and schedules one of the current snapshot.
    void scheduleRebuildLocked()
    {
        if (!rebuilder_) {
            return;
        }
        s_->buildJob.cancel();
        const std::uint64_t generation = ++s_->buildGeneration;
        DocStatePtr state = s_->current;
        s_->building = state;
        s_->buildError.reset();
        s_->buildJob = submitRebuild(state, "Rebuild");
        std::weak_ptr<Shared> weak = s_;
        IUiDispatcher* dispatcher = &dispatcher_;
        s_->buildJob.then([dispatcher, weak, generation, state](Result<BuildResultPtr> built) {
            finishBuild(*dispatcher, weak, generation, state, std::move(built));
        });
    }

    // Caller holds the mutex. Makes `after` the current state as the result of `command` (canonical arguments) on
    // `before`: one undo entry, redo cleared, the preview ended, a rebuild scheduled. Returns the listeners to notify.
    std::vector<int> installLocked(Command command, DocStatePtr before, const DocStatePtr& after)
    {
        std::vector<int> ids = idsOf(s_->listeners);
        s_->undo.push_back(UndoEntry{std::move(command), std::move(before), after});
        s_->redo.clear();
        s_->current = after;
        dropPreviewLocked();
        scheduleRebuildLocked();
        return ids;
    }

    // Caller holds the mutex.
    void dropPreviewLocked()
    {
        if (s_->preview) {
            s_->preview->job.cancel();
            s_->preview.reset();
        }
    }

    Status step(bool isUndo)
    {
        try {
            DocStatePtr now;
            std::vector<int> ids;
            {
                std::lock_guard<std::mutex> lock(s_->mutex);
                std::vector<UndoEntry>& from = isUndo ? s_->undo : s_->redo;
                std::vector<UndoEntry>& to = isUndo ? s_->redo : s_->undo;
                if (from.empty()) {
                    return failure(ErrorCode::InvalidArgument, isUndo ? "Nothing to undo" : "Nothing to redo");
                }
                ids = idsOf(s_->listeners);
                to.reserve(to.size() + 1); // the push_back below cannot fail after this
                UndoEntry entry = std::move(from.back());
                from.pop_back();
                s_->current = isUndo ? entry.before : entry.after;
                to.push_back(std::move(entry));
                dropPreviewLocked();
                now = s_->current;
                scheduleRebuildLocked();
            }
            notify(dispatcher_, s_, now, std::move(ids));
            return {};
        } catch (...) {
            return failure(ErrorCode::Internal, isUndo ? "Undo failed: out of memory" : "Redo failed: out of memory");
        }
    }

    std::optional<Command> topCommand(bool isUndo) const
    {
        try {
            std::lock_guard<std::mutex> lock(s_->mutex);
            const std::vector<UndoEntry>& stack = isUndo ? s_->undo : s_->redo;
            if (stack.empty()) {
                return std::nullopt;
            }
            return stack.back().command;
        } catch (...) {
            return std::nullopt;
        }
    }

    IUiDispatcher& dispatcher_;
    IJobSystem* jobs_;
    std::shared_ptr<IRebuilder> rebuilder_; // null: no rebuilds
    std::shared_ptr<const CommandRegistry> registry_;
    std::shared_ptr<Shared> s_;
};

std::unique_ptr<Document> make(IUiDispatcher& dispatcher, IJobSystem* jobs, std::shared_ptr<IRebuilder> rebuilder,
                               std::shared_ptr<const CommandRegistry> registry, DocStatePtr initial)
{
    try {
        if (!registry) {
            registry = builtinCommandRegistry();
            if (!registry) {
                return nullptr;
            }
        }
        if (!initial) {
            initial = std::make_shared<const DocState>();
        }
        return std::make_unique<DocumentImpl>(dispatcher, jobs, std::move(rebuilder), std::move(registry),
                                              std::move(initial));
    } catch (...) {
        return nullptr;
    }
}

} // namespace

std::unique_ptr<Document> makeDocument(IUiDispatcher& dispatcher, std::shared_ptr<const CommandRegistry> registry,
                                       DocStatePtr initial)
{
    return make(dispatcher, nullptr, nullptr, std::move(registry), std::move(initial));
}

std::unique_ptr<Document> makeDocument(IUiDispatcher& dispatcher, IJobSystem& jobs,
                                       std::shared_ptr<IRebuilder> rebuilder,
                                       std::shared_ptr<const CommandRegistry> registry, DocStatePtr initial)
{
    return make(dispatcher, &jobs, std::move(rebuilder), std::move(registry), std::move(initial));
}

} // namespace ray::doc
