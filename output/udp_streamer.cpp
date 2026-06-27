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
 * UDP/RTP output transport implementation. UDPStreamer sends MPEG-TS payloads over UDP multicast/unicast or RTP/MP2T, applies optional pacing, and manages socket setup for live transport output.
 */

#include "output/udp_streamer.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

bool setSocketBuffer(int fd, int option, int value, const char* name)
{
    if (value <= 0) {
        return true;
    }
    if (::setsockopt(fd, SOL_SOCKET, option, &value, sizeof(value)) < 0) {
        std::cerr << "[UDPStreamer] WARNING: failed to set " << name
                  << "=" << value << " error=" << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

uint16_t randomU16()
{
    const uint64_t seed = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) ^
        static_cast<uint64_t>(::getpid());
    std::mt19937 rng(static_cast<uint32_t>(seed));
    return static_cast<uint16_t>(rng() & 0xFFFFu);
}

uint32_t randomU32()
{
    const uint64_t seed = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()) ^
        (static_cast<uint64_t>(::getpid()) << 32);
    std::mt19937 rng(static_cast<uint32_t>(seed ^ (seed >> 32)));
    return static_cast<uint32_t>(rng());
}

void writeBe16(uint8_t* dst, uint16_t value) noexcept
{
    dst[0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[1] = static_cast<uint8_t>(value & 0xFFu);
}

void writeBe32(uint8_t* dst, uint32_t value) noexcept
{
    dst[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[3] = static_cast<uint8_t>(value & 0xFFu);
}

} // namespace

UDPStreamer::~UDPStreamer()
{
    requestStop();
    closeSocket();
}

int UDPStreamer::normalizePayloadSize(int payloadSize) noexcept
{
    int normalized = std::max(188, payloadSize);
    normalized -= (normalized % 188);
    return std::max(188, normalized);
}

bool UDPStreamer::isIPv4Multicast(const std::string& address)
{
    in_addr ipv4{};
    if (::inet_pton(AF_INET, address.c_str(), &ipv4) != 1) {
        return false;
    }
    const uint32_t host = ntohl(ipv4.s_addr);
    return host >= 0xE0000000u && host <= 0xEFFFFFFFu;
}

const char* UDPStreamer::stateToString(State state) noexcept
{
    switch (state) {
        case State::Closed: return "CLOSED";
        case State::Open: return "OPEN";
        case State::Failed: return "FAILED";
        default: return "UNKNOWN";
    }
}

UDPStreamer::State UDPStreamer::getState() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

std::string UDPStreamer::getLastError() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return last_error_;
}

void UDPStreamer::setLastError(const std::string& error)
{
    std::lock_guard<std::mutex> lk(mutex_);
    last_error_ = error;
}

