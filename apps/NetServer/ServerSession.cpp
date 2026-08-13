#include "ServerSession.hpp"

#include "Dbg.hpp"
#include "NetTransport.hpp"
#include "PeerUtils.hpp"
#include "StoreWorker.hpp"

ServerSession::ServerSession(StoreWorker& store, NetTransport& transport, bool allow_remote_shutdown)
    : store_(store), transport_(transport), allow_remote_shutdown_(allow_remote_shutdown) {
    protocol_stack_ = protocol::makeServerStack({
        .peer   = [this] { return peer(); },
        .send   = [this](const NetProtocol& msg) { return sendData(msg); },
        .onRead =
            [this](protocol::ServerHooks::StoreDone done) {
                const std::string peer_ip = peer();
                us_socket_t* sock = nativeSocket();
                // Capture transport* + session id, not this: the session can be
                // destroyed if the client disconnects, and socket pointers are reused.
                NetTransport* transport = &transport_;
                const std::uint64_t sid = sessionId();
                store_.get(peer_ip, [transport, sock, sid, done = std::move(done)](StoreWorker::GetResult result) mutable {
                    transport->post([transport, sock, sid, done = std::move(done), result = std::move(result)]() mutable {
                        if (!transport->hasSession(sock, sid)) {
                            return;
                        }
                        if (result.ok) {
                            ++transport->metrics().reads;
                            done(true, result.value.value_or(std::string{}));
                        } else {
                            ++transport->metrics().store_errors;
                            done(false, std::move(result.error));
                        }
                    });
                });
            },
        .onSet =
            [this](std::string_view data, protocol::ServerHooks::StoreDone done) {
                const std::string peer_ip = peer();
                const std::string body(data);
                us_socket_t* sock = nativeSocket();
                NetTransport* transport = &transport_;
                const std::uint64_t sid = sessionId();
                store_.put(body, peer_ip, [transport, sock, sid, done = std::move(done)](StoreWorker::PutResult result) mutable {
                    transport->post([transport, sock, sid, done = std::move(done), result = std::move(result)]() mutable {
                        if (!transport->hasSession(sock, sid)) {
                            return;
                        }
                        if (result.ok) {
                            ++transport->metrics().sets;
                            done(true, {});
                        } else {
                            ++transport->metrics().store_errors;
                            done(false, std::move(result.error));
                        }
                    });
                });
            },
        .onShutdown =
            [this](const NetProtocol& request) {
                if (!allow_remote_shutdown_ && !isLoopbackPeer(peer())) {
                    return false;
                }
                ++transport_.metrics().shutdowns;
                // Client asked to stop: server tells every other live session goodbye first.
                transport_.broadcast(NetProtocol(Opcode::Shutdown, "server is shutting down"), this);
                sendData(NetProtocol(Opcode::Ok, {}, request.seq()));
                // Do not stop() here: that closes this socket and destroys *this
                // while parseNetProtocol / on_data is still on the stack.
                NetTransport* transport = &transport_;
                transport->post([transport] { transport->stop(); });
                return true;
            },
    });
}

void ServerSession::onConnected() {
    sendData(protocol::versionHandshake());
    DBG(netdbg::event("ACCEPT", peer()));
}

void ServerSession::parseNetProtocol(NetProtocol data) {
    protocol::dispatchOrLog(protocol_stack_, data, peer());
}

void ServerSession::onProtocolError(std::string_view reason) {
    DBG(netdbg::event("Cannot parse this connection", peer(), reason.empty() ? "Frame is not a valid 0xBEEF message" : reason));
}
