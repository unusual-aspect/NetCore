#pragma once

#include "NetProtocol.hpp"
#include "ProtocolStack.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Versioned wire handlers live here (Vx, V1, later V2/V80). Sessions only
// supply side effects (store, transport, exit codes) via hooks.

namespace protocol {

struct ServerHooks {
    std::function<std::string()> peer;
    std::function<bool(const NetProtocol&)> send;
    // Async store path: call done(ok, payload) when finished.
    // ok → Opcode::Ok (payload = body, may be empty).
    // !ok → Opcode::Error with fixed kStoreUnavailableMsg on the wire; payload is DBG-only detail.
    // done may run later (store worker); callers must hop to the loop thread before send.
    using StoreDone = std::function<void(bool ok, std::string payload)>;
    std::function<void(StoreDone done)> onRead;
    std::function<void(std::string_view data, StoreDone done)> onSet;
    // Return false to refuse Shutdown (e.g. non-loopback without allow flag).
    // Session: broadcast goodbye, Ok to requester, stop listen loop when true.
    std::function<bool(const NetProtocol& request)> onShutdown;
};

struct ClientHooks {
    std::function<std::string()> peer;
    // After VersionSrv — client may send its pending Read/Set/Shutdown.
    std::function<void()> onVersionReady;
    std::function<void(std::string_view detail)> onShutdownGoodbye;
    std::function<void(std::string_view detail)> onError;
    std::function<void(const NetProtocol& ok)> onOk;
};

// Builds Vx (0) + V1 (1). Add V2/V80 layers inside these factories when needed.
ProtocolStack makeServerStack(ServerHooks hooks);
ProtocolStack makeClientStack(ClientHooks hooks);

NetProtocol versionHandshake();
// Read / Set / Shutdown outbound frames; nullopt if opcode is not a client request.
std::optional<NetProtocol> makeClientRequest(Opcode command, std::string_view payload = {});

void logRecv(const NetProtocol& data, std::string_view peer);
bool dispatchOrLog(const ProtocolStack& stack, const NetProtocol& data, std::string_view peer);

} // namespace protocol