// Open and configure a UDP socket for unicast or multicast MPEG-TS output.
// RTP mode is still sent over this socket; it only changes packetization.
bool UDPStreamer::init(const Config& config)
{
    closeSocket();
    stop_requested_.store(false, std::memory_order_release);
    state_.store(State::Closed, std::memory_order_release);

    if (config.address.empty()) {
        setLastError("UDP destination address is empty");
        state_.store(State::Failed, std::memory_order_release);
        return false;
    }
    if (config.port <= 0 || config.port > 65535) {
        setLastError("UDP destination port must be between 1 and 65535");
        state_.store(State::Failed, std::memory_order_release);
        return false;
    }

    Config cfg = config;
    cfg.payload_size = normalizePayloadSize(cfg.payload_size);
    if (cfg.pacing_bitrate_bps < 0) {
        cfg.pacing_bitrate_bps = 0;
    }
    cfg.rtp_payload_type &= 0x7Fu;
    if (cfg.rtp_packetize) {
        if (cfg.rtp_ssrc == 0) {
            cfg.rtp_ssrc = randomU32();
        }
        if (cfg.payload_size + 12 > 1472) {
            std::cerr << "[UDPStreamer] WARNING: RTP datagram exceeds typical Ethernet MTU: "
                      << (cfg.payload_size + 12)
                      << " bytes. Consider payload_size=1316 or lower.\n";
        }
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    const std::string port_str = std::to_string(cfg.port);
    const int gai = ::getaddrinfo(cfg.address.c_str(), port_str.c_str(), &hints, &result);
    if (gai != 0 || !result) {
        setLastError(std::string("UDP getaddrinfo failed: ") + gai_strerror(gai));
        state_.store(State::Failed, std::memory_order_release);
        return false;
    }

    const int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(result);
        setLastError(std::string("UDP socket create failed: ") + std::strerror(errno));
        state_.store(State::Failed, std::memory_order_release);
        return false;
    }

    if (!cfg.bind_address.empty()) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(0);
        if (::inet_pton(AF_INET, cfg.bind_address.c_str(), &local.sin_addr) != 1) {
            ::freeaddrinfo(result);
            ::close(fd);
            setLastError("UDP invalid bind_address=" + cfg.bind_address);
            state_.store(State::Failed, std::memory_order_release);
            return false;
        }
        if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            ::freeaddrinfo(result);
            ::close(fd);
            setLastError(std::string("UDP bind failed: ") + std::strerror(errno));
            state_.store(State::Failed, std::memory_order_release);
            return false;
        }
    }

    setSocketBuffer(fd, SO_SNDBUF, cfg.sndbuf, "SO_SNDBUF");

    if (isIPv4Multicast(cfg.address)) {
        const int ttl = std::max(0, std::min(255, cfg.ttl));
        ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        const unsigned char loop = cfg.multicast_loop ? 1 : 0;
        if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
            std::cerr << "[UDPStreamer] WARNING: failed to set IP_MULTICAST_LOOP="
                      << static_cast<int>(loop) << " error=" << std::strerror(errno) << "\n";
        }
    }

    {
        std::lock_guard<std::mutex> lk(mutex_);
        socket_fd_ = fd;
        config_ = cfg;
        std::memset(&destination_, 0, sizeof(destination_));
        std::memcpy(&destination_, result->ai_addr, result->ai_addrlen);
        destination_len_ = static_cast<socklen_t>(result->ai_addrlen);
        last_error_.clear();
    }

    {
        std::lock_guard<std::mutex> lk(tx_mutex_);
        pending_ts_bytes_.clear();
        resetPacingClockLocked();
        rtp_epoch_ = std::chrono::steady_clock::now();
        rtp_sequence_ = randomU16();
        rtp_ssrc_ = cfg.rtp_ssrc;
    }

    ::freeaddrinfo(result);
    state_.store(State::Open, std::memory_order_release);

    std::cout << "[UDPStreamer] Output endpoint: "
              << (cfg.rtp_packetize ? "rtp://" : "udp://")
              << cfg.address << ":" << cfg.port
              << " payload=" << cfg.payload_size;
    if (cfg.rtp_packetize) {
        std::cout << " rtp_pt=" << static_cast<int>(cfg.rtp_payload_type)
                  << " rtp_ssrc=0x" << std::hex << cfg.rtp_ssrc << std::dec;
    }
    if (cfg.pacing_enabled && cfg.pacing_bitrate_bps > 0) {
        std::cout << " pacing=" << cfg.pacing_bitrate_bps << "bps";
    }
    if (isIPv4Multicast(cfg.address)) {
        std::cout << " multicast_ttl=" << cfg.ttl
                  << " multicast_loop=" << (cfg.multicast_loop ? "on" : "off");
    }
    std::cout << "\n";

    return true;
}

void UDPStreamer::resetPacingClockLocked()
{
    next_send_time_ = std::chrono::steady_clock::time_point{};
}

// Optional sender-side pacing. It smooths MPEG-TS bursts from the muxer so
// receiver and NIC buffers see a steadier live bitrate.
void UDPStreamer::paceDatagram(int size)
{
    int64_t bitrate = 0;
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        enabled = config_.pacing_enabled;
        bitrate = config_.pacing_bitrate_bps;
    }

    if (!enabled || bitrate <= 0 || size <= 0) {
        return;
    }

    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();

    clock::time_point send_at;
    {
        std::lock_guard<std::mutex> lk(tx_mutex_);
        if (next_send_time_ == clock::time_point{} || now > next_send_time_ + std::chrono::milliseconds(250)) {
            next_send_time_ = now;
        }

        send_at = next_send_time_;

        const long double seconds =
            (static_cast<long double>(size) * 8.0L) / static_cast<long double>(bitrate);
        const int64_t ns = static_cast<int64_t>(seconds * 1000000000.0L);
        next_send_time_ += std::chrono::nanoseconds(std::max<int64_t>(1, ns));
    }

    if (send_at > now) {
        std::this_thread::sleep_until(send_at);
    }
}

uint32_t UDPStreamer::currentRtpTimestamp90k() const
{
    using clock = std::chrono::steady_clock;
    const clock::time_point now = clock::now();

    std::lock_guard<std::mutex> lk(tx_mutex_);
    if (rtp_epoch_ == clock::time_point{}) {
        return 0;
    }

    const uint64_t us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - rtp_epoch_).count());
    return static_cast<uint32_t>((us * 90ull) / 1000ull);
}

