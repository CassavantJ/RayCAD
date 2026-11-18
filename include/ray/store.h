// RayCAD interface contract: native file format and history
// Module: src/store   Depends on: core, doc, SQLite (private), zstd (private)
// Status: PLAN DRAFT. On-disk schema is in PLAN.md section 4.
#pragma once
#include "core.h"
#include "document.h"
#include <optional>
#include <variant>

namespace ray::store {

inline constexpr int kFormatVersion = 1;
inline constexpr std::string_view kFileExtension = ".raycad";

// ---------- Content-addressed objects ----------
enum class ObjectKind : std::uint8_t {
    DocState,        // canonical JSON manifest (features, params, bodies, CAM tree by reference)
    Json,            // any other canonical JSON object (feature props, operation, sketch)
    Brep,            // kernel::serialize output (source of truth for imports; cache for rebuilt bodies)
    ElementMap, Toolpath, Mesh, Thumbnail, Attachment
};
class IObjectStore {
public:
    virtual ~IObjectStore() = default;
    virtual Result<Hash256> put(ObjectKind, std::span<const std::byte>) = 0;   // idempotent; zstd-compressed at rest
    virtual Result<std::vector<std::byte>> get(const Hash256&) const = 0;     // verifies hash on read
    virtual bool has(const Hash256&) const = 0;
};

// ---------- History ----------
struct Commit {
    Hash256 id;                       // hash of (parents, root, author, time, message)
    std::vector<Hash256> parents;     // 0 = initial, 1 = normal, 2 = merge (v2)
    Hash256 root;                     // DocState object
    std::string author;
    std::int64_t unixTimeMs;
    std::string message;              // auto-generated from command labels for microversions
    bool named = false;               // named version (v2 UI); microversions may be pruned, named never
};
struct Ref { std::string name; Hash256 commit; };   // "heads/main", "heads/<branch>", "tags/<version name>"

// ---------- One open .raycad file ----------
enum class OpenMode { ReadWrite, ReadOnly };
// Who holds the write lock (PLAN.md 4.3). Heartbeat age is measured on the file server's clock.
struct LockInfo { std::string user, host; std::int64_t pid; std::int64_t sinceUnixMs; double heartbeatAgeSec; bool stale; bool ownerProvablyDead; };
struct OpenInfo {
    int formatVersion;                     // the file's format as found, before any migration
    bool migrated;                         // upgraded to kFormatVersion at this open (see Migration below)
    bool openedReadOnlyBecauseLocked;      // if true, the file opened is a VERIFIED SNAPSHOT COPY in a local temp
                                           // path (copied under the lock file's save counter, then hash-checked),
                                           // not a live connection: the writer's exclusive lock blocks all readers.
    std::optional<LockInfo> lock;
};
// Break a stale lock after the user agreed: renames the stale lock file away, then the caller opens ReadWrite.
// Fails with ErrorCode::Locked if the lock is live.
Status breakStaleLock(const std::string& path);

class IDocumentFile : public IObjectStore {
public:
    static Result<std::unique_ptr<IDocumentFile>> create(const std::string& path);
    static Result<std::unique_ptr<IDocumentFile>> open(const std::string& path, OpenMode, OpenInfo* info = nullptr);

    // Partial load: reads the head DocState manifest and thumbnail only (budget: under 200 ms for any file).
    virtual Result<doc::DocStatePtr> loadHead(std::string_view branch = "main") = 0;
    virtual Result<doc::DocStatePtr> loadCommit(const Hash256&) = 0;

    // Save = one SQLite transaction: put new objects, insert commit, move ref. Either all or nothing.
    // `derived` are cache objects (shapes, maps, toolpaths, meshes) stored so the next open needs no rebuild.
    struct SaveRequest {
        doc::DocStatePtr state; std::string message; std::string author;
        std::vector<std::pair<ObjectKind, std::vector<std::byte>>> derived;
        std::map<Hash256, Hash256> derivedIndex;      // input-hash -> object hash, for cache lookup on open
    };
    virtual Result<Commit> save(const SaveRequest&, JobContext&) = 0;

