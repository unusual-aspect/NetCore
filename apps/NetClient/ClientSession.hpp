#pragma once

#include "AbstractNetSession.hpp"
#include "Opcode.hpp"
#include "ProtocolLayers.hpp"

#include <functional>
#include <string>

// Thin client session: CLI/live exit behaviour; wire parsing is protocol::*.

class ClientSession : public AbstractNetSession {
public:
    ClientSession();

    // Args / request
    void setCommand(Opcode command, std::string payload = {});
    void setKeepAlive(bool keep_alive) { keep_alive_ = keep_alive; }
    int exitCode() const { return exit_code_; }
    const std::string& lastOkData() const { return last_ok_data_; }

    // Live mode: fired after Version (first prompt) and after each Ok (next prompt).
    void setOnReadyForPrompt(std::function<void()> cb) { on_ready_for_prompt_ = std::move(cb); }
    void sendLiveSet(std::string payload);
    void endLive();

    // Networking (session lifecycle)
    void onConnected() override;
    void onDisconnected() override;

protected:
    // Protocol (inbound wire)
    void parseNetProtocol(NetProtocol data) override;
    void onProtocolError(std::string_view reason) override;

private:
    // Networking
    bool sendRequest();

    // Args / request
    Opcode command_ = Opcode::Read;
    std::string payload_;
    bool request_sent_ = false;
    bool keep_alive_ = false;  // live: stay connected so we hear Shutdown
    int exit_code_ = 1;        // 0 after Ok (one-shot), Shutdown goodbye, or live stdin EOF
    std::string last_ok_data_; // last Opcode::Ok Data (for Read body / tests)
    std::function<void()> on_ready_for_prompt_;

    // Protocol
    ProtocolStack protocol_stack_;
};