// Packetize MPEG-TS payload bytes into RTP/MP2T. The input size excludes the
// 12-byte RTP header, which is generated here.
bool UDPStreamer::sendRtpDatagram(const unsigned char* data, int size)
{
    if (!data || size <= 0) {
        return true;
    }

    Config cfg;
    uint16_t seq = 0;
    uint32_t ssrc = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cfg = config_;
    }

    const int packetSize = size + 12;

    // Pace before stamping the RTP header so the 90 kHz RTP timestamp is close
    // to the actual socket send time rather than the time when the datagram was
    // queued for pacing. This keeps the RTP clock cleaner for external analyzers.
    paceDatagram(packetSize);

    if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(tx_mutex_);
        seq = rtp_sequence_++;
        ssrc = rtp_ssrc_;
    }

    std::vector<uint8_t> packet(static_cast<size_t>(packetSize));
    packet[0] = 0x80u; // RTP v2, no padding/extension/CSRC
    packet[1] = static_cast<uint8_t>(cfg.rtp_payload_type & 0x7Fu); // PT 33 = MPEG-TS
    writeBe16(packet.data() + 2, seq);
    writeBe32(packet.data() + 4, currentRtpTimestamp90k());
    writeBe32(packet.data() + 8, ssrc);
    std::memcpy(packet.data() + 12, data, static_cast<size_t>(size));

    return sendDatagramInternal(packet.data(), static_cast<int>(packet.size()), false);
}

bool UDPStreamer::sendDatagram(const unsigned char* data, int size)
{
    return sendDatagramInternal(data, size, true);
}

// Shared UDP send path. Plain UDP applies pacing here; RTP applies pacing once
// to the original MPEG-TS payload before creating the RTP header.
bool UDPStreamer::sendDatagramInternal(const unsigned char* data, int size, bool applyPacing)
{
    if (!data || size <= 0) {
        return true;
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
    }

    int fd = -1;
    sockaddr_storage dst{};
    socklen_t dst_len = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        fd = socket_fd_;
        dst = destination_;
        dst_len = destination_len_;
    }

    if (fd < 0 || dst_len == 0) {
        setLastError("UDP socket is not open");
        state_.store(State::Failed, std::memory_order_release);
        return false;
    }

    if (applyPacing) {
        paceDatagram(size);
    }

    if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
    }

    const ssize_t sent = ::sendto(fd,
                                  data,
                                  static_cast<size_t>(size),
                                  0,
                                  reinterpret_cast<const sockaddr*>(&dst),
                                  dst_len);
    if (sent != size) {
        if (!stop_requested_.load(std::memory_order_acquire)) {
            setLastError(std::string("UDP sendto failed: ") + std::strerror(errno));
            state_.store(State::Failed, std::memory_order_release);
        }
        return false;
    }

    bytes_sent_.fetch_add(static_cast<uint64_t>(size), std::memory_order_relaxed);
    datagrams_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool UDPStreamer::sendPacket(const unsigned char* data, int size)
{
    if (!data || size <= 0) {
        return true;
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
    }

    int payload = 1316;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        payload = normalizePayloadSize(config_.payload_size);
    }

    // Keep UDP datagrams aligned to complete 188-byte MPEG-TS packets. This is
    // friendlier to broadcast receivers/analyzers and avoids starting a receiver
    // on arbitrary byte offsets when joining a live UDP stream.
    std::vector<std::vector<uint8_t> > datagrams;
    {
        std::lock_guard<std::mutex> lk(tx_mutex_);
        pending_ts_bytes_.insert(pending_ts_bytes_.end(), data, data + size);

        while (pending_ts_bytes_.size() >= static_cast<size_t>(payload)) {
            datagrams.emplace_back(pending_ts_bytes_.begin(), pending_ts_bytes_.begin() + payload);
            pending_ts_bytes_.erase(pending_ts_bytes_.begin(), pending_ts_bytes_.begin() + payload);
        }

        const size_t ts_packet = 188u;
        const size_t whole_ts_bytes = (pending_ts_bytes_.size() / ts_packet) * ts_packet;
        if (whole_ts_bytes >= ts_packet && whole_ts_bytes + ts_packet > static_cast<size_t>(payload)) {
            datagrams.emplace_back(pending_ts_bytes_.begin(), pending_ts_bytes_.begin() + whole_ts_bytes);
            pending_ts_bytes_.erase(pending_ts_bytes_.begin(), pending_ts_bytes_.begin() + whole_ts_bytes);
        }
    }

    bool rtp = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        rtp = config_.rtp_packetize;
    }

    for (size_t i = 0; i < datagrams.size(); ++i) {
        const bool ok = rtp
            ? sendRtpDatagram(datagrams[i].data(), static_cast<int>(datagrams[i].size()))
            : sendDatagram(datagrams[i].data(), static_cast<int>(datagrams[i].size()));
        if (!ok) {
            return false;
        }
    }

    return true;
}

void UDPStreamer::requestStop()
{
    stop_requested_.store(true, std::memory_order_release);
}

void UDPStreamer::closeSocket()
{
    {
        std::lock_guard<std::mutex> tx_lk(tx_mutex_);
        pending_ts_bytes_.clear();
        resetPacingClockLocked();
    }

    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        fd = socket_fd_;
        socket_fd_ = -1;
        destination_len_ = 0;
    }
    if (fd >= 0) {
        ::close(fd);
    }
    state_.store(State::Closed, std::memory_order_release);
}
