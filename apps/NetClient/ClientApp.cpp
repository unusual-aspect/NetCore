#include "ClientApp.hpp"
#include "Dbg.hpp"
#include "NetDefaults.hpp"
#include "NetTransport.hpp"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

std::atomic<NetTransport*> g_active_client_transport{nullptr};

void onClientStopSignal(int /*signum*/) {
    // Async-signal-safe: no post()/mutex — flag + loop wakeup only.
    auto* transport = g_active_client_transport.load(std::memory_order_acquire);
    if (transport) {
        transport->requestStopFromSignal();
    }
}

struct TransportGuard {
    explicit TransportGuard(NetTransport& transport) {
        g_active_client_transport.store(&transport, std::memory_order_release);
    }
    ~TransportGuard() {
        g_active_client_transport.store(nullptr, std::memory_order_release);
    }
};

void ensureClientSignalsInstalled() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::signal(SIGINT, onClientStopSignal);
#ifndef _WIN32
        std::signal(SIGTERM, onClientStopSignal);
#endif
    });
}

// Bound stdin so a pasted gigabyte never lands in memory / SQLite / the wire.
// Returns 1 = ok, 0 = EOF, -1 = oversize (line drained to newline).
int readMessageLine(std::istream& input, std::string& out) {
    out.clear();
    char ch = 0;
    while (input.get(ch)) {
        if (ch == '\n') {
            return 1;
        }
        if (ch == '\r') {
            continue;
        }
        if (out.size() >= kMaxMessageBytes) {
            while (input.get(ch) && ch != '\n') {
            }
            out.clear();
            return -1;
        }
        out.push_back(ch);
    }
    return out.empty() ? 0 : 1;
}

} // namespace

ClientApp::ClientApp(ClientSettings settings) : settings_(std::move(settings)) {}

int ClientApp::run() {
    netdbg::setVerbose(settings_.verbose);
    ensureClientSignalsInstalled();

    // --live: one TCP connection stays up so Shutdown/goodbye reaches us at the prompt.
    if (settings_.live) {
        DBG("Live Set loop → " + settings_.host + ":" + std::to_string(settings_.port));
        return runLive();
    }

    // No flags → ask for the Set payload.
    if (settings_.prompt_message) {
        std::cout << "What message we like to send? " << std::flush;
        const int read_rc = readMessageLine(std::cin, settings_.payload);
        if (read_rc == 0) {
            DBG("Cannot send Set — stdin closed before a message was entered.");
            return 1;
        }
        if (read_rc < 0) {
            DBG("Cannot send Set — message exceeds " + std::to_string(kMaxMessageBytes) + " bytes.");
            return 1;
        }
    }

    if (settings_.opcode == Opcode::Set && exceedsMaxMessage(settings_.payload.size())) {
        DBG("Cannot send Set — message exceeds " + std::to_string(kMaxMessageBytes) + " bytes.");
        return 1;
    }

    // One-shot: connect, command, exit (one 5s reconnect if the service is restarting).
    switch (settings_.opcode) {
        case Opcode::Read:
            DBG("Command Read");
            return runWithRecover(Opcode::Read, {});
        case Opcode::Set:
            DBG("Command Set: " + settings_.payload);
            return runWithRecover(Opcode::Set, settings_.payload);
        case Opcode::Shutdown:
            DBG("Command Shutdown");
            return runWithRecover(Opcode::Shutdown, {});
        default:
            DBG("Cannot handle opcode '" + std::string(magic_enum::enum_name(settings_.opcode)) + "' as a client command.");
            return 1;
    }
}

int ClientApp::runOnce(Opcode command, std::string payload) {
    ClientSession session;
    session.setCommand(command, std::move(payload));

    NetTransport transport;
    TransportGuard guard(transport);
    transport.attach(session);
    transport.setOnError([&](std::string_view what) {
        DBG(what.empty() ? "Cannot reach the server — connect failed with no further detail." : what);
    });

    DBG("Connecting to " + settings_.host + ":" + std::to_string(settings_.port));
    if (!transport.connect(settings_.host, settings_.port)) {
        DBG("Cannot reach " + settings_.host + ":" + std::to_string(settings_.port) + " — outbound connect did not start.");
        return 1;
    }

    transport.run();
    DBG("Exit " + std::to_string(session.exitCode()));
    return session.exitCode();
}

