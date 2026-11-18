// Content-addressed objects in the `objects` table (PLAN.md 4.1): BLAKE3 keys, Zstandard at rest, verified reads.
// Private to src/store.
#pragma once
#include "file_db.h"

#include <ray/store.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ray::store::detail {

// The object key: BLAKE3-256 of the uncompressed bytes (the same hash doc::serializeState uses, so a DocState's
// manifest object is stored under DocState::stateHash()).
Hash256 hashObject(std::span<const std::byte> raw);

// Zstandard, one frame with the content size and a checksum. Largest object: kMaxObjectBytes.
inline constexpr std::size_t kMaxObjectBytes = 900u * 1024u * 1024u;
Result<std::vector<std::byte>> compressObject(std::span<const std::byte> raw);
// Decompresses and checks that the frame says `rawSize`, decodes to exactly `rawSize` bytes and hashes to `hash`.
Result<std::vector<std::byte>> decodeObject(std::span<const std::uint8_t> stored, std::int64_t rawSize,
                                            const Hash256& hash);

// Commit ids (store.h: "hash of (parents, root, author, time, message)").
Hash256 commitIdOf(const std::vector<Hash256>& parents, const Hash256& root, std::string_view author,
                   std::int64_t unixTimeMs, std::string_view message);

// An object compressed ahead of its transaction, so the write transaction (and the "saving" state that makes
// snapshot readers wait) stays short.
struct PackedObject {
    Hash256 hash;
    ObjectKind kind = ObjectKind::Json;
    std::int64_t rawSize = 0;
    std::vector<std::byte> data;
};

// Table access on an open connection. None of these starts or ends a transaction.
Result<bool> objectExists(Db& db, const Hash256& hash);
// Inserts the object unless it is already stored (then the bytes are not even compressed).
Status insertObject(Db& db, ObjectKind kind, std::span<const std::byte> raw, const Hash256& hash);
// The object compressed, or nullopt if it is stored already.
Result<std::optional<PackedObject>> packIfMissing(Db& db, ObjectKind kind, std::span<const std::byte> raw,
                                                  const Hash256& hash);
Status insertPacked(Db& db, const PackedObject& object);
// NotFound if absent; FormatError if the stored bytes do not decode to the hash.
Result<std::vector<std::byte>> readObject(Db& db, const Hash256& hash);

inline std::span<const std::uint8_t> hashBytes(const Hash256& hash)
{
    return {hash.bytes.data(), hash.bytes.size()};
}
// A 32-byte blob column as a hash; false if the column is not 32 bytes.
bool hashFromBlob(std::span<const std::uint8_t> blob, Hash256& out);

} // namespace ray::store::detail
