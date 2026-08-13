#include "AppConfig.hpp"
#include "ClientApp.hpp"
#include "Dbg.hpp"

#include <csignal>

int main(int argc, char** argv) {
    if (!netdbg::openLog("NetClient", argv[0])) {
        return 1;
    }

    auto settings = AppConfig::parseClient(argc, argv);
    if (!settings) {
        return 1;
    }
    if (settings->help_requested) {
        return 0;
    }

    netdbg::setVerbose(settings->verbose);
    DBG("Run " + netdbg::runId() + " → " + netdbg::logPathForDisplay());

    return ClientApp(std::move(*settings)).run();
}