int ClientApp::runWithRecover(Opcode command, std::string payload) {
    // Shutdown turns the service off — do not retry, we would kill the restarted instance.
    if (command == Opcode::Shutdown) {
        return runOnce(command, std::move(payload));
    }

    const std::string retry_payload = payload;
    int code = runOnce(command, std::move(payload));
    if (code == 0) {
        return 0;
    }

    // First shot failed — give the service 5s to come back, then one reconnect.
    DBG("Server is not answering — waiting 5 seconds for the service to restart, then one reconnect.");
    std::this_thread::sleep_for(recover_wait_);

    DBG("Retrying once → " + settings_.host + ":" + std::to_string(settings_.port));
    code = runOnce(command, retry_payload);
    if (code == 0) {
        return 0;
    }

    DBG("Cannot reach the server after one retry — it is not available. Stopping.");
    return 1;
}

int ClientApp::runLiveOnce() {
    ClientSession session;
    session.setKeepAlive(true);

    NetTransport transport;
    TransportGuard guard(transport);
    transport.attach(session);
    transport.setOnError([&](std::string_view what) {
        DBG(what.empty() ? "Cannot reach the server — connect failed with no further detail." : what);
    });

    DBG("Connecting to " + settings_.host + ":" + std::to_string(settings_.port) + " (live, staying connected)");

    if (!transport.connect(settings_.host, settings_.port)) {
        DBG("Cannot reach " + settings_.host + ":" + std::to_string(settings_.port) + " — outbound connect did not start.");
        return 1;
    }

    // Prompt only after Version (and again after each Ok) so DBG lines do not bury it.
    std::mutex prompt_mu;
    std::condition_variable prompt_cv;
    std::atomic<bool> prompting{true};
    std::atomic<bool> ready_for_prompt{false};

    session.setOnReadyForPrompt([&] {
        ready_for_prompt = true;
        prompt_cv.notify_one();
    });

    std::thread input([&] {
        while (prompting.load()) {
            {
                std::unique_lock lock(prompt_mu);
                prompt_cv.wait(lock, [&] { return ready_for_prompt.load() || !prompting.load(); });
                if (!prompting.load()) {
                    break;
                }
                ready_for_prompt = false;
            }

            std::cout << "Set new message: " << std::flush;
            std::string payload;
            const int read_rc = readMessageLine(std::cin, payload);
            if (read_rc == 0) {
                transport.post([&] { session.endLive(); });
                break;
            }
            if (read_rc < 0) {
                DBG("Cannot send Set — message exceeds " + std::to_string(kMaxMessageBytes) +
                    " bytes. Enter a shorter message.");
                ready_for_prompt = true;
                prompt_cv.notify_one();
                continue;
            }
            if (!prompting.load()) {
                break;
            }
            DBG("Command Set: " + payload);
            transport.post([payload = std::move(payload), &session] { session.sendLiveSet(payload); });
        }
    });

    transport.run();
    prompting = false;
    prompt_cv.notify_one();
    if (session.exitCode() == 0) {
        std::cout << "\nServer said goodbye. Press Enter to exit.\n" << std::flush;
    } else {
        std::cout << "\nServer is not available. Press Enter to exit.\n" << std::flush;
    }
    input.join();

    DBG("Exit " + std::to_string(session.exitCode()));
    return session.exitCode();
}

int ClientApp::runLive() {
    int code = runLiveOnce();
    if (code == 0) {
        return 0;
    }

    DBG("Server is not answering — waiting 5 seconds for the service to restart, then one reconnect.");
    std::this_thread::sleep_for(recover_wait_);

    DBG("Retrying live once → " + settings_.host + ":" + std::to_string(settings_.port));
    code = runLiveOnce();
    if (code == 0) {
        return 0;
    }

    DBG("Cannot reach the server after one retry — it is not available. Stopping.");
    return 1;
}
