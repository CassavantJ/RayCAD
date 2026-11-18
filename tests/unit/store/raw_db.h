// A raw SQLite connection, for tests that look at a file the store has closed (or prove it cannot be read).
#pragma once
#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <string>
#include <vector>

namespace storetest {

// A raw SQLite connection for looking at a file the store has closed (or for proving it cannot be read).
struct RawDb {
    sqlite3* db = nullptr;
    explicit RawDb(const std::string& path, int flags = SQLITE_OPEN_READWRITE)
    {
        REQUIRE(sqlite3_open_v2(path.c_str(), &db, flags, nullptr) == SQLITE_OK);
    }
    ~RawDb() { sqlite3_close_v2(db); }
    int exec(const std::string& sql) { return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr); }
    std::vector<std::vector<std::string>> rows(const std::string& sql)
    {
        std::vector<std::vector<std::string>> out;
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
                const auto* text = sqlite3_column_text(stmt, i);
                row.emplace_back(text ? reinterpret_cast<const char*>(text) : "");
            }
            out.push_back(std::move(row));
        }
        sqlite3_finalize(stmt);
        return out;
    }
    std::string text(const std::string& sql)
    {
        auto r = rows(sql);
        return r.empty() || r[0].empty() ? std::string() : r[0][0];
    }
};

} // namespace storetest
