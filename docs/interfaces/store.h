// RayCAD interface contract: native file format and history
// Module: src/store   Depends on: core, doc, SQLite (private), zstd (private)
// Status: PLAN DRAFT. On-disk schema is in PLAN.md section 4.
#pragma once
#include "core.h"
#include "document.h"
#include <optional>

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
    int formatVersion; bool migrated;
    bool openedReadOnlyBecauseLocked;      // if true, the file opened is a VERIFIED SNAPSHOT COPY in a local temp
                                           // path (copied under the lock file's save counter, then hash-checked),
                                           // not a live connection: the writer's exclusive lock blocks all readers.
    std::optional<LockInfo> lock;
};
// Break a stale lock after the user agreed: renames the stale lock file away, then the caller opens ReadWrite.
// Fails with Busy if the lock is live.
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
    virtual Status compact(int keepMicroversionsDays, JobContext&) = 0;   // prune unreachable objects, VACUUM
};

// ---------- Crash recovery (always on local disk, never on the network share) ----------
// Journal lives in <user data dir>/recovery/<file id>.journal: an append-only, fsync'd list of Commands
// applied since the last save plus the base commit id. Replayed on next launch.
class IRecoveryJournal {
public:
    virtual ~IRecoveryJournal() = default;
    virtual Status append(const doc::Command&) = 0;         // called after every executed command; under 2 ms
    virtual Status markSaved(const Hash256& commit) = 0;    // truncates the journal
    struct Pending { std::string filePath; Hash256 baseCommit; std::vector<doc::Command> commands; std::int64_t lastWriteMs; };
    static std::vector<Pending> findPending();
};

// ---------- Migration ----------
// Forward-only, one step at a time (v1->v2->v3). Runs inside a transaction on a copy; the original
// is kept as <name>.v<N>.bak until the migrated file passes an integrity check.
struct Migration { int from; int to; std::function<Status(struct MigrationCtx&)> run; };
void registerMigration(Migration);
// A file with formatVersion > kFormatVersion returns ErrorCode::FormatTooNew and is never modified.

// ---------- Three-way merge (v2; designed now so storage does not change) ----------
struct MergeConflict { std::string path; Hash256 base, ours, theirs; };   // path like "features/<id>" or "cam/ops/<id>"
struct MergeResult { doc::DocStatePtr merged; std::vector<MergeConflict> conflicts; };
Result<MergeResult> merge3(doc::DocStatePtr base, doc::DocStatePtr ours, doc::DocStatePtr theirs);

} // namespace ray::store
