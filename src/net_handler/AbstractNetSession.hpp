#pragma once

#include "NetProtocol.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct us_socket_t;

// One TCP connection's protocol state.
//
// NetTransport::bind() sets socket_, peer_ip_, close_socket_, then onConnected().
// TCP gone → onDisconnected() then socket_ = nullptr. Client transport also stop()s.
// Incoming bytes: feed() appends to receive_buffer_, pulls complete frames via
// NetProtocol::decode, then parseNetProtocol (subclass). Incomplete frame stays
// buffered. Fatal decode → onProtocolError, feed() returns false, transport closes.
// Outgoing: sendData() encodes and UsRuntime::write(). close() runs close_socket_.

class AbstractNetSession {
public:
    virtual ~AbstractNetSession() = default;

    bool sendData(const NetProtocol& data);
    void close();
    const std::string& peer() const { return peer_ip_; }
    std::size_t receiveBufferedBytes() const { return receive_buffer_.size(); }
    us_socket_t* nativeSocket() const { return socket_; }
    std::uint64_t sessionId() const { return session_id_; }

    virtual void onConnected() {}
    virtual void onDisconnected() {}

protected:
    virtual void parseNetProtocol(NetProtocol data) = 0;
    virtual void onProtocolError(std::string_view /*reason*/) {}

    bool feed(std::span<const std::uint8_t> chunk);

private:
    friend class NetTransport;

    void bind(us_socket_t* socket, std::string_view peer, std::function<void()> close_socket);

    us_socket_t* socket_ = nullptr;             // owned by transport, nulled on close
    std::uint64_t session_id_ = 0;              // identity; socket pointers are reused
    std::string peer_ip_;
    std::function<void()> close_socket_;        // closes this TCP socket
    std::vector<std::uint8_t> receive_buffer_;  // leftover of an incomplete frame
};
