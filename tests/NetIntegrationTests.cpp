#include <gtest/gtest.h>

#include "AppConfig.hpp"
#include "ClientApp.hpp"
#include "ClientSession.hpp"
#include "AbstractNetSession.hpp"
#include "FrameCodec.hpp"
#include "MessageStore.hpp"
#include "NetDefaults.hpp"
#include "NetTransport.hpp"
#include "Opcode.hpp"
#include "PeerUtils.hpp"
#include "ProtocolLayers.hpp"
#include "ServerApp.hpp"
#include "UsRuntime.hpp"

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

std::filesystem::path tempDbPath(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("netint-" + name + ".db");
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(std::filesystem::path(path.string() + "-wal"), error);
    std::filesystem::remove(std::filesystem::path(path.string() + "-shm"), error);
    return path;
}

std::uint16_t uniquePort() {
    static std::atomic<std::uint16_t> next{20000};
    return next.fetch_add(1);
}

ServerSettings makeServerSettings(std::uint16_t port, const std::filesystem::path& db) {
    ServerSettings settings;
    settings.bind_host = "127.0.0.1";
    settings.port = port;
    settings.db_path = db.string();
    settings.access_log_max_rows = 1000;
    settings.allow_remote_shutdown = false;
    return settings;
}

ClientSettings makeSetClient(std::uint16_t port, std::string payload) {
    ClientSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.opcode = Opcode::Set;
    settings.payload = std::move(payload);
    settings.prompt_message = false;
    settings.live = false;
    return settings;
}

ClientSettings makeShutdownClient(std::uint16_t port) {
    ClientSettings settings;
    settings.host = "127.0.0.1";
    settings.port = port;
    settings.opcode = Opcode::Shutdown;
    settings.prompt_message = false;
    settings.live = false;
    return settings;
}

int oneShotSession(std::uint16_t port, Opcode command, std::string payload, std::string* ok_out = nullptr) {
    ClientSession session;
    session.setCommand(command, std::move(payload));
    NetTransport transport;
    transport.attach(session);
    if (!transport.connect("127.0.0.1", port)) {
        return 1;
    }
    transport.run();
    if (ok_out) {
        *ok_out = session.lastOkData();
    }
    return session.exitCode();
}

