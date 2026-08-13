#pragma once

#include "AbstractNetSession.hpp"
#include "ProtocolLayers.hpp"

class StoreWorker;
class NetTransport;

// Thin accept-session: store worker + transport side effects; wire parsing is protocol::*.

class ServerSession : public AbstractNetSession {
public:
    ServerSession(StoreWorker& store, NetTransport& transport, bool allow_remote_shutdown);

    // Networking (session lifecycle)
    void onConnected() override;

protected:
    // Protocol (inbound wire)
    void parseNetProtocol(NetProtocol data) override;
    void onProtocolError(std::string_view reason) override;

private:
    // App side effects
    StoreWorker& store_;       // Read / Set (async queue)
    NetTransport& transport_;  // Shutdown → broadcast + stop
    bool allow_remote_shutdown_ = false;

    // Protocol
    ProtocolStack protocol_stack_;
};
