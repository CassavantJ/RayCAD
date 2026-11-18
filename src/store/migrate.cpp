// Format migration (store.h Migration, PLAN.md 4.4): ordered single steps, run at open inside one transaction on a
// working copy that replaces the original only after it verified; the original is kept as <name>.v<N>.bak.
#include "migrate.h"

#include "file.h"
#include "file_db.h"
#include "journal_io.h"
#include "lock.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <system_error>

namespace ray::store {
namespace detail {
namespace {

#define RAY_MTRY(expr)                                                                                                 \
    do {                                                                                                               \
        if (auto ray_mtry_status_ = (expr); !ray_mtry_status_) {                                                       \
            return tl::unexpected<Error>(std::move(ray_mtry_status_.error()));                                         \
        }                                                                                                              \
    } while (false)

std::mutex gStepsMutex;
std::map<int, Migration> gSteps; // by `from`

struct StmtHandle {
    sqlite3_stmt* stmt = nullptr;
    ~StmtHandle() { sqlite3_finalize(stmt); }
};

class Ctx final : public MigrationCtx {
public:
    Ctx(Db& db, int fromVersion, int toVersion) : db_(db)
    {
        from = fromVersion;
        to = toVersion;
    }

    Status exec(std::string_view sql, const std::vector<SqlValue>& params) override
    {
        if (params.empty()) {
            return db_.exec(sql);
        }
        StmtHandle stmt;
        RAY_MTRY(prepare(sql, params, stmt));
        const int rc = sqlite3_step(stmt.stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            return tl::unexpected<Error>(db_.error(rc, "migration statement"));
        }
        return {};
    }

    Result<std::vector<std::vector<SqlValue>>> query(std::string_view sql, const std::vector<SqlValue>& params) override
    {
        StmtHandle stmt;
        RAY_MTRY(prepare(sql, params, stmt));
        std::vector<std::vector<SqlValue>> rows;
        for (;;) {
            const int rc = sqlite3_step(stmt.stmt);
            if (rc == SQLITE_DONE) {
                return rows;
            }
            if (rc != SQLITE_ROW) {
                return tl::unexpected<Error>(db_.error(rc, "migration query"));
            }
            std::vector<SqlValue> row;
            const int columns = sqlite3_column_count(stmt.stmt);
            for (int c = 0; c < columns; ++c) {
                switch (sqlite3_column_type(stmt.stmt, c)) {
                case SQLITE_INTEGER:
                    row.emplace_back(static_cast<std::int64_t>(sqlite3_column_int64(stmt.stmt, c)));
                    break;
                case SQLITE_FLOAT:
                    row.emplace_back(sqlite3_column_double(stmt.stmt, c));
                    break;
                case SQLITE_TEXT: {
                    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.stmt, c));
                    row.emplace_back(
                        std::string(text ? text : "", static_cast<std::size_t>(sqlite3_column_bytes(stmt.stmt, c))));
                    break;
                }
                case SQLITE_BLOB: {
                    const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(stmt.stmt, c));
                    const auto size = static_cast<std::size_t>(sqlite3_column_bytes(stmt.stmt, c));
                    row.emplace_back(data ? std::vector<std::byte>(data, data + size) : std::vector<std::byte>());
                    break;
                }
                default:
                    row.emplace_back(std::monostate{});
                    break;
                }
            }
            rows.push_back(std::move(row));
        }
    }

private:
    Status prepare(std::string_view sql, const std::vector<SqlValue>& params, StmtHandle& out)
    {
        const char* tail = nullptr;
        const int rc = sqlite3_prepare_v2(db_.raw(), sql.data(), static_cast<int>(sql.size()), &out.stmt, &tail);
        if (rc != SQLITE_OK || !out.stmt) {
            return tl::unexpected<Error>(db_.error(rc == SQLITE_OK ? SQLITE_MISUSE : rc, "migration statement"));
        }
        for (const char* p = tail; p && p < sql.data() + sql.size(); ++p) {
            if (*p != ' ' && *p != '\n' && *p != '\r' && *p != '\t' && *p != ';') {
                return fail(ErrorCode::InvalidArgument, "A migration statement with parameters must be one statement");
            }
        }
        if (static_cast<int>(params.size()) > sqlite3_bind_parameter_count(out.stmt)) {
            return fail(ErrorCode::InvalidArgument, "Too many parameters for a migration statement");
        }
        for (std::size_t i = 0; i < params.size(); ++i) {
            const int index = static_cast<int>(i) + 1;
            const SqlValue& value = params[i];
            int bound = SQLITE_OK;
            if (const auto* integer = std::get_if<std::int64_t>(&value)) {
                bound = sqlite3_bind_int64(out.stmt, index, *integer);
            } else if (const auto* real = std::get_if<double>(&value)) {
                bound = sqlite3_bind_double(out.stmt, index, *real);
            } else if (const auto* text = std::get_if<std::string>(&value)) {
                bound = sqlite3_bind_text64(out.stmt, index, text->data(), text->size(), SQLITE_TRANSIENT, SQLITE_UTF8);
            } else if (const auto* blob = std::get_if<std::vector<std::byte>>(&value)) {
                bound = sqlite3_bind_blob64(out.stmt, index, blob->data(), blob->size(), SQLITE_TRANSIENT);
            } else {
                bound = sqlite3_bind_null(out.stmt, index);
            }
            if (bound != SQLITE_OK) {
                return tl::unexpected<Error>(db_.error(bound, "binding a migration parameter"));
            }
        }
        return {};
    }

