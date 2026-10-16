/* NxFrame diagnostic-only receiver transport capture. GPL-3.0-or-later. */
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ReceiverTsCaptureWriter {
public:
    ~ReceiverTsCaptureWriter() { stop(); }
    bool start(const std::string& path) {
        stop();
        if (path.empty()) return true;
        data_.open(path, std::ios::binary | std::ios::trunc);
        if (!data_) { std::cerr << "[Receiver][TS-CAPTURE] Cannot open " << path << "\n"; return false; }
        events_.open(path + ".timing.csv", std::ios::trunc);
        if (!events_) { std::cerr << "[Receiver][TS-CAPTURE] Cannot open timing sidecar\n"; data_.close(); return false; }
        events_ << "monotonic_ns,connection_generation,bytes,offset,kind\n";
        drops_ = 0;
        active_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { run(); });
        std::cerr << "[Receiver][TS-CAPTURE] Recording " << path << " (received TS, pre-demux)\n";
        return true;
    }
    void push(const std::vector<uint8_t>& bytes, uint64_t generation) {
        if (!active_.load(std::memory_order_acquire) || bytes.empty()) return;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lock(mutex_);
        // stop() may have disabled capture after the optimistic active check.
        if (!active_.load(std::memory_order_relaxed)) return;
        // Bounded at 8192 datagrams (~11 MB with 1316-byte SRT payloads).
        if (queue_.size() >= 8192) {
            ++drops_;
            return;
        }
        queue_.push_back({bytes, generation, ns});
        cv_.notify_one();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_.store(false, std::memory_order_release);
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
        if (data_.is_open()) data_.close();
        if (events_.is_open()) events_.close();
    }
private:
    struct Item { std::vector<uint8_t> bytes; uint64_t generation; int64_t ns; };
    void run() {
        uint64_t offset = 0;
        uint64_t last_generation = 0;
        uint64_t reported_drops = 0;
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !active_.load(std::memory_order_acquire) || !queue_.empty(); });
                if (queue_.empty()) break;
                item = std::move(queue_.front());
                queue_.pop_front();
                if (drops_ != reported_drops) {
                    std::cerr << "[Receiver][TS-CAPTURE] WARNING: capture queue dropped "
                              << (drops_ - reported_drops) << " datagram(s); capture is incomplete\n";
                    reported_drops = drops_;
                }
            }
            if (item.generation != last_generation) {
                events_ << item.ns << ',' << item.generation << ",0," << offset << ",generation\n";
                last_generation = item.generation;
            }
            data_.write(reinterpret_cast<const char*>(item.bytes.data()),
                        static_cast<std::streamsize>(item.bytes.size()));
            if (!data_) {
                std::cerr << "[Receiver][TS-CAPTURE] ERROR: disk write failed; disabling capture\n";
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.clear();
                active_.store(false, std::memory_order_release);
                break;
            }
            events_ << item.ns << ',' << item.generation << ',' << item.bytes.size()
                    << ',' << offset << ",data\n";
            offset += item.bytes.size();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (drops_ != reported_drops) {
                std::cerr << "[Receiver][TS-CAPTURE] WARNING: capture queue dropped "
                          << (drops_ - reported_drops) << " datagram(s); capture is incomplete\n";
            }
        }
        data_.flush();
        events_.flush();
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::thread worker_;
    std::ofstream data_;
    std::ofstream events_;
    std::atomic<bool> active_{false};
    uint64_t drops_ = 0;
};
