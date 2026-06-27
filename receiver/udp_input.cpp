/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/udp_input.cpp
 * Description: Implements UDP/RTP receive transport ingest.
 */

#include "receiver/udp_input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

bool isIPv4Multicast(const std::string& addr)
{
    in_addr ipv4{};
    if (inet_pton(AF_INET, addr.c_str(), &ipv4) != 1) {
        return false;
    }

    const uint32_t host = ntohl(ipv4.s_addr);
    return host >= 0xE0000000u && host <= 0xEFFFFFFFu;
}

std::string sockaddrToString(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

    std::ostringstream oss;
    oss << ip;
    return oss.str();
}

std::string ipv4ToString(uint32_t ipv4NetworkOrder)
{
    in_addr addr{};
    addr.s_addr = ipv4NetworkOrder;
    char ip[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &addr, ip, sizeof(ip)) != nullptr) {
        return std::string(ip);
    }
    return std::string("0.0.0.0");
}

int64_t monotonicNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int querySocketReceiveBuffer(int sockfd) noexcept
{
    int actual = 0;
    socklen_t len = sizeof(actual);
    if (getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &actual, &len) == 0) {
        return actual;
    }
    return 0;
}

void configureSocketReceiveBuffer(int sockfd, int requestedBytes)
{
    if (requestedBytes <= 0) {
        return;
    }

    bool forceAttempted = false;
    bool forceSucceeded = false;
    int forceErrno = 0;

#ifdef SO_RCVBUFFORCE
    forceAttempted = true;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUFFORCE, &requestedBytes, sizeof(requestedBytes)) == 0) {
        forceSucceeded = true;
    } else {
        forceErrno = errno;
    }
#endif

    if (!forceSucceeded) {
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &requestedBytes, sizeof(requestedBytes)) < 0) {
            std::cerr << "[UDPInput] WARN: SO_RCVBUF request failed requested="
                      << requestedBytes << " error=" << std::strerror(errno) << "\n";
        }
    }

    const int actual = querySocketReceiveBuffer(sockfd);

    std::cerr << "[UDPInput] UDP receive buffer requested=" << requestedBytes
              << " actual=" << actual;
#ifdef SO_RCVBUFFORCE
    std::cerr << " force_attempted=" << (forceAttempted ? "yes" : "no")
              << " force=" << (forceSucceeded ? "ok" : "fallback");
    if (forceAttempted && !forceSucceeded) {
        std::cerr << " force_error=" << std::strerror(forceErrno);
    }
#else
    std::cerr << " force=unsupported";
#endif
    std::cerr << "\n";

    // Linux reports roughly double the user value for socket buffers because it
    // accounts for kernel overhead. If the value is still below the requested
    // size, the process is capped by net.core.rmem_max or lacks CAP_NET_ADMIN.
    if (actual > 0 && actual < requestedBytes) {
        std::cerr << "[UDPInput] WARN: UDP receive buffer capped by OS. "
                  << "actual=" << actual << " requested=" << requestedBytes
                  << " recommendation='raise net.core.rmem_max/rmem_default or run with CAP_NET_ADMIN'\n";
    }
}

uint16_t readBe16(const uint8_t* p) noexcept
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t readBe32(const uint8_t* p) noexcept
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

struct RtpPacketInfo
{
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    uint8_t payload_type = 0;
    bool marker = false;
};

enum class RtpParseError
{
    None,
    Malformed,
    PayloadTypeMismatch
};

struct TsContinuityTracker
{
    std::array<uint8_t, 8192> last_cc{};
    std::array<bool, 8192> have{};
};