    Db& db_;
};

// The steps from `version` up to kFormatVersion, or nullopt if one is missing.
std::optional<std::vector<Migration>> chainFrom(int version)
{
    std::lock_guard<std::mutex> guard(gStepsMutex);
    std::vector<Migration> chain;
    for (int v = version; v < kFormatVersion; ++v) {
        const auto found = gSteps.find(v);
        if (found == gSteps.end()) {
            return std::nullopt;
        }
        chain.push_back(found->second);
    }
    return chain;
}

Status copyFile(const std::string& from, const std::string& to)
{
    std::error_code ec;
    std::filesystem::copy_file(toFsPath(from), toFsPath(to), std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return fail(ErrorCode::IoError, "Cannot copy the document to upgrade it", from + ": " + ec.message());
    }
    return jio::syncFile(to);
}

Status runSteps(Db& db, int from)
{
    auto chain = chainFrom(from);
    if (!chain) {
        return fail(ErrorCode::FormatError, "This document uses an old file format that cannot be read",
                    "no migration from user_version " + std::to_string(from));
    }
    RAY_MTRY(db.exec("BEGIN EXCLUSIVE"));
    Status result;
    for (const Migration& step : *chain) {
        Ctx ctx(db, step.from, step.to);
        try {
            result = step.run(ctx);
        } catch (const std::exception& e) {
            result = fail(ErrorCode::Internal, "A file format upgrade failed", e.what());
        } catch (...) {
            result = fail(ErrorCode::Internal, "A file format upgrade failed");
        }
        if (result) {
            result = db.exec("PRAGMA user_version = " + std::to_string(step.to));
        }
        if (!result) {
            result.error().context.push_back("migration " + std::to_string(step.from) + " -> " +
                                             std::to_string(step.to));
            break;
        }
    }
    if (result) {
        result = db.exec("COMMIT");
    }
    if (!result) {
        (void)db.exec("ROLLBACK");
    }
    return result;
}

} // namespace

bool canMigrate(int version)
{
    return version >= 0 && version <= kFormatVersion && chainFrom(version).has_value();
}

std::string backupPathFor(const std::string& path, int version)
{
    const auto fsPath = toFsPath(path);
    std::string name = fromFsPath(fsPath.filename());
    if (name.size() > kFileExtension.size() &&
        name.compare(name.size() - kFileExtension.size(), kFileExtension.size(), kFileExtension) == 0) {
        name.resize(name.size() - kFileExtension.size());
    }
    return fromFsPath(fsPath.parent_path() / toFsPath(name + ".v" + std::to_string(version) + ".bak"));
}

