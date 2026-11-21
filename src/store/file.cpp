// The .raycad document file (PLAN.md 4.1 to 4.3): one SQLite database per document, content-addressed objects,
// commits and refs, saved as one transaction, guarded by the lock file; read-only access through a verified copy.
#include "file.h"

#include "compact.h"
#include "file_db.h"
#include "lock.h"
#include "migrate.h"
#include "objects.h"

#include <ray/document.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace ray::store {
namespace detail {
namespace {

constexpr std::int64_t kApplicationId = 0x52415943; // 'RAYC'

// PLAN.md 4.1, statement for statement, with one change: `objects` and `thumbs` hold large blobs and are ordinary
// rowid tables, not WITHOUT ROWID. A WITHOUT ROWID table is a B-tree of whole rows, and SQLite reads a row that
// spills onto overflow pages in full each time a key lookup compares against it, so every lookup read megabytes:
// on a 500 MB file loadHead took 0.8 to 1.9 s and a 50 KB save about 1 s. As rowid tables the hash key lives in a
// small separate index: loadHead 5 ms, the same save 15 ms. Columns, keys and constraints are unchanged.
const std::vector<std::string> kSchema = {
    "CREATE TABLE meta    (key TEXT PRIMARY KEY, value TEXT NOT NULL)",
    "CREATE TABLE objects (hash BLOB PRIMARY KEY, kind INTEGER NOT NULL,\n"
    "                      raw_size INTEGER NOT NULL, data BLOB NOT NULL)",
    "CREATE TABLE commits (id BLOB PRIMARY KEY, root BLOB NOT NULL, author TEXT, time_ms INTEGER NOT NULL,\n"
    "                      message TEXT, named INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID",
    "CREATE TABLE commit_parents (commit_id BLOB NOT NULL, ord INTEGER NOT NULL, parent BLOB NOT NULL,\n"
    "                      PRIMARY KEY (commit_id, ord)) WITHOUT ROWID",
    "CREATE TABLE refs    (name TEXT PRIMARY KEY, commit_id BLOB NOT NULL)",
    "CREATE TABLE derived (input_hash BLOB PRIMARY KEY, object_hash BLOB NOT NULL, kernel_version TEXT NOT NULL) "
    "WITHOUT ROWID",
    "CREATE TABLE thumbs  (commit_id BLOB PRIMARY KEY, png BLOB NOT NULL)",
};

// How long a read-only opener keeps trying to take a consistent copy while the writer saves.
std::atomic<std::int64_t> gSnapshotPatienceMs{30'000};
constexpr int kSnapshotBadCopiesAllowed = 3;

#define RAY_TRY(expr)                                                                                                  \
    do {                                                                                                               \
        if (auto ray_try_status_ = (expr); !ray_try_status_) {                                                         \
            return tl::unexpected<Error>(std::move(ray_try_status_.error()));                                          \
        }                                                                                                              \
    } while (false)

std::string asString(const std::vector<std::byte>& bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::span<const std::byte> asBytes(std::string_view text)
{
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

Error internalError(const char* what)
{
    return makeError(ErrorCode::Internal, "Unexpected failure in the document file", what);
}

// ---------- Checks shared by live files and snapshot copies ----------

// `allowOld`: an older format is accepted when registered migrations lead from it to kFormatVersion.
Status checkHeader(Db& db, int& version, bool allowOld = false)
{
    auto appId = db.queryInt("PRAGMA application_id");
    if (!appId) {
        return tl::unexpected<Error>(appId.error());
    }
    auto userVersion = db.queryInt("PRAGMA user_version");
    if (!userVersion) {
        return tl::unexpected<Error>(userVersion.error());
    }
    if (*appId != kApplicationId) {
        return fail(ErrorCode::FormatError, "This is not a RayCAD document",
                    "application_id " + std::to_string(*appId));
    }
    version = static_cast<int>(*userVersion);
    if (*userVersion > kFormatVersion) {
        return fail(ErrorCode::FormatTooNew,
                    "This document was saved by a newer version of RayCAD (file format " +
                        std::to_string(*userVersion) + "). This version reads format " +
                        std::to_string(kFormatVersion) + "; update RayCAD to open it.",
                    "user_version " + std::to_string(*userVersion));
    }
    if (*userVersion < kFormatVersion && !(allowOld && canMigrate(version))) {
        return fail(ErrorCode::FormatError, "This document uses an old file format that cannot be read",
                    "user_version " + std::to_string(*userVersion));
    }
    return {};
}

Status checkSchema(Db& db)
{
    auto tables = db.queryInt("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name IN "
                              "('meta','objects','commits','commit_parents','refs','derived','thumbs')");
    if (!tables) {
        return tl::unexpected<Error>(tables.error());
    }
    if (*tables != 7) {
        return fail(ErrorCode::FormatError, "The document file is damaged", "missing tables");
    }
    return {};
}

Status checkIntegrity(Db& db)
{
    auto result = db.queryText("PRAGMA integrity_check");
    if (!result) {
        return tl::unexpected<Error>(result.error());
    }
    if (*result != "ok") {
        return fail(ErrorCode::FormatError, "The document file is damaged",
                    "integrity_check: " + result->substr(0, 200));
    }
    return {};
}

Result<std::optional<Hash256>> readRef(Db& db, const std::string& name)
{
    Stmt stmt;
    RAY_TRY(stmt.prepare(db, "SELECT commit_id FROM refs WHERE name = ?1"));
    stmt.bindText(1, name);
    auto rc = stmt.step();
    if (!rc) {
        return tl::unexpected<Error>(rc.error());
    }
    if (*rc != SQLITE_ROW) {
        return std::optional<Hash256>();
    }
    Hash256 id;
    if (!hashFromBlob(stmt.blob(0), id)) {
        return fail(ErrorCode::FormatError, "The document file is damaged", "ref " + name + ": bad commit id");
    }
    return std::optional<Hash256>(id);
}

// Reads a commit row and its parents and checks the id against its content.
Result<Commit> readCommit(Db& db, const Hash256& id)
{
    Stmt row;
    RAY_TRY(row.prepare(db, "SELECT root, author, time_ms, message, named FROM commits WHERE id = ?1"));
    row.bindBlob(1, hashBytes(id));
    auto rc = row.step();
    if (!rc) {
        return tl::unexpected<Error>(rc.error());
    }
    if (*rc != SQLITE_ROW) {
        return fail(ErrorCode::NotFound, "This version is not in the document", "commit " + id.hex());
    }
    Commit commit;
    commit.id = id;
    if (!hashFromBlob(row.blob(0), commit.root)) {
        return fail(ErrorCode::FormatError, "The document file is damaged", "commit " + id.hex() + ": bad root");
    }
    commit.author = row.text(1);
    commit.unixTimeMs = row.integer(2);
    commit.message = row.text(3);
    commit.named = row.integer(4) != 0;

    Stmt parents;
    RAY_TRY(parents.prepare(db, "SELECT ord, parent FROM commit_parents WHERE commit_id = ?1 ORDER BY ord"));
    parents.bindBlob(1, hashBytes(id));
    for (std::int64_t expected = 0;; ++expected) {
        auto prc = parents.step();
        if (!prc) {
            return tl::unexpected<Error>(prc.error());
        }
        if (*prc != SQLITE_ROW) {
            break;
        }
        Hash256 parent;
        if (parents.integer(0) != expected || !hashFromBlob(parents.blob(1), parent)) {
            return fail(ErrorCode::FormatError, "The document file is damaged", "commit " + id.hex() + ": parents");
        }
        commit.parents.push_back(parent);
    }
    if (commitIdOf(commit.parents, commit.root, commit.author, commit.unixTimeMs, commit.message) != id) {
        return fail(ErrorCode::FormatError, "The document file is damaged", "commit " + id.hex() + ": id mismatch");
    }
    return commit;
}

Result<doc::DocStatePtr> loadState(Db& db, const Hash256& root)
{
    auto manifest = readObject(db, root);
    if (!manifest) {
        return rethrow(manifest.error(), "reading the document manifest");
    }
    const std::string text = asString(*manifest);
    auto state = doc::deserializeState(text, [&db](const Hash256& hash) -> Result<std::string> {
        auto object = readObject(db, hash);
        if (!object) {
            return tl::unexpected<Error>(object.error());
        }
        return asString(*object);
    });
    if (!state) {
        return rethrow(state.error(), "reading document " + root.hex());
    }
    return doc::DocStatePtr(std::make_shared<const doc::DocState>(std::move(*state)));
}

Result<std::vector<Ref>> readRefs(Db& db)
{
    Stmt stmt;
    RAY_TRY(stmt.prepare(db, "SELECT name, commit_id FROM refs ORDER BY name"));
    std::vector<Ref> out;
    for (;;) {
        auto rc = stmt.step();
        if (!rc) {
            return tl::unexpected<Error>(rc.error());
        }
        if (*rc != SQLITE_ROW) {
            return out;
        }
        Ref ref;
        ref.name = stmt.text(0);
        if (!hashFromBlob(stmt.blob(1), ref.commit)) {
            return fail(ErrorCode::FormatError, "The document file is damaged", "ref " + ref.name);
        }
        out.push_back(std::move(ref));
    }
}

// ---------- Snapshot copies (PLAN.md 4.3) ----------

std::FILE* openStdio(const std::string& path, bool write)
{
#ifdef _WIN32
    return _wfopen(toFsPath(path).c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

// Copies a database file. SQLite's lock bytes sit in the page at offset 1 GiB, which the writer holds byte-range
// locks on and which never holds data; that page is written as zeros instead of being read (on Windows the
// locks are mandatory, so reading it would fail).
Status copyDatabaseFile(const std::string& from, const std::string& to, bool isDatabase)
{
    std::FILE* in = openStdio(from, false);
    if (!in) {
        return fail(ErrorCode::IoError, "Cannot read the document file", "open " + from);
    }
    std::FILE* out = openStdio(to, true);
    if (!out) {
        std::fclose(in);
        return fail(ErrorCode::IoError, "Cannot write a temporary copy of the document", "open " + to);
    }
    std::vector<unsigned char> buffer(4u << 20);
    std::uint64_t lockStart = UINT64_MAX;
    std::uint64_t lockEnd = UINT64_MAX;
    bool ok = true;
    std::uint64_t pos = 0;
    for (;;) {
        std::size_t want = buffer.size();
        if (pos >= lockStart && pos < lockEnd) {
            const std::size_t gap = static_cast<std::size_t>(lockEnd - pos);
            std::vector<unsigned char> zeros(gap, 0);
#ifdef _WIN32
            const bool seekOk = _fseeki64(in, static_cast<long long>(lockEnd), SEEK_SET) == 0;
#else
            const bool seekOk = fseeko(in, static_cast<off_t>(lockEnd), SEEK_SET) == 0;
#endif
            // Write the zeros only if the file goes on past the lock page.
            const int next = seekOk ? std::fgetc(in) : EOF;
            if (next == EOF) {
                break;
            }
            std::ungetc(next, in);
            ok = std::fwrite(zeros.data(), 1, gap, out) == gap;
            pos = lockEnd;
            if (!ok) {
                break;
            }
            continue;
        }
        if (pos < lockStart && lockStart - pos < want) {
            want = static_cast<std::size_t>(lockStart - pos);
        }
        const std::size_t got = std::fread(buffer.data(), 1, want, in);
        if (got == 0) {
            ok = !std::ferror(in);
            break;
        }
        if (pos == 0 && isDatabase && got >= 100) {
            // Page size: header bytes 16-17, big-endian; 1 means 65536.
            std::uint32_t pageSize = (static_cast<std::uint32_t>(buffer[16]) << 8) | buffer[17];
            if (pageSize == 1) {
                pageSize = 65536;
            }
            if (pageSize >= 512) {
                lockStart = 0x40000000ULL;
                lockEnd = lockStart + pageSize;
            }
        }
        if (std::fwrite(buffer.data(), 1, got, out) != got) {
            ok = false;
            break;
        }
        pos += got;
    }
    ok = std::fflush(out) == 0 && ok;
    ok = std::fclose(out) == 0 && ok;
    std::fclose(in);
    if (!ok) {
        return fail(ErrorCode::IoError, "Cannot copy the document file", from);
    }
    return {};
}

struct FileStamp {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    bool operator==(const FileStamp&) const = default;
};

FileStamp stampOf(const std::string& path)
{
    FileStamp stamp;
    std::error_code ec;
    const auto fsPath = toFsPath(path);
    stamp.size = std::filesystem::file_size(fsPath, ec);
    stamp.mtime = std::filesystem::last_write_time(fsPath, ec);
    return stamp;
}

bool sameOwner(const LockState& a, const LockState& b)
{
    return a.readable == b.readable && a.fields.pid == b.fields.pid && a.fields.sinceUnixMs == b.fields.sinceUnixMs &&
           a.fields.host == b.fields.host && a.fields.saving == b.fields.saving &&
           a.fields.saveCount == b.fields.saveCount;
}

// If this process itself has the document open for writing, copies it through that connection with SQLite's
// backup API and returns true. Copying the file with plain file I/O would be wrong here: on POSIX, closing any
// descriptor of a file drops every fcntl lock this process holds on it, including the writer's SQLite lock.
bool backupFromOpenWriter(const std::string& path, const std::string& copy, Status& result);

// Copies the document to a local temp file while no save overlaps the copy, then verifies the copy (migrating the
// copy if the document has an older format; `upgradedFrom` then receives that format).
Result<std::string> takeSnapshot(const std::string& path, int& upgradedFrom)
{
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "raycad-snapshots";
    std::filesystem::create_directories(dir, ec);
    const std::string copy = fromFsPath(dir / (randomName() + ".raycad"));
    const auto cleanup = [&copy] {
        removeQuietly(copy);
        removeQuietly(copy + "-journal");
    };
    if (Status backedUp; backupFromOpenWriter(path, copy, backedUp)) {
        if (backedUp) {
            backedUp = verifyCopy(copy, &upgradedFrom);
        }
        if (!backedUp) {
            cleanup();
            return tl::unexpected<Error>(backedUp.error());
        }
        return copy;
    }
    const std::int64_t patienceMs = gSnapshotPatienceMs.load();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(patienceMs);
    int badCopies = 0;
    std::string writer = "another user";
    while (std::chrono::steady_clock::now() < deadline) {
        auto before = readLock(path);
        if (!before && before.error().code != ErrorCode::NotFound) {
            cleanup();
            return tl::unexpected<Error>(before.error());
        }
        const bool locked = before.has_value();
        // Without a writer the files cannot change, so a hot journal (left by a crash) is copied with the
        // database and rolled back on the copy. With a live writer, only an idle moment will do.
        bool withJournal = !locked;
        if (locked) {
            writer = before->fields.user + "@" + before->fields.host;
            const bool busy = !before->readable || before->fields.saving;
            if (busy && !before->ownerDead) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }
            withJournal = busy; // a writer that died while saving: its files no longer change
        }
        const FileStamp stamp0 = stampOf(path);
        if (auto st = copyDatabaseFile(path, copy, true); !st) {
            cleanup();
            return tl::unexpected<Error>(st.error());
        }
        removeQuietly(copy + "-journal");
        if (withJournal && pathExists(path + "-journal")) {
            if (auto st = copyDatabaseFile(path + "-journal", copy + "-journal", false); !st) {
                cleanup();
                return tl::unexpected<Error>(st.error());
            }
        }
        const FileStamp stamp1 = stampOf(path);
        auto after = readLock(path);
        bool consistent = stamp0 == stamp1;
        if (locked) {
            consistent = consistent && after && sameOwner(*before, *after) && (!withJournal || after->ownerDead);
        } else {
            consistent = consistent && !after && after.error().code == ErrorCode::NotFound;
        }
        if (!consistent) {
            continue; // a save (or a new writer) overlapped the copy
        }
        auto verified = verifyCopy(copy, &upgradedFrom);
        if (verified) {
            return copy;
        }
        if (verified.error().code == ErrorCode::FormatTooNew) {
            cleanup();
            return tl::unexpected<Error>(verified.error());
        }
        ray::log(LogLevel::Warn, "store",
                 "A snapshot copy failed verification and was discarded: " + verified.error().message);
        if (++badCopies >= kSnapshotBadCopiesAllowed) {
            cleanup();
            return rethrow(verified.error(),
                           "snapshot copy failed verification " + std::to_string(kSnapshotBadCopiesAllowed) + " times");
        }
    }
    cleanup();
    return fail(ErrorCode::Locked, "The document is being saved by " + writer + "; try again in a moment",
                "no idle moment to copy it in " + std::to_string(patienceMs) + " ms");
}

} // namespace

const std::vector<std::string>& schemaStatements()
{
    return kSchema;
}

void setSnapshotPatienceMs(std::int64_t ms) noexcept
{
    gSnapshotPatienceMs = ms;
}

Status verifyCopy(const std::string& path, int* upgradedFrom)
{
    try {
        Db db;
        RAY_TRY(db.open(path, SQLITE_OPEN_READWRITE));
        int version = 0;
        RAY_TRY(checkHeader(db, version, upgradedFrom != nullptr));
        if (version < kFormatVersion) {
            // A private copy of an older file: upgrade it in place (migrateInPlace verifies it again).
            db.close();
            RAY_TRY(migrateInPlace(path, version));
            *upgradedFrom = version;
            return {};
        }
        RAY_TRY(checkIntegrity(db));
        RAY_TRY(checkSchema(db));
        auto refs = readRefs(db);
        if (!refs) {
            return tl::unexpected<Error>(refs.error());
        }
        std::set<Hash256> checked;
        for (const Ref& ref : *refs) {
            if (!checked.insert(ref.commit).second) {
                continue;
            }
            auto commit = readCommit(db, ref.commit);
            if (!commit) {
                return rethrow(commit.error(), "ref " + ref.name);
            }
            auto state = loadState(db, commit->root);
            if (!state) {
                return rethrow(state.error(), "ref " + ref.name);
            }
        }
        return {};
    } catch (const std::exception& e) {
        return fail(ErrorCode::Internal, "Unexpected failure while checking a document copy", e.what());
    }
}

// ---------- The open file ----------

// Documents this process has open for writing, by canonical path (see backupFromOpenWriter).
class FileImpl;
std::mutex gWritersMutex;
std::map<std::string, FileImpl*> gWriters;

std::string canonicalKey(const std::string& path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(toFsPath(path), ec);
    return ec ? path : fromFsPath(canonical);
}

class FileImpl final : public IDocumentFile {
public:
    FileImpl(Db db, std::unique_ptr<OwnedLock> lock, std::string snapshot)
        : snapshot_(std::move(snapshot)), lock_(std::move(lock)), db_(std::move(db))
    {
        sqlite3_commit_hook(
            db_.raw(),
            [](void* counter) -> int {
                static_cast<std::atomic<std::uint64_t>*>(counter)->fetch_add(1);
                return 0;
            },
            &commits_);
    }

    ~FileImpl() override
    {
        if (!writerKey_.empty()) {
            std::lock_guard<std::mutex> registry(gWritersMutex);
            gWriters.erase(writerKey_);
        }
        std::lock_guard<std::mutex> guard(mutex_);
        // The connection closes first (that deletes the -journal on a clean close), then the lock file goes.
        sqlite3_commit_hook(db_.raw(), nullptr, nullptr);
        db_.close();
        lock_.reset();
        if (!snapshot_.empty()) {
            removeQuietly(snapshot_);
            removeQuietly(snapshot_ + "-journal");
        }
    }

    // ---- IObjectStore ----
    Result<Hash256> put(ObjectKind kind, std::span<const std::byte> bytes) override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            RAY_TRY(requireWritable());
            const Hash256 hash = hashObject(bytes);
            auto exists = objectExists(db_, hash);
            if (!exists) {
                return tl::unexpected<Error>(exists.error());
            }
            if (!*exists) {
                RAY_TRY(writeTransaction([&]() -> Status { return insertObject(db_, kind, bytes, hash); }));
            }
            return hash;
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Result<std::vector<std::byte>> get(const Hash256& hash) const override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            return readObject(db_, hash);
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    bool has(const Hash256& hash) const override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            auto exists = objectExists(db_, hash);
            return exists && *exists;
        } catch (...) {
            return false;
        }
    }

    // ---- IDocumentFile ----
    Result<doc::DocStatePtr> loadHead(std::string_view branch) override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            if (branch.empty()) {
                return fail(ErrorCode::InvalidArgument, "No branch given");
            }
            const std::string name = "heads/" + std::string(branch);
            auto head = readRef(db_, name);
            if (!head) {
                return tl::unexpected<Error>(head.error());
            }
            if (!*head) {
                return fail(ErrorCode::NotFound, "The document has no saved version on this branch", name);
            }
            auto commit = readCommit(db_, **head);
            if (!commit) {
                return tl::unexpected<Error>(commit.error());
            }
            auto state = loadState(db_, commit->root);
            if (state) {
                branch_ = std::string(branch);
            }
            return state;
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Result<doc::DocStatePtr> loadCommit(const Hash256& id) override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            auto commit = readCommit(db_, id);
            if (!commit) {
                return tl::unexpected<Error>(commit.error());
            }
            return loadState(db_, commit->root);
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Result<Commit> save(const SaveRequest& request, JobContext& ctx) override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            return saveLocked(request, ctx);
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Result<std::vector<Commit>> log(std::string_view branch, int limit) const override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            std::vector<Commit> out;
            auto head = readRef(db_, "heads/" + std::string(branch));
            if (!head) {
                return tl::unexpected<Error>(head.error());
            }
            std::optional<Hash256> next = *head;
            std::set<Hash256> seen;
            while (next && (limit <= 0 || static_cast<int>(out.size()) < limit)) {
                if (!seen.insert(*next).second) {
                    return fail(ErrorCode::FormatError, "The document file is damaged", "history has a cycle");
                }
                auto commit = readCommit(db_, *next);
                if (!commit) {
                    if (!out.empty() && commit.error().code == ErrorCode::NotFound) {
                        break; // compact() pruned the history from here on
                    }
                    return tl::unexpected<Error>(commit.error());
                }
                next = commit->parents.empty() ? std::nullopt : std::optional<Hash256>(commit->parents.front());
                out.push_back(std::move(*commit));
            }
            return out;
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Result<std::vector<Ref>> refs() const override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            return readRefs(db_);
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Status setRef(const Ref& ref) override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            RAY_TRY(requireWritable());
            RAY_TRY(checkRefName(ref.name));
            auto target = readCommit(db_, ref.commit);
            if (!target) {
                return tl::unexpected<Error>(target.error());
            }
            auto previous = readRef(db_, ref.name);
            if (!previous) {
                return tl::unexpected<Error>(previous.error());
            }
            const bool tag = ref.name.rfind("tags/", 0) == 0;
            return writeTransaction([&]() -> Status {
                Stmt upsert;
                RAY_TRY(upsert.prepare(db_, "INSERT INTO refs (name, commit_id) VALUES (?1, ?2) "
                                            "ON CONFLICT(name) DO UPDATE SET commit_id = excluded.commit_id"));
                upsert.bindText(1, ref.name);
                upsert.bindBlob(2, hashBytes(ref.commit));
                RAY_TRY(upsert.run());
                if (tag) {
                    // A commit is a named version while at least one tag points at it.
                    RAY_TRY(refreshNamed(ref.commit));
                    if (*previous && **previous != ref.commit) {
                        RAY_TRY(refreshNamed(**previous));
                    }
                }
                return {};
            });
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Result<std::optional<Hash256>> cachedDerived(const Hash256& inputHash) const override
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            Stmt stmt;
            RAY_TRY(stmt.prepare(db_, "SELECT object_hash FROM derived WHERE input_hash = ?1"));
            stmt.bindBlob(1, hashBytes(inputHash));
            auto rc = stmt.step();
            if (!rc) {
                return tl::unexpected<Error>(rc.error());
            }
            if (*rc != SQLITE_ROW) {
                return std::optional<Hash256>();
            }
            Hash256 object;
            if (!hashFromBlob(stmt.blob(0), object)) {
                return fail(ErrorCode::FormatError, "The document file is damaged", "derived index");
            }
            return std::optional<Hash256>(object);
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    Status compact(int keepMicroversionsDays, JobContext& ctx) override
    {
        return compactAsOf(keepMicroversionsDays, unixNowMs(), ctx, nullptr);
    }

    Status compactAsOf(int keepMicroversionsDays, std::int64_t nowMs, JobContext& ctx, CompactStats* stats)
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            RAY_TRY(requireWritable());
            return compactDatabase(
                db_, keepMicroversionsDays, nowMs, ctx, [this] { lock_->beginWrite(); }, [this] { lock_->endWrite(); },
                stats);
        } catch (const std::exception& e) {
            return tl::unexpected<Error>(internalError(e.what()));
        }
    }

