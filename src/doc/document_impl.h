// The document behind ray::doc::IDocument, the command registry and the JSON form of commands (task T-009).
// Private to src/doc; store (recovery journal) and api (app session) are its intended users.
//
// Undo model: the document keeps a pointer to the current immutable DocState. Every executed command pushes one
// undo entry holding the command (with canonical arguments) and the snapshots before and after it, so undo and
// redo are pointer swaps and unchanged snapshots are shared, never copied. History is unlimited. A new command
// clears the redo stack. A command that fails changes nothing: not the state, not the stacks, no notification.
//
// Commands are pure functions of (snapshot, canonical arguments): handlers never generate ids, read the clock or
// depend on anything else, so replaying a command log on the snapshot it started from gives the same state hash.
// execute() canonicalises the arguments before the handler sees them, so the journal's canonical copy replays to
// exactly the same snapshot as the original call.
//
// Threads: execute, undo, redo, the preview calls, subscribe and unsubscribe are made on the UI thread (PLAN.md 7).
// state() and the other const queries may be called from any thread. Listeners are never called from inside
// those calls: each change (execute, undo, redo) posts exactly one notification through the IUiDispatcher, and
// that notification calls every listener that was subscribed at the time of the change and still is, once, with
// the snapshot that change produced. Notifications that run after the document is destroyed do nothing.
//
// Previews: beginPreview(command) applies the command to the current snapshot off to the side. It never changes
// state(), the undo or redo stack, dirty() or the listeners. A second beginPreview replaces the first; cancelPreview
// drops it. A preview is committed by executing its command: execute() ends any preview, as do undo and redo
// (the preview was computed from a snapshot that is no longer current). T-010 adds the preview rebuild.
//
// Nothing here throws. Failures are returned as ray::Error.
#pragma once

#include <ray/document.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ray::doc {

// ---------- Handler registry ----------
class CommandRegistry {
public:
    CommandRegistry();
    ~CommandRegistry();
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    // InvalidArgument if the handler is null, its name is empty or the name is already registered.
    Status add(std::unique_ptr<ICommandHandler> handler);
    const ICommandHandler* find(std::string_view name) const; // null if unknown
    std::vector<std::string> names() const;                   // sorted

private:
    std::map<std::string, std::unique_ptr<ICommandHandler>, std::less<>> handlers_;
};

// Registers feature.add, feature.edit, feature.remove, feature.reorder, feature.suppress, param.set and body.set
// (src/doc/commands/). Their argument objects are described by argsSchemaJson() and in commands/model_commands.cpp.
Status registerBuiltinCommands(CommandRegistry&);
// A registry holding just the built-in commands.
std::shared_ptr<const CommandRegistry> builtinCommandRegistry();

// ---------- JSON form of commands ----------
// A command is the canonical JSON object  {"args":{...},"label":"...","name":"feature.add","schema":1}
// with the arguments embedded as a JSON object (not as a string). A command log is a JSON array of these, in
// execution order. Reading rejects missing or mistyped keys, unknown keys and a newer "schema" (FormatTooNew).
// commandToJson fails with InvalidArgument if argsJson is not a JSON object.
Result<std::string> commandToJson(const Command&);
Result<Command> commandFromJson(std::string_view json); // argsJson comes back canonical
Result<std::string> commandLogToJson(const std::vector<Command>&);
Result<std::vector<Command>> commandLogFromJson(std::string_view json);

// ---------- Document ----------
class Document : public IDocument {
public:
    // The preview snapshot, or null when no preview is active or the previewed command failed.
    virtual DocStatePtr previewState() const = 0;
    // Why the active preview's command failed; nullopt when there is no preview or it succeeded.
    virtual std::optional<Error> previewError() const = 0;

    virtual std::size_t undoCount() const = 0;
    virtual std::size_t redoCount() const = 0;
    // The command the next undo() or redo() would revert or repeat (its label is for the undo menu).
    virtual std::optional<Command> undoCommand() const = 0;
    virtual std::optional<Command> redoCommand() const = 0;

    // dirty() is true while state() is not the snapshot that was current at the last markClean() (or at creation).
    // The store calls this after a save.
    virtual void markClean() = 0;
};

// `dispatcher` must outlive the document and queue posted functions, never run them inside post() (core.h).
// A null `registry` means builtinCommandRegistry(); a null `initial` means an empty DocState.
std::unique_ptr<Document> makeDocument(IUiDispatcher& dispatcher, std::shared_ptr<const CommandRegistry> registry = {},
                                       DocStatePtr initial = {});

} // namespace ray::doc
