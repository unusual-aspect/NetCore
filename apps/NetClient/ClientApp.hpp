#pragma once

#include "AppConfig.hpp"
#include "ClientSession.hpp"
#include "Opcode.hpp"

#include <chrono>
#include <string>

// Process root for NetClient: one-shot Read/Set/Shutdown (one 5s reconnect if the
// service is restarting) or --live (stdin prompt + same reconnect policy).
// ClientSession owns the protocol; this class owns connect/run/retry.

class ClientApp {
public:
    explicit ClientApp(ClientSettings settings);

    int run();

    // Tests / ops: shorten the 5s "wait for restart" between the first failure and one retry.
    void setRecoverWait(std::chrono::milliseconds wait) { recover_wait_ = wait; }

private:
    // One TCP connection, one request opcode, then the loop exits when the session closes.
    int runOnce(Opcode command, std::string payload);

    // First attempt, then one wait + one reconnect (Shutdown never retries).
    int runWithRecover(Opcode command, std::string payload);

    // --live: stay connected; stdin thread posts Set onto the loop.
    int runLiveOnce();
    int runLive();

    // Server is supposed to run forever and restart after a crash. One wait, one reconnect.
    std::chrono::milliseconds recover_wait_{std::chrono::seconds(5)};

    ClientSettings settings_;
};
