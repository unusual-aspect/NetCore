#include "UsRuntime.hpp"
#include "Dbg.hpp"
#include "NetDefaults.hpp"

#ifndef LIBUS_USE_LIBUV
#define LIBUS_USE_LIBUV
#endif
#include <libusockets.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <new>

namespace {

void wakeup_cb(us_loop_t* loop) {
    auto* runtime = *static_cast<UsRuntime**>(us_loop_ext(loop));
    if (runtime) {
        runtime->drainPosted();
    }
}
void pre_cb(us_loop_t*) {}
void post_cb(us_loop_t*) {}

// Context ext is a UsRuntime* we stuffed in at construct time.
UsRuntime* runtime_from_socket(us_socket_t* socket) {
    auto* socket_context = us_socket_context(0, socket);
    return *static_cast<UsRuntime**>(us_socket_context_ext(0, socket_context));
}

// uSockets hands raw address bytes, not a string. 4 = IPv4, 16 = IPv6
// (and ::ffff:a.b.c.d is IPv4 mapped — peel the last 4 bytes).
std::string formatPeerIp(const void* ip, int byte_count) {
    if (!ip || byte_count <= 0) {
        return {};
    }
    const auto* bytes = static_cast<const unsigned char*>(ip);
    if (byte_count == 4) {
        return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
               std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
    }
    if (byte_count == 16) {
        bool ipv4_mapped = true;
        for (int index = 0; index < 10; ++index) {
            if (bytes[index] != 0) {
                ipv4_mapped = false;
                break;
            }
        }
        if (ipv4_mapped && bytes[10] == 0xff && bytes[11] == 0xff) {
            return formatPeerIp(bytes + 12, 4);
        }
        char text[INET6_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET6, ip, text, sizeof(text))) {
            return text;
        }
    }
    return {};
}

std::string peerIp(us_socket_t* socket, char* ip, int ip_length) {
    // on_open usually already has the peer bytes.
    std::string peer = formatPeerIp(ip, ip_length);
    if (!peer.empty()) {
        return peer;
    }
    // Fallback: ask the socket again.
    char address_bytes[16]{};
    int address_length = static_cast<int>(sizeof(address_bytes));
    us_socket_remote_address(0, socket, address_bytes, &address_length);
    peer = formatPeerIp(address_bytes, address_length);
    return peer.empty() ? "unknown" : peer;
}

us_socket_t* on_open(us_socket_t* socket, int /*is_client*/, char* ip, int ip_length) {
    // Placement-new the per-socket write buffer (unsent_bytes_).
    auto* connection = static_cast<UsConnExt*>(us_socket_ext(0, socket));
    new (connection) UsConnExt{};

    auto* runtime = runtime_from_socket(socket);
    if (runtime) {
        runtime->markExtLive(socket);
        runtime->notify_open(socket, peerIp(socket, ip, ip_length));
    }
    return socket;
}

us_socket_t* on_data(us_socket_t* socket, char* data, int length) {
    auto* runtime = runtime_from_socket(socket);
    if (runtime && data && length > 0) {
        runtime->notify_data(socket, std::span<const std::uint8_t>(
                                         reinterpret_cast<const std::uint8_t*>(data),
                                         static_cast<std::size_t>(length)));
    }
    return socket;
}

us_socket_t* on_writable(us_socket_t* socket) {
    // Kernel buffer has room — flush what write() could not send last time.
    auto* connection = static_cast<UsConnExt*>(us_socket_ext(0, socket));
    if (!connection || connection->unsent_bytes_.empty()) {
        return socket;
    }
    const int written = us_socket_write(
        0, socket, reinterpret_cast<const char*>(connection->unsent_bytes_.data()),
        static_cast<int>(connection->unsent_bytes_.size()), 0);
    if (written > 0) {
        connection->unsent_bytes_.erase(
            connection->unsent_bytes_.begin(),
            connection->unsent_bytes_.begin() + written);
    }
    return socket;
}

