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
 * UDP/RTP output transport declarations. UDPStreamer owns UDP socket state, multicast options, RTP packetization settings, pacing configuration, and datagram send lifecycle.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <sys/socket.h>

// UDP/RTP sender for MPEG-TS chunks. Supports unicast, multicast, optional
// RTP/MP2T packetization, and pacing to reduce live output burstiness.
class UDPStreamer
{
public:
    enum class State
    {
        Closed,
        Open,
        Failed
    };

    // UDP/RTP runtime configuration loaded from presets and CLI options.
    struct Config
    {
        std::string address;
        int port = 0;
        std::string bind_address;
        int payload_size = 1316;
        int sndbuf = 4 * 1024 * 1024;
        int ttl = 16;
        // Keep enabled by default so sender and receiver can be tested on the same host
        // with multicast addresses such as 239.x.x.x. Disable from preset only when
        // loopback is explicitly unwanted on a production network.
        bool multicast_loop = true;

        // True RTP mode. payload_size remains MPEG-TS payload bytes; the actual
        // UDP datagram adds the fixed 12-byte RTP header. Payload type 33 is the
        // common static payload type for MPEG-TS over RTP (MP2T).
        bool rtp_packetize = false;
        uint8_t rtp_payload_type = 33;
        uint32_t rtp_ssrc = 0;

        // MPEG-TS over UDP is connectionless. Without pacing, muxer output can be
        // sent in short bursts even when the average bitrate is low, which can
        // overflow receiver/NIC buffers and corrupt H.264 elementary streams.
        // A value of 0 means OutputManager will choose a safe bitrate from the
        // active encoder contexts.
        bool pacing_enabled = true;
        int64_t pacing_bitrate_bps = 0;
    };

    UDPStreamer() = default;
    ~UDPStreamer();

    UDPStreamer(const UDPStreamer&) = delete;
    UDPStreamer& operator=(const UDPStreamer&) = delete;

    bool init(const Config& config);
    bool sendPacket(const unsigned char* data, int size);
    void requestStop();
    void closeSocket();

    State getState() const noexcept;
    std::string getLastError() const;

    static const char* stateToString(State state) noexcept;

private:
    void setLastError(const std::string& error);
    static bool isIPv4Multicast(const std::string& address);
    static int normalizePayloadSize(int payloadSize) noexcept;
    bool sendRtpDatagram(const unsigned char* data, int size);
    uint32_t currentRtpTimestamp90k() const;

    bool sendDatagram(const unsigned char* data, int size);
    bool sendDatagramInternal(const unsigned char* data, int size, bool applyPacing);
    void paceDatagram(int size);
    void resetPacingClockLocked();

    mutable std::mutex mutex_;
    int socket_fd_ = -1;
    Config config_{};
    sockaddr_storage destination_{};
    socklen_t destination_len_ = 0;
    std::string last_error_;

    mutable std::mutex tx_mutex_;
    std::vector<uint8_t> pending_ts_bytes_;
    std::chrono::steady_clock::time_point next_send_time_{};
    std::chrono::steady_clock::time_point rtp_epoch_{};
    uint16_t rtp_sequence_ = 0;
    uint32_t rtp_ssrc_ = 0;

    std::atomic<bool> stop_requested_{false};
    std::atomic<State> state_{State::Closed};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> datagrams_sent_{0};
};
