#include "MessageStore.hpp"
#include "Dbg.hpp"
#include "NetDefaults.hpp"
#include "UtcTime.hpp"

#include <sqlite3.h>

#include <stdexcept>

namespace {

// Deny the ops that matter for injection / lateral file access.
// CREATE INDEX may be reported as REINDEX on some paths — do not blanket-deny REINDEX.
int sqlAuthorizer(void* /*user*/,
                  int action,
                  const char* /*arg1*/,
                  const char* /*arg2*/,
                  const char* /*db_name*/,
                  const char* /*trigger*/) {
    switch (action) {
        case SQLITE_ATTACH:
        case SQLITE_DETACH:
        case SQLITE_DROP_TABLE:
        case SQLITE_DROP_INDEX:
        case SQLITE_DROP_TEMP_TABLE:
        case SQLITE_DROP_TEMP_INDEX:
        case SQLITE_DROP_VIEW:
        case SQLITE_DROP_TEMP_VIEW:
        case SQLITE_DROP_TRIGGER:
        case SQLITE_DROP_TEMP_TRIGGER:
        case SQLITE_DROP_VTABLE:
        case SQLITE_ALTER_TABLE:
        case SQLITE_CREATE_VTABLE:
        case SQLITE_CREATE_TRIGGER:
        case SQLITE_CREATE_TEMP_TRIGGER:
        case SQLITE_CREATE_VIEW:
        case SQLITE_CREATE_TEMP_VIEW:
            return SQLITE_DENY;
        default:
            return SQLITE_OK;
    }
}

void hardenDb(sqlite3* db) {
    sqlite3_set_authorizer(db, sqlAuthorizer, nullptr);
#if defined(SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION)
    sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
#endif
    sqlite3_enable_load_extension(db, 0);
#if defined(SQLITE_DBCONFIG_DEFENSIVE)
    {
        int on = 1;
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, &on);
    }
#endif
#if defined(SQLITE_DBCONFIG_TRUSTED_SCHEMA)
    {
        int trusted = 0;
        sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &trusted);
    }
#endif
}

} // namespace

MessageStore::MessageStore(const std::string& path, std::uint64_t access_log_max_rows)
    : access_log_max_rows_(access_log_max_rows) {
    // No SQLITE_OPEN_URI — path is a filesystem path, not a URI attack surface.
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        const std::string error_message = db_ ? sqlite3_errmsg(db_) : "sqlite open failed";
        DBG("Cannot open the message store at '" + path + "' — " + error_message);
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error(error_message);
    }
    hardenDb(db_);

    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    // New DBs: reclaim pages after DELETE without a blocking VACUUM. No-op if already set.
    exec("PRAGMA auto_vacuum=INCREMENTAL;");
    // Wait briefly on lock contention instead of failing immediately.
    sqlite3_busy_timeout(db_, kSqliteBusyTimeoutMs);
    // One row for the live message, append-only audit next to it.
    exec(R"(
        CREATE TABLE IF NOT EXISTS message(
            id   INTEGER PRIMARY KEY CHECK(id = 1),
            body BLOB NOT NULL,
            ts   INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS access_log(
            id     INTEGER PRIMARY KEY AUTOINCREMENT,
            ts     INTEGER NOT NULL,
            peer   TEXT NOT NULL,
            op     TEXT NOT NULL,
            detail TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_access_ts ON access_log(ts);
    )");

    // Warm cache so first Read after restart does not hit the hot path cold.
    sqlite3_stmt* select_statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT body FROM message WHERE id=1;", -1, &select_statement, nullptr) != SQLITE_OK) {
        const std::string error_message = sqlite3_errmsg(db_);
        DBG("Cannot warm message cache — " + error_message);
        throw std::runtime_error(error_message);
    }
    const int step = sqlite3_step(select_statement);
    if (step == SQLITE_ROW) {
        // Zero-length BLOB: sqlite3_column_blob() returns NULL. The row still exists
        // (empty Set must round-trip across restart, distinct from never-set).
        const int byte_count = sqlite3_column_bytes(select_statement, 0);
        const auto* blob = static_cast<const char*>(sqlite3_column_blob(select_statement, 0));
        if (byte_count > 0 && blob != nullptr) {
            cache_.assign(blob, static_cast<std::size_t>(byte_count));
        } else {
            cache_.clear();
        }
        has_cache_ = true;
    } else if (step != SQLITE_DONE) {
        const std::string error_message = sqlite3_errmsg(db_);
        sqlite3_finalize(select_statement);
        DBG("Cannot warm message cache — " + error_message);
        throw std::runtime_error(error_message);
    }
    sqlite3_finalize(select_statement);
}

