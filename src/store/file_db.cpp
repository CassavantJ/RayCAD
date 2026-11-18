// RAII layer over the SQLite C API.
#include "file_db.h"

namespace ray::store::detail {

Db::~Db()
{
    close();
}

Db& Db::operator=(Db&& other) noexcept
{
    if (this != &other) {
        close();
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

Status Db::open(const std::string& utf8Path, int flags)
{
    close();
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(utf8Path.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        sqlite3_close_v2(db);
        const ErrorCode code = rc == SQLITE_CANTOPEN                        ? ErrorCode::IoError
                               : (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) ? ErrorCode::Locked
                                                                            : ErrorCode::IoError;
        return fail(code, "Cannot open the document file", "sqlite3_open_v2: " + message);
    }
    db_ = db;
    sqlite3_extended_result_codes(db_, 0);
    return {};
}

void Db::close() noexcept
{
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

Status Db::exec(std::string_view sql)
{
    const std::string text(sql);
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string detail = message ? message : sqlite3_errstr(rc);
        sqlite3_free(message);
        Error e = error(rc, text);
        e.context.insert(e.context.begin(), detail);
        return tl::unexpected<Error>(std::move(e));
    }
    return {};
}

Result<std::string> Db::queryText(std::string_view sql)
{
    Stmt stmt;
    if (auto st = stmt.prepare(*this, sql); !st) {
        return tl::unexpected<Error>(st.error());
    }
    auto rc = stmt.step();
    if (!rc) {
        return tl::unexpected<Error>(rc.error());
    }
    if (*rc != SQLITE_ROW) {
        return std::string();
    }
    return stmt.text(0);
}

Result<std::int64_t> Db::queryInt(std::string_view sql)
{
    Stmt stmt;
    if (auto st = stmt.prepare(*this, sql); !st) {
        return tl::unexpected<Error>(st.error());
    }
    auto rc = stmt.step();
    if (!rc) {
        return tl::unexpected<Error>(rc.error());
    }
    if (*rc != SQLITE_ROW) {
        return std::int64_t{0};
    }
    return stmt.integer(0);
}

Error Db::error(int rc, std::string_view what) const
{
    const int primary = rc & 0xff;
    ErrorCode code = ErrorCode::IoError;
    std::string message = "The document file could not be read or written";
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
        code = ErrorCode::Locked;
        message = "The document file is in use by another program";
    } else if (primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB) {
        code = ErrorCode::FormatError;
        message = "The document file is damaged or is not a RayCAD document";
    } else if (primary == SQLITE_FULL) {
        message = "The disk is full";
    } else if (primary == SQLITE_NOMEM) {
        code = ErrorCode::Internal;
        message = "Out of memory";
    } else if (primary == SQLITE_TOOBIG) {
        code = ErrorCode::InvalidArgument;
        message = "An object is too large for the document file";
    }
    std::string detail = std::string(what) + ": " + sqlite3_errstr(rc);
    if (db_ && sqlite3_errcode(db_) == rc) {
        detail += " (" + std::string(sqlite3_errmsg(db_)) + ")";
    }
    return makeError(code, std::move(message), std::move(detail));
}

Stmt::~Stmt()
{
    sqlite3_finalize(stmt_);
}

Status Stmt::prepare(Db& db, std::string_view sql)
{
    sqlite3_finalize(stmt_);
    stmt_ = nullptr;
    db_ = &db;
    const int rc = sqlite3_prepare_v2(db.raw(), sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        return tl::unexpected<Error>(db.error(rc, "prepare " + std::string(sql)));
    }
    return {};
}

void Stmt::bindBlob(int index, std::span<const std::uint8_t> bytes)
{
    // A zero-length blob must still be a blob, not NULL: SQLite binds NULL for a null pointer.
    static const std::uint8_t kEmpty = 0;
    sqlite3_bind_blob64(stmt_, index, bytes.empty() ? &kEmpty : bytes.data(), bytes.size(), SQLITE_STATIC);
}

void Stmt::bindBlob(int index, std::span<const std::byte> bytes)
{
    bindBlob(index, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

void Stmt::bindText(int index, std::string_view text)
{
    // Copied: callers often pass temporaries. Blobs are bound without a copy and must outlive the statement.
    sqlite3_bind_text64(stmt_, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
}

void Stmt::bindInt(int index, std::int64_t value)
{
    sqlite3_bind_int64(stmt_, index, value);
}

void Stmt::bindNull(int index)
{
    sqlite3_bind_null(stmt_, index);
}

Result<int> Stmt::step()
{
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
        return rc;
    }
    return tl::unexpected<Error>(db_->error(rc, std::string("step ") + sqlite3_sql(stmt_)));
}

Status Stmt::run()
{
    auto rc = step();
    if (!rc) {
        return tl::unexpected<Error>(rc.error());
    }
    return {};
}

void Stmt::reset()
{
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

std::span<const std::uint8_t> Stmt::blob(int column) const
{
    const void* data = sqlite3_column_blob(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    if (!data || size <= 0) {
        return {};
    }
    return {static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size)};
}

std::string Stmt::text(int column) const
{
    const unsigned char* data = sqlite3_column_text(stmt_, column);
    const int size = sqlite3_column_bytes(stmt_, column);
    if (!data || size <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(size));
}

std::int64_t Stmt::integer(int column) const
{
    return sqlite3_column_int64(stmt_, column);
}

bool Stmt::isNull(int column) const
{
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

} // namespace ray::store::detail
