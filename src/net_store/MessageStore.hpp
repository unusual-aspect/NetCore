#pragma once

#include "NetDefaults.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;

// Singleton row (message.id=1) plus access_log, SQLite WAL.
//
// SQL injection: all client/peer/body values use sqlite3_bind_* on fixed SQL.
// exec() is literals only. Authorizer denies ATTACH/DROP/ALTER/…. Not a TLS
// substitute — protects the DB engine from hostile message content.
//
// put(): empty body is a no-op (no row, no audit). Otherwise BEGIN IMMEDIATE →
// upsert body+ts → INSERT access_log PUT → COMMIT. Both rows get the same
// utc::now_ns(). Then cache_ is updated.
// get(): INSERT access_log READ, then return cache_ (nullopt if never put).
// mutex_ is the message lock: put/get never interleave on the same store.
// Production path: StoreWorker also takes an exclusive message-Set lock around
// put so concurrent client Sets queue and run one-at-a-time (last commit wins).
// Throws std::runtime_error on SQLite failures (callers map to Opcode::Error).

class MessageStore {
public:
    explicit MessageStore(const std::string& path,
                          std::uint64_t access_log_max_rows = kDefaultAccessLogMaxRows);
    ~MessageStore();

    MessageStore(const MessageStore&) = delete;
    MessageStore& operator=(const MessageStore&) = delete;

    std::int64_t put(std::string_view body, std::string_view peer);
    std::optional<std::string> get(std::string_view peer);

    // Test / ops helpers
    std::uint64_t accessLogCount();
    std::uint64_t accessLogMaxRows() const { return access_log_max_rows_; }

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;          // message lock — put/get share the singleton row
    std::string cache_;         // last committed body (id=1)
    bool has_cache_ = false;    // false until first successful put or ctor load
    std::uint64_t access_log_max_rows_ = kDefaultAccessLogMaxRows;

    void exec(std::string_view sql);
    void pruneAccessLogLocked();
    void compactAfterPruneLocked(int deleted_rows);
    static std::string accessLogDetail(std::string_view body);
    static std::int64_t now_ns();

    std::uint32_t ops_since_compact_ = 0;  // batch WAL/vacuum after prune
};