uint64_t inspectTsContinuity(const uint8_t* data, size_t size, TsContinuityTracker& tracker)
{
    static const size_t kTsPacketSize = 188u;
    if (!data || size < kTsPacketSize) {
        return 0;
    }

    uint64_t errors = 0;
    const size_t packets = size / kTsPacketSize;
    for (size_t i = 0; i < packets; ++i) {
        const uint8_t* p = data + i * kTsPacketSize;
        if (p[0] != 0x47u) {
            ++errors;
            continue;
        }

        const uint16_t pid = static_cast<uint16_t>(((p[1] & 0x1Fu) << 8) | p[2]);
        if (pid == 0x1FFFu) {
            continue; // null packets are filler and are not useful for diagnostics here
        }

        const uint8_t adaptationControl = static_cast<uint8_t>((p[3] >> 4) & 0x03u);
        const uint8_t cc = static_cast<uint8_t>(p[3] & 0x0Fu);
        const bool hasAdaptation = adaptationControl == 2u || adaptationControl == 3u;
        const bool hasPayload = adaptationControl == 1u || adaptationControl == 3u;
        bool discontinuity = false;

        if (hasAdaptation && p[4] > 0u && 5u < kTsPacketSize) {
            const uint8_t flags = p[5];
            discontinuity = (flags & 0x80u) != 0u;
        }

        if (!hasPayload) {
            continue;
        }

        if (discontinuity || !tracker.have[pid]) {
            tracker.have[pid] = true;
            tracker.last_cc[pid] = cc;
            continue;
        }

        const uint8_t expected = static_cast<uint8_t>((tracker.last_cc[pid] + 1u) & 0x0Fu);
        if (cc != expected) {
            ++errors;
        }
        tracker.last_cc[pid] = cc;
    }

    return errors;
}

bool depacketizeRtpMpegTs(const uint8_t* data,
                          int size,
                          uint8_t expectedPayloadType,
                          bool strictPayloadType,
                          std::vector<uint8_t>& out,
                          RtpPacketInfo& info,
                          RtpParseError& error)
{
    error = RtpParseError::None;
    info = RtpPacketInfo{};
    out.clear();
    if (!data || size < 12) {
        error = RtpParseError::Malformed;
        return false;
    }

    const uint8_t vpxcc = data[0];
    const uint8_t version = static_cast<uint8_t>((vpxcc >> 6) & 0x03u);
    if (version != 2) {
        error = RtpParseError::Malformed;
        return false;
    }

    const uint8_t payloadType = static_cast<uint8_t>(data[1] & 0x7Fu);
    info.payload_type = payloadType;
    info.marker = (data[1] & 0x80u) != 0u;
    info.sequence = readBe16(data + 2);
    info.timestamp = readBe32(data + 4);
    info.ssrc = readBe32(data + 8);

    const bool hasPadding = (vpxcc & 0x20u) != 0;
    const bool hasExtension = (vpxcc & 0x10u) != 0;
    const uint8_t csrcCount = static_cast<uint8_t>(vpxcc & 0x0Fu);
    if (strictPayloadType && payloadType != (expectedPayloadType & 0x7Fu)) {
        error = RtpParseError::PayloadTypeMismatch;
        return false;
    }

    size_t header = 12u + static_cast<size_t>(csrcCount) * 4u;
    if (header > static_cast<size_t>(size)) {
        error = RtpParseError::Malformed;
        return false;
    }

    if (hasExtension) {
        if (header + 4u > static_cast<size_t>(size)) {
            error = RtpParseError::Malformed;
            return false;
        }
        const uint16_t extWords = readBe16(data + header + 2u);
        header += 4u + static_cast<size_t>(extWords) * 4u;
        if (header > static_cast<size_t>(size)) {
            error = RtpParseError::Malformed;
            return false;
        }
    }

    size_t payloadBytes = static_cast<size_t>(size) - header;
    if (hasPadding) {
        const uint8_t pad = data[size - 1];
        if (pad == 0 || static_cast<size_t>(pad) > payloadBytes) {
            error = RtpParseError::Malformed;
            return false;
        }
        payloadBytes -= pad;
    }

    if (payloadBytes < 188u || (payloadBytes % 188u) != 0u) {
        error = RtpParseError::Malformed;
        return false;
    }

    out.assign(data + header, data + header + payloadBytes);
    return true;
}

bool sanitizeMpegTsPayload(const uint8_t* data, int size, std::vector<uint8_t>& out)
{
    static const size_t kTsPacketSize = 188u;
    static const uint8_t kSync = 0x47u;

    out.clear();
    if (!data || size < static_cast<int>(kTsPacketSize)) {
        return false;
    }

    const size_t n = static_cast<size_t>(size);
    size_t best_offset = n;
    size_t best_packets = 0;

    const size_t max_probe = std::min(kTsPacketSize, n);
    for (size_t offset = 0; offset < max_probe; ++offset) {
        if (data[offset] != kSync) {
            continue;
        }

        size_t packets = 0;
        for (size_t pos = offset; pos + kTsPacketSize <= n; pos += kTsPacketSize) {
            if (data[pos] != kSync) {
                break;
            }
            ++packets;
        }

        if (packets > best_packets) {
            best_packets = packets;
            best_offset = offset;
        }
    }

    if (best_packets == 0 || best_offset >= n) {
        return false;
    }

    const size_t bytes = best_packets * kTsPacketSize;
    out.assign(data + best_offset, data + best_offset + bytes);
    return true;
}

} // namespace