us_socket_t* on_close(us_socket_t* socket, int /*code*/, void* /*reason*/) {
    auto* runtime = runtime_from_socket(socket);
    if (runtime) {
        runtime->notify_close(socket);
    }
    // Manual dtor only if on_open ran. Failed connect closes without on_open.
    auto* connection = static_cast<UsConnExt*>(us_socket_ext(0, socket));
    if (connection && runtime && runtime->takeExtLive(socket)) {
        connection->~UsConnExt();
    }
    return socket;
}

us_socket_t* on_end(us_socket_t* socket) {
    // Peer half-closed — we close fully so on_close runs.
    return us_socket_close(0, socket, 0, nullptr);
}

us_socket_t* on_connect_error(us_socket_t* socket, int /*code*/) {
    DBG("Cannot connect — the outbound TCP handshake failed.");
    auto* runtime = runtime_from_socket(socket);
    if (runtime) {
        runtime->notify_error("connect failed");
    }
    return socket;
}

us_socket_t* on_timeout(us_socket_t* socket) {
    // Idle too long — drop (server accept flood / abandoned clients).
    DBG("Cannot keep idle connection — socket timeout elapsed. Closing.");
    return us_socket_close(0, socket, 0, nullptr);
}

} // namespace

UsRuntime::UsRuntime() {
    // Loop ext = UsRuntime* so wakeup_cb can drain post() jobs.
    event_loop_ = us_create_loop(nullptr, wakeup_cb, pre_cb, post_cb, static_cast<int>(sizeof(UsRuntime*)));
    *static_cast<UsRuntime**>(us_loop_ext(event_loop_)) = this;
    us_socket_context_options_t options{};
    // Context ext = sizeof(UsRuntime*) so every socket can find `this`.
    socket_context_ = us_create_socket_context(0, event_loop_, static_cast<int>(sizeof(UsRuntime*)), options);
    *static_cast<UsRuntime**>(us_socket_context_ext(0, socket_context_)) = this;

    us_socket_context_on_open(0, socket_context_, on_open);
    us_socket_context_on_data(0, socket_context_, on_data);
    us_socket_context_on_writable(0, socket_context_, on_writable);
    us_socket_context_on_close(0, socket_context_, on_close);
    us_socket_context_on_end(0, socket_context_, on_end);
    us_socket_context_on_connect_error(0, socket_context_, on_connect_error);
    us_socket_context_on_timeout(0, socket_context_, on_timeout);
}

UsRuntime::~UsRuntime() {
    stop();
    if (socket_context_) {
        us_socket_context_free(0, socket_context_);
        socket_context_ = nullptr;
    }
    if (event_loop_) {
        us_loop_free(event_loop_);
        event_loop_ = nullptr;
    }
}

bool UsRuntime::listen(const std::string& host, std::uint16_t port) {
    // Last arg = per-socket ext size so each accept gets a UsConnExt.
    // Empty host → all interfaces; default callers pass NET_DEFAULT_BIND (127.0.0.1).
    // LIBUS_LISTEN_DEFAULT enables SO_REUSEPORT on Linux — two NetServers would both
    // bind 9555 and share one SQLite path. Exclusive is the single-instance gate.
    const char* bind_host = host.empty() ? nullptr : host.c_str();
    listen_socket_ = us_socket_context_listen(
        0, socket_context_, bind_host, static_cast<int>(port), LIBUS_LISTEN_EXCLUSIVE_PORT,
        static_cast<int>(sizeof(UsConnExt)));
    return listen_socket_ != nullptr;
}

bool UsRuntime::connect(const std::string& host, std::uint16_t port) {
    // Same UsConnExt size as listen — write() expects it on every socket.
    auto* socket = us_socket_context_connect(
        0, socket_context_, host.c_str(), static_cast<int>(port), nullptr, 0,
        static_cast<int>(sizeof(UsConnExt)));
    return socket != nullptr;
}

void UsRuntime::run() {
    // Blocks until stop() wakes the loop (listen/client polls keep num_polls > 0).
    loop_running_ = true;
    us_loop_run(event_loop_);
    loop_running_ = false;
}

void UsRuntime::requestStopFromSignal() {
    // Async-signal-safe path: no mutex, no heap, no std::function.
    signal_stop_.store(true, std::memory_order_release);
    if (event_loop_) {
        us_wakeup_loop(event_loop_);
    }
}

