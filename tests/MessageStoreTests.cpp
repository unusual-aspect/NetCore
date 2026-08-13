#include <gtest/gtest.h>

#include "MessageStore.hpp"
#include "NetDefaults.hpp"
#include "PeerUtils.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path tempDbPath(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("netstore-" + name + ".db");
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(std::filesystem::path(path.string() + "-wal"), error);
    std::filesystem::remove(std::filesystem::path(path.string() + "-shm"), error);
    return path;
}

} // namespace

TEST(PeerUtils, LoopbackDetection) {
    EXPECT_TRUE(isLoopbackPeer("127.0.0.1"));
    EXPECT_TRUE(isLoopbackPeer("::1"));
    EXPECT_TRUE(isLoopbackPeer("::ffff:127.0.0.1"));
    EXPECT_FALSE(isLoopbackPeer("10.0.0.1"));
    EXPECT_FALSE(isLoopbackPeer("unknown"));
}

TEST(MessageStore, EmptyGetIsNullopt) {
    const auto path = tempDbPath("empty");
    MessageStore store(path.string(), 100);
    EXPECT_FALSE(store.get("127.0.0.1").has_value());
    EXPECT_GE(store.accessLogCount(), 1u);
}

TEST(MessageStore, PutGetRoundTrip) {
    const auto path = tempDbPath("roundtrip");
    MessageStore store(path.string(), 100);
    store.put("hello", "127.0.0.1");
    const auto value = store.get("127.0.0.1");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "hello");
}

TEST(MessageStore, SecondPutOverwrites) {
    const auto path = tempDbPath("overwrite");
    MessageStore store(path.string(), 100);
    store.put("first", "127.0.0.1");
    store.put("second", "127.0.0.1");
    const auto value = store.get("127.0.0.1");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "second");
}

TEST(MessageStore, SurvivesReopen) {
    const auto path = tempDbPath("reopen");
    {
        MessageStore store(path.string(), 100);
        store.put("persist-me", "127.0.0.1");
    }
    MessageStore store(path.string(), 100);
    const auto value = store.get("127.0.0.1");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "persist-me");
}

TEST(MessageStore, RetentionPrunesOldRows) {
    const auto path = tempDbPath("retain");
    MessageStore store(path.string(), 5);
    for (int index = 0; index < 20; ++index) {
        store.put("body-" + std::to_string(index), "127.0.0.1");
    }
    // Each put writes one audit row; prune keeps newest 5.
    EXPECT_LE(store.accessLogCount(), 5u);
}

TEST(MessageStore, LogRetainZeroDisablesPrune) {
    const auto path = tempDbPath("retain0");
    MessageStore store(path.string(), 0);
    for (int index = 0; index < 12; ++index) {
        store.put("body-" + std::to_string(index), "127.0.0.1");
    }
    EXPECT_EQ(store.accessLogCount(), 12u);
}

TEST(MessageStore, PutDetailIsTruncated) {
    const auto path = tempDbPath("detail");
    const std::string big(kMaxAccessLogDetailBytes + 200, 'Z');
    {
        MessageStore store(path.string(), 100);
        store.put(big, "127.0.0.1");
        // Live message body stays full.
        const auto value = store.get("127.0.0.1");
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(value->size(), big.size());
    }

    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &db), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
                    "SELECT detail FROM access_log WHERE op='PUT' ORDER BY id DESC LIMIT 1;",
                    -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    ASSERT_NE(text, nullptr);
    const std::string detail(text);
    EXPECT_LT(detail.size(), big.size());
    EXPECT_NE(detail.find("truncated,len="), std::string::npos);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

TEST(MessageStore, SqlInjectionPayloadsStayLiteralData) {
    const auto path = tempDbPath("sqli");
    MessageStore store(path.string(), 100);

    const char* payloads[] = {
        "'; DROP TABLE message;--",
        "' OR '1'='1",
        "1; ATTACH DATABASE 'evil.db' AS evil;--",
        "'); DELETE FROM access_log;--",
    };

    for (const char* payload : payloads) {
        const std::string body(payload);
        store.put(body, "127.0.0.1");
        const auto value = store.get("127.0.0.1");
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, body);
    }

    // Embedded NUL must not truncate binds or act as SQL terminator.
    std::string with_nul("ok");
    with_nul.push_back('\0');
    with_nul.append("; DROP TABLE message;--");
    store.put(with_nul, "127.0.0.1");
    {
        const auto value = store.get("127.0.0.1");
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, with_nul);
    }

    // Peer string is also bound, never interpolated.
    store.put("safe-body", "127.0.0.1'; DROP TABLE access_log;--");
    const auto value = store.get("127.0.0.1");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "safe-body");

    // Schema must still exist after all payloads.
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('message','access_log');",
                    -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 2);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

TEST(MessageStore, RejectsOversizeMessage) {
    const auto path = tempDbPath("toobig");
    MessageStore store(path.string(), 100);
    const std::string huge(kMaxMessageBytes + 1, 'X');
    EXPECT_THROW(store.put(huge, "127.0.0.1"), std::runtime_error);
    EXPECT_FALSE(store.get("127.0.0.1").has_value());
}
