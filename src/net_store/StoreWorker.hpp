#pragma once

#include "MessageStore.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <variant>

// Runs MessageStore on a dedicated thread. The uSockets loop only enqueues work
// and posts replies; SQLite (including prune/checkpoint) never blocks accepts.
//
// Concurrent Set: every put takes the exclusive message-set lock on this worker
// (one writer at a time). Extra Sets wait in the job queue until the lock is free,
// then run in order — last successful commit wins. Completion callbacks run on the
// worker thread — callers must transport.post() before sockets / metrics / sendData.

class StoreWorker {
public:
    struct GetResult {
        bool ok = false;
        std::optional<std::string> value;
        std::string error;
    };

    struct PutResult {
        bool ok = false;
        std::string error;
    };

    using GetDone = std::function<void(GetResult)>;
    using PutDone = std::function<void(PutResult)>;

    explicit StoreWorker(std::unique_ptr<MessageStore> store);
    ~StoreWorker();

    StoreWorker(const StoreWorker&) = delete;
    StoreWorker& operator=(const StoreWorker&) = delete;

    void get(std::string peer, GetDone done);
    void put(std::string body, std::string peer, PutDone done);

    // Stop accepting jobs, drain queue, join worker. Safe to call once.
    void stop();

    MessageStore& store();

private:
    struct GetJob {
        std::string peer;
        GetDone done;
    };
    struct PutJob {
        std::string body;
        std::string peer;
        PutDone done;
    };
    struct StopJob {};

    using Job = std::variant<GetJob, PutJob, StopJob>;

    void workerMain();
    void finishPutLocked();  // caller holds mutex_

    std::unique_ptr<MessageStore> store_;
    std::mutex mutex_;                 // job queue
    std::mutex message_set_mutex_;     // exclusive Set (put) lock
    std::condition_variable cv_;
    std::queue<Job> jobs_;
    std::size_t puts_outstanding_ = 0; // queued + running Sets
    bool stopping_ = false;
    std::thread worker_;
};
