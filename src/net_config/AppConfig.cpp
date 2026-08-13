#include "AppConfig.hpp"
#include "Dbg.hpp"
#include "NetDefaults.hpp"

#include <cxxopts.hpp>

#include <iostream>
#include <string>

std::optional<ServerSettings> AppConfig::parseServer(int argc, char** argv) {
    cxxopts::Options options("NetServer", "NetProtocol server");
    options.add_options()
        ("B,bind", "listen bind address", cxxopts::value<std::string>()->default_value(NET_DEFAULT_BIND))
        ("d,db", "sqlite path", cxxopts::value<std::string>()->default_value(NET_DEFAULT_DB_PATH))
        ("allow-remote-shutdown", "allow Shutdown from non-loopback peers")
        ("log-retain", "max access_log rows to keep (0 = unbounded, no prune)",
         cxxopts::value<std::uint64_t>()->default_value(std::to_string(kDefaultAccessLogMaxRows)))
        ("v,verbose", "log full message bodies in DBG events")
        ("h,help", "Show help");

    try {
        auto result = options.parse(argc, argv);
        ServerSettings settings;

        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            settings.help_requested = true;
            return settings;
        }

        // Port is fixed (NET_DEFAULT_PORT) — no --port on the server so a second
        // process always collides on bind instead of sneaking in on another port.
        settings.bind_host = result["bind"].as<std::string>();
        settings.port = NET_DEFAULT_PORT;
        settings.db_path = result["db"].as<std::string>();
        settings.allow_remote_shutdown = result.count("allow-remote-shutdown") > 0;
        settings.access_log_max_rows = result["log-retain"].as<std::uint64_t>();
        settings.verbose = result.count("verbose") > 0;
        return settings;
    } catch (const std::exception& error) {
        DBG(std::string("Cannot start — command line is invalid: ") + error.what());
        return std::nullopt;
    }
}

std::optional<ClientSettings> AppConfig::parseClient(int argc, char** argv) {
    cxxopts::Options options("NetClient", "NetProtocol client");
    options.add_options()
        ("H,host", "server host", cxxopts::value<std::string>()->default_value(NET_DEFAULT_HOST))
        ("P,port", "server port", cxxopts::value<std::uint16_t>()->default_value(NET_DEFAULT_PORT_STR))
        ("r,read", "read current message")
        ("s,set", "set message (max 1 MiB)", cxxopts::value<std::string>())
        ("k,shutdown", "terminate server")
        ("l,live", "prompt and set latest message, over and over")
        ("v,verbose", "log full message bodies in DBG events")
        ("h,help", "show help");

    try {
        auto result = options.parse(argc, argv);
        ClientSettings settings;

        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            settings.help_requested = true;
            return settings;
        }

        settings.host = result["host"].as<std::string>();
        settings.port = result["port"].as<std::uint16_t>();
        settings.verbose = result.count("verbose") > 0;

        // First matching flag wins. No flag → Set and prompt for the message.
        if (result.count("live")) {
            settings.live = true;
            settings.opcode = Opcode::Set;
        } else if (result.count("read")) {
            settings.opcode = Opcode::Read;
        } else if (result.count("set")) {
            settings.opcode = Opcode::Set;
            settings.payload = result["set"].as<std::string>();
            if (exceedsMaxMessage(settings.payload.size())) {
                DBG("Cannot start — --set payload is " + std::to_string(settings.payload.size()) +
                    " bytes, over the " + std::to_string(kMaxMessageBytes) + " byte limit.");
                return std::nullopt;
            }
        } else if (result.count("shutdown")) {
            settings.opcode = Opcode::Shutdown;
        } else {
            settings.opcode = Opcode::Set;
            settings.prompt_message = true;
        }

        return settings;
    } catch (const std::exception& error) {
        DBG(std::string("Cannot start — command line is invalid: ") + error.what());
        return std::nullopt;
    }
}
