/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * License / EULA notice:
 * This file is part of NxFrame. Use, redistribution, and modification are
 * governed by the project license and any written EULA or commercial license
 * agreement supplied with the project. If no separate written agreement is
 * supplied, the GPL-3.0-or-later terms apply.
 *
 * Description:
 * Bounded live-pipeline queue. This template provides blocking and drop-policy queue semantics for sender, receiver, and output worker threads.
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <chrono>
#include <utility>


// Live media queues must choose explicitly between backpressure and dropping stale data.
enum class QueueOverflowPolicy {
    Block,
    DropOldest,
    DropNewest
};

enum class QueuePushResult {
    Pushed,
    DroppedOldestAndPushed,
    DroppedNewest,
    Stopped
};

template<typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : cap_(capacity) {}

    // Blocking push used where preserving every item is more important than live latency.
    bool push(T item) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (cap_ == 0) {
            return false;
        }
        cv_not_full_.wait(lk, [&] { return stopped_ || q_.size() < cap_; });
        if (stopped_) return false;
        q_.push(std::move(item));
        cv_not_empty_.notify_one();
        return true;
    }

    bool try_push(T item) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopped_ || q_.size() >= cap_) return false;
        q_.push(std::move(item));
        cv_not_empty_.notify_one();
        return true;
    }

    bool push_drop_oldest(T item) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopped_) return false;
        if (cap_ == 0) return false;
        if (q_.size() >= cap_) {
            q_.pop();
        }
        q_.push(std::move(item));
        cv_not_empty_.notify_one();
        return true;
    }

    // Non-blocking policy push used by live paths that must avoid unbounded latency growth.
    QueuePushResult push_with_policy(T item, QueueOverflowPolicy policy) {
        if (policy == QueueOverflowPolicy::Block) {
            return push(std::move(item)) ? QueuePushResult::Pushed : QueuePushResult::Stopped;
        }

        std::lock_guard<std::mutex> lk(mtx_);
        if (stopped_ || cap_ == 0) {
            return QueuePushResult::Stopped;
        }

        if (q_.size() >= cap_) {
            if (policy == QueueOverflowPolicy::DropNewest) {
                return QueuePushResult::DroppedNewest;
            }
            q_.pop();
            q_.push(std::move(item));
            cv_not_empty_.notify_one();
            return QueuePushResult::DroppedOldestAndPushed;
        }

        q_.push(std::move(item));
        cv_not_empty_.notify_one();
        return QueuePushResult::Pushed;
    }

    QueuePushResult push_for_with_policy(T item,
                                         std::chrono::milliseconds timeout,
                                         QueueOverflowPolicy overflowPolicy) {
        if (timeout.count() <= 0) {
            return push_with_policy(std::move(item), overflowPolicy);
        }

        std::unique_lock<std::mutex> lk(mtx_);
        if (cap_ == 0) {
            return QueuePushResult::Stopped;
        }

        const bool hasSpace = cv_not_full_.wait_for(lk, timeout, [&] {
            return stopped_ || q_.size() < cap_;
        });

        if (stopped_) {
            return QueuePushResult::Stopped;
        }

        if (hasSpace && q_.size() < cap_) {
            q_.push(std::move(item));
            cv_not_empty_.notify_one();
            return QueuePushResult::Pushed;
        }

        if (overflowPolicy == QueueOverflowPolicy::Block) {
            lk.unlock();
            return push(std::move(item)) ? QueuePushResult::Pushed : QueuePushResult::Stopped;
        }

        if (overflowPolicy == QueueOverflowPolicy::DropNewest) {
            return QueuePushResult::DroppedNewest;
        }

        q_.pop();
        q_.push(std::move(item));
        cv_not_empty_.notify_one();
        return QueuePushResult::DroppedOldestAndPushed;
    }

    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_not_empty_.wait(lk, [&] { return stopped_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        cv_not_full_.notify_one();
        return true;
    }

    bool pop_for(T& out, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (!cv_not_empty_.wait_for(lk, timeout, [&] { return stopped_ || !q_.empty(); })) {
            return false;
        }
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        cv_not_full_.notify_one();
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        cv_not_full_.notify_one();
        return true;
    }

    // Wake all waiting producers/consumers during shutdown.
    void stop() {
        std::lock_guard<std::mutex> lk(mtx_);
        stopped_ = true;
        cv_not_full_.notify_all();
        cv_not_empty_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.empty();
    }

private:
    size_t cap_;
    mutable std::mutex mtx_;
    std::condition_variable cv_not_full_, cv_not_empty_;
    std::queue<T> q_;
    bool stopped_ = false;
};