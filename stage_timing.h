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
 * Lightweight optional stage timing registry. Pipeline stages can record scoped
 * timing samples without changing frame ownership or queue behaviour.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace stage_timing {

// Per-stage aggregate timing counters. All updates are relaxed atomics because
// these counters are diagnostic only and must not affect realtime pipeline order.
struct StageStats {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> total_ns{0};
    std::atomic<uint64_t> max_ns{0};

    void add(uint64_t ns)
    {
        calls.fetch_add(1, std::memory_order_relaxed);
        total_ns.fetch_add(ns, std::memory_order_relaxed);

        uint64_t old = max_ns.load(std::memory_order_relaxed);
        while (ns > old && !max_ns.compare_exchange_weak(old, ns, std::memory_order_relaxed)) {
        }
    }

    void reset()
    {
        calls.store(0, std::memory_order_relaxed);
        total_ns.store(0, std::memory_order_relaxed);
        max_ns.store(0, std::memory_order_relaxed);
    }
};

// Process-wide timing registry. It is intentionally header-only and lightweight
// so hot-path code can opt in with a ScopedTimer when timing is enabled.
class Registry {
public:
    void setEnabled(bool enabled, bool verbose)
    {
        enabled_.store(enabled, std::memory_order_release);
        verbose_.store(verbose, std::memory_order_release);
    }

    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    bool verboseEnabled() const { return verbose_.load(std::memory_order_acquire); }

    StageStats& get(const std::string& name)
    {
        // Registration is serialized, but individual StageStats updates are lock-free.

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = stats_.find(name);
        if (it != stats_.end()) {
            return *(it->second);
        }
        auto holder = std::unique_ptr<StageStats>(new StageStats());
        StageStats* raw = holder.get();
        stats_.emplace(name, std::move(holder));
        order_.push_back(name);
        return *raw;
    }

    std::string reportAndReset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;
        bool first = true;
        for (const auto& name : order_) {
            auto it = stats_.find(name);
            if (it == stats_.end() || !it->second) {
                continue;
            }
            StageStats& s = *(it->second);
            const uint64_t calls = s.calls.load(std::memory_order_relaxed);
            if (calls == 0) {
                continue;
            }
            const uint64_t total = s.total_ns.load(std::memory_order_relaxed);
            const uint64_t maxv = s.max_ns.load(std::memory_order_relaxed);
            const double avg_us = static_cast<double>(total) / static_cast<double>(calls) / 1000.0;
            const double max_us = static_cast<double>(maxv) / 1000.0;
            if (!first) {
                oss << "  ";
            }
            first = false;
            oss << name << ":avg=" << avg_us << "us,max=" << max_us << "us,n=" << calls;
            s.reset();
        }
        return oss.str();
    }

private:
    std::atomic<bool> enabled_{false};
    std::atomic<bool> verbose_{false};
    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<StageStats>> stats_;
    std::vector<std::string> order_;
};

inline Registry& registry()
{
    static Registry r;
    return r;
}

inline void set_enabled(bool enabled, bool verbose = false)
{
    registry().setEnabled(enabled, verbose);
}

inline bool enabled()
{
    return registry().enabled();
}

inline bool verbose_enabled()
{
    return registry().verboseEnabled();
}

inline StageStats& get(const std::string& name)
{
    return registry().get(name);
}

inline std::string report_and_reset()
{
    return registry().reportAndReset();
}

// RAII helper used around individual processing stages. When timing is disabled,
// construction is cheap and no duration is recorded.
class ScopedTimer {
public:
    explicit ScopedTimer(StageStats& stats)
        : stats_(&stats)
    {
        if (enabled()) {
            active_ = true;
            start_ = std::chrono::steady_clock::now();
        }
    }

    ~ScopedTimer()
    {
        stop();
    }

    void stop()
    {
        if (!active_ || !stats_) {
            return;
        }
        const auto end = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        stats_->add(static_cast<uint64_t>(ns));
        active_ = false;
    }

private:
    StageStats* stats_ = nullptr;
    bool active_ = false;
    std::chrono::steady_clock::time_point start_{};
};

inline void add_duration(StageStats& stats, uint64_t ns)
{
    if (!enabled()) {
        return;
    }
    stats.add(ns);
}

} // namespace stage_timing