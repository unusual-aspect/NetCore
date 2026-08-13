#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

// Single place for listen/connect CLI defaults and hardening caps.
// Change here — AppConfig and ServerApp pick it up.

#define NET_DEFAULT_PORT     9555
#define NET_DEFAULT_HOST     "127.0.0.1"
#define NET_DEFAULT_BIND     "127.0.0.1"
#define NET_DEFAULT_DB_PATH  "message.db"

#define NET_STR_HELPER(x) #x
#define NET_STR(x) NET_STR_HELPER(x)
#define NET_DEFAULT_PORT_STR NET_STR(NET_DEFAULT_PORT)

inline constexpr std::uint16_t kNetDefaultPort = NET_DEFAULT_PORT;

// Hardening caps (server)
inline constexpr std::size_t kMaxConnections = 128;
inline constexpr std::size_t kMaxUnsentBytes = 4u * 1024u * 1024u;  // 4 MiB per socket
inline constexpr std::size_t kMaxTotalReceiveBytes = 16u * 1024u * 1024u;  // all sessions
inline constexpr unsigned kIdleTimeoutSec = 120;  // incomplete-frame idle only; 0 disables
// One-shot client: close if no reply (Ok/Error/goodbye). Quiet --live is not armed.
inline constexpr unsigned kOneShotReplyTimeoutSec = 3;
inline constexpr std::uint64_t kDefaultAccessLogMaxRows = 100000;
inline constexpr std::size_t kMaxAccessLogDetailBytes = 4096;  // PUT detail truncate
inline constexpr std::size_t kMaxMessageBytes = 1u * 1024u * 1024u;  // Set body / store
inline constexpr std::size_t kDbgLogMaxBytes = 10u * 1024u * 1024u;   // also rotate within a day if huge
inline constexpr std::size_t kDbgLogMaxFiles = 14;                    // keep newest N under logs/ (~2 weeks)
inline constexpr int kSqliteBusyTimeoutMs = 5000;
// Wire Opcode::Error Data for store failures — detail stays in DBG only.
inline constexpr std::string_view kStoreUnavailableMsg = "store unavailable";

inline constexpr bool exceedsMaxMessage(std::size_t byte_count) {
    return byte_count > kMaxMessageBytes;
}
