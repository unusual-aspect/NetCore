#include "NetTransport.hpp"
#include "Dbg.hpp"
#include "UsRuntime.hpp"

#include <span>

NetTransport::NetTransport() : runtime_(std::make_unique<UsRuntime>()) {
    hookRuntimeCallbacks();
}

NetTransport::~NetTransport() = default;

void NetTransport::hookRuntimeCallbacks() {
    // Socket just opened (accept or connect finished).
    runtime_->set_on_open([this](us_socket_t* socket, std::string_view peer) {
        // Server: factory builds a new session per accept.
        if (session_factory_) {
            if (sessions_by_socket_.size() >= max_connections_) {
                ++metrics_.rejected_conns;
                DBG("Cannot accept " + std::string(peer) + " — connection limit (" +
                    std::to_string(max_connections_) + ") reached. Closing the socket.");
                UsRuntime::close(socket);
                return;
            }
            auto session = session_factory_(peer);
            if (!session) {
                DBG("Cannot accept " + std::string(peer) + " — session factory returned nothing, so there is no protocol handler. Closing the socket.");
                UsRuntime::close(socket);
                return;
            }
            bindSession(*session, socket, peer);
            sessions_by_socket_[socket] = std::move(session);
            ++metrics_.accepts;
            sessions_by_socket_[socket]->onConnected();
            return;
        }

        // Client: one attached session, reuse it.
        if (client_session_) {
            DBG("Connected to " + std::string(peer) + " — binding the client session.");
            bindSession(*client_session_, socket, peer);
            client_session_->onConnected();
        }
    });

    // Bytes arrived — find the session and push them into the framer.
    runtime_->set_on_data([this](us_socket_t* socket, std::span<const std::uint8_t> data) {
        AbstractNetSession* session = client_session_;
        // Server lookup wins if this socket is in the map.
        if (auto found = sessions_by_socket_.find(socket); found != sessions_by_socket_.end()) {
            session = found->second.get();
        }
        if (!session) {
            return;
        }

        // Bound aggregate incomplete-frame buffers across all live sessions.
        std::size_t buffered = data.size();
        for (const auto& [_, live] : sessions_by_socket_) {
            if (live) {
                buffered += live->receiveBufferedBytes();
            }
        }
        if (client_session_) {
            buffered += client_session_->receiveBufferedBytes();
        }
        if (buffered > kMaxTotalReceiveBytes) {
            ++metrics_.rejected_conns;
            DBG("Cannot accept more bytes from " + session->peer() +
                " — total receive buffers would exceed " + std::to_string(kMaxTotalReceiveBytes) +
                " bytes. Closing the socket.");
            UsRuntime::close(socket);
            return;
        }

        // feed() false = fatal frame → drop the socket.
        if (!session->feed(data)) {
            UsRuntime::close(socket);
            return;
        }
        // Incomplete frame: arm idle timeout. Complete parse: clear so --live can wait.
        if (kIdleTimeoutSec > 0) {
            const unsigned seconds =
                session->receiveBufferedBytes() > 0 ? kIdleTimeoutSec : 0u;
            UsRuntime::armIdleTimeout(socket, seconds);
        }
    });

    // Socket closed by peer or by us.
    runtime_->set_on_close([this](us_socket_t* socket) {
        // Find in map (server accept).
        if (auto found = sessions_by_socket_.find(socket); found != sessions_by_socket_.end()) {
            DBG(netdbg::event("DISCONNECT", found->second->peer()));
        }
        // Outbound client that finished handshake (bound in on_open).
        else if (client_session_ && client_session_->socket_ == socket) {
            DBG(netdbg::event("DISCONNECT", client_session_->peer()));
            client_session_->onDisconnected();
        }
        // Connect failed before on_open, or ghost socket — error path already logged.
        else {
            DBG(netdbg::event("DISCONNECT", "unknown"));
        }
        // Dump from map
        sessions_by_socket_.erase(socket);
        // Reset so sendData() does not write a dead socket.
        if (client_session_ && client_session_->socket_ == socket) {
            client_session_->socket_ = nullptr;
            // One client socket — leave us_loop_run so the app can retry or exit.
            runtime_->stop();
        }
    });

    // Connect failed / listen died — DBG always, then optional app hook.
    runtime_->set_on_error([this](std::string_view what) {
        DBG(what.empty() ? "Cannot continue on this transport — uSockets reported a failure with no detail." : what);
        if (error_handler_) {
            error_handler_(what);
        }
        // Handshake never finished — do not sit in run() forever.
        if (client_session_) {
            runtime_->stop();
        }
    });
}

void NetTransport::bindSession(AbstractNetSession& session, us_socket_t* socket, std::string_view peer) {
    // Glue this TCP socket onto the session:
    //   socket_  → sendData() can write
    //   peer_ip_ → DBG / store audit
    //   close()  → UsRuntime::close(this socket) → on_close
    session.bind(socket, peer, [socket] { UsRuntime::close(socket); });
}

bool NetTransport::listen(const std::string& host, std::uint16_t port) {
    // Needs setFactory() first — on_open uses it.
    if (!runtime_->listen(host, port)) {
        DBG("Cannot listen on " + host + ":" + std::to_string(port) +
            " — bind failed (address/port in use, or no permission).");
        return false;
    }
    DBG("Start listen on " + host + ":" + std::to_string(port));
    return true;
}

bool NetTransport::connect(const std::string& host, std::uint16_t port) {
    // Needs attach() first — on_open binds client_session_.
    DBG("Connect " + host + ":" + std::to_string(port));
    if (!runtime_->connect(host, port)) {
        DBG("Cannot reach " + host + ":" + std::to_string(port) + " — outbound connect did not start.");
        return false;
    }
    return true;
}

void NetTransport::run() {
    runtime_->run();
}

void NetTransport::stop() {
    runtime_->stop();
}

void NetTransport::post(std::function<void()> job) {
    runtime_->post(std::move(job));
}

void NetTransport::requestStopFromSignal() {
    runtime_->requestStopFromSignal();
}

void NetTransport::broadcast(const NetProtocol& data, AbstractNetSession* except) {
    for (auto& [socket, session] : sessions_by_socket_) {
        if (!session || session.get() == except) {
            continue;
        }
        session->sendData(data);
    }
}
