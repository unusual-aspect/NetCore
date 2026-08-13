#include "AppConfig.hpp"
#include "Dbg.hpp"
#include "ServerApp.hpp"

#include <atomic>
#include <csignal>
#include <memory>

namespace {

std::atomic<ServerApp*> g_server_app{nullptr};

void onStopSignal(int /*signum*/) {
    // Async-signal-safe: load pointer + requestStopFromSignal (atomic flag + wakeup).
    // Do not call post() here — that takes a mutex.
    auto* app = g_server_app.load(std::memory_order_acquire);
    if (!app) {
        return;
    }
    app->transport().requestStopFromSignal();
}

} // namespace

int main(int argc, char** argv) {
    if (!netdbg::openLog("NetServer", argv[0])) {
        return 1;
    }

    auto settings = AppConfig::parseServer(argc, argv);
    if (!settings) {
        return 1;
    }
    if (settings->help_requested) {
        return 0;
    }

    netdbg::setVerbose(settings->verbose);
    DBG("Run " + netdbg::runId() + " → " + netdbg::logPathForDisplay());

    ServerApp app(std::move(*settings));
    g_server_app.store(&app, std::memory_order_release);
    std::signal(SIGINT, onStopSignal);
#ifndef _WIN32
    std::signal(SIGTERM, onStopSignal);
#endif

    const int code = app.run();
    g_server_app.store(nullptr, std::memory_order_release);
    return code;
}