bool waitForServer(std::uint16_t port) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string ignored;
    while (std::chrono::steady_clock::now() < deadline) {
        if (oneShotSession(port, Opcode::Read, {}, &ignored) == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool waitUntil(const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void put_be16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void put_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::vector<std::uint8_t> rawFrame(std::uint16_t magic, std::uint32_t declared_len, std::string_view body) {
    std::vector<std::uint8_t> frame;
    put_be16(frame, magic);
    put_be32(frame, declared_len);
    put_be16(frame, kNetworkVersionWire);
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

// Fire a raw TCP payload then close. Used for malformed / oversized frames.
int writeRawAndClose(std::uint16_t port, std::vector<std::uint8_t> payload) {
    class RawSession : public AbstractNetSession {
    public:
        explicit RawSession(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

        void onConnected() override {
            if (auto* socket = nativeSocket()) {
                UsRuntime::write(socket, bytes_);
            }
            close();
        }

    protected:
        void parseNetProtocol(NetProtocol) override {}
        void onProtocolError(std::string_view) override { close(); }

    private:
        std::vector<std::uint8_t> bytes_;
    };

    RawSession session(std::move(payload));
    NetTransport transport;
    transport.attach(session);
    if (!transport.connect("127.0.0.1", port)) {
        return 1;
    }
    transport.run();
    return 0;
}

class CloseAfterSendSession : public ClientSession {
public:
    void onConnected() override {
        ClientSession::onConnected();
        close();
    }
};

// CLI Read has no query string. This sends a crafted Read with Data (SQL in "params").
int readWithQueryParams(std::uint16_t port, std::string params, std::string* ok_out) {
    class ReadWithParamsSession : public AbstractNetSession {
    public:
        explicit ReadWithParamsSession(std::string params) : params_(std::move(params)) {}

        int exit_code = 1;
        std::string ok_data;

        void onConnected() override {
            sendData(NetProtocol(Opcode::Read, params_, 1));
            UsRuntime::armIdleTimeout(nativeSocket(), kOneShotReplyTimeoutSec);
        }

    protected:
        void parseNetProtocol(NetProtocol data) override {
            if (data.getMsgType() == Opcode::Ok) {
                ok_data = std::string(data.getMsgData());
                exit_code = 0;
                close();
                return;
            }
            if (data.getMsgType() == Opcode::Error) {
                exit_code = 1;
                close();
            }
        }

        void onProtocolError(std::string_view) override {
            exit_code = 1;
            close();
        }

    private:
        std::string params_;
    };

    ReadWithParamsSession session(std::move(params));
    NetTransport transport;
    transport.attach(session);
    if (!transport.connect("127.0.0.1", port)) {
        return 1;
    }
    transport.run();
    if (ok_out) {
        *ok_out = session.ok_data;
    }
    return session.exit_code;
}

} // namespace

TEST(NetIntegration, ReadSetShutdownLoopback) {
    const auto db = tempDbPath("rw");
    const std::uint16_t port = uniquePort();
    ServerSettings settings;
    settings.bind_host = "127.0.0.1";
    settings.port = port;
    settings.db_path = db.string();
    settings.access_log_max_rows = 1000;
    settings.allow_remote_shutdown = false;

    ServerApp app(settings);
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    {
        ClientSession session;
        session.setCommand(Opcode::Set, "integration-hello");
        NetTransport transport;
        transport.attach(session);
        ASSERT_TRUE(transport.connect("127.0.0.1", port));
        transport.run();
        EXPECT_EQ(session.exitCode(), 0);
    }

    {
        ClientSession session;
        session.setCommand(Opcode::Read, {});
        NetTransport transport;
        transport.attach(session);
        ASSERT_TRUE(transport.connect("127.0.0.1", port));
        transport.run();
        EXPECT_EQ(session.exitCode(), 0);
    }

    {
        ClientSession session;
        session.setCommand(Opcode::Shutdown, {});
        NetTransport transport;
        transport.attach(session);
        ASSERT_TRUE(transport.connect("127.0.0.1", port));
        transport.run();
        EXPECT_EQ(session.exitCode(), 0);
    }

    server_thread.join();
}

TEST(NetIntegration, RemoteShutdownRefusedByHooks) {
    std::atomic<bool> stopped{false};
    bool refused = false;
    auto stack = protocol::makeServerStack({
        .peer = [] { return std::string("10.0.0.5"); },
        .send =
            [&](const NetProtocol& msg) {
                if (msg.getMsgType() == Opcode::Error) {
                    refused = (msg.getMsgData() == "shutdown not allowed");
                }
                return true;
            },
        .onRead = [](auto done) { done(true, {}); },
        .onSet = [](std::string_view, auto done) { done(true, {}); },
        .onShutdown =
            [&](const NetProtocol&) {
                if (!isLoopbackPeer("10.0.0.5")) {
                    return false;
                }
                stopped = true;
                return true;
            },
    });

    EXPECT_TRUE(stack.dispatch(NetProtocol(Opcode::Shutdown)));
    EXPECT_TRUE(refused);
    EXPECT_FALSE(stopped.load());
}

TEST(NetIntegration, MaxConnectionsRejectsExtra) {
    const auto db = tempDbPath("maxconn");
    ServerSettings settings;
    settings.bind_host = "127.0.0.1";
    settings.port = uniquePort();
    settings.db_path = db.string();

    ServerApp app(settings);
    app.transport().setMaxConnections(1);

    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(settings.port));

    ClientSession live;
    live.setKeepAlive(true);
    NetTransport first;
    first.attach(live);
    ASSERT_TRUE(first.connect("127.0.0.1", settings.port));

    std::thread first_thread([&] { first.run(); });
    ASSERT_TRUE(waitUntil([&] { return app.transport().metrics().accepts >= 2; }))
        << "kept-alive client did not get accepted";

    ClientSession second_session;
    second_session.setCommand(Opcode::Read, {});
    NetTransport second;
    second.attach(second_session);
    // Connect may succeed at TCP level then be closed by accept limit.
    second.connect("127.0.0.1", settings.port);
    second.run();

    // Stop server and the kept-alive first client.
    app.transport().post([&] { app.transport().stop(); });
    first.post([&] { live.endLive(); });
    first_thread.join();
    server_thread.join();

    EXPECT_GE(app.transport().metrics().rejected_conns, 1u);
    EXPECT_GE(app.transport().metrics().accepts, 2u);
}

// --- App scenarios (ClientApp + ServerApp) ---------------------------------

// Second NetServer on the same bind/port fails listen and never opens SQLite.
TEST(NetIntegration, DuplicateBindDoesNotOpenSecondDb) {
    const std::uint16_t port = uniquePort();
    const auto first_db = tempDbPath("dup-first");
    const auto second_db = tempDbPath("dup-second");

    ServerApp first(makeServerSettings(port, first_db));
    std::thread first_thread([&] { EXPECT_EQ(first.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    // Same host:port, different --db. Listen must fail before MessageStore opens.
    ServerApp second(makeServerSettings(port, second_db));
    EXPECT_EQ(second.run(), 1);
    EXPECT_FALSE(std::filesystem::exists(second_db));
    EXPECT_FALSE(std::filesystem::exists(second_db.string() + "-wal"));
    EXPECT_FALSE(std::filesystem::exists(second_db.string() + "-shm"));

    // First instance still owns the port and can serve.
    EXPECT_EQ(oneShotSession(port, Opcode::Set, "dup-bind-ok"), 0);
    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "dup-bind-ok");
    EXPECT_TRUE(std::filesystem::exists(first_db));

    first.transport().post([&] { first.transport().stop(); });
    first_thread.join();
}

// 1) NetClient with no server: wait/retry then exit "not available".
TEST(NetAppScenarios, ClientTimesOutWhenServerDown) {
    const std::uint16_t port = uniquePort();
    ClientApp client(makeSetClient(port, "no-server"));
    client.setRecoverWait(std::chrono::milliseconds(200));

    const auto started = std::chrono::steady_clock::now();
    const int code = client.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(code, 1);
    EXPECT_GE(elapsed, std::chrono::milliseconds(150));
}

// 2) Server up → client Set succeeds → client exits → server still accepting.
TEST(NetAppScenarios, SetWorksThenClientLeavesServerRunning) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-set-keep");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    ClientApp client(makeSetClient(port, "keep-server-up"));
    EXPECT_EQ(client.run(), 0);

    // Server must still be alive: another Read succeeds.
    {
        ClientSession session;
        session.setCommand(Opcode::Read, {});
        NetTransport transport;
        transport.attach(session);
        ASSERT_TRUE(transport.connect("127.0.0.1", port));
        transport.run();
        EXPECT_EQ(session.exitCode(), 0);
    }

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
    EXPECT_GE(app.transport().metrics().sets, 1u);
    EXPECT_GE(app.transport().metrics().reads, 1u);
}

// 3) Client connected (live) after a Set → kill server → client sees server down.
TEST(NetAppScenarios, KillServerWhileClientConnected) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-kill");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    ClientSession live;
    live.setKeepAlive(true);
    NetTransport transport;
    transport.attach(live);

    std::atomic<int> prompt_count{0};
    live.setOnReadyForPrompt([&] {
        // First prompt (after Version): send one Set. Ignore later Ok prompts.
        if (prompt_count.fetch_add(1) == 0) {
            live.sendLiveSet("before-kill");
        }
    });

    ASSERT_TRUE(transport.connect("127.0.0.1", port));
    std::thread client_thread([&] { transport.run(); });

    // Wait until the Set completed (server metrics) then kill the server.
    for (int attempt = 0; attempt < 50 && app.transport().metrics().sets < 1; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GE(app.transport().metrics().sets, 1u);

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
    client_thread.join();

    // Abrupt stop is not a graceful Shutdown goodbye — live session stays failed.
    EXPECT_NE(live.exitCode(), 0);
}

// 4) Fresh server → Read empty body succeeds.
TEST(NetAppScenarios, ReadEmptyWhenNeverSet) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-read-empty");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_TRUE(body.empty());

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// 5) Set then Read round-trip via ClientApp / session.
TEST(NetAppScenarios, SetThenReadRoundTrip) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-roundtrip");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "round-trip-body")).run(), 0);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "round-trip-body");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// Empty Set is Ok but does not insert or overwrite.
TEST(NetAppScenarios, EmptySetDoesNotChangeStoredMessage) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-empty-set");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));
    EXPECT_EQ(app.transport().metrics().sets, 0u);

    EXPECT_EQ(oneShotSession(port, Opcode::Set, {}), 0);
    EXPECT_EQ(app.transport().metrics().sets, 0u);
    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_TRUE(body.empty());

    EXPECT_EQ(ClientApp(makeSetClient(port, "keep-me")).run(), 0);
    EXPECT_EQ(app.transport().metrics().sets, 1u);
    EXPECT_EQ(oneShotSession(port, Opcode::Set, {}), 0);
    EXPECT_EQ(app.transport().metrics().sets, 1u);
    body.clear();
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "keep-me");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// Client Set of SQL text must be stored as the message body, not executed.
TEST(NetAppScenarios, ClientSqlInjectionPayloadIsLiteralData) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-sqli");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    const char* payloads[] = {
        "'; DROP TABLE message;--",
        "DROP TABLE access_log;",
        "' OR '1'='1",
        "1; ATTACH DATABASE 'evil.db' AS evil;--",
        "'); DELETE FROM access_log;--",
    };
    for (const char* payload : payloads) {
        EXPECT_EQ(ClientApp(makeSetClient(port, payload)).run(), 0) << payload;
        std::string body;
        EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0) << payload;
        EXPECT_EQ(body, payload);
    }

    EXPECT_EQ(ClientApp(makeSetClient(port, "still-here")).run(), 0);
    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "still-here");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();

    sqlite3* sqlite = nullptr;
    ASSERT_EQ(sqlite3_open_v2(db.string().c_str(), &sqlite, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(sqlite,
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('message','access_log');",
                    -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 2);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_prepare_v2(sqlite, "SELECT body FROM message WHERE id=1;", -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)), "still-here");
    sqlite3_finalize(statement);
    sqlite3_close(sqlite);
}

