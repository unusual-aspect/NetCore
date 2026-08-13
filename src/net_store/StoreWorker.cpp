#include "StoreWorker.hpp"

#include "Dbg.hpp"

#include <stdexcept>
#include <utility>

StoreWorker::StoreWorker(std::unique_ptr<MessageStore> store) : store_(std::move(store)) {
    if (!store_) {
        throw std::invalid_argument("StoreWorker requires a MessageStore");
    }
    worker_ = std::thread([this] { workerMain(); });
}

StoreWorker::~StoreWorker() {
    stop();
}

MessageStore& StoreWorker::store() {
    return *store_;
}

void StoreWorker::stop() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        jobs_.push(StopJob{});
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void StoreWorker::finishPutLocked() {
    if (puts_outstanding_ > 0) {
        --puts_outstanding_;
    }
}

void StoreWorker::get(std::string peer, GetDone done) {
    if (!done) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_) {
            GetResult result;
            result.ok = false;
            result.error = "store stopping";
            done(std::move(result));
            return;
        }
        jobs_.push(GetJob{std::move(peer), std::move(done)});
    }
    cv_.notify_one();
}

void StoreWorker::put(std::string body, std::string peer, PutDone done) {
    if (!done) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_) {
            PutResult result;
            result.ok = false;
            result.error = "store stopping";
            done(std::move(result));
            return;
        }
        ++puts_outstanding_;
        // Another Set already queued or running — this one waits for the message-set lock.
        if (puts_outstanding_ > 1) {
            DBG("SET waiting for message lock — peer=" + peer + " (" +
                std::to_string(puts_outstanding_ - 1) + " Set(s) ahead)");
        }
        jobs_.push(PutJob{std::move(body), std::move(peer), std::move(done)});
    }
    cv_.notify_one();
}

void StoreWorker::workerMain() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop();
        }

        if (std::holds_alternative<StopJob>(job)) {
            std::queue<Job> leftover;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                leftover.swap(jobs_);
                puts_outstanding_ = 0;
            }
            while (!leftover.empty()) {
                Job left = std::move(leftover.front());
                leftover.pop();
                if (auto* get = std::get_if<GetJob>(&left)) {
                    if (get->done) {
                        GetResult result;
                        result.ok = false;
                        result.error = "store stopping";
                        get->done(std::move(result));
                    }
                } else if (auto* put = std::get_if<PutJob>(&left)) {
                    if (put->done) {
                        PutResult result;
                        result.ok = false;
                        result.error = "store stopping";
                        put->done(std::move(result));
                    }
                }
            }
            return;
        }

        if (auto* get = std::get_if<GetJob>(&job)) {
            GetResult result;
            try {
                // Shares MessageStore mutex with put — never reads a half-written row.
                result.value = store_->get(get->peer);
                result.ok = true;
            } catch (const std::exception& error) {
                result.ok = false;
                result.error = error.what();
                DBG(std::string("StoreWorker get failed — ") + error.what());
            }
            if (get->done) {
                get->done(std::move(result));
            }
            continue;
        }

        if (auto* put = std::get_if<PutJob>(&job)) {
            PutResult result;
            {
                // Exclusive message-Set lock: one writer at a time across concurrent clients.
                std::lock_guard<std::mutex> message_set_lock(message_set_mutex_);
                DBG("Message Set lock acquired — peer=" + put->peer);
                try {
                    store_->put(put->body, put->peer);
                    result.ok = true;
                } catch (const std::exception& error) {
                    result.ok = false;
                    result.error = error.what();
                    DBG(std::string("StoreWorker put failed — ") + error.what());
                }
                DBG("Message Set lock released — peer=" + put->peer);
            }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                finishPutLocked();
            }
            if (put->done) {
                put->done(std::move(result));
            }
        }
    }
}
