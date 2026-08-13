#include "ProtocolLayers.hpp"

#include "Dbg.hpp"
#include "NetDefaults.hpp"

#include <stdexcept>
#include <string>

namespace protocol {
namespace {

void sendStoreUnavailable(const ServerHooks& hooks, std::string_view peer, std::uint32_t seq,
                          std::string_view detail) {
    // Keep SQLite / exception text in logs always (not MSG= redaction) — wire stays generic.
    DBG(std::string("STORE ERROR IP=") + (peer.empty() ? "unknown" : std::string(peer)) +
        " detail=" + (detail.empty() ? "(no detail)" : std::string(detail)));
    if (hooks.send) {
        hooks.send(NetProtocol(Opcode::Error, std::string(kStoreUnavailableMsg), seq));
    }
}

bool parseServerVx(const NetProtocol& data, const ServerHooks& hooks) {
    const auto peer = hooks.peer ? hooks.peer() : std::string{};
    switch (data.getMsgType()) {
        case Opcode::LastMsgType:
            return true;
        case Opcode::Error:
            DBG(netdbg::event("Peer reported an error", peer, data.getMsgData().empty() ? "(no detail)" : std::string(data.getMsgData())));
            return true;
        case Opcode::VersionSrv:
            DBG(netdbg::event("VERSION", peer, data.getMsgData()));
            return true;
        case Opcode::None:
            DBG(netdbg::event("Cannot map wire Type to an opcode", peer, "Unknown name '" + data.getInvalidType() + "' — keeping the connection"));
            return true;
        default: {
            // Future majors need their own layer; do not run V1 handlers against them.
            const int major = versionMajor(data.getVersion());
            if (major > kProtoMajor) {
                DBG(netdbg::event("Unsupported protocol major", peer,
                                  "Peer speaks " + data.getVersion() + ", we speak " + std::string(kNetworkVersion)));
                if (hooks.send) {
                    hooks.send(NetProtocol(Opcode::Error, "unsupported protocol major", data.seq()));
                }
                return true;
            }
            return false;
        }
    }
}

bool parseServerV1(const NetProtocol& data, const ServerHooks& hooks) {
    const auto peer = hooks.peer ? hooks.peer() : std::string{};
    switch (data.getMsgType()) {
        case Opcode::Read: {
            const auto seq = data.seq();
            auto reply = [hooks, peer, seq](bool ok, std::string payload) {
                if (ok) {
                    DBG(netdbg::event("READ", peer, payload.empty() ? "<empty>" : payload));
                    if (hooks.send) {
                        hooks.send(NetProtocol(Opcode::Ok, std::move(payload), seq));
                    }
                } else {
                    sendStoreUnavailable(hooks, peer, seq, payload);
                }
            };
            if (!hooks.onRead) {
                reply(true, {});
                return true;
            }
            try {
                hooks.onRead(std::move(reply));
            } catch (const std::exception& error) {
                sendStoreUnavailable(hooks, peer, seq, error.what());
            }
            return true;
        }
        case Opcode::Set: {
            if (exceedsMaxMessage(data.getMsgData().size())) {
                DBG(netdbg::event("SET refused", peer,
                                  "body " + std::to_string(data.getMsgData().size()) + " bytes over limit"));
                if (hooks.send) {
                    hooks.send(NetProtocol(Opcode::Error, "message too large", data.seq()));
                }
                return true;
            }
            if (data.getMsgData().empty()) {
                DBG(netdbg::event("SET ignored", peer, "<empty>"));
                if (hooks.send) {
                    hooks.send(NetProtocol(Opcode::Ok, {}, data.seq()));
                }
                return true;
            }
            const auto seq = data.seq();
            const auto body = std::string(data.getMsgData());
            auto reply = [hooks, peer, seq, body](bool ok, std::string payload) {
                if (ok) {
                    DBG(netdbg::event("SET", peer, body));
                    if (hooks.send) {
                        hooks.send(NetProtocol(Opcode::Ok, {}, seq));
                    }
                } else {
                    sendStoreUnavailable(hooks, peer, seq, payload);
                }
            };
            if (!hooks.onSet) {
                reply(true, {});
                return true;
            }
            try {
                hooks.onSet(body, std::move(reply));
            } catch (const std::exception& error) {
                sendStoreUnavailable(hooks, peer, seq, error.what());
            }
            return true;
        }
        case Opcode::Shutdown: {
            DBG(netdbg::event("SHUTDOWN", peer, "Graceful stop request"));
            if (hooks.onShutdown) {
                if (!hooks.onShutdown(data)) {
                    DBG(netdbg::event("SHUTDOWN refused", peer, "shutdown not allowed"));
                    if (hooks.send) {
                        hooks.send(NetProtocol(Opcode::Error, "shutdown not allowed", data.seq()));
                    }
                    return true;
                }
            }
            return true;
        }
        default:
            DBG(netdbg::event("Cannot handle this opcode in server V1", peer, data.getType()));
            return false;
    }
}

bool parseClientVx(const NetProtocol& data, const ClientHooks& hooks) {
    const auto peer = hooks.peer ? hooks.peer() : std::string{};
    switch (data.getMsgType()) {
        case Opcode::VersionSrv: {
            DBG(netdbg::event("VERSION", peer, data.getMsgData().empty() ? data.getVersion() : std::string(data.getMsgData())));
            if (hooks.onVersionReady) {
                hooks.onVersionReady();
            }
            return true;
        }
        case Opcode::None:
            DBG(netdbg::event("Cannot map wire Type to an opcode", peer, "Unknown name '" + data.getInvalidType() + "' (peer version " + data.getVersion() + ")"));
            return true;
        case Opcode::Shutdown: {
            DBG(netdbg::event("Server is shutting down", peer, data.getMsgData().empty() ? "Goodbye" : std::string(data.getMsgData())));
            if (hooks.onShutdownGoodbye) {
                hooks.onShutdownGoodbye(data.getMsgData());
            }
            return true;
        }
        case Opcode::LastMsgType:
        case Opcode::Error: {
            DBG(netdbg::event("Server reported an error", peer, data.getMsgData().empty() ? "(no detail)" : std::string(data.getMsgData())));
            if (hooks.onError) {
                hooks.onError(data.getMsgData());
            }
            return true;
        }
        default: {
            const int major = versionMajor(data.getVersion());
            if (major > kProtoMajor) {
                DBG(netdbg::event("Unsupported protocol major", peer,
                                  "Peer speaks " + data.getVersion() + ", we speak " + std::string(kNetworkVersion)));
                if (hooks.onError) {
                    hooks.onError("unsupported protocol major");
                }
                return true;
            }
            return false;
        }
    }
}

bool parseClientV1(const NetProtocol& data, const ClientHooks& hooks) {
    const auto peer = hooks.peer ? hooks.peer() : std::string{};
    switch (data.getMsgType()) {
        case Opcode::Ok: {
            DBG(netdbg::event("OK", peer, data.isEmpty() ? "Ok" : data.getMsgData()));
            if (hooks.onOk) {
                hooks.onOk(data);
            }
            return true;
        }
        default:
            DBG(netdbg::event("Cannot handle this opcode in client V1", peer, data.getType()));
            return false;
    }
}

} // namespace

ProtocolStack makeServerStack(ServerHooks hooks) {
    ProtocolStack stack;
    // Layer 0: always. Layer 1: peer major in [1, kProtoMajor] only.
    // Future: stack.addLayer(2, V2, 2); stack.addLayer(80, V80, 80);
    stack.addLayer(0, [hooks](const NetProtocol& data) { return parseServerVx(data, hooks); });
    stack.addLayer(1, [hooks](const NetProtocol& data) { return parseServerV1(data, hooks); }, kProtoMajor);
    return stack;
}

ProtocolStack makeClientStack(ClientHooks hooks) {
    ProtocolStack stack;
    // Same major gates as the server. Add V2/V80 here when the wire grows.
    stack.addLayer(0, [hooks](const NetProtocol& data) { return parseClientVx(data, hooks); });
    stack.addLayer(1, [hooks](const NetProtocol& data) { return parseClientV1(data, hooks); }, kProtoMajor);
    return stack;
}

NetProtocol versionHandshake() {
    return NetProtocol(Opcode::VersionSrv, kNetworkVersion);
}

std::optional<NetProtocol> makeClientRequest(Opcode command, std::string_view payload) {
    switch (command) {
        case Opcode::Read:
            return NetProtocol(Opcode::Read);
        case Opcode::Set:
            if (exceedsMaxMessage(payload.size())) {
                DBG("Cannot send Set — message is " + std::to_string(payload.size()) +
                    " bytes, over the " + std::to_string(kMaxMessageBytes) + " byte limit.");
                return std::nullopt;
            }
            return NetProtocol(Opcode::Set, std::string(payload));
        case Opcode::Shutdown:
            return NetProtocol(Opcode::Shutdown);
        default:
            DBG("Cannot handle opcode '" + std::string(magic_enum::enum_name(command)) + "' as a client request.");
            return std::nullopt;
    }
}

void logRecv(const NetProtocol& data, std::string_view peer) {
    DBG(netdbg::event("RECV " + data.getType(), peer, data.getMsgData()));
}

bool dispatchOrLog(const ProtocolStack& stack, const NetProtocol& data, std::string_view peer) {
    logRecv(data, peer);
    if (stack.dispatch(data)) {
        return true;
    }
    DBG(netdbg::event("Cannot handle this message", peer,
                      "'" + data.getType() + "' is not known to Vx or V1 (peer version " +
                          data.getVersion() + ")"));
    return false;
}

} // namespace protocol
