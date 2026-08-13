#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct us_loop_t;
struct us_socket_t;
struct us_socket_context_t;
struct us_listen_socket_t;

// Thin wrap of one uSockets loop + one TCP context.
//
// listen()/connect() register the C callbacks (on_open/data/writable/close/end).
// Those call notify_* which forward to NetTransport.
// write(): us_socket_write; leftover bytes go on UsConnExt::unsent_bytes_
// and flush from on_writable. Socket ext is UsConnExt; context ext is UsRuntime*.

class UsRuntime {
public:
    using OpenFn    = std::function<void(us_socket_t* socket, std::string_view peer)>;
    using DataFn    = std::function<void(us_socket_t* socket, std::span<const std::uint8_t> data)>;
    using CloseFn   = std::function<void(us_socket_t* socket)>;
    using ErrorFn   = std::function<void(std::string_view what)>;

    UsRuntime();
    ~UsRuntime();

    UsRuntime(const UsRuntime&) = delete;
    UsRuntime& operator=(const UsRuntime&) = delete;

    void set_on_open(OpenFn callback)   { on_socket_open_ = std::move(callback); }
    void set_on_data(DataFn callback)   { on_socket_data_ = std::move(callback); }
    void set_on_close(CloseFn callback) { on_socket_close_ = std::move(callback); }
    void set_on_error(ErrorFn callback) { error_handler_ = std::move(callback); }

    bool listen(const std::string& host, std::uint16_t port);
    bool connect(const std::string& host, std::uint16_t port);
    void run();
    void stop();

    // Async-signal-safe stop request: set a flag + wakeup the loop (no mutex).
    // drainPosted() on the loop thread turns that into stop(). Use from SIGINT/SIGTERM.
    void requestStopFromSignal();

    // Run `job` on the loop thread (stdin → send Set for --live). Not signal-safe.
    void post(std::function<void()> job);
    void drainPosted();

    static bool write(us_socket_t* socket, std::span<const std::uint8_t> data);
    static void close(us_socket_t* socket);
    // seconds=0 clears. Used for incomplete-frame idle (slowloris), not quiet --live.
    static void armIdleTimeout(us_socket_t* socket, unsigned seconds);

    void notify_open(us_socket_t* socket, std::string_view peer);
    void notify_data(us_socket_t* socket, std::span<const std::uint8_t> data);
    void notify_close(us_socket_t* socket);
    void notify_error(std::string_view what);

    // UsConnExt is placement-new'd in on_open only. Failed connect skips on_open but
    // still hits on_close — track which sockets own a live ext so we do not ~ garbage.
    void markExtLive(us_socket_t* socket);
    bool takeExtLive(us_socket_t* socket);

private:
    us_loop_t* event_loop_ = nullptr;
    us_socket_context_t* socket_context_ = nullptr;
    us_listen_socket_t* listen_socket_ = nullptr;  // null on client / after stop()

    OpenFn on_socket_open_;
    DataFn on_socket_data_;
    CloseFn on_socket_close_;
    ErrorFn error_handler_;

    std::unordered_set<us_socket_t*> live_exts_;

    std::mutex posted_mutex_;
    std::vector<std::function<void()>> posted_jobs_;

    // Signal path: handler only stores true + us_wakeup_loop (no lock / no alloc).
    std::atomic<bool> signal_stop_{false};

    // True while us_loop_run is on the stack. stop() must not re-enter the loop.
    bool loop_running_ = false;
};

// Lives in us_socket_ext. write() leftover + on_writable drain.
struct UsConnExt {
    std::vector<std::uint8_t> unsent_bytes_;
};
