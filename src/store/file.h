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
// root manifest and feature objects. May roll back a hot journal copied with it.
Status verifyCopy(const std::string& path);
// How long a read-only opener waits for an idle moment to copy in (30 s; tests shorten it).
void setSnapshotPatienceMs(std::int64_t ms) noexcept;

} // namespace ray::store::detail
