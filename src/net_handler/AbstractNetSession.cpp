#include "AbstractNetSession.hpp"
#include "Dbg.hpp"
#include "FrameCodec.hpp"
#include "UsRuntime.hpp"

bool AbstractNetSession::sendData(const NetProtocol& data) {
    // Not bound yet, or on_close already nulled us.
    if (!socket_) {
        DBG("Cannot send " + data.getType() + " — this session has no TCP socket (not connected yet, or already closed).");
        return false;
    }
    const auto bytes = data.encode();
    if (bytes.size() < kFrameHeaderSize) {
        DBG("Cannot send " + data.getType() + " — encode refused (message over size limit, or empty frame).");
        return false;
    }
    if (!UsRuntime::write(socket_, bytes)) {
        DBG("Cannot send " + data.getType() + " — socket write failed (peer gone, or the kernel would not take the bytes).");
        return false;
    }
    return true;
}

void AbstractNetSession::close() {
    // Hook comes from bind() — usually UsRuntime::close(socket).
    if (close_socket_) {
        close_socket_();
    }
}

void AbstractNetSession::bind(us_socket_t* socket, std::string_view peer, std::function<void()> close_socket) {
    // Called once from NetTransport after accept/connect.
    socket_ = socket;
    peer_ip_ = std::string(peer);
    close_socket_ = std::move(close_socket);
}

bool AbstractNetSession::feed(std::span<const std::uint8_t> chunk) {
    // Stitch this chunk onto whatever we already had.
    receive_buffer_.insert(receive_buffer_.end(), chunk.begin(), chunk.end());

    for (;;) {
        std::size_t consumed = 0;
        bool fatal = false;
        auto message = NetProtocol::decode(receive_buffer_, consumed, &fatal);

        // Bad magic / size / JSON — drop the rest, tell the subclass, caller closes.
        if (fatal) {
            DBG("Cannot parse incoming bytes from " + peer_ip_ + " — the frame is not a valid 0xBEEF message. Closing this connection.");
            onProtocolError("Cannot parse the TCP stream — header/body is not a valid 0xBEEF frame.");
            receive_buffer_.clear();
            return false;
        }

        // Not a full frame yet — wait for more bytes.
        if (!message) {
            return true;
        }

        // One complete message: hand to subclass, then eat those bytes and loop
        // (two frames in one TCP chunk is normal).
        parseNetProtocol(std::move(*message));
        receive_buffer_.erase(receive_buffer_.begin(), receive_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
}
