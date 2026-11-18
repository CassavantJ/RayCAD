// A thin RAII layer over the SQLite C API. Private to src/store; SQLite is never visible outside it.
#pragma once
#include "file_util.h"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ray::store::detail {

class Db {
public:
    Db() = default;
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&& other) noexcept : db_(other.db_) { other.db_ = nullptr; }
    Db& operator=(Db&& other) noexcept;

    // `flags` are SQLITE_OPEN_*; the path is UTF-8 (SQLite's own convention on every platform).
    Status open(const std::string& utf8Path, int flags);
    void close() noexcept;
    bool isOpen() const noexcept { return db_ != nullptr; }
    sqlite3* raw() const noexcept { return db_; }

    // Runs one or more statements, discarding rows.
    Status exec(std::string_view sql);
    // Runs one statement and returns the first column of the first row as text ("" if there is none).
    Result<std::string> queryText(std::string_view sql);
    Result<std::int64_t> queryInt(std::string_view sql);

    // Maps an SQLite result code to a RayCAD error (BUSY/LOCKED -> Locked, CORRUPT/NOTADB -> FormatError,
    // anything else -> IoError) and names the step that failed.
    Error error(int rc, std::string_view what) const;

private:
    sqlite3* db_ = nullptr;
};

class Stmt {
public:
    Stmt() = default;
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Status prepare(Db& db, std::string_view sql);
    // bindBlob does not copy: the bytes must stay valid until the statement is reset or destroyed.
    void bindBlob(int index, std::span<const std::uint8_t> bytes);
    void bindBlob(int index, std::span<const std::byte> bytes);
    void bindText(int index, std::string_view text); // copied
    void bindInt(int index, std::int64_t value);
    void bindNull(int index);
    // Returns SQLITE_ROW or SQLITE_DONE, or the error.
    Result<int> step();
    Status run(); // step() that must end with SQLITE_DONE
    void reset();

    std::span<const std::uint8_t> blob(int column) const;
    std::string text(int column) const;
    std::int64_t integer(int column) const;
    bool isNull(int column) const;

private:
    Db* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace ray::store::detail
