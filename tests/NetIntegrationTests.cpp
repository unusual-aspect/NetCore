#include <gtest/gtest.h>

#include "AppConfig.hpp"
#include "ClientApp.hpp"
#include "ClientSession.hpp"
#include "MessageStore.hpp"
#include "NetTransport.hpp"
#include "PeerUtils.hpp"
#include "ProtocolLayers.hpp"
#include "ServerApp.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
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

std::uint16_t testPort() {
    // Fixed high port for local integration; avoid clashing with default 9555.
    return 19555;
}

} // namespace

TEST(NetIntegration, ReadSetShutdownLoopback) {
    const auto db = tempDbPath("rw");
    ServerSettings settings;
    settings.bind_host = "127.0.0.1";
    settings.port = testPort();
    settings.db_path = db.string();
    settings.access_log_max_rows = 1000;
    settings.allow_remote_shutdown = false;

    ServerApp app(settings);
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });

    // Give the listen socket a moment to come up.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    {
        ClientSession session;
        session.setCommand(Opcode::Set, "integration-hello");
        NetTransport transport;
        transport.attach(session);
        ASSERT_TRUE(transport.connect("127.0.0.1", testPort()));
        transport.run();
        EXPECT_EQ(session.exitCode(), 0);
    }

    {
        ClientSession session;
        session.setCommand(Opcode::Read, {});
        NetTransport transport;
        transport.attach(session);
        ASSERT_TRUE(transport.connect("127.0.0.1", testPort()));
        transport.run();
        EXPECT_EQ(session.exitCode(), 0);
    }

    {
        ClientSession session;
        session.setCommand(Opcode::Shutdown, {});
        NetTransport transport;
        transport.attach(session);
        ASSERT_TRUE(transport.connect("127.0.0.1", testPort()));
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
    settings.port = static_cast<std::uint16_t>(testPort() + 1);
    settings.db_path = db.string();

    ServerApp app(settings);
    app.transport().setMaxConnections(1);

    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    ClientSession live;
    live.setKeepAlive(true);
    NetTransport first;
    first.attach(live);
    ASSERT_TRUE(first.connect("127.0.0.1", settings.port));

    std::thread first_thread([&] { first.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

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
    EXPECT_GE(app.transport().metrics().accepts, 1u);
}

// --- App scenarios (ClientApp + ServerApp) ---------------------------------

namespace {

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

} // namespace

// Second NetServer on the same bind/port fails listen and never opens SQLite.
TEST(NetIntegration, DuplicateBindDoesNotOpenSecondDb) {
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 2);
    const auto first_db = tempDbPath("dup-first");
    const auto second_db = tempDbPath("dup-second");

    ServerApp first(makeServerSettings(port, first_db));
    std::thread first_thread([&] { EXPECT_EQ(first.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

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
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 10);
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
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 11);
    const auto db = tempDbPath("app-set-keep");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

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
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 12);
    const auto db = tempDbPath("app-kill");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

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
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 13);
    const auto db = tempDbPath("app-read-empty");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_TRUE(body.empty());

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// 5) Set then Read round-trip via ClientApp / session.
TEST(NetAppScenarios, SetThenReadRoundTrip) {
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 14);
    const auto db = tempDbPath("app-roundtrip");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    EXPECT_EQ(ClientApp(makeSetClient(port, "round-trip-body")).run(), 0);

    std::string body;
    EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
    EXPECT_EQ(body, "round-trip-body");

    app.transport().post([&] { app.transport().stop(); });
    server_thread.join();
}

// 6) Second Set overwrites; Read sees the latest.
TEST(NetAppScenarios, SecondSetOverwrites) {
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 15);
    const auto db = tempDbPath("app-overwrite");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

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
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 16);
    ClientApp client(makeShutdownClient(port));
    client.setRecoverWait(std::chrono::seconds(5));

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(client.run(), 1);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

// 8) Live client receives graceful Shutdown goodbye (exit 0); server stops.
TEST(NetAppScenarios, GracefulShutdownNotifiesLiveClient) {
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 17);
    const auto db = tempDbPath("app-goodbye");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    ClientSession live;
    live.setKeepAlive(true);
    NetTransport live_transport;
    live_transport.attach(live);
    ASSERT_TRUE(live_transport.connect("127.0.0.1", port));
    std::thread live_thread([&] { live_transport.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(ClientApp(makeShutdownClient(port)).run(), 0);

    live_thread.join();
    server_thread.join();
    EXPECT_EQ(live.exitCode(), 0);
    EXPECT_GE(app.transport().metrics().shutdowns, 1u);
}

// 9) Server comes back during ClientApp recover wait → Set succeeds on retry.
TEST(NetAppScenarios, ClientRecoversWhenServerStartsDuringWait) {
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 18);
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
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 19);
    const auto db = tempDbPath("app-two");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

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
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 20);
    const auto db = tempDbPath("app-persist");

    {
        ServerApp app(makeServerSettings(port, db));
        std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        EXPECT_EQ(ClientApp(makeSetClient(port, "persisted")).run(), 0);
        app.transport().post([&] { app.transport().stop(); });
        server_thread.join();
    }

    {
        ServerApp app(makeServerSettings(port, db));
        std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        std::string body;
        EXPECT_EQ(oneShotSession(port, Opcode::Read, {}, &body), 0);
        EXPECT_EQ(body, "persisted");
        app.transport().post([&] { app.transport().stop(); });
        server_thread.join();
    }
}

// 12) Live client can Set twice on one TCP session.
TEST(NetAppScenarios, LiveClientTwoSetsOneConnection) {
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 21);
    const auto db = tempDbPath("app-live-two");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

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
    const std::uint16_t port = static_cast<std::uint16_t>(testPort() + 22);
    const auto db = tempDbPath("app-concurrent");
    ServerApp app(makeServerSettings(port, db));
    std::thread server_thread([&] { EXPECT_EQ(app.run(), 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

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