// Crafted Read with SQL in Data must not execute; stored message is unchanged.
TEST(NetAppScenarios, ClientReadQueryParamsAreNotSql) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-read-sqli");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "safe-message")).run(), 0);

    const char* params[] = {
        "'; DROP TABLE message;--",
        "DROP TABLE access_log;",
        "?id=1 OR 1=1",
        "' OR '1'='1",
        "1; ATTACH DATABASE 'evil.db' AS evil;--",
        "'); DELETE FROM access_log;--",
    };
    for (const char* param : params) {
        std::string body;
        EXPECT_EQ(readWithQueryParams(port, param, &body), 0) << param;
        EXPECT_EQ(body, "safe-message") << param;
    }

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "safe-message");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();

    sqlite3* sqlite = nullptr;
    ASSERT_EQ(sqlite3_open_v2(db.string().c_str(), &sqlite, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(sqlite,
                    "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('message','access_log');",
                    -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 2);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_prepare_v2(sqlite, "SELECT body FROM message WHERE id=1;", -1, &statement, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)), "safe-message");
    sqlite3_finalize(statement);
    sqlite3_close(sqlite);
}

// 6) Second Set overwrites; Read sees the latest.
TEST(NetAppScenarios, SecondSetOverwrites) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-overwrite");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "first")).run(), 0);
    EXPECT_EQ(ClientApp(makeSetClient(port, "second")).run(), 0);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "second");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// 7) Shutdown with no server does not wait/retry (unlike Read/Set).