    std::string fileId() const
    {
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            Stmt stmt;
            if (!stmt.prepare(db_, "SELECT value FROM meta WHERE key = 'file_id'")) {
                return {};
            }
            auto rc = stmt.step();
            return rc && *rc == SQLITE_ROW ? stmt.text(0) : std::string();
        } catch (...) {
            return {};
        }
    }

    // Called once, right after a live open (lock order: gWritersMutex, then mutex_).
    void registerWriter(const std::string& path)
    {
        std::lock_guard<std::mutex> registry(gWritersMutex);
        writerKey_ = canonicalKey(path);
        gWriters[writerKey_] = this;
    }

    // A consistent copy through this connection; the caller holds gWritersMutex.
    Status backupTo(const std::string& copy)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        Db out;
        RAY_TRY(out.open(copy, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
        sqlite3_backup* backup = sqlite3_backup_init(out.raw(), "main", db_.raw(), "main");
        if (!backup) {
            return tl::unexpected<Error>(out.error(sqlite3_errcode(out.raw()), "sqlite3_backup_init"));
        }
        const int rc = sqlite3_backup_step(backup, -1);
        sqlite3_backup_finish(backup);
        if (rc != SQLITE_DONE) {
            return tl::unexpected<Error>(out.error(rc, "sqlite3_backup_step"));
        }
        return {};
    }

    // ---- Introspection (file.h) ----
    std::uint64_t committed() const { return commits_.load(); }
    std::string settings() const
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto journal = db_.queryText("PRAGMA journal_mode");
        const auto sync = db_.queryInt("PRAGMA synchronous");
        const auto locking = db_.queryText("PRAGMA locking_mode");
        return "journal_mode=" + journal.value_or("?") + " synchronous=" + std::to_string(sync.value_or(-1)) +
               " locking_mode=" + locking.value_or("?");
    }
    const std::string& snapshot() const { return snapshot_; }

private:
    Status requireWritable() const
    {
        if (!lock_) {
            return fail(ErrorCode::Locked, "The document is open read-only", "no write lock");
        }
        return {};
    }