UDPInput::UDPInput() = default;

UDPInput::~UDPInput()
{
    stop();
}

const char* UDPInput::stateToString(State state) noexcept
{
    switch (state) {
        case State::Stopped: return "STOPPED";
        case State::Binding: return "BINDING";
        case State::Listening: return "LISTENING";
        case State::Reconnecting: return "RECONNECTING";
        case State::Failed: return "FAILED";
        case State::Closing: return "CLOSING";
        default: return "UNKNOWN";
    }
}

void UDPInput::setState(State state) noexcept
{
    state_.store(state, std::memory_order_release);
}

UDPInput::State UDPInput::getState() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

std::string UDPInput::getLastError() const
{
    std::lock_guard<std::mutex> lk(state_mutex_);
    return last_error_;
}

void UDPInput::setLastError(const std::string& error)
{
    std::lock_guard<std::mutex> lk(state_mutex_);
    last_error_ = error;
}

uint64_t UDPInput::receivedPackets() const noexcept
{
    return received_packets_.load(std::memory_order_relaxed);
}

uint64_t UDPInput::receivedBytes() const noexcept
{
    return received_bytes_.load(std::memory_order_relaxed);
}

uint64_t UDPInput::droppedPackets() const noexcept
{
    return dropped_packets_.load(std::memory_order_relaxed);
}

UDPInput::Diagnostics UDPInput::diagnostics() const noexcept
{
    Diagnostics d;
    d.received_packets = received_packets_.load(std::memory_order_relaxed);
    d.received_bytes = received_bytes_.load(std::memory_order_relaxed);
    d.dropped_packets = dropped_packets_.load(std::memory_order_relaxed);
    d.rtp_packets = rtp_packets_.load(std::memory_order_relaxed);
    d.rtp_malformed = rtp_malformed_.load(std::memory_order_relaxed);
    d.rtp_payload_type_mismatch = rtp_payload_type_mismatch_.load(std::memory_order_relaxed);
    d.rtp_sequence_gaps = rtp_sequence_gaps_.load(std::memory_order_relaxed);
    d.rtp_out_of_order = rtp_out_of_order_.load(std::memory_order_relaxed);
    d.rtp_duplicates = rtp_duplicates_.load(std::memory_order_relaxed);
    d.rtp_source_changes = rtp_source_changes_.load(std::memory_order_relaxed);
    d.rtp_source_rejected = rtp_source_rejected_.load(std::memory_order_relaxed);
    d.last_rtp_sequence = last_rtp_sequence_.load(std::memory_order_relaxed);
    d.last_rtp_timestamp = last_rtp_timestamp_.load(std::memory_order_relaxed);
    d.last_rtp_ssrc = last_rtp_ssrc_.load(std::memory_order_relaxed);
    d.locked_rtp_source_ipv4 = locked_rtp_source_ipv4_.load(std::memory_order_relaxed);
    d.locked_rtp_source_port = locked_rtp_source_port_.load(std::memory_order_relaxed);
    d.have_rtp_sequence = have_rtp_sequence_.load(std::memory_order_relaxed);
    d.have_rtp_source = have_rtp_source_.load(std::memory_order_relaxed);
    d.ts_sync_errors = ts_sync_errors_.load(std::memory_order_relaxed);
    d.ts_continuity_errors = ts_continuity_errors_.load(std::memory_order_relaxed);
    d.actual_rcvbuf = actual_rcvbuf_.load(std::memory_order_relaxed);
    d.last_rtp_expected_sequence = last_rtp_expected_sequence_.load(std::memory_order_relaxed);
    d.last_rtp_gap_observed_sequence = last_rtp_gap_observed_sequence_.load(std::memory_order_relaxed);
    d.last_rtp_gap_missing = last_rtp_gap_missing_.load(std::memory_order_relaxed);
    return d;
}

