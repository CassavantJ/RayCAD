// Format migration (store.h Migration, PLAN.md 4.4). Private to src/store.
#pragma once
#include <ray/store.h>

#include <string>

namespace ray::store::detail {

// True if registered steps lead from `version` to kFormatVersion (also for version == kFormatVersion).
bool canMigrate(int version);

// Migrates the closed document at `path` (user_version `from`, older than kFormatVersion; the caller holds its lock
// file): <name>.v<from>.bak first, then the steps on a working copy beside it, verified, moved over the original.
// On failure the original is untouched and the .bak removed. `backup` receives the .bak path.
Status migrateFile(const std::string& path, int from, std::string* backup = nullptr);

// Runs the steps from `from` on `path` itself (a private copy: a read-only open's snapshot) and verifies it.
Status migrateInPlace(const std::string& path, int from);

// The backup name of PLAN.md 4.4: <name>.v<N>.bak beside the document (name without .raycad).
std::string backupPathFor(const std::string& path, int version);

// Tests only: forgets every registered step.
void clearMigrations();

} // namespace ray::store::detail
