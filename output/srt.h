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
 * SRT transport declarations. SRTStreamer exposes sender/receiver-oriented SRT configuration, connection state, socket lifecycle, packet sending, reconnect control, and runtime statistics.
 */

#ifndef SRT_STREAMER_H
#define SRT_STREAMER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nxframe { struct SenderRuntimeTelemetry; }

class SRTStreamerExternalStopFlag;

#include <srt/srt.h>

// Thin libsrt wrapper for live MPEG-TS contribution transport. The class owns
// socket lifecycle, mode-specific connection setup, reconnect, and stats.
class SRTStreamer {
public:
    // User-facing connection state tracked independently from raw SRT status.
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Listening,
        Connected,
        Reconnecting,
        Failed,
        Closing
    };

    enum class Mode {
        Caller,
        Listener,
        Rendezvous
    };

    // Complete SRT runtime configuration loaded from presets and CLI options.
    struct Config {
        std::string address;
        int port = 0;

        Mode mode = Mode::Caller;
        std::string bind_address;
        int listen_backlog = 1;

        int latency = 120;
        int peer_latency = 0;
        int rcv_latency = 0;
        int connect_timeout_ms = 3000;
        int send_timeout_ms = 1000;
        int recv_timeout_ms = 250;
        int peer_idle_timeout_ms = 3000;

        int payload_size = 1316;
        int sndbuf = 16 * 1024 * 1024;
        int rcvbuf = 16 * 1024 * 1024;
        int oheadbw = 35;
        int snddropdelay = 0;
        int linger = 0;

        int64_t maxbw = 0;
        int64_t inputbw = 0;

        // Optional sender-side application pacing. This does not replace SRT
        // congestion control; it prevents NxFrame from dumping muxed TS chunks
        // into libsrt in bursts. Use MPEG-TS muxrate as the normal source.
        bool pacing_enabled = false;
        int64_t pacing_bitrate_bps = 0;

        bool sender = true;
        bool messageapi = true;
        bool tlpktdrop = true;
        bool nakreport = true;

        std::string streamid;
        std::string passphrase;
        int pbkeylen = 0;

        int stats_interval_ms = 2000;
        int reconnect_backoff_ms = 1000;
        int reconnect_backoff_max_ms = 8000;
        int reconnect_attempts = 5;
        bool reconnect_forever = false;
    };

    SRTStreamer();
    ~SRTStreamer();

    bool init(const std::string& address, int port, int latency = 120);
    bool reconnect(const std::string& address, int port, int attempts = 5, int latency = 120);

    bool init(const Config& config);
    bool reconnect(const Config& config, int attempts = 5);

    bool sendPacket(const unsigned char* data, int size);
    void requestStop();
    void setExternalStopFlag(const std::atomic<bool>* flag);
    void closeSocket();

    ConnectionState getState() const;
    std::string getLastError() const;

    static const char* connectionStateToString(ConnectionState state);
    static const char* socketStateToString(SRT_SOCKSTATUS status);
    static const char* modeToString(Mode mode);
    static Mode modeFromString(const std::string& mode);

private:
    SRTSOCKET srt_socket = SRT_INVALID_SOCK;
    SRTSOCKET listener_socket_ = SRT_INVALID_SOCK;
    Config current_config_{};

    mutable std::mutex socket_mutex;
    std::thread stats_thread;
    std::atomic<bool> stats_running{false};
    std::condition_variable stats_cv;

    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    std::string last_error_;

    std::atomic<uint64_t> app_bytes_sent_{0};
    std::atomic<uint64_t> app_msgs_sent_{0};
    std::atomic<uint64_t> app_send_failures_{0};
    std::atomic<uint64_t> app_reconnects_{0};
    std::atomic<bool> stop_requested_{false};
    const std::atomic<bool>* external_stop_flag_ = nullptr;

    // Telemetry is observational only. State changes and the periodic SRT
    // statistics thread can publish concurrently, so serialize the compact
    // shared-memory record independently from the transport send path.
    std::mutex telemetry_mutex_;
    nxframe::SenderRuntimeTelemetry* telemetry_shared_ = nullptr;
    std::size_t telemetry_mapping_size_ = 0;

    mutable std::mutex tx_mutex_;
    // MPEG-TS/SRT payloadizer state. FFmpeg custom IO can hand us arbitrary
    // byte chunks; keep SRT messages aligned to complete TS packets
    // (normally 7 * 188 = 1316 bytes) instead of sending partial TS packets.
    std::vector<uint8_t> pending_ts_bytes_;
    std::chrono::steady_clock::time_point next_send_time_{};

    bool initInternal(const Config& config);
    bool resolveAndConnect(SRTSOCKET socket, const Config& config);
    bool resolveBindListenAccept(SRTSOCKET listen_socket, const Config& config, SRTSOCKET& accepted_socket);
    bool applyCommonOptions(SRTSOCKET socket, const Config& config);
    bool applyModeOptions(SRTSOCKET socket, const Config& config);
    void logSRTStats();
    void attachTelemetrySink();
    void detachTelemetrySink();
    void publishConnectionState(const char* connectionState);
    void publishTelemetry(double bitrateMbps,
                          uint64_t bytesSent,
                          uint64_t messagesSent,
                          uint64_t packetsSent,
                          uint64_t packetsRetransmitted,
                          uint64_t packetsLost,
                          uint64_t packetsDropped,
                          uint64_t sendFailures,
                          uint64_t reconnects,
                          const char* connectionState,
                          const char* socketState);
    void setState(ConnectionState state);
    void setLastError(const std::string& error);
    void resetPacingClock();
    void pacePayload(int size, const Config& config);

    static constexpr int kDefaultPayloadSize = 1316;
};

#endif