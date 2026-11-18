// The document: current snapshot, unlimited undo and redo, previews and change notifications (document_impl.h).
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
};

// Everything the document owns that a posted notification may still read after the document is gone.
struct Shared {
    mutable std::mutex mutex;
    DocStatePtr current;
    DocStatePtr clean;
    BuildResultPtr build; // the rebuilder (T-010) sets this; null until then
    std::vector<UndoEntry> undo;
    std::vector<UndoEntry> redo;
    std::optional<Preview> preview;
    std::map<int, std::shared_ptr<const IDocument::Listener>> listeners;
    int nextListener = 1;
};

// Runs the handler on the canonical arguments; never throws.
Result<DocStatePtr> applyCommand(const CommandRegistry& registry, const Command& command, const DocStatePtr& before,
                                 std::string& canonicalArgs)
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
            return handler->apply(before, canonicalArgs);
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

class DocumentImpl final : public Document {
public:
    DocumentImpl(IUiDispatcher& dispatcher, std::shared_ptr<const CommandRegistry> registry, DocStatePtr initial)
        : dispatcher_(dispatcher), registry_(std::move(registry)), s_(std::make_shared<Shared>())
    {
        s_->current = initial;
        s_->clean = std::move(initial);
    }

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
                ids = listenerIds();
                UndoEntry entry{Command{command.name, std::move(args), command.label}, s_->current, after};
                s_->undo.push_back(std::move(entry));
                s_->redo.clear();
                s_->current = after;
                s_->preview.reset();
            }
            notify(after, std::move(ids));
            return after;
        } catch (const std::bad_alloc&) {
            return failure(ErrorCode::Internal, command.name + ": out of memory");
        } catch (...) {
            return failure(ErrorCode::Internal, command.name + ": unexpected failure");
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
            s_->preview.reset();
            Preview preview{command, nullptr, std::nullopt};
            std::string args;
            auto result = applyCommand(*registry_, command, s_->current, args);
            preview.command.argsJson = std::move(args);
            if (result) {
                preview.state = std::move(*result);
            } else {
                preview.error = std::move(result.error());
            }
            // A rebuild of preview.state off to the side starts here once the rebuilder exists (T-010).
            s_->preview = std::move(preview);
        } catch (...) {
            // Out of memory: no preview is shown. preview was reset above, so the old one is not left behind.
            log(LogLevel::Warn, "doc", "Could not start a preview: out of memory");
        }
    }

    void cancelPreview() override
    {
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->preview.reset();
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
    // Caller holds the mutex.
    std::vector<int> listenerIds() const
    {
        std::vector<int> ids;
        ids.reserve(s_->listeners.size());
        for (const auto& entry : s_->listeners) {
            ids.push_back(entry.first);
        }
        return ids;
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
                ids = listenerIds();
                to.reserve(to.size() + 1); // the push_back below cannot fail after this
                UndoEntry entry = std::move(from.back());
                from.pop_back();
                s_->current = isUndo ? entry.before : entry.after;
                to.push_back(std::move(entry));
                s_->preview.reset();
                now = s_->current;
            }
            notify(now, std::move(ids));
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

    // Posts one notification for one change. It calls, once each, the listeners that were subscribed at the time of
    // the change (`ids`) and still are when it runs.
    void notify(DocStatePtr state, std::vector<int> ids)
    {
        if (ids.empty()) {
            return;
        }
        try {
            std::weak_ptr<Shared> weak = s_;
            dispatcher_.post([weak, state = std::move(state), ids = std::move(ids)]() {
                const std::shared_ptr<Shared> shared = weak.lock();
                if (!shared) {
                    return; // the document is gone
                }
                for (const int id : ids) {
                    std::shared_ptr<const Listener> listener;
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

    IUiDispatcher& dispatcher_;
    std::shared_ptr<const CommandRegistry> registry_;
    std::shared_ptr<Shared> s_;
};

} // namespace

std::unique_ptr<Document> makeDocument(IUiDispatcher& dispatcher, std::shared_ptr<const CommandRegistry> registry,
                                       DocStatePtr initial)
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
        return std::make_unique<DocumentImpl>(dispatcher, std::move(registry), std::move(initial));
    } catch (...) {
        return nullptr;
    }
}

} // namespace ray::doc
