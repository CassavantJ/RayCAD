// Introspection of an open document file, for tests (tests/unit/store) only. Private to src/store.
#pragma once
#include <ray/store.h>

#include <cstdint>
#include <string>

namespace ray::store::detail {

// The `.raycad` schema of PLAN.md 4.1, one CREATE statement per entry, in creation order.
const std::vector<std::string>& schemaStatements();

// Number of write transactions this connection has committed since it was opened (SQLite's commit hook).
std::uint64_t committedTransactions(const IDocumentFile& file);
// "journal_mode=<m> synchronous=<n> locking_mode=<m>" as the open connection reports them.
std::string connectionSettings(const IDocumentFile& file);
// The local copy a read-only file was opened from, or "" for a live connection.
std::string snapshotPath(const IDocumentFile& file);
// The checks a snapshot copy passes before it is opened: header, PRAGMA integrity_check, every ref's commit id,
// root manifest and feature objects. May roll back a hot journal copied with it. With `upgradedFrom`, a copy in an
// older format that registered migrations can upgrade is upgraded in place first and its old format stored there.
Status verifyCopy(const std::string& path, int* upgradedFrom = nullptr);
// The document's file id (meta table), "" if it has none or `file` is not a document file.
std::string fileIdOf(const IDocumentFile& file);
// IDocumentFile::compact with the age policy measured against `nowMs` instead of the clock (tests).
struct CompactStats;
Status compactAsOf(IDocumentFile& file, int keepMicroversionsDays, std::int64_t nowMs, JobContext& ctx,
                   CompactStats* stats = nullptr);
// How long a read-only opener waits for an idle moment to copy in (30 s; tests shorten it).
void setSnapshotPatienceMs(std::int64_t ms) noexcept;

} // namespace ray::store::detail
