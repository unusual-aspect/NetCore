#pragma once

#include "AbstractNetSession.hpp"

#include "NetDefaults.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct us_socket_t;
class UsRuntime;

// Owns the uSockets loop and maps sockets onto AbstractNetSession.
//
// Shared I/O path (both roles):
//   on_data  → lookup session → feed(); feed() false → close the socket
//   on_close → erase map / null client socket_
//
// Server path:
//   setFactory() → listen() → run()
//   on_open: factory(peer) → store in sessions_by_socket_ → bind() → onConnected()
//
// Client path:
//   attach() → connect() → run()
//   on_open: bind the attached session (no factory, no map entry)
//   socket gone or connect error → stop() so run() returns (no hang)

class NetTransport {
public:
    using SessionFactory = std::function<std::unique_ptr<AbstractNetSession>(std::string_view peer)>;
    using ErrorFn = std::function<void(std::string_view what)>;

    NetTransport();
    ~NetTransport();

    NetTransport(const NetTransport&) = delete;
    NetTransport& operator=(const NetTransport&) = delete;

    // Shared (server + client)

    // Optional app hook for transport/runtime failures (DBG always runs first).
    void setOnError(ErrorFn callback) { error_handler_ = std::move(callback); }

    // Drive / leave the uSockets loop. post() queues work onto that loop.
    void run();
    void stop();
    void post(std::function<void()> job);
    // Async-signal-safe: flag + loop wakeup; no mutex. For SIGINT/SIGTERM handlers.
    void requestStopFromSignal();

    // Server
    // Call setFactory() before listen(). Each accept gets a new session from the factory.

    void setFactory(SessionFactory make_session) { session_factory_ = std::move(make_session); }
    bool listen(const std::string& host, std::uint16_t port);

    // Caps (server). 0 = use NetDefaults.
    void setMaxConnections(std::size_t max) { max_connections_ = max; }
    std::size_t sessionCount() const { return sessions_by_socket_.size(); }

    // True while this accept socket still has a live session (store replies check this).
    bool hasSession(us_socket_t* socket) const {
        return socket && sessions_by_socket_.find(socket) != sessions_by_socket_.end();
    }

    // Send to every accepted session except `except` (e.g. the Shutdown requester).
    void broadcast(const NetProtocol& data, AbstractNetSession* except = nullptr);

    // Metrics (server)
    struct Metrics {
        std::uint64_t accepts = 0;
        std::uint64_t rejected_conns = 0;
        std::uint64_t reads = 0;
        std::uint64_t sets = 0;
        std::uint64_t shutdowns = 0;
        std::uint64_t store_errors = 0;
    };
    Metrics& metrics() { return metrics_; }
    const Metrics& metrics() const { return metrics_; }

    // Client
    // Call attach() before connect(). One session is reused for the outbound socket.

    void attach(AbstractNetSession& session) { client_session_ = &session; }
    bool connect(const std::string& host, std::uint16_t port);

private:
    void hookRuntimeCallbacks();
    void bindSession(AbstractNetSession& session, us_socket_t* socket, std::string_view peer);

    // Shared
    std::unique_ptr<UsRuntime> runtime_;
    ErrorFn error_handler_;

    // Server: factory + one owned session per accepted socket
    SessionFactory session_factory_;
    std::unordered_map<us_socket_t*, std::unique_ptr<AbstractNetSession>> sessions_by_socket_;
    std::size_t max_connections_ = kMaxConnections;
    Metrics metrics_{};

    // Client: non-owning pointer to the single attached session
    AbstractNetSession* client_session_ = nullptr;
};