TEST(NetAppScenarios, ShutdownDoesNotRetryWhenServerDown) {
    const std::uint16_t port = uniquePort();
    ClientApp client(makeShutdownClient(port));
    client.setRecoverWait(std::chrono::seconds(5));

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(client.run(), 1);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(8));
}

// 8) Live client receives graceful Shutdown goodbye (exit 0); server stops.
TEST(NetAppScenarios, GracefulShutdownNotifiesLiveClient) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-goodbye");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    ClientSession live;
    live.setKeepAlive(true);
    NetTransport live_transport;
    live_transport.attach(live);
    ASSERT_TRUE(live_transport.connect("127.0.0.1", port));
    std::thread live_thread([&] { live_transport.run(); });
    ASSERT_TRUE(waitUntil([&] { return app.transport().metrics().accepts >= 2; }))
        << "live client was not accepted";

    EXPECT_EQ(ClientApp(makeShutdownClient(port)).run(), 0);

    live_thread.join();
    server_thread.join();
    EXPECT_EQ(live.exitCode(), 0);
    EXPECT_GE(app.transport().metrics().shutdowns, 1u);
}

// 9) Server comes back during ClientApp recover wait → Set succeeds on retry.
TEST(NetAppScenarios, ClientRecoversWhenServerStartsDuringWait) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-recover");

    ClientApp client(makeSetClient(port, "after-restart"));
    client.setRecoverWait(std::chrono::milliseconds(400));

    std::thread client_thread([&] { EXPECT_EQ(client.run(), 0); });

    // First attempt fails; start server during the recover wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });

    client_thread.join();

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "after-restart");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// 10) Two clients: one Sets, the other Reads the shared message.
TEST(NetAppScenarios, TwoClientsShareMessage) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-two");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "shared")).run(), 0);

    std::string body_a;
    std::string body_b;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body_a), 0);
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body_b), 0);
    EXPECT_EQ(body_a, "shared");
    EXPECT_EQ(body_b, "shared");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// 11) Message survives server process restart (same db path).