    static Status checkRefName(const std::string& name)
    {
        const bool head = name.rfind("heads/", 0) == 0;
        const bool tag = name.rfind("tags/", 0) == 0;
        const std::size_t prefix = head ? 6 : tag ? 5 : 0;
        if (prefix == 0 || name.size() == prefix || name.size() > 256) {
            return fail(ErrorCode::InvalidArgument, "A version name must be heads/<name> or tags/<name>", name);
        }
        for (const char c : name) {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
                return fail(ErrorCode::InvalidArgument, "A version name cannot contain control characters");
            }
        }
        return {};
    }

    Status refreshNamed(const Hash256& commit)
    {
        Stmt stmt;
        RAY_TRY(stmt.prepare(db_, "UPDATE commits SET named = EXISTS (SELECT 1 FROM refs WHERE commit_id = ?1 AND "
                                  "substr(name, 1, 5) = 'tags/') WHERE id = ?1"));
        stmt.bindBlob(1, hashBytes(commit));
        return stmt.run();
    }

    // One write transaction, bracketed by the lock file's save counter so snapshot readers never copy it halfway.
    template <class Body>
    Status writeTransaction(Body&& body)
    {
        lock_->beginWrite();
        Status result = db_.exec("BEGIN IMMEDIATE");
        if (result) {
            try {
                result = body();
            } catch (const std::exception& e) {
                result = tl::unexpected<Error>(internalError(e.what()));
            }
            if (result) {
                result = db_.exec("COMMIT");
            }
            if (!result) {
                (void)db_.exec("ROLLBACK");
            }
        }
        lock_->endWrite();
        return result;
    }

    Result<Commit> saveLocked(const SaveRequest& request, JobContext& ctx)
    {
        RAY_TRY(requireWritable());
        if (!request.state) {
            return fail(ErrorCode::InvalidArgument, "Nothing to save", "SaveRequest::state is null");
        }
        RAY_TRY(ctx.cancel.check());
        const doc::StateObjects objects = doc::serializeState(*request.state);
        const Hash256 root = hashObject(asBytes(objects.manifest));
        auto parent = readRef(db_, "heads/" + branch_);
        if (!parent) {
            return tl::unexpected<Error>(parent.error());
        }
        Commit commit;
        if (*parent) {
            commit.parents.push_back(**parent);
        }
        commit.root = root;
        commit.author = request.author;
        commit.unixTimeMs = unixNowMs();
        commit.message = request.message;
        commit.named = false;
        commit.id = commitIdOf(commit.parents, commit.root, commit.author, commit.unixTimeMs, commit.message);

        // Hashing and compression happen before the transaction: it then only inserts, so the time the lock file
        // says "saving" (and snapshot readers wait) is short. The exclusive lock keeps other writers out meanwhile.
        const double total = static_cast<double>(2 * (objects.features.size() + request.derived.size() + 1));
        double done = 0;
        const auto step = [&]() -> Status {
            done += 1;
            ctx.progress.set(done / total, "Saving");
            return ctx.cancel.check();
        };
        std::vector<PackedObject> packed;
        std::set<Hash256> seen;
        const auto pack = [&](ObjectKind kind, std::span<const std::byte> raw, const Hash256& hash) -> Status {
            if (seen.insert(hash).second) {
                auto object = packIfMissing(db_, kind, raw, hash);
                if (!object) {
                    return tl::unexpected<Error>(object.error());
                }
                if (*object) {
                    packed.push_back(std::move(**object));
                }
            }
            return step();
        };
        RAY_TRY(pack(ObjectKind::DocState, asBytes(objects.manifest), root));
        for (const auto& [hash, bytes] : objects.features) {
            RAY_TRY(pack(ObjectKind::Json, asBytes(bytes), hash));
        }
        const std::vector<std::byte>* thumbnail = nullptr;
        for (const auto& [kind, bytes] : request.derived) {
            RAY_TRY(pack(kind, bytes, hashObject(bytes)));
            if (kind == ObjectKind::Thumbnail) {
                thumbnail = &bytes;
            }
        }

        auto status = writeTransaction([&]() -> Status {
            for (const PackedObject& object : packed) {
                RAY_TRY(insertPacked(db_, object));
                RAY_TRY(step());
            }
            if (!request.derivedIndex.empty()) {
                Stmt index;
                // The caller's input hash already covers the kernel version (PLAN.md 4.1); SaveRequest does not
                // carry the version string itself, so the column records the RayCAD build instead.
                RAY_TRY(index.prepare(db_,
                                      "INSERT INTO derived (input_hash, object_hash, kernel_version) "
                                      "VALUES (?1, ?2, ?3) ON CONFLICT(input_hash) DO UPDATE SET "
                                      "object_hash = excluded.object_hash, kernel_version = excluded.kernel_version"));
                for (const auto& [input, object] : request.derivedIndex) {
                    auto exists = objectExists(db_, object);
                    if (!exists) {
                        return tl::unexpected<Error>(exists.error());
                    }
                    if (!*exists) {
                        return fail(ErrorCode::InvalidArgument, "A cached result refers to an object that is not saved",
                                    "derivedIndex " + input.hex() + " -> " + object.hex());
                    }
                    index.reset();
                    index.bindBlob(1, hashBytes(input));
                    index.bindBlob(2, hashBytes(object));
                    index.bindText(3, "raycad " RAY_VERSION_STRING);
                    RAY_TRY(index.run());
                }
            }
            Stmt row;
            RAY_TRY(row.prepare(db_, "INSERT OR IGNORE INTO commits (id, root, author, time_ms, message, named) "
                                     "VALUES (?1, ?2, ?3, ?4, ?5, 0)"));
            row.bindBlob(1, hashBytes(commit.id));
            row.bindBlob(2, hashBytes(commit.root));
            row.bindText(3, commit.author);
            row.bindInt(4, commit.unixTimeMs);
            row.bindText(5, commit.message);
            RAY_TRY(row.run());
            for (std::size_t i = 0; i < commit.parents.size(); ++i) {
                Stmt link;
                RAY_TRY(link.prepare(db_, "INSERT OR IGNORE INTO commit_parents (commit_id, ord, parent) "
                                          "VALUES (?1, ?2, ?3)"));
                link.bindBlob(1, hashBytes(commit.id));
                link.bindInt(2, static_cast<std::int64_t>(i));
                link.bindBlob(3, hashBytes(commit.parents[i]));
                RAY_TRY(link.run());
            }
            if (thumbnail) {
                Stmt thumb;
                RAY_TRY(thumb.prepare(db_, "INSERT OR REPLACE INTO thumbs (commit_id, png) VALUES (?1, ?2)"));
                thumb.bindBlob(1, hashBytes(commit.id));
                thumb.bindBlob(2, std::span<const std::byte>(*thumbnail));
                RAY_TRY(thumb.run());
            }
            Stmt ref;
            RAY_TRY(ref.prepare(db_, "INSERT INTO refs (name, commit_id) VALUES (?1, ?2) "
                                     "ON CONFLICT(name) DO UPDATE SET commit_id = excluded.commit_id"));
            ref.bindText(1, "heads/" + branch_);
            ref.bindBlob(2, hashBytes(commit.id));
            RAY_TRY(ref.run());
            Stmt units;
            RAY_TRY(units.prepare(db_, "INSERT OR REPLACE INTO meta (key, value) VALUES ('units', ?1)"));
            units.bindText(1, request.state->settings.units == UnitSystem::Metric ? "metric" : "inch");
            RAY_TRY(units.run());
            // The point of no return (CancelToken::close), just before COMMIT: a cancel before it rolls the save back,
            // one after it is too late, so the caller never reports Cancelled for a commit that landed.
            if (!ctx.cancel.close()) {
                return fail(ErrorCode::Cancelled, "Cancelled");
            }
            return {};
        });
        if (!status) {
            return tl::unexpected<Error>(status.error());
        }
        ctx.progress.set(1.0, "Saved");
        return commit;
    }

    std::string snapshot_;
    std::string writerKey_;
    std::unique_ptr<OwnedLock> lock_;
    mutable Db db_;
    mutable std::mutex mutex_;
    std::string branch_ = "main";
    std::atomic<std::uint64_t> commits_{0};
};

