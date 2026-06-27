/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/udp_input.h
 * Description: Declares the UDP input transport and packet queue contract.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <array>

// UDP/RTP transport input. Owns socket binding, optional multicast membership
// and the bounded packet queue consumed by the receiver.
class UDPInput
{
public:
    enum class State
    {
        Stopped,
        Binding,
        Listening,
        Reconnecting,
        Failed,
        Closing
    };

    struct Config
    {
        std::string bind_address = "0.0.0.0";
        int port = 0;

        // Optional IPv4 multicast support.
        std::string multicast_group;
        std::string multicast_interface;

        int recv_timeout_ms = 250;
        int peer_idle_timeout_ms = 3000; // RTP source is considered lost/replaceable after this idle period
        int reconnect_backoff_ms = 1000;
        int reconnect_backoff_max_ms = 8000;
        int reconnect_attempts = 5;
        bool reconnect_forever = true;

        int rcvbuf = 32 * 1024 * 1024;
        bool reuse_address = true;
        bool auto_join_multicast = true;

        size_t max_queue_packets = 4096;
        size_t max_packet_size = 2048;

        // For MPEG-TS over UDP, keep only complete 188-byte TS packets and
        // reject datagrams that are not sync-aligned. This prevents a noisy
        // late join or a malformed datagram from feeding arbitrary byte offsets
        // into the demuxer.
        bool mpegts_sync_filter = true;

        // True RTP mode: expect UDP datagrams with an RTP v2 fixed header and
        // MPEG-TS payload type 33, strip RTP, and feed only TS bytes onward.
        bool rtp_depacketize = false;
        uint8_t rtp_payload_type = 33;
        bool rtp_strict_payload_type = true;
    };

    struct Packet
    {
        std::vector<uint8_t> data;
        int64_t receive_time_us = 0;
        std::string source_address;
        int source_port = 0;
    };

    // Receiver-side diagnostics. For UDP/RTP this is the only reliable place
    // to know if packets were malformed, missing, duplicated, out of order, or
    // whether MPEG-TS continuity was broken. Sender-side UDP/RTP has no
    // receiver feedback, unlike SRT.
    struct Diagnostics
    {
        uint64_t received_packets = 0;      // packets accepted into receiver queue
        uint64_t received_bytes = 0;        // MPEG-TS payload bytes accepted
        uint64_t dropped_packets = 0;       // local queue drops + rejected datagrams

        uint64_t rtp_packets = 0;           // valid RTP packets, when RTP mode is enabled
        uint64_t rtp_malformed = 0;         // invalid RTP header/extension/padding/payload
        uint64_t rtp_payload_type_mismatch = 0;
        uint64_t rtp_sequence_gaps = 0;     // number of missing RTP sequence numbers
        uint64_t rtp_out_of_order = 0;
        uint64_t rtp_duplicates = 0;

        // RTP source locking diagnostics. In multicast networks it is possible
        // to receive multiple senders on the same group/port. The receiver
        // locks to the first valid RTP source+SSRC and rejects unrelated RTP
        // packets until the active source is considered idle/lost.
        uint64_t rtp_source_changes = 0;    // accepted switch to a new source/SSRC after idle/reset
        uint64_t rtp_source_rejected = 0;   // packets ignored from non-active source/SSRC
        uint16_t last_rtp_sequence = 0;
        uint16_t last_rtp_expected_sequence = 0;
        uint16_t last_rtp_gap_observed_sequence = 0;
        uint64_t last_rtp_gap_missing = 0;
        uint32_t last_rtp_timestamp = 0;
        uint32_t last_rtp_ssrc = 0;
        uint32_t locked_rtp_source_ipv4 = 0; // network byte order IPv4 address
        uint16_t locked_rtp_source_port = 0;
        bool have_rtp_sequence = false;
        bool have_rtp_source = false;

        uint64_t ts_sync_errors = 0;        // rejected by MPEG-TS sync filter
        uint64_t ts_continuity_errors = 0;  // continuity counter breaks after filtering
        int actual_rcvbuf = 0;              // kernel-confirmed SO_RCVBUF after bind
    };

    UDPInput();
    ~UDPInput();

    bool start(const Config& config);
    void stop();

    bool popPacket(Packet& out, int timeout_ms = 100);

    State getState() const noexcept;
    std::string getLastError() const;

    uint64_t receivedPackets() const noexcept;
    uint64_t receivedBytes() const noexcept;
    uint64_t droppedPackets() const noexcept;
    Diagnostics diagnostics() const noexcept;

    static const char* stateToString(State state) noexcept;

private:
    bool openSocket();
    void receiveLoop();
    void closeSocket();

    void setState(State state) noexcept;
    void setLastError(const std::string& error);

    bool shouldJoinMulticast() const;
    bool joinMulticastGroup(int sockfd);

private:
    mutable std::mutex state_mutex_;
    std::string last_error_;

    mutable std::mutex socket_mutex_;
    int socket_fd_ = -1;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::deque<Packet> queue_;

    std::thread recv_thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<State> state_{State::Stopped};

    std::atomic<uint64_t> received_packets_{0};
    std::atomic<uint64_t> received_bytes_{0};
    std::atomic<uint64_t> dropped_packets_{0};

    std::atomic<uint64_t> rtp_packets_{0};
    std::atomic<uint64_t> rtp_malformed_{0};
    std::atomic<uint64_t> rtp_payload_type_mismatch_{0};
    std::atomic<uint64_t> rtp_sequence_gaps_{0};
    std::atomic<uint64_t> rtp_out_of_order_{0};
    std::atomic<uint64_t> rtp_duplicates_{0};
    std::atomic<uint64_t> rtp_source_changes_{0};
    std::atomic<uint64_t> rtp_source_rejected_{0};
    std::atomic<uint64_t> ts_sync_errors_{0};
    std::atomic<uint64_t> ts_continuity_errors_{0};
    std::atomic<int> actual_rcvbuf_{0};

    std::atomic<uint16_t> last_rtp_sequence_{0};
    std::atomic<uint16_t> last_rtp_expected_sequence_{0};
    std::atomic<uint16_t> last_rtp_gap_observed_sequence_{0};
    std::atomic<uint64_t> last_rtp_gap_missing_{0};
    std::atomic<uint32_t> last_rtp_timestamp_{0};
    std::atomic<uint32_t> last_rtp_ssrc_{0};
    std::atomic<uint32_t> locked_rtp_source_ipv4_{0};
    std::atomic<uint16_t> locked_rtp_source_port_{0};
    std::atomic<bool> have_rtp_sequence_{false};
    std::atomic<bool> have_rtp_source_{false};

    Config config_{};
};