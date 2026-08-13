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
                store_.get(peer_ip, [this, sock, done = std::move(done)](StoreWorker::GetResult result) mutable {
                    transport_.post([this, sock, done = std::move(done), result = std::move(result)]() mutable {
                        if (!transport_.hasSession(sock)) {
                            return;
                        }
                        if (result.ok) {
                            ++transport_.metrics().reads;
                            done(true, result.value.value_or(std::string{}));
                        } else {
                            ++transport_.metrics().store_errors;
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
                store_.put(body, peer_ip, [this, sock, done = std::move(done)](StoreWorker::PutResult result) mutable {
                    transport_.post([this, sock, done = std::move(done), result = std::move(result)]() mutable {
                        if (!transport_.hasSession(sock)) {
                            return;
                        }
                        if (result.ok) {
                            ++transport_.metrics().sets;
                            done(true, {});
                        } else {
                            ++transport_.metrics().store_errors;
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
                transport_.stop();
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