    virtual Result<std::vector<Commit>> log(std::string_view branch, int limit) const = 0;
    virtual Result<std::vector<Ref>> refs() const = 0;
    virtual Status setRef(const Ref&) = 0;                          // branches and named versions (API in v1, UI in v2)
    virtual Result<std::optional<Hash256>> cachedDerived(const Hash256& inputHash) const = 0;
    // Compaction (PLAN.md 4.5). Keeps every commit a ref points at, every named version, and every commit reachable
    // from a ref whose time is within the last keepMicroversionsDays days; drops every other commit with its parent
    // links and thumbnail (a kept commit whose parent was dropped ends its history there: log() stops at it), then
    // every object that no kept commit (manifest, feature objects, objects the features name) and no `derived` entry
    // needs, then VACUUMs so the file shrinks. Crash-safe: the deletions are one transaction and SQLite makes VACUUM
    // atomic. If any kept commit cannot be read, nothing is deleted. InvalidArgument for a negative age, Locked when
    // read-only, Cancelled (nothing changed, or the deletions kept and VACUUM not done).
    virtual Status compact(int keepMicroversionsDays, JobContext&) = 0;
};

// ---------- Crash recovery (always on local disk, never on the network share; PLAN.md 4.2) ----------
// One journal per document open for writing, in <user data dir>/recovery/<file id>.journal: on Windows
// %LOCALAPPDATA%\RayCAD\recovery, on macOS ~/Library/Application Support/RayCAD/recovery, elsewhere
// $XDG_DATA_HOME/raycad/recovery (default ~/.local/share/raycad/recovery). It names the base commit (the last save)
// and holds every step since, in order: executed commands, undos, redos, and the source objects (imported BREPs)
// that are not saved yet. Every record carries its length and a checksum, so a torn or damaged record ends the journal
// (it and everything after it are ignored, never replayed). Every append is on the disk (flushed) before it returns
// and costs the same whatever the size of the document: the stateHash() of state() after each step is computed on the
// journal's own thread and recorded after it, and replay checks every step that has one.
// Undo and redo are recorded as undo and redo while the replayed document can repeat them (they go back to steps
// after the base). An undo or redo into history from before the base (the user undoes past the last save) is
// recorded as a snapshot of the whole state instead, so replay always reproduces state() exactly; the replayed undo
// history then starts at that snapshot.
// All members are thread-safe. The appends of steps are made on the UI thread right after the step succeeded.
struct JournalRecord {
    enum class Kind : std::uint8_t { Execute, Undo, Redo, Snapshot };
    Kind kind = Kind::Execute;
    doc::Command command;          // Execute: the command as executed
    doc::DocStatePtr snapshot;     // Snapshot: state() after the step
    std::optional<Hash256> stateAfter;   // stateHash() of state() after the step: always for a snapshot, for other
                                         // steps once the journal had checked it (maybe not the last before a crash)
};
class IRecoveryJournal {
public:
    virtual ~IRecoveryJournal() = default;   // closes the journal and keeps it: unless discard() ran, findPending lists it
    virtual Status append(const doc::Command&, const doc::DocStatePtr& after) = 0;   // after execute(); under 2 ms
    virtual Status appendUndo(const doc::DocStatePtr& after) = 0;                    // after a successful undo()
    virtual Status appendRedo(const doc::DocStatePtr& after) = 0;                    // after a successful redo()
    // A source object a step needs that the file does not hold yet (each of PreparedImport::objects), from any
    // thread, before the step's append. InvalidArgument if `hash` is not the BLAKE3 of the bytes.
    virtual Status appendObject(const Hash256& hash, std::span<const std::byte> bytes) = 0;
    // After save() returned `saved`: the journal now continues from it, and is empty again unless steps were appended
    // while the save ran (state() is no longer the saved snapshot); those are kept as one snapshot record.
    virtual Status markSaved(const Commit& saved) = 0;
    // Clean close, or the user chose not to save: deletes the journal. Every later call fails.
    virtual Status discard() = 0;

    // Starts the journal of `file` (opened for writing; `filePath` as the app names it) whose document's state() is
    // `base`: the head of `branch`, or, when the branch has no commit yet, any state (a null `base` is an empty
    // DocState). InvalidArgument if `base` is not the head's state, or if the file still has steps from an earlier
    // session (findPendingFor: recover or discard them first); Locked if another process journals this file.
    static Result<std::unique_ptr<IRecoveryJournal>> start(const IDocumentFile& file, const std::string& filePath,
                                                           doc::DocStatePtr base, std::string_view branch = "main");

