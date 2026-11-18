// IDocumentFile::compact (store.h, PLAN.md 4.5): prune old unnamed microversions, drop objects nothing needs, VACUUM.
//
// What is kept is worked out first, reading only; then one transaction deletes the rest; then VACUUM rebuilds the
// file. A kill at any point leaves either the old file or the compacted one: the deletions commit atomically through
// the rollback journal, and SQLite's VACUUM writes the rebuilt pages back through the same journal.
#include "compact.h"

#include "objects.h"

#include <ray/document.h>

#include <map>
#include <set>
#include <vector>

namespace ray::store::detail {
namespace {

#define RAY_CTRY(expr)                                                                                                 \
    do {                                                                                                               \
        if (auto ray_ctry_status_ = (expr); !ray_ctry_status_) {                                                       \
            return tl::unexpected<Error>(std::move(ray_ctry_status_.error()));                                         \
        }                                                                                                              \
    } while (false)

constexpr std::int64_t kMsPerDay = 86'400'000;

struct CommitRow {
    Hash256 root;
    std::int64_t timeMs = 0;
    bool named = false;
    std::vector<Hash256> parents;
};

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Every run of exactly 64 hex digits in `text`, read as a hash. A superset of the objects a JSON object names: kept
// objects are never missed because a feature type stores a hash under a key this version does not know.
void collectHexHashes(std::string_view text, std::set<Hash256>& out)
{
    std::size_t i = 0;
    while (i < text.size()) {
        if (hexValue(text[i]) < 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && hexValue(text[j]) >= 0) {
            ++j;
        }
        if (j - i == 64) {
            Hash256 h;
            for (std::size_t k = 0; k < 32; ++k) {
                h.bytes[k] = static_cast<std::uint8_t>(hexValue(text[i + 2 * k]) * 16 + hexValue(text[i + 2 * k + 1]));
            }
            out.insert(h);
        }
        i = j;
    }
}

std::string asText(const std::vector<std::byte>& bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Result<std::map<Hash256, CommitRow>> readCommits(Db& db)
{
    std::map<Hash256, CommitRow> commits;
    Stmt rows;
    RAY_CTRY(rows.prepare(db, "SELECT id, root, time_ms, named FROM commits"));
    for (;;) {
        auto rc = rows.step();
        if (!rc) {
            return tl::unexpected<Error>(rc.error());
        }
        if (*rc != SQLITE_ROW) {
            break;
        }
        Hash256 id;
        CommitRow row;
        if (!hashFromBlob(rows.blob(0), id) || !hashFromBlob(rows.blob(1), row.root)) {
            return fail(ErrorCode::FormatError, "The document file is damaged", "commits: bad id or root");
        }
        row.timeMs = rows.integer(2);
        row.named = rows.integer(3) != 0;
        commits.emplace(id, std::move(row));
    }
    Stmt parents;
    RAY_CTRY(parents.prepare(db, "SELECT commit_id, parent FROM commit_parents ORDER BY commit_id, ord"));
    for (;;) {
        auto rc = parents.step();
        if (!rc) {
            return tl::unexpected<Error>(rc.error());
        }
        if (*rc != SQLITE_ROW) {
            break;
        }
        Hash256 id, parent;
        if (!hashFromBlob(parents.blob(0), id) || !hashFromBlob(parents.blob(1), parent)) {
            return fail(ErrorCode::FormatError, "The document file is damaged", "commit_parents");
        }
        if (auto found = commits.find(id); found != commits.end()) {
            found->second.parents.push_back(parent);
        }
    }
    return commits;
}

Result<std::vector<Hash256>> readHashColumn(Db& db, const char* sql)
{
    std::vector<Hash256> out;
    Stmt stmt;
    RAY_CTRY(stmt.prepare(db, sql));
    for (;;) {
        auto rc = stmt.step();
        if (!rc) {
            return tl::unexpected<Error>(rc.error());
        }
        if (*rc != SQLITE_ROW) {
            return out;
        }
        Hash256 h;
        if (!hashFromBlob(stmt.blob(0), h)) {
            return fail(ErrorCode::FormatError, "The document file is damaged", sql);
        }
        out.push_back(h);
    }
}

// Adds every object one kept snapshot needs: its manifest, its feature objects, and whatever they name.
Status keepSnapshot(Db& db, const Hash256& root, std::set<Hash256>& keep)
{
    auto manifest = readObject(db, root);
    if (!manifest) {
        return tl::unexpected<Error>(manifest.error());
    }
    keep.insert(root);
    const std::string manifestText = asText(*manifest);
    collectHexHashes(manifestText, keep);
    auto state = doc::deserializeState(manifestText, [&db, &keep](const Hash256& hash) -> Result<std::string> {
        auto object = readObject(db, hash);
        if (!object) {
            return tl::unexpected<Error>(object.error());
        }
        keep.insert(hash);
        std::string text = asText(*object);
        collectHexHashes(text, keep);
        return text;
    });
    if (!state) {
        return tl::unexpected<Error>(state.error());
    }
    for (const Hash256& h : doc::referencedObjects(*state)) {
        keep.insert(h);
    }
    return {};
}

Status fillKeepTable(Db& db, const char* table, const std::set<Hash256>& hashes)
{
    Stmt insert;
    RAY_CTRY(insert.prepare(db, std::string("INSERT OR IGNORE INTO ") + table + " VALUES (?1)"));
    for (const Hash256& h : hashes) {
        insert.reset();
        insert.bindBlob(1, hashBytes(h));
        RAY_CTRY(insert.run());
    }
    return {};
}

int progressHandler(void* token)
{
    return static_cast<const CancelToken*>(token)->cancelled() ? 1 : 0;
}

} // namespace

Status compactDatabase(Db& db, int keepMicroversionsDays, std::int64_t nowMs, JobContext& ctx,
                       const std::function<void()>& beginWrite, const std::function<void()>& endWrite,
                       CompactStats* stats)
{
    if (keepMicroversionsDays < 0) {
        return fail(ErrorCode::InvalidArgument, "The age of versions to keep cannot be negative");
    }
    RAY_CTRY(ctx.cancel.check());
    ctx.progress.set(0.0, "Compacting");

    // ---- What to keep (reading only) ----
    auto commits = readCommits(db);
    if (!commits) {
        return tl::unexpected<Error>(commits.error());
    }
    auto refTargets = readHashColumn(db, "SELECT commit_id FROM refs ORDER BY name");
    if (!refTargets) {
        return tl::unexpected<Error>(refTargets.error());
    }
    std::set<Hash256> reachable;
    std::vector<Hash256> stack;
    for (const Hash256& target : *refTargets) {
        if (commits->find(target) == commits->end()) {
            return fail(ErrorCode::FormatError, "The document file is damaged",
                        "a ref names a missing commit " + target.hex());
        }
        stack.push_back(target);
    }
    while (!stack.empty()) {
        const Hash256 id = stack.back();
        stack.pop_back();
        const auto found = commits->find(id);
        if (found == commits->end() || !reachable.insert(id).second) {
            continue; // a parent pruned by an earlier compaction, or seen already
        }
        for (const Hash256& parent : found->second.parents) {
            stack.push_back(parent);
        }
    }
    const std::int64_t cutoff = nowMs - static_cast<std::int64_t>(keepMicroversionsDays) * kMsPerDay;
    std::set<Hash256> keepCommits(refTargets->begin(), refTargets->end());
    for (const auto& [id, row] : *commits) {
        if (row.named || (reachable.count(id) != 0 && row.timeMs >= cutoff)) {
            keepCommits.insert(id);
        }
    }

    std::set<Hash256> keepObjects;
    std::set<Hash256> roots;
    for (const Hash256& id : keepCommits) {
        roots.insert(commits->at(id).root);
    }
    double done = 0;
    for (const Hash256& root : roots) {
        if (auto st = keepSnapshot(db, root, keepObjects); !st) {
            return rethrow(st.error(), "compaction keeps nothing it cannot read; nothing was deleted");
        }
        done += 1;
        ctx.progress.set(0.5 * done / static_cast<double>(roots.size()), "Compacting");
        RAY_CTRY(ctx.cancel.check());
    }
    auto objectCount = db.queryInt("SELECT count(*) FROM objects");
    if (!objectCount) {
        return tl::unexpected<Error>(objectCount.error());
    }

    // ---- Delete the rest: one transaction ----
    RAY_CTRY(db.exec("CREATE TEMP TABLE IF NOT EXISTS compact_keep_commits (id BLOB PRIMARY KEY);"
                     "CREATE TEMP TABLE IF NOT EXISTS compact_keep_objects (hash BLOB PRIMARY KEY);"
                     "DELETE FROM temp.compact_keep_commits; DELETE FROM temp.compact_keep_objects"));
    std::int64_t objectsDropped = 0;
    beginWrite();
    Status deleted = db.exec("BEGIN IMMEDIATE");
    if (deleted) {
        deleted = [&]() -> Status {
            RAY_CTRY(fillKeepTable(db, "temp.compact_keep_commits", keepCommits));
            RAY_CTRY(fillKeepTable(db, "temp.compact_keep_objects", keepObjects));
            RAY_CTRY(
                db.exec("DELETE FROM commit_parents WHERE commit_id NOT IN (SELECT id FROM temp.compact_keep_commits);"
                        "DELETE FROM thumbs WHERE commit_id NOT IN (SELECT id FROM temp.compact_keep_commits);"
                        "DELETE FROM commits WHERE id NOT IN (SELECT id FROM temp.compact_keep_commits)"));
            RAY_CTRY(db.exec("DELETE FROM objects WHERE hash NOT IN (SELECT hash FROM temp.compact_keep_objects) "
                             "AND hash NOT IN (SELECT object_hash FROM derived)"));
            objectsDropped = sqlite3_changes64(db.raw());
            return db.exec("COMMIT");
        }();
        if (!deleted) {
            (void)db.exec("ROLLBACK");
        }
    }
    endWrite();
    (void)db.exec("DROP TABLE IF EXISTS temp.compact_keep_commits; DROP TABLE IF EXISTS temp.compact_keep_objects");
    if (!deleted) {
        return tl::unexpected<Error>(deleted.error());
    }
    if (stats) {
        stats->commitsKept = static_cast<std::int64_t>(keepCommits.size());
        stats->commitsDropped = static_cast<std::int64_t>(commits->size()) - stats->commitsKept;
        stats->objectsDropped = objectsDropped;
        stats->objectsKept = *objectCount - objectsDropped;
    }
    ctx.progress.set(0.6, "Compacting");

    // ---- Shrink the file ----
    RAY_CTRY(ctx.cancel.check());
    beginWrite();
    sqlite3_progress_handler(db.raw(), 1000, progressHandler, const_cast<CancelToken*>(&ctx.cancel));
    Status vacuum = db.exec("VACUUM");
    sqlite3_progress_handler(db.raw(), 0, nullptr, nullptr);
    endWrite();
    if (!vacuum) {
        if (ctx.cancel.cancelled()) {
            return fail(ErrorCode::Cancelled, "Compacting was cancelled; unused data was removed but the file was not "
                                              "made smaller");
        }
        return tl::unexpected<Error>(vacuum.error());
    }
    ctx.progress.set(1.0, "Compacted");
    return {};
}

} // namespace ray::store::detail
