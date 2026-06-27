/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/srt_input.cpp
 * Description: Implements SRT receive/listen/reconnect transport ingest.
 */

#include "receiver/srt_input.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <thread>
#include <syslog.h>

namespace
{

void configureSrtLibraryLogging()
{
    static std::once_flag once;
    std::call_once(once, []() {
        // Keep NxFrame application logging intact while suppressing noisy
        // libsrt ERROR-level messages produced by non-blocking accept polling.
        // Critical/fatal SRT library messages remain enabled.
        srt_setloglevel(LOG_CRIT);
    });
}

bool setSockOptInt(SRTSOCKET socket, SRT_SOCKOPT opt, int value)
{
    return srt_setsockopt(socket, 0, opt, &value, sizeof(value)) != SRT_ERROR;
}

bool setSockOptInt64(SRTSOCKET socket, SRT_SOCKOPT opt, int64_t value)
{
    return srt_setsockopt(socket, 0, opt, &value, sizeof(value)) != SRT_ERROR;
}

bool setSockOptString(SRTSOCKET socket, SRT_SOCKOPT opt, const std::string& value)
{
    return value.empty() ||
           srt_setsockopt(socket, 0, opt, value.c_str(),
                          static_cast<int>(value.size())) != SRT_ERROR;
}

bool setSockOptLinger(SRTSOCKET socket, int seconds)
{
    struct linger ling{};
    if (seconds > 0) {
        ling.l_onoff = 1;
        ling.l_linger = seconds;
    }
    return srt_setsockopt(socket, 0, SRTO_LINGER, &ling, sizeof(ling)) != SRT_ERROR;
}

using AddrInfoPtr = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>;

AddrInfoPtr resolveAddress(const std::string& host, int port, int flags)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = flags;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);

    if (getaddrinfo(host.empty() ? nullptr : host.c_str(),
                    port_str.c_str(),
                    &hints,
                    &result) != 0) {
        return AddrInfoPtr(nullptr, freeaddrinfo);
    }

    return AddrInfoPtr(result, freeaddrinfo);
}

int64_t monotonicNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}


static const size_t kTsPacketSize = 188u;
static const uint8_t kTsSyncByte = 0x47u;

size_t findLikelyTsSyncOffset(const std::vector<uint8_t>& data)
{
    const size_t n = data.size();
    if (n < kTsPacketSize) {
        return n;
    }

    const size_t max_probe = std::min(kTsPacketSize, n);
    size_t best_offset = n;
    size_t best_packets = 0;

    for (size_t offset = 0; offset < max_probe; ++offset) {
        if (data[offset] != kTsSyncByte) {
            continue;
        }

        size_t packets = 0;
        for (size_t pos = offset; pos < n; pos += kTsPacketSize) {
            if (data[pos] != kTsSyncByte) {
                break;
            }
            ++packets;
        }

        if (packets > best_packets) {
            best_packets = packets;
            best_offset = offset;
        }
    }

    return (best_packets > 0) ? best_offset : n;
}

bool extractAlignedTsPayload(std::vector<uint8_t>& pending,
                             std::vector<uint8_t>& out,
                             uint64_t& dropped_bytes)
{
    out.clear();
    dropped_bytes = 0;

    while (pending.size() >= kTsPacketSize) {
        if (pending[0] != kTsSyncByte) {
            const size_t sync = findLikelyTsSyncOffset(pending);
            if (sync == pending.size()) {
                // Keep only a small tail in case the next receive completes a TS packet.
                const size_t keep = std::min(pending.size(), kTsPacketSize - 1u);
                dropped_bytes += pending.size() - keep;
                if (keep > 0) {
                    std::vector<uint8_t> tail(pending.end() - static_cast<std::ptrdiff_t>(keep), pending.end());
                    pending.swap(tail);
                } else {
                    pending.clear();
                }
                return false;
            }
            if (sync > 0) {
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(sync));
                dropped_bytes += sync;
            }
        }

        if (pending.size() < kTsPacketSize) {
            return false;
        }

        size_t packet_count = 0;
        const size_t complete_packets = pending.size() / kTsPacketSize;
        for (; packet_count < complete_packets; ++packet_count) {
            const size_t pos = packet_count * kTsPacketSize;
            if (pending[pos] != kTsSyncByte) {
                break;
            }
        }

        if (packet_count > 0) {
            const size_t bytes = packet_count * kTsPacketSize;
            out.assign(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(bytes));
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(bytes));
            return true;
        }

        // We had a sync byte at zero but failed at the next packet boundary.
        // Drop one byte and search again instead of poisoning the demuxer with
        // permanently misaligned data.
        pending.erase(pending.begin());
        ++dropped_bytes;
    }

    return false;
}