Status migrateInPlace(const std::string& path, int from)
{
    {
        Db db;
        RAY_MTRY(db.open(path, SQLITE_OPEN_READWRITE));
        RAY_MTRY(db.exec("PRAGMA journal_mode = DELETE; PRAGMA synchronous = FULL"));
        auto version = db.queryInt("PRAGMA user_version");
        if (!version) {
            return tl::unexpected<Error>(version.error());
        }
        if (*version != from) {
            return fail(ErrorCode::FormatError, "The document changed while it was being upgraded",
                        "user_version " + std::to_string(*version));
        }
        RAY_MTRY(runSteps(db, from));
    }
    auto verified = verifyCopy(path);
    if (!verified) {
        return rethrow(verified.error(), "checking the upgraded document");
    }
    return {};
}

Status migrateFile(const std::string& path, int from, std::string* backup)
{
    if (!canMigrate(from)) {
        return fail(ErrorCode::FormatError, "This document uses an old file format that cannot be read",
                    "no migration from user_version " + std::to_string(from));
    }
    // A <name>.raycad-journal beside the file is never copied: the caller's open has rolled back a hot one already,
    // so whatever is left is inert (SQLite ignores a journal that is not hot) and the database file alone is whole.
    // The backup never replaces an older one of the same name.
    std::string bak = backupPathFor(path, from);
    for (int n = 2; pathExists(bak); ++n) {
        const std::string first = backupPathFor(path, from);
        bak = first.substr(0, first.size() - 4) + "." + std::to_string(n) + ".bak";
    }
    const std::string bakTemp = bak + ".tmp-" + randomName();
    const std::string work = path + ".migrating-" + randomName();
    const auto cleanup = [&](bool removeBackup) {
        removeQuietly(work);
        removeQuietly(work + "-journal");
        removeQuietly(bakTemp);
        if (removeBackup) {
            removeQuietly(bak);
        }
    };
    Status st = copyFile(path, bakTemp);
    if (st) {
        st = os::renameNoReplace(bakTemp, bak);
    }
    if (st) {
        st = jio::syncFolderOf(bak);
    }
    if (!st) {
        cleanup(false);
        return rethrow(st.error(), "keeping a backup before the upgrade");
    }
    st = copyFile(path, work);
    if (st) {
        st = migrateInPlace(work, from);
    }
    if (st) {
        st = jio::syncFile(work);
    }
    if (st) {
        st = jio::replaceFile(work, path);
    }
    if (!st) {
        cleanup(true);
        return tl::unexpected<Error>(st.error());
    }
    if (backup) {
        *backup = bak;
    }
    ray::log(LogLevel::Info, "store",
             "Upgraded a document from file format " + std::to_string(from) + " to " + std::to_string(kFormatVersion) +
                 "; the original is kept as " + bak);
    return {};
}

void clearMigrations()
{
    std::lock_guard<std::mutex> guard(gStepsMutex);
    gSteps.clear();
}

} // namespace detail

void registerMigration(Migration migration)
{
    try {
        const std::string what = "migration " + std::to_string(migration.from) + " -> " + std::to_string(migration.to);
        if (migration.from < 0 || migration.to != migration.from + 1 || migration.to > kFormatVersion ||
            !migration.run) {
            ray::log(LogLevel::Error, "store", "Ignored an invalid " + what);
            return;
        }
        const int from = migration.from;
        std::lock_guard<std::mutex> guard(detail::gStepsMutex);
        if (!detail::gSteps.emplace(from, std::move(migration)).second) {
            ray::log(LogLevel::Error, "store", "Ignored a second " + what);
        }
    } catch (...) {
        ray::log(LogLevel::Error, "store", "Could not register a migration");
    }
}

} // namespace ray::store
