#pragma once

#include "AppConfig.hpp"
#include "NetDefaults.hpp"
#include "NetTransport.hpp"
#include "StoreWorker.hpp"

#include <cstdint>
#include <memory>
#include <string>

// Process root: listen(bind, fixed port) first; open StoreWorker (MessageStore on a
// queue thread) only after bind succeeds; setFactory → ServerSession; then
// transport_.run() until Shutdown or stop.

class ServerApp {
public:
    explicit ServerApp(ServerSettings settings);

    int run();

    NetTransport& transport() { return transport_; }
    StoreWorker& store();

private:
    ServerSettings settings_;
    std::unique_ptr<StoreWorker> store_;  // opened after successful listen
    NetTransport transport_;
};