namespace {

bool backupFromOpenWriter(const std::string& path, const std::string& copy, Status& result)
{
    std::lock_guard<std::mutex> registry(gWritersMutex);
    const auto found = gWriters.find(canonicalKey(path));
    if (found == gWriters.end()) {
        return false;
    }
    result = found->second->backupTo(copy);
    return true;
}

} // namespace

const FileImpl& impl(const IDocumentFile& file)
{
    return dynamic_cast<const FileImpl&>(file);
}

std::uint64_t committedTransactions(const IDocumentFile& file)
{
    return impl(file).committed();
}

std::string connectionSettings(const IDocumentFile& file)
{
    return impl(file).settings();
}

std::string snapshotPath(const IDocumentFile& file)
{
    return impl(file).snapshot();
}

std::string fileIdOf(const IDocumentFile& file)
{
    const auto* found = dynamic_cast<const FileImpl*>(&file);
    return found ? found->fileId() : std::string();
}

Status compactAsOf(IDocumentFile& file, int keepMicroversionsDays, std::int64_t nowMs, JobContext& ctx,
                   CompactStats* stats)
{
    auto* found = dynamic_cast<FileImpl*>(&file);
    if (!found) {
        return fail(ErrorCode::InvalidArgument, "Not a RayCAD document file");
    }
    return found->compactAsOf(keepMicroversionsDays, nowMs, ctx, stats);
}