    struct Pending {
        std::string journalPath;                 // UTF-8
        std::string filePath;                    // the document, as given to start()
        std::string fileId;                      // the document's file id (meta table)
        std::string branch;
        std::optional<Hash256> baseCommit;       // nullopt: the branch had no commit when the journal began
        std::vector<JournalRecord> records;      // every intact step, in order
        std::vector<std::pair<Hash256, std::vector<std::byte>>> objects;   // verified against their hashes
        bool damagedTail = false;                // a torn or damaged record ended the journal early
        std::int64_t lastWriteMs = 0;            // ms since 1970
    };
    // Journals that hold at least one step and that no running process has open, newest first. Never throws.
    static std::vector<Pending> findPending();
    static std::optional<Pending> findPendingFor(const IDocumentFile& file);
    static Status discard(const Pending&);     // deletes it (the user declined recovery); Locked if it is in use

    // Makes a document whose state() is `initial` (with the app's dispatcher, rebuilder and command registry).
    using DocumentFactory = std::function<std::unique_ptr<doc::Document>(doc::DocStatePtr initial)>;
    struct Recovered {
        std::unique_ptr<doc::Document> document;     // state() as it was before the crash
        std::unique_ptr<IRecoveryJournal> journal;   // the journal continued, holding the replayed steps
        std::size_t applied = 0;                     // records replayed
        std::optional<Error> stopped;                // why replay stopped before the last record; nullopt: all replayed
        bool modified = false;                       // state() is not the file's head: show it as modified even if
                                                     // document->dirty() is false (after a snapshot record)
    };
    // Replays `pending` onto `file` (opened for writing): makeDocument(base commit's state), then every record in
    // order (Snapshot records call makeDocument again), checking the state hash after each; a step that fails or
    // gives another hash stops the replay there and the rest is dropped. Put `pending.objects` into the document's
    // object pool before calling. InvalidArgument when the journal belongs to another document or the file's head is
    // not the base commit (saved again later, or an older copy of the file); one exception: a head whose first parent
    // is the base and whose state the journal reached (the save landed, markSaved did not) is accepted and the
    // journal continues from it. Never changes the file; the document is clean if state() equals the head's state.
    static Result<Recovered> recover(const Pending&, IDocumentFile& file, const DocumentFactory& makeDocument);
};

// ---------- Migration (PLAN.md 4.4) ----------
// Forward-only single steps (from -> from + 1). A read-write open of a file whose user_version is older than
// kFormatVersion first copies the original to <name>.v<N>.bak (kept afterwards), then runs every step from N up, in
// order, on a working copy inside one transaction (user_version is set after each step); the copy must pass PRAGMA
// integrity_check and a load of every ref before it replaces the original (an atomic rename), and OpenInfo::migrated
// is set. A step that fails, a missing step or a copy that does not verify leaves the original untouched and
// removes the .bak (the step's error, or FormatError). A read-only open migrates its snapshot copy only.
// A file with formatVersion > kFormatVersion returns ErrorCode::FormatTooNew and is never modified.
using SqlValue = std::variant<std::monostate, std::int64_t, double, std::string, std::vector<std::byte>>;
// What one step sees: the working copy, inside the migration's transaction. Parameters bind to ?1, ?2, ...
// exec runs one or more statements, or exactly one when it has parameters.
struct MigrationCtx {
    int from = 0, to = 0;
    virtual ~MigrationCtx() = default;
    virtual Status exec(std::string_view sql, const std::vector<SqlValue>& params = {}) = 0;
    virtual Result<std::vector<std::vector<SqlValue>>> query(std::string_view sql,
                                                             const std::vector<SqlValue>& params = {}) = 0;
};
struct Migration { int from; int to; std::function<Status(MigrationCtx&)> run; };
// For start-up, before files are opened; thread-safe. A step with to != from + 1, from < 0, to > kFormatVersion or
// no function, or a second step from the same version, is ignored and logged.
void registerMigration(Migration);

// ---------- Three-way merge (v2; designed now so storage does not change) ----------
struct MergeConflict { std::string path; Hash256 base, ours, theirs; };   // path like "features/<id>" or "cam/ops/<id>"
struct MergeResult { doc::DocStatePtr merged; std::vector<MergeConflict> conflicts; };
Result<MergeResult> merge3(doc::DocStatePtr base, doc::DocStatePtr ours, doc::DocStatePtr theirs);

} // namespace ray::store
