#include "ClientSession.hpp"

#include "Dbg.hpp"

ClientSession::ClientSession() {
    protocol_stack_ = protocol::makeClientStack({
        .peer = [this] { return peer(); },
        .onVersionReady =
            [this] {
                // Live with no pending Set: wait for stdin; otherwise send the CLI command.
                if (keep_alive_ && payload_.empty()) {
                    if (on_ready_for_prompt_) {
                        on_ready_for_prompt_();
                    }
                    return;
                }
                sendRequest();
            },
        .onShutdownGoodbye =
            [this](std::string_view) {
                exit_code_ = 0;
                close();
            },
        .onError =
            [this](std::string_view) {
                exit_code_ = 1;
                close();
            },
        .onOk =
            [this](const NetProtocol& ok) {
                last_ok_data_ = std::string(ok.getMsgData());
                request_sent_ = false;
                if (!keep_alive_) {
                    exit_code_ = 0;
                    close();
                    return;
                }
                if (on_ready_for_prompt_) {
                    on_ready_for_prompt_();
                }
            },
    });
}

void ClientSession::setCommand(Opcode command, std::string payload) {
    command_ = command;
    payload_ = std::move(payload);
}

void ClientSession::sendLiveSet(std::string payload) {
    command_ = Opcode::Set;
    payload_ = std::move(payload);
    request_sent_ = false;
    sendRequest();
}

void ClientSession::endLive() {
    exit_code_ = 0;
    close();
}

void ClientSession::onConnected() {
    DBG("Connected to server!");
    if (keep_alive_ && payload_.empty()) {
        return;
    }
    sendRequest();
}

void ClientSession::onDisconnected() {
    if (exit_code_ == 0) {
        return;
    }
    DBG(netdbg::event("Cannot continue", peer(),
                      "Server closed the connection before we got a reply — it may have crashed"));
}

void ClientSession::parseNetProtocol(NetProtocol data) {
    protocol::dispatchOrLog(protocol_stack_, data, peer());
}

void ClientSession::onProtocolError(std::string_view reason) {
    DBG(reason.empty() ? "Cannot parse the TCP stream — closing." : reason);
    exit_code_ = 1;
    close();
}

bool ClientSession::sendRequest() {
    if (request_sent_) {
        DBG("Request already sent");
        return true;
    }

    const auto request = protocol::makeClientRequest(command_, payload_);
    if (!request) {
        DBG("Cannot send — outbound command is not Read/Set/Shutdown.");
        return false;
    }

    switch (command_) {
        case Opcode::Read:
            DBG(netdbg::event("SEND READ", peer()));
            break;
        case Opcode::Set:
            DBG(netdbg::event("SEND SET", peer(), payload_));
            break;
        case Opcode::Shutdown:
            DBG(netdbg::event("SEND SHUTDOWN", peer()));
            break;
        default:
            DBG("Cannot handle opcode '" + std::string(magic_enum::enum_name(command_)) + "' when sending a client request.");
            break;
    }

    request_sent_ = sendData(*request);
    if (!request_sent_) {
        DBG("Cannot send the request — socket write failed, so the server never got it.");
    }
    return request_sent_;
}