namespace {

// Opens the document for writing; `lock` is the lock file this process has just created.
Result<std::unique_ptr<IDocumentFile>> openLive(const std::string& path, std::unique_ptr<OwnedLock> lock,
                                                OpenInfo* info)
{
    // In exclusive locking mode SQLite keeps <name>.raycad-journal for the whole session and deletes it on a clean
    // close, so a journal found now means the last session did not end cleanly (PLAN.md 4.2).
    const bool unclean = pathExists(path + "-journal");
    Db db;
    const auto connect = [&db, &path]() -> Status {
        RAY_TRY(db.open(path, SQLITE_OPEN_READWRITE));
        sqlite3_busy_timeout(db.raw(), 2000);
        return db.exec("PRAGMA locking_mode = EXCLUSIVE");
    };
    RAY_TRY(connect());
    int version = 0;
    // Reads the header (rolling back a hot journal first, if there is one) before anything could write.
    RAY_TRY(checkHeader(db, version, true));
    if (info) {
        info->formatVersion = version;
    }
    if (version < kFormatVersion) {
        // PLAN.md 4.4: upgrade with the connection closed (the lock file stays ours), then open the result.
        db.close();
        RAY_TRY(migrateFile(path, version));
        RAY_TRY(connect());
        int migrated = 0;
        RAY_TRY(checkHeader(db, migrated));
        if (info) {
            info->migrated = true;
        }
    }
    auto journal = db.queryText("PRAGMA journal_mode = DELETE");
    if (!journal) {
        return tl::unexpected<Error>(journal.error());
    }
    if (*journal != "delete") {
        return fail(ErrorCode::IoError, "The document file cannot be opened safely", "journal_mode " + *journal);
    }
    RAY_TRY(db.exec("PRAGMA synchronous = FULL"));
    // Exclusive locking mode only takes the lock at the first write: take it now, so the lock file is not the only
    // guard between open and the first save.
    if (auto st = db.exec("BEGIN EXCLUSIVE; COMMIT"); !st) {
        (void)db.exec("ROLLBACK");
        return tl::unexpected<Error>(st.error());
    }
    if (unclean) {
        RAY_TRY(checkIntegrity(db));
    }
    RAY_TRY(checkSchema(db));
    lock->endWrite();
    auto file = std::make_unique<FileImpl>(std::move(db), std::move(lock), std::string());
    file->registerWriter(path);
    return std::unique_ptr<IDocumentFile>(std::move(file));
}

Status writeNewFile(const std::string& path)
{
    Db db;
    RAY_TRY(db.open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE));
    RAY_TRY(db.exec("PRAGMA journal_mode = DELETE; PRAGMA synchronous = FULL"));
    RAY_TRY(db.exec("BEGIN IMMEDIATE"));
    for (const std::string& statement : kSchema) {
        RAY_TRY(db.exec(statement));
    }
    RAY_TRY(db.exec("PRAGMA application_id = " + std::to_string(kApplicationId)));
    RAY_TRY(db.exec("PRAGMA user_version = " + std::to_string(kFormatVersion)));
    Stmt meta;
    RAY_TRY(meta.prepare(db, "INSERT INTO meta (key, value) VALUES (?1, ?2)"));
    const std::pair<std::string, std::string> rows[] = {
        {"file_id", Uuid::generate().toString()},
        {"created_by", "RayCAD " RAY_VERSION_STRING},
    };
    for (const auto& [key, value] : rows) {
        meta.reset();
        meta.bindText(1, key);
        meta.bindText(2, value);
        RAY_TRY(meta.run());
    }
    RAY_TRY(db.exec("COMMIT"));
    db.close();
    return {};
}

