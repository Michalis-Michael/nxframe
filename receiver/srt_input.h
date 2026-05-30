/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: receiver/srt_input.h
 * Description: Declares the SRT input transport and packet queue contract.
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

#include <srt/srt.h>

// SRT transport input. Owns socket state, receive loop, reconnect behavior
// and the bounded packet queue consumed by the receiver.
class SRTInput
{
public:
    enum class Mode
    {
        Caller,
        Listener,
        Rendezvous
    };

    enum class State
    {
        Stopped,
        Connecting,
        Listening,
        Connected,
        Reconnecting,
        Failed,
        Closing
    };

    struct Config
    {
        std::string address;
        int port = 0;
        Mode mode = Mode::Listener;

        std::string bind_address = "0.0.0.0";
        int listen_backlog = 1;

        int latency = 120;
        int peer_latency = 0;
        int rcv_latency = 0;

        int connect_timeout_ms = 3000;
        int recv_timeout_ms = 250;
        int peer_idle_timeout_ms = 3000;

        int payload_size = 1316;
        int sndbuf = 4 * 1024 * 1024;
        int rcvbuf = 4 * 1024 * 1024;
        int oheadbw = 25;
        int linger = 0;

        int64_t maxbw = 0;
        int64_t inputbw = 0;

        bool messageapi = true;
        bool tlpktdrop = true;
        bool nakreport = true;

        std::string streamid;
        std::string passphrase;
        int pbkeylen = 0;

        int reconnect_backoff_ms = 1000;
        int reconnect_backoff_max_ms = 8000;
        int reconnect_attempts = 5;
        bool reconnect_forever = true;

        size_t max_queue_packets = 2048;
        size_t max_packet_size = 2048;
    };

    struct Packet
    {
        std::vector<uint8_t> data;
        int64_t receive_time_us = 0;
    };

    SRTInput();
    ~SRTInput();

    bool start(const Config& config);
    void stop();
    bool reconnect();

    bool popPacket(Packet& out, int timeout_ms = 100);

    State getState() const noexcept;
    std::string getLastError() const;

    uint64_t receivedPackets() const noexcept;
    uint64_t receivedBytes() const noexcept;
    uint64_t droppedPackets() const noexcept;
    uint64_t realignedPackets() const noexcept;
    uint64_t realignedBytes() const noexcept;

    static const char* stateToString(State state);
    static const char* modeToString(Mode mode);

private:
    bool initSocketAndConnect();
    bool applyCommonOptions(SRTSOCKET socket, const Config& config);
    bool resolveAndConnect(SRTSOCKET socket, const Config& config);
    bool resolveBindListenAccept(SRTSOCKET socket, const Config& config, SRTSOCKET& out_socket);

    void receiveLoop();
    void closeSockets();

    void setState(State state) noexcept;
    void setLastError(const std::string& error);

    static bool isNoPendingAcceptError(const std::string& err);

private:
    mutable std::mutex state_mutex_;
    std::string last_error_;

    mutable std::mutex socket_mutex_;
    SRTSOCKET socket_ = SRT_INVALID_SOCK;
    SRTSOCKET listener_socket_ = SRT_INVALID_SOCK;

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
    std::atomic<uint64_t> realigned_packets_{0};
    std::atomic<uint64_t> realigned_bytes_{0};

    Config config_{};
};