bool isTransientReceiveNoDataError(const std::string& err)
{
    std::string e = err;
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return e.find("timeout") != std::string::npos ||
           e.find("timed out") != std::string::npos ||
           e.find("non-blocking") != std::string::npos ||
           e.find("temporarily unavailable") != std::string::npos ||
           e.find("try again") != std::string::npos ||
           e.find("would block") != std::string::npos ||
           e.find("again") != std::string::npos;
}

} // namespace

SRTInput::SRTInput()
{
    srt_startup();
    configureSrtLibraryLogging();
}

SRTInput::~SRTInput()
{
    stop();
    srt_cleanup();
}

const char* SRTInput::stateToString(State state)
{
    switch (state) {
        case State::Stopped: return "STOPPED";
        case State::Connecting: return "CONNECTING";
        case State::Listening: return "LISTENING";
        case State::Connected: return "CONNECTED";
        case State::Reconnecting: return "RECONNECTING";
        case State::Failed: return "FAILED";
        case State::Closing: return "CLOSING";
        default: return "UNKNOWN";
    }
}

const char* SRTInput::modeToString(Mode mode)
{
    switch (mode) {
        case Mode::Caller: return "caller";
        case Mode::Listener: return "listener";
        case Mode::Rendezvous: return "rendezvous";
        default: return "listener";
    }
}

void SRTInput::setState(State state) noexcept
{
    state_.store(state, std::memory_order_release);
}

SRTInput::State SRTInput::getState() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

std::string SRTInput::getLastError() const
{
    std::lock_guard<std::mutex> lk(state_mutex_);
    return last_error_;
}

void SRTInput::setLastError(const std::string& error)
{
    std::lock_guard<std::mutex> lk(state_mutex_);
    last_error_ = error;
}

uint64_t SRTInput::receivedPackets() const noexcept
{
    return received_packets_.load(std::memory_order_relaxed);
}

uint64_t SRTInput::receivedBytes() const noexcept
{
    return received_bytes_.load(std::memory_order_relaxed);
}

uint64_t SRTInput::droppedPackets() const noexcept
{
    return dropped_packets_.load(std::memory_order_relaxed);
}

uint64_t SRTInput::realignedPackets() const noexcept
{
    return realigned_packets_.load(std::memory_order_relaxed);
}

uint64_t SRTInput::realignedBytes() const noexcept
{
    return realigned_bytes_.load(std::memory_order_relaxed);
}

bool SRTInput::start(const Config& config)
{
    stop();

    config_ = config;
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    received_packets_.store(0, std::memory_order_release);
    received_bytes_.store(0, std::memory_order_release);
    dropped_packets_.store(0, std::memory_order_release);
    realigned_packets_.store(0, std::memory_order_release);
    realigned_bytes_.store(0, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        last_error_.clear();
    }

    setState(config.mode == Mode::Listener ? State::Listening : State::Connecting);

    std::cerr << "[SRTInput] Starting mode=" << modeToString(config.mode)
              << " target="
              << (config.mode == Mode::Listener
                      ? (config.bind_address.empty() ? "0.0.0.0" : config.bind_address)
                      : config.address)
              << ":" << config.port
              << " latency=" << config.latency
              << " payload=" << config.payload_size
              << "\n";

    recv_thread_ = std::thread(&SRTInput::receiveLoop, this);
    return true;
}

void SRTInput::stop()
{
    stop_requested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    setState(State::Closing);

    closeSockets();
    cv_.notify_all();

    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        queue_.clear();
    }

    setState(State::Stopped);
}

bool SRTInput::reconnect()
{
    closeSockets();
    return initSocketAndConnect();
}

void SRTInput::closeSockets()
{
    std::lock_guard<std::mutex> lk(socket_mutex_);

    if (listener_socket_ != SRT_INVALID_SOCK) {
        srt_close(listener_socket_);
        listener_socket_ = SRT_INVALID_SOCK;
    }

    if (socket_ != SRT_INVALID_SOCK) {
        srt_close(socket_);
        socket_ = SRT_INVALID_SOCK;
    }
}