Result<std::unique_ptr<IDocumentFile>> createFile(const std::string& path)
{
    if (path.empty()) {
        return fail(ErrorCode::InvalidArgument, "No file name given");
    }
    if (pathExists(path)) {
        return fail(ErrorCode::InvalidArgument, "A file with this name already exists", path);
    }
    auto lock = OwnedLock::acquire(path);
    if (!lock) {
        return tl::unexpected<Error>(lock.error());
    }
    // Built under a temporary name and moved into place, so a crash never leaves a half-created document.
    const std::string temp = path + ".creating-" + randomName();
    const auto discard = [&temp] {
        removeQuietly(temp);
        removeQuietly(temp + "-journal");
    };
    if (auto st = writeNewFile(temp); !st) {
        discard();
        return tl::unexpected<Error>(st.error());
    }
    if (auto st = os::renameNoReplace(temp, path); !st) {
        discard();
        return tl::unexpected<Error>(st.error());
    }
    return openLive(path, std::move(*lock), nullptr);
}

Result<std::unique_ptr<IDocumentFile>> openFile(const std::string& path, OpenMode mode, OpenInfo* info)
{
    if (info) {
        *info = OpenInfo{0, false, false, std::nullopt};
    }
    if (path.empty() || !pathExists(path)) {
        return fail(ErrorCode::NotFound, "The document file does not exist", path);
    }
    std::optional<LockState> holder;
    for (int attempt = 0; attempt < 3 && !holder; ++attempt) {
        if (mode == OpenMode::ReadWrite) {
            auto lock = OwnedLock::acquire(path);
            if (lock) {
                return openLive(path, std::move(*lock), info);
            }
            if (lock.error().code != ErrorCode::Locked) {
                return tl::unexpected<Error>(lock.error());
            }
        }
        auto state = readLock(path);
        if (state) {
            holder = std::move(*state);
        } else if (state.error().code != ErrorCode::NotFound) {
            return tl::unexpected<Error>(state.error());
        } else if (mode == OpenMode::ReadOnly) {
            break; // nobody holds it
        }
    }
    if (info && holder) {
        info->lock = toLockInfo(*holder);
        info->openedReadOnlyBecauseLocked = mode == OpenMode::ReadWrite;
    }
    int upgradedFrom = -1;
    auto copy = takeSnapshot(path, upgradedFrom);
    if (!copy) {
        return tl::unexpected<Error>(copy.error());
    }
    Db db;
    if (auto st = db.open(*copy, SQLITE_OPEN_READONLY); !st) {
        removeQuietly(*copy);
        return tl::unexpected<Error>(st.error());
    }
    int version = 0;
    if (auto st = checkHeader(db, version); !st) {
        db.close();
        removeQuietly(*copy);
        return tl::unexpected<Error>(st.error());
    }
    if (info) {
        info->formatVersion = upgradedFrom >= 0 ? upgradedFrom : version;
        info->migrated = upgradedFrom >= 0;
    }
    return std::unique_ptr<IDocumentFile>(new FileImpl(std::move(db), nullptr, *copy));
}

} // namespace
} // namespace detail

Result<std::unique_ptr<IDocumentFile>> IDocumentFile::create(const std::string& path)
{
    try {
        return detail::createFile(path);
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(detail::internalError(e.what()));
    }
}

Result<std::unique_ptr<IDocumentFile>> IDocumentFile::open(const std::string& path, OpenMode mode, OpenInfo* info)
{
    try {
        return detail::openFile(path, mode, info);
    } catch (const std::exception& e) {
        return tl::unexpected<Error>(detail::internalError(e.what()));
    }
}

} // namespace ray::store