void UsRuntime::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> guard(posted_mutex_);
        posted_jobs_.push_back(std::move(job));
    }
    if (event_loop_) {
        us_wakeup_loop(event_loop_);
    }
}

void UsRuntime::drainPosted() {
    // Honor signal-requested stop on the loop thread (safe place to call stop()).
    if (signal_stop_.exchange(false, std::memory_order_acq_rel)) {
        stop();
    }

    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard<std::mutex> guard(posted_mutex_);
        jobs.swap(posted_jobs_);
    }
    for (auto& job : jobs) {
        job();
    }
}

void UsRuntime::stop() {
    // Close listener first so we stop accepting.
    if (listen_socket_) {
        us_listen_socket_close(0, listen_socket_);
        listen_socket_ = nullptr;
    }
    // Then every live socket on this context.
    if (socket_context_) {
        us_socket_context_close(0, socket_context_);
    }
    if (!event_loop_) {
        return;
    }
    // Wake the loop so us_loop_run() can exit.
    us_wakeup_loop(event_loop_);
    // uSockets queues listen/socket polls on closed_head and only us_poll_free()s
    // them in the next loop post(). Bind-then-fail-store never enters run(), so
    // drain here or LeakSanitizer reports the listen poll (80 bytes).
    if (!loop_running_) {
        us_loop_run(event_loop_);
    }
}

bool UsRuntime::write(us_socket_t* socket, std::span<const std::uint8_t> data) {
    if (!socket || data.empty()) {
        return false;
    }
    auto* connection = static_cast<UsConnExt*>(us_socket_ext(0, socket));
    if (!connection) {
        return false;
    }
    // Already back-pressured — just queue, on_writable will flush.
    if (!connection->unsent_bytes_.empty()) {
        if (connection->unsent_bytes_.size() + data.size() > kMaxUnsentBytes) {
            DBG("Cannot queue outbound bytes — write buffer would exceed " +
                std::to_string(kMaxUnsentBytes) + " bytes. Dropping the socket.");
            us_socket_close(0, socket, 0, nullptr);
            return false;
        }
        connection->unsent_bytes_.insert(connection->unsent_bytes_.end(), data.begin(), data.end());
        return true;
    }
    const int written = us_socket_write(0, socket, reinterpret_cast<const char*>(data.data()),
                                        static_cast<int>(data.size()), 0);
    if (written < 0) {
        return false;
    }
    // Partial write — keep the tail for on_writable.
    if (static_cast<std::size_t>(written) < data.size()) {
        const auto remaining = data.size() - static_cast<std::size_t>(written);
        if (remaining > kMaxUnsentBytes) {
            DBG("Cannot queue outbound bytes — partial write leftover exceeds the unsent cap. Dropping the socket.");
            us_socket_close(0, socket, 0, nullptr);
            return false;
        }
        connection->unsent_bytes_.insert(connection->unsent_bytes_.end(),
                                         data.begin() + written, data.end());
    }
    return true;
}

void UsRuntime::close(us_socket_t* socket) {
    if (socket) {
        us_socket_close(0, socket, 0, nullptr);
    }
}

void UsRuntime::notify_open(us_socket_t* socket, std::string_view peer) {
    if (on_socket_open_) {
        on_socket_open_(socket, peer);
    }
}

void UsRuntime::notify_data(us_socket_t* socket, std::span<const std::uint8_t> data) {
    if (on_socket_data_) {
        on_socket_data_(socket, data);
    }
}

void UsRuntime::armIdleTimeout(us_socket_t* socket, unsigned seconds) {
    if (socket) {
        us_socket_timeout(0, socket, seconds);
    }
}

void UsRuntime::notify_close(us_socket_t* socket) {
    if (on_socket_close_) {
        on_socket_close_(socket);
    }
}

void UsRuntime::notify_error(std::string_view what) {
    if (error_handler_) {
        error_handler_(what);
    }
}

void UsRuntime::markExtLive(us_socket_t* socket) {
    if (socket) {
        live_exts_.insert(socket);
    }
}

bool UsRuntime::takeExtLive(us_socket_t* socket) {
    return socket && live_exts_.erase(socket) > 0;
}