bool UDPInput::start(const Config& config)
{
    stop();

    config_ = config;
    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    received_packets_.store(0, std::memory_order_release);
    received_bytes_.store(0, std::memory_order_release);
    dropped_packets_.store(0, std::memory_order_release);
    rtp_packets_.store(0, std::memory_order_release);
    rtp_malformed_.store(0, std::memory_order_release);
    rtp_payload_type_mismatch_.store(0, std::memory_order_release);
    rtp_sequence_gaps_.store(0, std::memory_order_release);
    rtp_out_of_order_.store(0, std::memory_order_release);
    rtp_duplicates_.store(0, std::memory_order_release);
    rtp_source_changes_.store(0, std::memory_order_release);
    rtp_source_rejected_.store(0, std::memory_order_release);
    ts_sync_errors_.store(0, std::memory_order_release);
    ts_continuity_errors_.store(0, std::memory_order_release);
    actual_rcvbuf_.store(0, std::memory_order_release);
    last_rtp_expected_sequence_.store(0, std::memory_order_release);
    last_rtp_gap_observed_sequence_.store(0, std::memory_order_release);
    last_rtp_gap_missing_.store(0, std::memory_order_release);
    last_rtp_sequence_.store(0, std::memory_order_release);
    last_rtp_timestamp_.store(0, std::memory_order_release);
    last_rtp_ssrc_.store(0, std::memory_order_release);
    locked_rtp_source_ipv4_.store(0, std::memory_order_release);
    locked_rtp_source_port_.store(0, std::memory_order_release);
    have_rtp_sequence_.store(false, std::memory_order_release);
    have_rtp_source_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        last_error_.clear();
    }

    setState(State::Binding);

    std::cerr << "[UDPInput] Starting bind="
              << (config_.bind_address.empty() ? "0.0.0.0" : config_.bind_address)
              << ":" << config_.port
              << " packet_size=" << config_.max_packet_size;
    if (!config_.multicast_group.empty()) {
        std::cerr << " multicast_group=" << config_.multicast_group;
    }
    if (config_.rtp_depacketize) {
        std::cerr << " rtp=yes payload_type=" << static_cast<int>(config_.rtp_payload_type);
    }
    if (config_.rcvbuf > 0) {
        std::cerr << " rcvbuf_req=" << config_.rcvbuf;
    }
    std::cerr << "\n";

    recv_thread_ = std::thread(&UDPInput::receiveLoop, this);
    return true;
}

