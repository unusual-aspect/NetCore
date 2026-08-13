#pragma once

#include "NetDefaults.hpp"
#include "Opcode.hpp"

#include <cstdint>
#include <optional>
#include <string>

// argv → settings for NetServer and NetClient.

struct ServerSettings {
    // Listen (port is fixed at NET_DEFAULT_PORT — not a server CLI flag)
    std::string bind_host = NET_DEFAULT_BIND;
    std::uint16_t port = NET_DEFAULT_PORT;
    std::string db_path = NET_DEFAULT_DB_PATH;

    // Hardening / ops
    bool allow_remote_shutdown = false;
    std::uint64_t access_log_max_rows = kDefaultAccessLogMaxRows;
    bool verbose = false;

    // CLI
    bool help_requested = false;
};

struct ClientSettings {
    // Connect
    std::string host = NET_DEFAULT_HOST;
    std::uint16_t port = NET_DEFAULT_PORT;

    // Request (same Opcode as the wire; --live is CLI-only)
    Opcode opcode = Opcode::Set;
    std::string payload;
    bool live = false;            // --live: stay connected, prompt for Set
    bool prompt_message = false;  // no --set/--read/--live/--shutdown → ask stdin
    bool verbose = false;

    // CLI
    bool help_requested = false;
};

class AppConfig {
public:
    static std::optional<ServerSettings> parseServer(int argc, char** argv);
    static std::optional<ClientSettings> parseClient(int argc, char** argv);
};
