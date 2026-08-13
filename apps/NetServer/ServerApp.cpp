#include "ServerApp.hpp"
#include "Dbg.hpp"
#include "ServerSession.hpp"

#include <memory>
#include <stdexcept>
#include <string>

ServerApp::ServerApp(ServerSettings settings): settings_(std::move(settings)) {
    transport_.setMaxConnections(kMaxConnections);
}

StoreWorker& ServerApp::store() {
    if (!store_) {
        throw std::logic_error("StoreWorker is not open yet (listen first)");
    }
    return *store_;
}

int ServerApp::run() {
    // Bind before opening SQLite so a second instance exits cleanly with no DB side effects.
    if (!transport_.listen(settings_.bind_host, settings_.port)) {
        DBG("Cannot start NetServer on " + settings_.bind_host + ":" + std::to_string(settings_.port) +
            " — listen/bind failed (address/port in use, or no permission).");
        return 1;
    }

    try {
        store_ = std::make_unique<StoreWorker>(std::make_unique<MessageStore>(settings_.db_path, settings_.access_log_max_rows));
    } catch (const std::exception& error) {
        DBG(std::string("Cannot open message store after bind — ") + error.what());
        transport_.stop();
        return 1;
    }

    // Each accept gets its own session, same store worker + transport + shutdown policy.
    transport_.setFactory([this](std::string_view) {
        return std::make_unique<ServerSession>(*store_, transport_, settings_.allow_remote_shutdown);
    });

    DBG("Start NetServer on " + settings_.bind_host + ":" + std::to_string(settings_.port));
    // Blocks here. Shutdown opcode or stop() lets it return.
    transport_.run();

    if (store_) {
        store_->stop();
    }

    return 0;
}