TEST(NetAppScenarios, MessageSurvivesServerRestart) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-persist");

    {
        ServerApp app(makeServerSettings(port, db));
        std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
        ASSERT_TRUE(waitForServer(port));
        EXPECT_EQ(ClientApp(makeSetClient(port, "persisted")).run(), 0);
        app.transport().post([&] { app.transport().stop(); });
        server_thread.join();
    }

    {
        ServerApp app(makeServerSettings(port, db));
        std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
        ASSERT_TRUE(waitForServer(port));
        std::string body;
        EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
        EXPECT_EQ(body, "persisted");
        app.transport().post([&] { app.transport().stop(); });
        server_thread.join();
    }
}

// 12) Live client can Set twice on one TCP session.
TEST(NetAppScenarios, LiveClientTwoSetsOneConnection) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-live-two");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    ClientSession live;
    live.setKeepAlive(true);
    NetTransport transport;
    transport.attach(live);

    std::atomic<int> prompts{0};
    live.setOnReadyForPrompt([&] {
        const int n = prompts.fetch_add(1);
        if (n == 0) {
            live.sendLiveSet("live-one");
        } else if (n == 1) {
            live.sendLiveSet("live-two");
        } else {
            transport.post([&] { live.endLive(); });
        }
    });

    ASSERT_TRUE(transport.connect("127.0.0.1", port));
    transport.run();

    EXPECT_EQ(live.exitCode(), 0);
    EXPECT_GE(app.transport().metrics().sets, 2u);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "live-two");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// Concurrent Sets — message lock serializes writers; last commit wins; all succeed.
TEST(NetAppScenarios, ConcurrentSetsLastWriteWins) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-concurrent");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    constexpr int kClients = 10;
    std::atomic<int> done{0};
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (int index = 0; index < kClients; ++index) {
        clients.emplace_back([port, index, &done] {
            const std::string body = "from-" + std::to_string(index);
            EXPECT_EQ(ClientApp(makeSetClient(port, body)).run(), 0);
            done.fetch_add(1);
        });
    }
    for (auto& client : clients) {
        client.join();
    }
    EXPECT_EQ(done.load(), kClients);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body.rfind("from-", 0), 0u);
    EXPECT_GE(app.transport().metrics().sets, static_cast<std::uint64_t>(kClients));

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

