#pragma once

#include <string_view>

// Peer IP helpers for Shutdown gating (no crypto).

inline bool isLoopbackPeer(std::string_view peer) {
    return peer == "127.0.0.1" || peer == "::1" || peer == "0:0:0:0:0:0:0:1" ||
           peer == "::ffff:127.0.0.1";
}
