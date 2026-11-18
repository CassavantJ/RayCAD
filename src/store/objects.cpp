// Content-addressed objects: hashing, Zstandard, the `objects` table.
#include "objects.h"

#include <zstd.h>

#include <cstring>
#include <memory>

namespace ray::store::detail {
namespace {

// Level 3 is zstd's default: fast enough that a 40 MB B-rep costs well under a second, a good ratio on JSON.
constexpr int kZstdLevel = 3;

struct CCtxDeleter {
    void operator()(ZSTD_CCtx* ctx) const noexcept { ZSTD_freeCCtx(ctx); }
};
struct DCtxDeleter {
    void operator()(ZSTD_DCtx* ctx) const noexcept { ZSTD_freeDCtx(ctx); }
};

ZSTD_CCtx* compressionContext()
{
    thread_local std::unique_ptr<ZSTD_CCtx, CCtxDeleter> ctx(ZSTD_createCCtx());
    return ctx.get();
}

ZSTD_DCtx* decompressionContext()
{
    thread_local std::unique_ptr<ZSTD_DCtx, DCtxDeleter> ctx(ZSTD_createDCtx());
    return ctx.get();
}

} // namespace

Hash256 hashObject(std::span<const std::byte> raw)
{
    Hasher hasher;
    hasher.bytes(raw);
    return hasher.finish();
}

Result<std::vector<std::byte>> compressObject(std::span<const std::byte> raw)
{
    if (raw.size() > kMaxObjectBytes) {
        return fail(ErrorCode::InvalidArgument, "An object is too large to store in a document",
                    std::to_string(raw.size()) + " bytes");
    }
    ZSTD_CCtx* ctx = compressionContext();
    if (!ctx) {
        return fail(ErrorCode::Internal, "Out of memory", "ZSTD_createCCtx");
    }
    ZSTD_CCtx_reset(ctx, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, kZstdLevel);
    ZSTD_CCtx_setParameter(ctx, ZSTD_c_checksumFlag, 1);
    ZSTD_CCtx_setParameter(ctx, ZSTD_c_contentSizeFlag, 1);
    std::vector<std::byte> out(ZSTD_compressBound(raw.size()));
    const std::size_t written = ZSTD_compress2(ctx, out.data(), out.size(), raw.data(), raw.size());
    if (ZSTD_isError(written)) {
        return fail(ErrorCode::Internal, "Compression failed", ZSTD_getErrorName(written));
    }
    out.resize(written);
    return out;
}

Result<std::vector<std::byte>> decodeObject(std::span<const std::uint8_t> stored, std::int64_t rawSize,
                                            const Hash256& hash)
{
    const std::string where = "object " + hash.hex();
    if (rawSize < 0 || static_cast<std::uint64_t>(rawSize) > kMaxObjectBytes) {
        return fail(ErrorCode::FormatError, "The document file is damaged", where + ": bad raw_size");
    }
    // Checked before allocating, so a damaged size field cannot make us allocate gigabytes.
    const unsigned long long frameSize = ZSTD_getFrameContentSize(stored.data(), stored.size());
    if (frameSize == ZSTD_CONTENTSIZE_ERROR || frameSize == ZSTD_CONTENTSIZE_UNKNOWN ||
        frameSize != static_cast<unsigned long long>(rawSize)) {
        return fail(ErrorCode::FormatError, "The document file is damaged", where + ": bad compressed frame header");
    }
    ZSTD_DCtx* ctx = decompressionContext();
    if (!ctx) {
        return fail(ErrorCode::Internal, "Out of memory", "ZSTD_createDCtx");
    }
    std::vector<std::byte> out(static_cast<std::size_t>(rawSize));
    const std::size_t got = ZSTD_decompressDCtx(ctx, out.data(), out.size(), stored.data(), stored.size());
    if (ZSTD_isError(got) || got != out.size()) {
        return fail(ErrorCode::FormatError, "The document file is damaged",
                    where + ": " + (ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short object"));
    }
    if (hashObject(out) != hash) {
        return fail(ErrorCode::FormatError, "The document file is damaged", where + ": content hash mismatch");
    }
    return out;
}

Hash256 commitIdOf(const std::vector<Hash256>& parents, const Hash256& root, std::string_view author,
                   std::int64_t unixTimeMs, std::string_view message)
{
    Hasher hasher;
    hasher.str("raycad.commit.1");
    hasher.i64(static_cast<std::int64_t>(parents.size()));
    for (const Hash256& parent : parents) {
        hasher.bytes(std::as_bytes(std::span<const std::uint8_t>(parent.bytes)));
    }
    hasher.bytes(std::as_bytes(std::span<const std::uint8_t>(root.bytes)));
    hasher.str(author);
    hasher.i64(unixTimeMs);
    hasher.str(message);
    return hasher.finish();
}

bool hashFromBlob(std::span<const std::uint8_t> blob, Hash256& out)
{
    if (blob.size() != out.bytes.size()) {
        return false;
    }
    std::memcpy(out.bytes.data(), blob.data(), blob.size());
    return true;
}

Result<bool> objectExists(Db& db, const Hash256& hash)
{
    Stmt stmt;
    if (auto st = stmt.prepare(db, "SELECT 1 FROM objects WHERE hash = ?1"); !st) {
        return tl::unexpected<Error>(st.error());
    }
    stmt.bindBlob(1, hashBytes(hash));
    auto rc = stmt.step();
    if (!rc) {
        return tl::unexpected<Error>(rc.error());
    }
    return *rc == SQLITE_ROW;
}

Result<std::optional<PackedObject>> packIfMissing(Db& db, ObjectKind kind, std::span<const std::byte> raw,
                                                  const Hash256& hash)
{
    auto exists = objectExists(db, hash);
    if (!exists) {
        return tl::unexpected<Error>(exists.error());
    }
    if (*exists) {
        return std::optional<PackedObject>();
    }
    auto packed = compressObject(raw);
    if (!packed) {
        return tl::unexpected<Error>(packed.error());
    }
    return std::optional<PackedObject>(
        PackedObject{hash, kind, static_cast<std::int64_t>(raw.size()), std::move(*packed)});
}

Status insertPacked(Db& db, const PackedObject& object)
{
    Stmt stmt;
    if (auto st =
            stmt.prepare(db, "INSERT OR IGNORE INTO objects (hash, kind, raw_size, data) VALUES (?1, ?2, ?3, ?4)");
        !st) {
        return st;
    }
    stmt.bindBlob(1, hashBytes(object.hash));
    stmt.bindInt(2, static_cast<std::int64_t>(object.kind));
    stmt.bindInt(3, object.rawSize);
    stmt.bindBlob(4, std::span<const std::byte>(object.data));
    return stmt.run();
}

Status insertObject(Db& db, ObjectKind kind, std::span<const std::byte> raw, const Hash256& hash)
{
    auto packed = packIfMissing(db, kind, raw, hash);
    if (!packed) {
        return tl::unexpected<Error>(packed.error());
    }
    if (!*packed) {
        return {};
    }
    return insertPacked(db, **packed);
}

Result<std::vector<std::byte>> readObject(Db& db, const Hash256& hash)
{
    Stmt stmt;
    if (auto st = stmt.prepare(db, "SELECT raw_size, data FROM objects WHERE hash = ?1"); !st) {
        return tl::unexpected<Error>(st.error());
    }
    stmt.bindBlob(1, hashBytes(hash));
    auto rc = stmt.step();
    if (!rc) {
        return tl::unexpected<Error>(rc.error());
    }
    if (*rc != SQLITE_ROW) {
        return fail(ErrorCode::NotFound, "An object of the document is missing", "object " + hash.hex());
    }
    return decodeObject(stmt.blob(1), stmt.integer(0), hash);
}

} // namespace ray::store::detail