bool SRTInput::applyCommonOptions(SRTSOCKET socket, const Config& c)
{
    return setSockOptInt(socket, SRTO_RCVSYN, 1) &&
           setSockOptInt(socket, SRTO_SNDSYN, 1) &&
           setSockOptInt(socket, SRTO_MESSAGEAPI, c.messageapi ? 1 : 0) &&
           setSockOptInt(socket, SRTO_TLPKTDROP, c.tlpktdrop ? 1 : 0) &&
           setSockOptInt(socket, SRTO_NAKREPORT, c.nakreport ? 1 : 0) &&
           setSockOptInt(socket, SRTO_PAYLOADSIZE, c.payload_size) &&
           setSockOptInt(socket, SRTO_RCVBUF, c.rcvbuf) &&
           setSockOptInt(socket, SRTO_SNDBUF, c.sndbuf) &&
           setSockOptInt(socket, SRTO_OHEADBW, c.oheadbw) &&
           setSockOptInt(socket, SRTO_LATENCY, c.latency) &&
           (c.peer_latency <= 0 || setSockOptInt(socket, SRTO_PEERLATENCY, c.peer_latency)) &&
           (c.rcv_latency <= 0 || setSockOptInt(socket, SRTO_RCVLATENCY, c.rcv_latency)) &&
           setSockOptInt(socket, SRTO_RCVTIMEO, c.recv_timeout_ms) &&
           setSockOptInt(socket, SRTO_PEERIDLETIMEO, c.peer_idle_timeout_ms) &&
           setSockOptLinger(socket, c.linger) &&
           (c.maxbw == 0 || setSockOptInt64(socket, SRTO_MAXBW, c.maxbw)) &&
           (c.inputbw == 0 || setSockOptInt64(socket, SRTO_INPUTBW, c.inputbw)) &&
           setSockOptString(socket, SRTO_STREAMID, c.streamid) &&
           setSockOptString(socket, SRTO_PASSPHRASE, c.passphrase) &&
           (c.pbkeylen <= 0 || setSockOptInt(socket, SRTO_PBKEYLEN, c.pbkeylen)) &&
           setSockOptInt(socket, SRTO_TRANSTYPE, SRTT_LIVE) &&
           setSockOptInt(socket, SRTO_CONNTIMEO, c.connect_timeout_ms) &&
           setSockOptInt(socket, SRTO_RENDEZVOUS, c.mode == Mode::Rendezvous ? 1 : 0);
}

