// IDocumentFile::compact (store.h, PLAN.md 4.5). Private to src/store.
#pragma once
#include "file_db.h"

#include <ray/store.h>

#include <cstdint>
#include <functional>

namespace ray::store::detail {

// What compaction did, for logs and tests.
struct CompactStats {
    std::int64_t commitsKept = 0, commitsDropped = 0;
    std::int64_t objectsKept = 0, objectsDropped = 0;
};

// Compacts the open, writable connection `db`. `nowMs` is the clock the age policy is measured against.
// `beginWrite` and `endWrite` bracket every write (the lock file's save counter, so snapshot readers never copy a
// file that is being changed). No transaction may be open on `db`.
Status compactDatabase(Db& db, int keepMicroversionsDays, std::int64_t nowMs, JobContext& ctx,
                       const std::function<void()>& beginWrite, const std::function<void()>& endWrite,
                       CompactStats* stats = nullptr);

} // namespace ray::store::detail
