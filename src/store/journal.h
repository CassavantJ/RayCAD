// The recovery journal (store.h IRecoveryJournal, PLAN.md 4.2): what tests and the test helper need to see of it.
// Private to src/store.
//
// File layout: 8 magic bytes "RAYCJRNL", then records. A record is a 16-byte frame (payload length u32, sequence
// number u32, checksum u64, all little-endian) and its payload. The checksum is the first 8 bytes of BLAKE3 over
// (generation, sequence number, payload), so a record torn by a crash, flipped bits, or a stale record left behind a
// rewrite (another generation) all fail it, and reading stops at the first record that fails. Record 0 is the header
// (generation 0 in its checksum): format version, the generation of the records that follow, file id, file path,
// branch, base commit, creation time. Then one record per step or object, and check records: the stateHash() after
// an earlier step, written by the journal's own thread.
#pragma once
#include <ray/store.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ray::store::detail {

// Where journals live: setRecoveryDir("") (the default) means jio::defaultRecoveryDir(). Tests and their helper
// processes point it at a temp folder.
void setRecoveryDir(const std::string& utf8Dir);
std::string recoveryDir();
std::string journalPathFor(const std::string& fileId);

// Tests only: false stops new check records (the stateHash() after a step, written by the journal's own thread),
// so a journal holds exactly one record per step.
void setJournalChecking(bool on) noexcept;

// The byte offset just past each intact record of a journal file (the header first), for tests that tear or damage
// a chosen record. Empty if the header is missing or damaged.
std::vector<std::uint64_t> journalRecordEnds(const std::string& journalPath);

} // namespace ray::store::detail