MessageStore::~MessageStore() {
    if (db_) {
        // Best-effort: shrink WAL on clean exit so overnight runs leave a compact file.
        char* sqlite_error = nullptr;
        sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, &sqlite_error);
        sqlite3_free(sqlite_error);
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void MessageStore::exec(std::string_view sql) {
    // Fixed literals only (pragma / schema / BEGIN-COMMIT-ROLLBACK).
    // Never pass client/peer/body strings here — that would be SQL injection.
    const std::string sql_text{sql};
    char* sqlite_error = nullptr;
    if (sqlite3_exec(db_, sql_text.c_str(), nullptr, nullptr, &sqlite_error) != SQLITE_OK) {
        const std::string message = sqlite_error ? sqlite_error : "sqlite error";
        sqlite3_free(sqlite_error);
        DBG("Cannot run SQL on the message store — " + message);
        throw std::runtime_error(message);
    }
}

std::int64_t MessageStore::now_ns() {
    return utc::now_ns();
}

std::string MessageStore::accessLogDetail(std::string_view body) {
    // Bound audit disk growth: keep a prefix, note original length when truncated.
    if (body.size() <= kMaxAccessLogDetailBytes) {
        return std::string(body);
    }
    std::string detail(body.substr(0, kMaxAccessLogDetailBytes));
    detail.append("...(truncated,len=");
    detail.append(std::to_string(body.size()));
    detail.push_back(')');
    return detail;
}

void MessageStore::compactAfterPruneLocked(int deleted_rows) {
    if (deleted_rows <= 0) {
        return;
    }
    // Don't TRUNCATE the WAL on every Read — batch so overnight traffic stays cheap.
    ++ops_since_compact_;
    if (deleted_rows < 100 && ops_since_compact_ < 64) {
        return;
    }
    ops_since_compact_ = 0;
    // Flush WAL sidecar and free deleted pages so the on-disk footprint tracks --log-retain.
    exec("PRAGMA wal_checkpoint(TRUNCATE);");
    exec("PRAGMA incremental_vacuum(64);");
}

void MessageStore::pruneAccessLogLocked() {
    // 0 = unbounded (operator opted out of prune — document in CLI / DECISIONS).
    if (access_log_max_rows_ == 0) {
        return;
    }
    // Keep the newest N rows by id. Safe under mutex_; called after audit inserts.
    sqlite3_stmt* prune_statement = nullptr;
    const char* sql =
        "DELETE FROM access_log WHERE id <= ("
        "  SELECT COALESCE(MAX(id), 0) - ?1 FROM access_log"
        ") AND (SELECT COUNT(*) FROM access_log) > ?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &prune_statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(prune_statement, 1, static_cast<sqlite3_int64>(access_log_max_rows_));
    const int step = sqlite3_step(prune_statement);
    sqlite3_finalize(prune_statement);
    if (step != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    compactAfterPruneLocked(sqlite3_changes(db_));
}

std::uint64_t MessageStore::accessLogCount() {
    std::lock_guard<std::mutex> guard(mutex_);
    sqlite3_stmt* count_statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM access_log;", -1, &count_statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    std::uint64_t count = 0;
    if (sqlite3_step(count_statement) == SQLITE_ROW) {
        count = static_cast<std::uint64_t>(sqlite3_column_int64(count_statement, 0));
    }
    sqlite3_finalize(count_statement);
    return count;
}

std::int64_t MessageStore::put(std::string_view body, std::string_view peer) {
    if (body.empty()) {
        DBG(netdbg::event("STORE PUT ignored", peer, "<empty>"));
        return 0;
    }
    if (exceedsMaxMessage(body.size())) {
        throw std::runtime_error("message too large");
    }
    // Message lock: serialize with get() and any other put() (tests / safety net).
    std::lock_guard<std::mutex> guard(mutex_);
    // Same UTC ns on the data row and the audit row.
    const auto timestamp_ns = now_ns();
    // SQLite reserved lock for this write transaction (writers don't overlap).
    exec("BEGIN IMMEDIATE;");

    sqlite3_stmt* update_statement = nullptr;
    sqlite3_stmt* log_statement = nullptr;
    try {
        // Upsert the singleton (id=1). body is bound as BLOB — never interpolated into SQL.
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO message(id, body, ts) VALUES(1, ?1, ?2) "
                "ON CONFLICT(id) DO UPDATE SET body=excluded.body, ts=excluded.ts;",
                -1, &update_statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_bind_blob(update_statement, 1, body.data(), static_cast<int>(body.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(update_statement, 2, timestamp_ns);
        if (sqlite3_step(update_statement) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(update_statement);
        update_statement = nullptr;

        // Audit: peer/detail bound as text parameters (injection-safe).
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO access_log(ts, peer, op, detail) VALUES(?1, ?2, 'PUT', ?3);",
                -1, &log_statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        const std::string detail = accessLogDetail(body);
        sqlite3_bind_int64(log_statement, 1, timestamp_ns);
        sqlite3_bind_text(log_statement, 2, peer.data(), static_cast<int>(peer.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(log_statement, 3, detail.data(), static_cast<int>(detail.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(log_statement) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        sqlite3_finalize(log_statement);
        log_statement = nullptr;

        pruneAccessLogLocked();
        exec("COMMIT;");
        cache_.assign(body.begin(), body.end());
        has_cache_ = true;
        DBG(netdbg::event("STORE PUT", peer, body));
        return timestamp_ns;
    } catch (...) {
        // Anything failed after BEGIN — drop statements and roll back.
        if (update_statement) {
            sqlite3_finalize(update_statement);
        }
        if (log_statement) {
            sqlite3_finalize(log_statement);
        }
        try {
            exec("ROLLBACK;");
        } catch (...) {
            // Already failing — surface the original error.
        }
        throw;
    }
}

std::optional<std::string> MessageStore::get(std::string_view peer) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto timestamp_ns = now_ns();

    // Every Read is an access — fail loudly if audit cannot be written.
    sqlite3_stmt* log_statement = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO access_log(ts, peer, op, detail) VALUES(?1, ?2, 'READ', NULL);",
            -1, &log_statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db_));
    }
    sqlite3_bind_int64(log_statement, 1, timestamp_ns);
    sqlite3_bind_text(log_statement, 2, peer.data(), static_cast<int>(peer.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(log_statement) != SQLITE_DONE) {
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(log_statement);
        throw std::runtime_error(message);
    }
    sqlite3_finalize(log_statement);
    pruneAccessLogLocked();

    // Never written (or process just started with an empty db).
    if (!has_cache_) {
        DBG(netdbg::event("STORE READ", peer, "<empty>"));
        return std::nullopt;
    }
    DBG(netdbg::event("STORE READ", peer, cache_));
    return cache_;
}