void UDPInput::stop()
{
    stop_requested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    setState(State::Closing);

    closeSocket();
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

void UDPInput::closeSocket()
{
    std::lock_guard<std::mutex> lk(socket_mutex_);
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

bool UDPInput::shouldJoinMulticast() const
{
    if (!config_.multicast_group.empty()) {
        return true;
    }

    return config_.auto_join_multicast && isIPv4Multicast(config_.bind_address);
}

bool UDPInput::joinMulticastGroup(int sockfd)
{
    std::string group =
        config_.multicast_group.empty() ? config_.bind_address : config_.multicast_group;
    if (group.empty()) {
        return true;
    }

    ip_mreq mreq{};
    if (inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) {
        setLastError("UDPInput invalid multicast_group=" + group);
        return false;
    }

    const std::string iface =
        config_.multicast_interface.empty() ? "0.0.0.0" : config_.multicast_interface;

    if (inet_pton(AF_INET, iface.c_str(), &mreq.imr_interface) != 1) {
        setLastError("UDPInput invalid multicast_interface=" + iface);
        return false;
    }

    if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        setLastError(std::string("UDPInput failed to join multicast group: ") +
                     std::strerror(errno));
        return false;
    }

    std::cerr << "[UDPInput] Joined multicast group="
              << group
              << " interface="
              << iface
              << "\n";

    return true;
}

bool UDPInput::openSocket()
{
    closeSocket();

    const int sockfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        setLastError(std::string("UDPInput socket create failed: ") + std::strerror(errno));
        return false;
    }

    if (config_.reuse_address) {
        int one = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    }

    configureSocketReceiveBuffer(sockfd, config_.rcvbuf);
    actual_rcvbuf_.store(querySocketReceiveBuffer(sockfd), std::memory_order_release);

    timeval tv{};
    tv.tv_sec = std::max(0, config_.recv_timeout_ms) / 1000;
    tv.tv_usec = (std::max(0, config_.recv_timeout_ms) % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<uint16_t>(config_.port));

    const std::string bind_addr =
        config_.bind_address.empty() ? "0.0.0.0" : config_.bind_address;

    if (inet_pton(AF_INET, bind_addr.c_str(), &local.sin_addr) != 1) {
        ::close(sockfd);
        setLastError("UDPInput invalid bind_address=" + bind_addr);
        return false;
    }

    if (::bind(sockfd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        ::close(sockfd);
        setLastError(std::string("UDPInput bind failed: ") + std::strerror(errno));
        return false;
    }

    if (shouldJoinMulticast() && !joinMulticastGroup(sockfd)) {
        ::close(sockfd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(socket_mutex_);
        socket_fd_ = sockfd;
    }

    setState(State::Listening);

    std::cerr << "[UDPInput] Listening on "
              << bind_addr
              << ":" << config_.port
              << " packet_size=" << config_.max_packet_size;
    if (!config_.multicast_group.empty()) {
        std::cerr << " multicast_group=" << config_.multicast_group;
    }
    if (config_.rtp_depacketize) {
        std::cerr << " rtp=yes payload_type=" << static_cast<int>(config_.rtp_payload_type);
    }
    std::cerr << " rcvbuf_req=" << config_.rcvbuf
              << " rcvbuf_actual=" << actual_rcvbuf_.load(std::memory_order_acquire)
              << "\n";

    return true;
}

void UDPInput::receiveLoop()
{
    int attempt = 0;
    int backoff_ms = std::max(1, config_.reconnect_backoff_ms);
    bool logged_first_packet = false;
    TsContinuityTracker ts_tracker;
    auto last_diag_log = std::chrono::steady_clock::now();

    bool have_locked_rtp_source = false;
    uint32_t locked_rtp_source_ipv4 = 0;
    uint16_t locked_rtp_source_port = 0;
    uint32_t locked_rtp_ssrc = 0;
    auto last_rtp_source_accept = std::chrono::steady_clock::now();
    auto last_rtp_source_reject_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto last_rtp_gap_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto last_ts_cc_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    auto resetRtpSequenceTracking = [&]() {
        have_rtp_sequence_.store(false, std::memory_order_relaxed);
        last_rtp_sequence_.store(0, std::memory_order_relaxed);
        last_rtp_timestamp_.store(0, std::memory_order_relaxed);
        last_rtp_expected_sequence_.store(0, std::memory_order_relaxed);
        last_rtp_gap_observed_sequence_.store(0, std::memory_order_relaxed);
        last_rtp_gap_missing_.store(0, std::memory_order_relaxed);
    };

    std::vector<uint8_t> buffer(
        config_.max_packet_size > 0 ? config_.max_packet_size : static_cast<size_t>(2048));
    std::vector<uint8_t> filtered_payload;
    std::vector<uint8_t> rtp_payload;

    auto logDiagnosticsIfDue = [&]() {
        const auto now = std::chrono::steady_clock::now();
        if ((now - last_diag_log) < std::chrono::seconds(5)) {
            return;
        }
        last_diag_log = now;

        const Diagnostics d = diagnostics();
        std::cerr << "[UDPInput] diag packets=" << d.received_packets
                  << " bytes=" << d.received_bytes
                  << " dropped=" << d.dropped_packets
                  << " ts_sync_err=" << d.ts_sync_errors
                  << " ts_cc_err=" << d.ts_continuity_errors
                  << " rcvbuf=" << d.actual_rcvbuf;
        if (config_.rtp_depacketize) {
            std::cerr << " rtp_pkts=" << d.rtp_packets
                      << " rtp_bad=" << d.rtp_malformed
                      << " rtp_pt_mismatch=" << d.rtp_payload_type_mismatch
                      << " rtp_seq_gap=" << d.rtp_sequence_gaps
                      << " rtp_ooo=" << d.rtp_out_of_order
                      << " rtp_dup=" << d.rtp_duplicates
                      << " rtp_src_change=" << d.rtp_source_changes
                      << " rtp_src_reject=" << d.rtp_source_rejected;
            if (d.have_rtp_sequence) {
                std::cerr << " rtp_last_seq=" << d.last_rtp_sequence
                          << " rtp_ts=" << d.last_rtp_timestamp
                          << " rtp_ssrc=0x" << std::hex << d.last_rtp_ssrc << std::dec;
            }
            if (d.have_rtp_source) {
                std::cerr << " rtp_src=" << ipv4ToString(d.locked_rtp_source_ipv4)
                          << ":" << d.locked_rtp_source_port;
            }
        }
        std::cerr << "\n";
    };

    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (!openSocket()) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                break;
            }

            ++attempt;
            const bool exhausted =
                !config_.reconnect_forever &&
                attempt >= std::max(1, config_.reconnect_attempts);

            if (exhausted) {
                setState(State::Failed);
                std::cerr << "[UDPInput] Bind attempts exhausted. last_error='"
                          << getLastError() << "'\n";
                break;
            }

            setState(State::Reconnecting);
            std::cerr << "[UDPInput] Bind attempt "
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

        while (!stop_requested_.load(std::memory_order_acquire)) {
            int sockfd = -1;
            {
                std::lock_guard<std::mutex> lk(socket_mutex_);
                sockfd = socket_fd_;
            }

            if (sockfd < 0) {
                break;
            }

            sockaddr_in src{};
            socklen_t src_len = sizeof(src);

            const int received = ::recvfrom(sockfd,
                                            buffer.data(),
                                            buffer.size(),
                                            0,
                                            reinterpret_cast<sockaddr*>(&src),
                                            &src_len);

            if (received > 0) {
                const uint8_t* payload_data = buffer.data();
                int payload_size = received;
                bool from_rtp = false;

                RtpPacketInfo rtp_info;
                if (config_.rtp_depacketize) {
                    RtpParseError rtp_error = RtpParseError::None;
                    if (!depacketizeRtpMpegTs(buffer.data(),
                                             received,
                                             config_.rtp_payload_type,
                                             config_.rtp_strict_payload_type,
                                             rtp_payload,
                                             rtp_info,
                                             rtp_error)) {
                        if (rtp_error == RtpParseError::PayloadTypeMismatch) {
                            rtp_payload_type_mismatch_.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            rtp_malformed_.fetch_add(1, std::memory_order_relaxed);
                        }
                        dropped_packets_.fetch_add(1, std::memory_order_relaxed);
                        logDiagnosticsIfDue();
                        continue;
                    }

                    const uint32_t src_ipv4 = src.sin_addr.s_addr;
                    const uint16_t src_port = ntohs(src.sin_port);
                    const auto now_for_source = std::chrono::steady_clock::now();
                    const int idle_ms = have_locked_rtp_source
                        ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              now_for_source - last_rtp_source_accept).count())
                        : 0;
                    const int source_loss_ms = std::max(100, config_.peer_idle_timeout_ms);

                    const bool same_source =
                        have_locked_rtp_source &&
                        locked_rtp_source_ipv4 == src_ipv4 &&
                        locked_rtp_source_port == src_port &&
                        locked_rtp_ssrc == rtp_info.ssrc;

                    if (!have_locked_rtp_source) {
                        have_locked_rtp_source = true;
                        locked_rtp_source_ipv4 = src_ipv4;
                        locked_rtp_source_port = src_port;
                        locked_rtp_ssrc = rtp_info.ssrc;
                        last_rtp_source_accept = now_for_source;

                        locked_rtp_source_ipv4_.store(locked_rtp_source_ipv4, std::memory_order_relaxed);
                        locked_rtp_source_port_.store(locked_rtp_source_port, std::memory_order_relaxed);
                        last_rtp_ssrc_.store(locked_rtp_ssrc, std::memory_order_relaxed);
                        have_rtp_source_.store(true, std::memory_order_relaxed);

                        std::cerr << "[UDPInput] RTP source locked: "
                                  << ipv4ToString(locked_rtp_source_ipv4)
                                  << ":" << locked_rtp_source_port
                                  << " ssrc=0x" << std::hex << locked_rtp_ssrc << std::dec
                                  << "\n";
                    } else if (!same_source) {
                        if (idle_ms >= source_loss_ms) {
                            const uint32_t old_ip = locked_rtp_source_ipv4;
                            const uint16_t old_port = locked_rtp_source_port;
                            const uint32_t old_ssrc = locked_rtp_ssrc;

                            have_locked_rtp_source = true;
                            locked_rtp_source_ipv4 = src_ipv4;
                            locked_rtp_source_port = src_port;
                            locked_rtp_ssrc = rtp_info.ssrc;
                            last_rtp_source_accept = now_for_source;

                            locked_rtp_source_ipv4_.store(locked_rtp_source_ipv4, std::memory_order_relaxed);
                            locked_rtp_source_port_.store(locked_rtp_source_port, std::memory_order_relaxed);
                            last_rtp_ssrc_.store(locked_rtp_ssrc, std::memory_order_relaxed);
                            have_rtp_source_.store(true, std::memory_order_relaxed);
                            rtp_source_changes_.fetch_add(1, std::memory_order_relaxed);
                            resetRtpSequenceTracking();
                            ts_tracker = TsContinuityTracker{};

                            std::cerr << "[UDPInput] RTP source changed after idle: old="
                                      << ipv4ToString(old_ip) << ":" << old_port
                                      << " ssrc=0x" << std::hex << old_ssrc << std::dec
                                      << " new=" << ipv4ToString(locked_rtp_source_ipv4)
                                      << ":" << locked_rtp_source_port
                                      << " ssrc=0x" << std::hex << locked_rtp_ssrc << std::dec
                                      << " idle_ms=" << idle_ms
                                      << "\n";
                        } else {
                            rtp_source_rejected_.fetch_add(1, std::memory_order_relaxed);
                            dropped_packets_.fetch_add(1, std::memory_order_relaxed);

                            if ((now_for_source - last_rtp_source_reject_log) >=
                                std::chrono::seconds(5)) {
                                last_rtp_source_reject_log = now_for_source;
                                std::cerr << "[UDPInput] RTP packet rejected from non-active source: active="
                                          << ipv4ToString(locked_rtp_source_ipv4)
                                          << ":" << locked_rtp_source_port
                                          << " ssrc=0x" << std::hex << locked_rtp_ssrc
                                          << " rejected=" << ipv4ToString(src_ipv4)
                                          << ":" << std::dec << src_port
                                          << " ssrc=0x" << std::hex << rtp_info.ssrc << std::dec
                                          << " idle_ms=" << idle_ms
                                          << "\n";
                            }

                            logDiagnosticsIfDue();
                            continue;
                        }
                    } else {
                        last_rtp_source_accept = now_for_source;
                    }

                    bool updateRtpSequence = true;
                    if (have_rtp_sequence_.load(std::memory_order_relaxed)) {
                        const uint16_t last = last_rtp_sequence_.load(std::memory_order_relaxed);
                        const uint16_t expected = static_cast<uint16_t>(last + 1u);
                        if (rtp_info.sequence == last) {
                            rtp_duplicates_.fetch_add(1, std::memory_order_relaxed);
                            updateRtpSequence = false;
                        } else if (rtp_info.sequence != expected) {
                            const uint16_t forward =
                                static_cast<uint16_t>(rtp_info.sequence - expected);
                            if (forward < 0x8000u) {
                                const uint64_t missing = static_cast<uint64_t>(forward);
                                const uint64_t total = rtp_sequence_gaps_.fetch_add(missing,
                                                             std::memory_order_relaxed) + missing;
                                last_rtp_expected_sequence_.store(expected, std::memory_order_relaxed);
                                last_rtp_gap_observed_sequence_.store(rtp_info.sequence, std::memory_order_relaxed);
                                last_rtp_gap_missing_.store(missing, std::memory_order_relaxed);
                                const auto now_for_gap = std::chrono::steady_clock::now();
                                if ((now_for_gap - last_rtp_gap_log) >= std::chrono::seconds(1)) {
                                    last_rtp_gap_log = now_for_gap;
                                    std::cerr << "[UDPInput] RTP sequence gap: expected="
                                              << expected
                                              << " got=" << rtp_info.sequence
                                              << " missing=" << missing
                                              << " total_missing=" << total
                                              << " src=" << ipv4ToString(locked_rtp_source_ipv4)
                                              << ":" << locked_rtp_source_port
                                              << " ssrc=0x" << std::hex << locked_rtp_ssrc << std::dec
                                              << "\n";
                                }
                            } else {
                                rtp_out_of_order_.fetch_add(1, std::memory_order_relaxed);
                                updateRtpSequence = false;
                            }
                        }
                    }

                    if (updateRtpSequence) {
                        have_rtp_sequence_.store(true, std::memory_order_relaxed);
                        last_rtp_sequence_.store(rtp_info.sequence, std::memory_order_relaxed);
                        last_rtp_timestamp_.store(rtp_info.timestamp, std::memory_order_relaxed);
                        last_rtp_ssrc_.store(rtp_info.ssrc, std::memory_order_relaxed);
                    }
                    rtp_packets_.fetch_add(1, std::memory_order_relaxed);

                    payload_data = rtp_payload.data();
                    payload_size = static_cast<int>(rtp_payload.size());
                    from_rtp = true;
                }

                if (config_.mpegts_sync_filter) {
                    if (!sanitizeMpegTsPayload(payload_data, payload_size, filtered_payload)) {
                        ts_sync_errors_.fetch_add(1, std::memory_order_relaxed);
                        dropped_packets_.fetch_add(1, std::memory_order_relaxed);
                        logDiagnosticsIfDue();
                        continue;
                    }
                    payload_data = filtered_payload.data();
                    payload_size = static_cast<int>(filtered_payload.size());
                }

                const uint64_t ts_cc_errors =
                    inspectTsContinuity(payload_data, static_cast<size_t>(payload_size), ts_tracker);
                if (ts_cc_errors > 0) {
                    const uint64_t total_cc = ts_continuity_errors_.fetch_add(ts_cc_errors, std::memory_order_relaxed) + ts_cc_errors;
                    const auto now_for_cc = std::chrono::steady_clock::now();
                    if ((now_for_cc - last_ts_cc_log) >= std::chrono::seconds(1)) {
                        last_ts_cc_log = now_for_cc;
                        std::cerr << "[UDPInput] MPEG-TS continuity error after UDP/RTP receive:"
                                  << " cc_err+=" << ts_cc_errors
                                  << " total_cc=" << total_cc;
                        if (config_.rtp_depacketize) {
                            std::cerr << " rtp_gap_total=" << rtp_sequence_gaps_.load(std::memory_order_relaxed)
                                      << " last_expected=" << last_rtp_expected_sequence_.load(std::memory_order_relaxed)
                                      << " last_observed=" << last_rtp_gap_observed_sequence_.load(std::memory_order_relaxed)
                                      << " last_missing=" << last_rtp_gap_missing_.load(std::memory_order_relaxed);
                        }
                        std::cerr << "\n";
                    }
                }

                if (!logged_first_packet) {
                    logged_first_packet = true;
                    std::cerr << "[UDPInput] First transport packet received: "
                              << payload_size << " bytes from "
                              << sockaddrToString(src) << ":" << ntohs(src.sin_port)
                              << (from_rtp ? " rtp=yes" : "")
                              << (config_.mpegts_sync_filter ? " ts_aligned=yes" : "")
                              << "\n";
                }

                Packet pkt;
                pkt.data.assign(payload_data, payload_data + payload_size);
                pkt.receive_time_us = monotonicNowUs();
                pkt.source_address = sockaddrToString(src);
                pkt.source_port = ntohs(src.sin_port);

                {
                    std::lock_guard<std::mutex> lk(queue_mutex_);
                    while (queue_.size() >= config_.max_queue_packets && !queue_.empty()) {
                        queue_.pop_front();
                        dropped_packets_.fetch_add(1, std::memory_order_relaxed);
                    }
                    queue_.push_back(std::move(pkt));
                }

                cv_.notify_one();
                received_packets_.fetch_add(1, std::memory_order_relaxed);
                received_bytes_.fetch_add(static_cast<uint64_t>(payload_size),
                                          std::memory_order_relaxed);
                logDiagnosticsIfDue();
                continue;
            }

            if (received == 0) {
                continue;
            }

            if (received < 0) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }

                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }

                setLastError(std::string("UDPInput recvfrom failed: ") + std::strerror(errno));
                std::cerr << "[UDPInput] Receive error: " << getLastError() << "\n";
                closeSocket();
                setState(State::Reconnecting);
                break;
            }
        }
    }

    closeSocket();
    cv_.notify_all();
    setState(State::Stopped);
}

bool UDPInput::popPacket(Packet& out, int timeout_ms)
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