bool SRTInput::resolveAndConnect(SRTSOCKET socket, const Config& c)
{
    auto addr_list = resolveAddress(c.address, c.port, 0);
    if (!addr_list) {
        setLastError("Failed to resolve SRT remote address");
        return false;
    }

    if (!c.bind_address.empty() && c.mode != Mode::Rendezvous) {
        auto bind_list = resolveAddress(c.bind_address, 0, AI_PASSIVE);
        if (bind_list) {
            bool bound = false;
            for (addrinfo* ai = bind_list.get(); ai; ai = ai->ai_next) {
                if ((ai->ai_family == AF_INET || ai->ai_family == AF_INET6) &&
                    srt_bind(socket, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
                    bound = true;
                    break;
                }
            }
            if (!bound) {
                std::cerr << "[SRTInput] Warning: caller bind_address="
                          << c.bind_address << " could not be bound.\n";
            }
        }
    }

    std::cerr << "[SRTInput] Connecting as caller to "
              << c.address << ":" << c.port << "\n";

    for (addrinfo* ai = addr_list.get(); ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }

        if (srt_connect(socket, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            return true;
        }
    }

    setLastError(std::string("srt_connect failed: ") + srt_getlasterror_str());
    return false;
}

bool SRTInput::isNoPendingAcceptError(const std::string& err)
{
    return err.find("no pending connection") != std::string::npos ||
           err.find("No pending connection") != std::string::npos ||
           err.find("no pending accept") != std::string::npos;
}

bool SRTInput::resolveBindListenAccept(SRTSOCKET socket,
                                       const Config& c,
                                       SRTSOCKET& out_socket)
{
    auto bind_list = resolveAddress(c.bind_address, c.port, AI_PASSIVE);
    if (!bind_list) {
        setLastError("Failed to resolve SRT bind/listen address");
        return false;
    }

    bool bound = false;
    for (addrinfo* ai = bind_list.get(); ai; ai = ai->ai_next) {
        if ((ai->ai_family == AF_INET || ai->ai_family == AF_INET6) &&
            srt_bind(socket, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            bound = true;
            break;
        }
    }

    if (!bound) {
        setLastError(std::string("srt_bind failed: ") + srt_getlasterror_str());
        return false;
    }

    if (srt_listen(socket, std::max(1, c.listen_backlog)) == SRT_ERROR) {
        setLastError(std::string("srt_listen failed: ") + srt_getlasterror_str());
        return false;
    }

    setState(State::Listening);
    std::cerr << "[SRTInput] Listening on "
              << (c.bind_address.empty() ? "0.0.0.0" : c.bind_address)
              << ":" << c.port << "\n";

    sockaddr_storage remote_addr{};
    int remote_len = sizeof(remote_addr);

    while (!stop_requested_.load(std::memory_order_acquire)) {
        out_socket = srt_accept(socket,
                                reinterpret_cast<sockaddr*>(&remote_addr),
                                &remote_len);
        if (out_socket != SRT_INVALID_SOCK) {
            return true;
        }

        const std::string err = srt_getlasterror_str();
        if (isNoPendingAcceptError(err)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (stop_requested_.load(std::memory_order_acquire)) {
            return false;
        }

        setLastError(std::string("srt_accept failed: ") + err);
        return false;
    }

    return false;
}

bool SRTInput::initSocketAndConnect()
{
    closeSockets();

    const Config c = config_;

    SRTSOCKET base_socket = srt_create_socket();
    if (base_socket == SRT_INVALID_SOCK) {
        setLastError(std::string("srt_create_socket failed: ") + srt_getlasterror_str());
        return false;
    }

    if (!applyCommonOptions(base_socket, c)) {
        setLastError(std::string("Failed to apply SRT socket options: ") + srt_getlasterror_str());
        srt_close(base_socket);
        return false;
    }

    SRTSOCKET active_socket = base_socket;

    if (c.mode == Mode::Listener) {
        SRTSOCKET accepted = SRT_INVALID_SOCK;
        if (!resolveBindListenAccept(base_socket, c, accepted)) {
            srt_close(base_socket);
            return false;
        }

        if (!applyCommonOptions(accepted, c)) {
            setLastError(std::string("Failed to apply options to accepted SRT socket: ") +
                         srt_getlasterror_str());
            srt_close(accepted);
            srt_close(base_socket);
            return false;
        }

        active_socket = accepted;
    } else {
        setState(State::Connecting);
        if (!resolveAndConnect(base_socket, c)) {
            srt_close(base_socket);
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(socket_mutex_);
        if (c.mode == Mode::Listener) {
            listener_socket_ = base_socket;
            socket_ = active_socket;
        } else {
            listener_socket_ = SRT_INVALID_SOCK;
            socket_ = active_socket;
        }
    }

    setState(State::Connected);
    return true;
}

void SRTInput::receiveLoop()
{
    int attempt = 0;
    int backoff_ms = std::max(1, config_.reconnect_backoff_ms);
    bool logged_first_packet = false;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (!initSocketAndConnect()) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }

            ++attempt;
            const bool exhausted =
                !config_.reconnect_forever &&
                attempt >= std::max(1, config_.reconnect_attempts);

            if (exhausted) {
                setState(State::Failed);
                std::cerr << "[SRTInput] Connect attempts exhausted. last_error='"
                          << getLastError() << "'\n";
                break;
            }

            setState(State::Reconnecting);
            std::cerr << "[SRTInput] Reconnect attempt "
                      << attempt
                      << " failed. Waiting "
                      << backoff_ms
                      << " ms last_error='"
                      << getLastError()
                      << "'\n";

            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2,
                                  std::max(backoff_ms, config_.reconnect_backoff_max_ms));
            continue;
        }

        attempt = 0;
        backoff_ms = std::max(1, config_.reconnect_backoff_ms);

        std::vector<uint8_t> buffer(config_.max_packet_size > 0 ? config_.max_packet_size : 2048);
        std::vector<uint8_t> ts_pending;
        ts_pending.reserve(static_cast<size_t>(std::max(config_.payload_size, 2048)) * 2u);
        bool logged_realign = false;

        while (!stop_requested_.load(std::memory_order_acquire)) {
            SRTSOCKET active_socket = SRT_INVALID_SOCK;
            {
                std::lock_guard<std::mutex> lk(socket_mutex_);
                active_socket = socket_;
            }

            if (active_socket == SRT_INVALID_SOCK) {
                break;
            }

            const int received =
                srt_recvmsg(active_socket,
                            reinterpret_cast<char*>(buffer.data()),
                            static_cast<int>(buffer.size()));

            if (received > 0) {
                if (!logged_first_packet) {
                    logged_first_packet = true;
                    std::cerr << "[SRTInput] First transport packet received: "
                              << received << " bytes\n";
                }

                ts_pending.insert(ts_pending.end(),
                                  buffer.begin(),
                                  buffer.begin() + received);

                bool queued_any = false;
                for (;;) {
                    std::vector<uint8_t> aligned_payload;
                    uint64_t dropped_bytes = 0;
                    if (!extractAlignedTsPayload(ts_pending, aligned_payload, dropped_bytes)) {
                        if (dropped_bytes > 0) {
                            realigned_bytes_.fetch_add(dropped_bytes, std::memory_order_relaxed);
                            if (!logged_realign) {
                                logged_realign = true;
                                std::cerr << "[SRTInput] MPEG-TS receive alignment recovered; dropped_leading_bytes="
                                          << dropped_bytes
                                          << ". This is normal when joining a live SRT stream mid-packet.\n";
                            }
                        }
                        break;
                    }

                    if (dropped_bytes > 0) {
                        realigned_bytes_.fetch_add(dropped_bytes, std::memory_order_relaxed);
                        if (!logged_realign) {
                            logged_realign = true;
                            std::cerr << "[SRTInput] MPEG-TS receive alignment recovered; dropped_leading_bytes="
                                      << dropped_bytes
                                      << ". This is normal when joining a live SRT stream mid-packet.\n";
                        }
                    }

                    if (aligned_payload.empty()) {
                        continue;
                    }

                    Packet pkt;
                    pkt.data = std::move(aligned_payload);
                    pkt.receive_time_us = monotonicNowUs();

                    {
                        std::lock_guard<std::mutex> lk(queue_mutex_);
                        while (queue_.size() >= config_.max_queue_packets && !queue_.empty()) {
                            queue_.pop_front();
                            dropped_packets_.fetch_add(1, std::memory_order_relaxed);
                        }
                        queue_.push_back(std::move(pkt));
                    }

                    cv_.notify_one();
                    queued_any = true;
                    realigned_packets_.fetch_add(1, std::memory_order_relaxed);
                }

                received_packets_.fetch_add(1, std::memory_order_relaxed);
                received_bytes_.fetch_add(static_cast<uint64_t>(received),
                                          std::memory_order_relaxed);
                (void)queued_any;
                continue;
            }

            if (received == 0) {
                continue;
            }

            if (received == SRT_ERROR) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }

                const auto status = srt_getsockstate(active_socket);
                const std::string err = srt_getlasterror_str();

                if (status == SRTS_BROKEN || status == SRTS_CLOSED) {
                    setLastError(std::string("SRT connection broken: ") + err);
                    std::cerr << "[SRTInput] Connection broken: " << err << "\n";
                    closeSockets();
                    setState(State::Reconnecting);
                    break;
                }

                if (status == SRTS_NONEXIST || status == SRTS_CLOSING) {
                    closeSockets();
                    setState(State::Reconnecting);
                    break;
                }

                // SRT may report a receive timeout as e.g.
                // "Non-blocking call failure: transmission timed out" while the
                // connection is still valid. This is normal during sender startup,
                // reconnect/keyframe gating, or a brief live-source gap. Do not tear
                // down the caller socket for this; keep the connection open so the
                // sender can release the next clean TS session.
                if (isTransientReceiveNoDataError(err)) {
                    continue;
                }

                setLastError(std::string("srt_recvmsg failed: ") + err);
                std::cerr << "[SRTInput] Receive error: " << err << "\n";
                closeSockets();
                setState(State::Reconnecting);
                break;
            }
        }
    }

    closeSockets();
    cv_.notify_all();
    setState(State::Stopped);
}

bool SRTInput::popPacket(Packet& out, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(queue_mutex_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            return !queue_.empty() ||
                   !running_.load(std::memory_order_acquire) ||
                   stop_requested_.load(std::memory_order_acquire);
        })) {
        return false;
    }

    if (queue_.empty()) {
        return false;
    }

    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}