TEST(NetAppScenarios, ConcurrentReadersSeeConsistentMessage) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-readers");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "shared-read")).run(), 0);

    constexpr int kReaders = 12;
    std::vector<std::string> bodies(static_cast<std::size_t>(kReaders));
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int index = 0; index < kReaders; ++index) {
        readers.emplace_back([port, &bodies, index] {
            EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &bodies[static_cast<std::size_t>(index)]), 0);
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    for (const auto& body : bodies) {
        EXPECT_EQ(body, "shared-read");
    }

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

TEST(NetAppScenarios, ConcurrentReadersAndWriters) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-rw-mix");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "base")).run(), 0);

    constexpr int kWriters = 8;
    constexpr int kReaders = 8;
    std::atomic<int> writes_ok{0};
    std::atomic<int> reads_ok{0};
    std::vector<std::thread> workers;
    workers.reserve(kWriters + kReaders);

    for (int index = 0; index < kWriters; ++index) {
        workers.emplace_back([port, index, &writes_ok] {
            EXPECT_EQ(ClientApp(makeSetClient(port, "from-" + std::to_string(index))).run(), 0);
            writes_ok.fetch_add(1);
        });
    }
    for (int index = 0; index < kReaders; ++index) {
        workers.emplace_back([port, &reads_ok] {
            std::string body;
            EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
            EXPECT_TRUE(body == "base" || body.rfind("from-", 0) == 0u);
            reads_ok.fetch_add(1);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(writes_ok.load(), kWriters);
    EXPECT_EQ(reads_ok.load(), kReaders);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body.rfind("from-", 0), 0u);

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

TEST(NetAppScenarios, ConnectDisconnectChurn) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-churn");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "churn")).run(), 0);
    for (int index = 0; index < 20; ++index) {
        std::string body;
        EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0) << "cycle " << index;
        EXPECT_EQ(body, "churn");
    }

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

TEST(NetAppScenarios, ClientDisconnectDuringSetDoesNotKillServer) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-disc");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    CloseAfterSendSession session;
    session.setCommand(Opcode::Set, "disconnect-persist");
    NetTransport transport;
    transport.attach(session);
    ASSERT_TRUE(transport.connect("127.0.0.1", port));
    transport.run();

    // Closing immediately after send can RST before the server reads (TCP).
    // The requirement under test is that the server keeps serving, not that
    // an unflushed Set is committed.
    std::string body;
    const int read_rc = oneShotSession(port, Opcode::Read, {}, &body);
    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
    EXPECT_EQ(read_rc, 0);
    EXPECT_TRUE(body.empty() || body == "disconnect-persist");
}

TEST(NetAppScenarios, MalformedFrameDoesNotKillServer) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-badmagic");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "still-here")).run(), 0);
    EXPECT_EQ(writeRawAndClose(port, {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xFF}), 0);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "still-here");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

TEST(NetAppScenarios, OversizedDeclaredLengthDoesNotKillServer) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-oversize-len");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "keep")).run(), 0);
    auto header = rawFrame(kFrameMagic, kMaxPayloadLen + 1, {});
    ASSERT_EQ(header.size(), kFrameHeaderSize);
    EXPECT_EQ(writeRawAndClose(port, std::move(header)), 0);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "keep");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

TEST(NetAppScenarios, OversizedSetDoesNotReplaceMessage) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-oversize-set");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "keep")).run(), 0);

    const std::string huge(kMaxMessageBytes + 1, 'Y');
    const std::string json = std::string(R"({"Type":"Set","Seq":0,"Data":")") + huge + R"("})";
    ASSERT_LE(json.size(), kMaxPayloadLen);
    EXPECT_EQ(writeRawAndClose(port, rawFrame(kFrameMagic, static_cast<std::uint32_t>(json.size()), json)), 0);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "keep");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

TEST(NetAppScenarios, UnknownOpcodeDoesNotKillServer) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-unknown-op");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "keep")).run(), 0);
    const std::string json = R"({"Type":"NotARealOp","Seq":0,"Data":"x"})";
    EXPECT_EQ(writeRawAndClose(port, rawFrame(kFrameMagic, static_cast<std::uint32_t>(json.size()), json)), 0);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "keep");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

TEST(NetAppScenarios, RequestStopFromSignalStopsListen) {
    const std::uint16_t port = uniquePort();
    const auto db = tempDbPath("app-signal-stop");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    ASSERT_TRUE(waitForServer(port));

    EXPECT_EQ(ClientApp(makeSetClient(port, "signal-persist")).run(), 0);
    // SIGINT and SIGTERM share this path (see NetServer main).
    app.transport().requestStopFromSignal();
    server_thread.join();

    ServerApp restarted(makeServerSettings(port, db));
    std::thread restarted_thread([&] { EXPECT_EQ(restarted.run(), 0); });
    ASSERT_TRUE(waitForServer(port));
    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "signal-persist");
    restarted.transport().post([&] { restarted.transport().stop(); });
    restarted_thread.join();
}

TEST(NetAppScenarios, StoreOpenFailureAfterBindExits) {
    const std::uint16_t port = uniquePort();
    const auto dir = std::filesystem::temp_directory_path() / ("netint-db-dir-" + std::to_string(port));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    ASSERT_TRUE(std::filesystem::create_directory(dir));

    ServerApp app(makeServerSettings(port, dir));
    EXPECT_EQ(app.run(), 1);

    std::filesystem::remove_all(dir, error);
}
