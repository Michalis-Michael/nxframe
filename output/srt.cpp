#include "core/sender_dashboard.h"
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
 * SRT transport implementation. SRTStreamer owns libsrt setup, caller/listener/rendezvous connection handling, socket options, live packet sending, reconnect behavior, and periodic SRT statistics logging.
 */

#include "srt.h"
#include "core/sender_runtime_telemetry.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include <vector>

namespace {

// Configure libsrt logging once for the process. NxFrame keeps its own logs
// readable while suppressing expected poll-time noise from non-blocking modes.
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


int normalizeMpegTsPayloadSize(int configuredPayload)
{
    constexpr int kTsPacketBytes = 188;
    constexpr int kDefaultTsPayloadBytes = 1316; // 7 TS packets

    int payload = (configuredPayload > 0) ? configuredPayload : kDefaultTsPayloadBytes;
    payload = (payload / kTsPacketBytes) * kTsPacketBytes;
    if (payload < kTsPacketBytes) {
        payload = kTsPacketBytes;
    }
    return payload;
}

void updateAtomicMin(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t current = target.load(std::memory_order_relaxed);
    while (value < current &&
           !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void updateAtomicMax(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

bool setSockOptInt(SRTSOCKET socket, SRT_SOCKOPT opt, int value, const char* name, bool required = true)
{
    if (srt_setsockopt(socket, 0, opt, &value, sizeof(value)) == SRT_ERROR) {
        if (required) {
            std::cerr << "[SRT] Failed to set " << name << "=" << value
                      << " error=" << srt_getlasterror_str() << "\n";
        }
        return false;
    }
    return true;
}

bool setSockOptInt64(SRTSOCKET socket, SRT_SOCKOPT opt, int64_t value, const char* name, bool required = true)
{
    if (srt_setsockopt(socket, 0, opt, &value, sizeof(value)) == SRT_ERROR) {
        if (required) {
            std::cerr << "[SRT] Failed to set " << name << "=" << value
                      << " error=" << srt_getlasterror_str() << "\n";
        }
        return false;
    }
    return true;
}

bool setSockOptString(SRTSOCKET socket, SRT_SOCKOPT opt, const std::string& value, const char* name, bool required = true)
{
    if (value.empty()) {
        return true;
    }

    if (srt_setsockopt(socket, 0, opt, value.c_str(), static_cast<int>(value.size())) == SRT_ERROR) {
        if (required) {
            std::cerr << "[SRT] Failed to set " << name
                      << " error=" << srt_getlasterror_str() << "\n";
        }
        return false;
    }
    return true;
}

bool setSockOptLinger(SRTSOCKET socket, int seconds, const char* name, bool required = true)
{
    struct linger ling;
    std::memset(&ling, 0, sizeof(ling));

    if (seconds > 0) {
        ling.l_onoff = 1;
        ling.l_linger = seconds;
    }

    if (srt_setsockopt(socket, 0, SRTO_LINGER, &ling, sizeof(ling)) == SRT_ERROR) {
        if (required) {
            std::cerr << "[SRT] Failed to set " << name << "=" << seconds
                      << " error=" << srt_getlasterror_str() << "\n";
        }
        return false;
    }
    return true;
}

std::string classifyErrorText(const std::string& err)
{
    if (err.find("Bad parameters") != std::string::npos) return "configuration_error";
    if (err.find("Connection setup failure") != std::string::npos) return "connect_failure";
    if (err.find("Connection timeout") != std::string::npos) return "timeout";
    if (err.find("Operation not supported") != std::string::npos) return "unsupported_option";
    if (err.find("Incorrect password") != std::string::npos) return "authentication_error";
    if (err.find("Connection was broken") != std::string::npos) return "broken_connection";
    return "generic_error";
}

bool isTransientSendError(const std::string& err)
{
    std::string e = err;
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return e.find("timeout") != std::string::npos ||
           e.find("non-blocking") != std::string::npos ||
           e.find("temporarily unavailable") != std::string::npos ||
           e.find("try again") != std::string::npos;
}



bool shouldStop(const std::atomic<bool>& local_stop, const std::atomic<bool>* external_stop)
{
    if (local_stop.load(std::memory_order_acquire)) {
        return true;
    }
    return external_stop && external_stop->load(std::memory_order_acquire);
}

bool isFatalConfiguration(const SRTStreamer::Config& config, std::string& reason)
{
    if (config.port <= 0 || config.port > 65535) {
        reason = "port must be between 1 and 65535";
        return true;
    }
    if (config.latency < 0) {
        reason = "latency must be >= 0";
        return true;
    }
    if (config.payload_size <= 0) {
        reason = "payload_size must be > 0";
        return true;
    }
    if (!config.passphrase.empty() &&
        (config.passphrase.size() < 10 || config.passphrase.size() > 79)) {
        reason = "passphrase length must be between 10 and 79 characters";
        return true;
    }
    if (!config.passphrase.empty() &&
        config.pbkeylen != 0 && config.pbkeylen != 16 &&
        config.pbkeylen != 24 && config.pbkeylen != 32) {
        reason = "pbkeylen must be 0, 16, 24, or 32";
        return true;
    }
    if (config.mode != SRTStreamer::Mode::Listener && config.address.empty()) {
        reason = "address must not be empty in caller/rendezvous mode";
        return true;
    }
    return false;
}

} // namespace

SRTStreamer::SRTStreamer()
{
    srt_startup();
    configureSrtLibraryLogging();
    attachTelemetrySink();
}

SRTStreamer::~SRTStreamer()
{
    closeSocket();
    detachTelemetrySink();
    srt_cleanup();
}



void SRTStreamer::attachTelemetrySink()
{
    std::lock_guard<std::mutex> telemetryLock(telemetry_mutex_);

    const char* rawFd = std::getenv("NXFRAME_SENDER_TELEMETRY_FD");
    if (!rawFd || !*rawFd) return;

    char* end = nullptr;
    const long parsed = std::strtol(rawFd, &end, 10);
    if (!end || *end != '\0' || parsed < 0) return;

    const int fd = static_cast<int>(parsed);
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(nxframe::SenderRuntimeTelemetry))) {
        return;
    }

    void* mapping = ::mmap(nullptr,
                           sizeof(nxframe::SenderRuntimeTelemetry),
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           fd,
                           0);
    if (mapping == MAP_FAILED) return;

    telemetry_shared_ = static_cast<nxframe::SenderRuntimeTelemetry*>(mapping);
    telemetry_mapping_size_ = sizeof(nxframe::SenderRuntimeTelemetry);
    ::close(fd);
}

void SRTStreamer::detachTelemetrySink()
{
    std::lock_guard<std::mutex> telemetryLock(telemetry_mutex_);

    if (!telemetry_shared_) return;
    ::munmap(telemetry_shared_, telemetry_mapping_size_);
    telemetry_shared_ = nullptr;
    telemetry_mapping_size_ = 0;
}

void SRTStreamer::publishConnectionState(const char* connectionState)
{
    std::lock_guard<std::mutex> telemetryLock(telemetry_mutex_);

    auto* telemetry = telemetry_shared_;
    if (!telemetry) return;

    uint64_t sequence = __atomic_load_n(&telemetry->sequence, __ATOMIC_RELAXED);
    if (sequence & 1U) ++sequence;
    __atomic_store_n(&telemetry->sequence, sequence + 1U, __ATOMIC_RELEASE);

    telemetry->magic = nxframe::kSenderTelemetryMagic;
    telemetry->version = nxframe::kSenderTelemetryVersion;
    telemetry->record_size = static_cast<uint16_t>(sizeof(nxframe::SenderRuntimeTelemetry));
    telemetry->updated_monotonic_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    std::snprintf(telemetry->connection_state,
                  sizeof(telemetry->connection_state),
                  "%s",
                  connectionState ? connectionState : "UNKNOWN");
    if (!telemetry->socket_state[0]) {
        std::snprintf(telemetry->socket_state, sizeof(telemetry->socket_state), "%s", "INVALID");
    }

    __atomic_store_n(&telemetry->sequence, sequence + 2U, __ATOMIC_RELEASE);
}

void SRTStreamer::publishTelemetry(double bitrateMbps,
                                   uint64_t bytesSent,
                                   uint64_t messagesSent,
                                   uint64_t packetsSent,
                                   uint64_t packetsRetransmitted,
                                   uint64_t packetsLost,
                                   uint64_t packetsDropped,
                                   double rttMs,
                                   double bandwidthMbps,
                                   uint64_t sndbufPackets,
                                   uint64_t sndbufBytes,
                                   uint64_t sndbufMs,
                                   uint64_t sndbufAvailBytes,
                                   uint64_t sendFailures,
                                   uint64_t reconnects,
                                   const char* connectionState,
                                   const char* socketState)
{
    std::lock_guard<std::mutex> telemetryLock(telemetry_mutex_);

    auto* telemetry = telemetry_shared_;
    if (!telemetry) return;

    uint64_t sequence = __atomic_load_n(&telemetry->sequence, __ATOMIC_RELAXED);
    if (sequence & 1U) ++sequence;
    __atomic_store_n(&telemetry->sequence, sequence + 1U, __ATOMIC_RELEASE);

    telemetry->magic = nxframe::kSenderTelemetryMagic;
    telemetry->version = nxframe::kSenderTelemetryVersion;
    telemetry->record_size = static_cast<uint16_t>(sizeof(nxframe::SenderRuntimeTelemetry));
    telemetry->updated_monotonic_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    telemetry->bitrate_mbps = bitrateMbps;
    telemetry->bytes_sent = bytesSent;
    telemetry->messages_sent = messagesSent;
    telemetry->packets_sent = packetsSent;
    telemetry->packets_retransmitted = packetsRetransmitted;
    telemetry->packets_lost = packetsLost;
    telemetry->packets_dropped = packetsDropped;
    telemetry->rtt_ms = rttMs;
    telemetry->bandwidth_mbps = bandwidthMbps;
    telemetry->sndbuf_packets = sndbufPackets;
    telemetry->sndbuf_bytes = sndbufBytes;
    telemetry->sndbuf_ms = sndbufMs;
    telemetry->sndbuf_avail_bytes = sndbufAvailBytes;
    telemetry->send_failures = sendFailures;
    telemetry->reconnects = reconnects;
    std::snprintf(telemetry->connection_state,
                  sizeof(telemetry->connection_state),
                  "%s",
                  connectionState ? connectionState : "UNKNOWN");
    std::snprintf(telemetry->socket_state,
                  sizeof(telemetry->socket_state),
                  "%s",
                  socketState ? socketState : "INVALID");

    __atomic_store_n(&telemetry->sequence, sequence + 2U, __ATOMIC_RELEASE);
}

void SRTStreamer::requestStop()
{
    SRTSOCKET listener_to_close = SRT_INVALID_SOCK;

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        stop_requested_.store(true, std::memory_order_release);
        listener_to_close = listener_socket_;
        listener_socket_ = SRT_INVALID_SOCK;
    }

    stats_cv.notify_all();

    if (listener_to_close != SRT_INVALID_SOCK) {
        srt_close(listener_to_close);
    }
}

void SRTStreamer::setExternalStopFlag(const std::atomic<bool>* flag)
{
    std::lock_guard<std::mutex> lock(socket_mutex);
    external_stop_flag_ = flag;
}

const char* SRTStreamer::modeToString(Mode mode)
{
    switch (mode) {
        case Mode::Caller: return "caller";
        case Mode::Listener: return "listener";
        case Mode::Rendezvous: return "rendezvous";
        default: return "caller";
    }
}

SRTStreamer::Mode SRTStreamer::modeFromString(const std::string& mode)
{
    if (mode == "listener") return Mode::Listener;
    if (mode == "rendezvous") return Mode::Rendezvous;
    return Mode::Caller;
}

const char* SRTStreamer::connectionStateToString(ConnectionState state)
{
    switch (state) {
        case ConnectionState::Disconnected: return "DISCONNECTED";
        case ConnectionState::Connecting: return "CONNECTING";
        case ConnectionState::Listening: return "LISTENING";
        case ConnectionState::Connected: return "CONNECTED";
        case ConnectionState::Reconnecting: return "RECONNECTING";
        case ConnectionState::Failed: return "FAILED";
        case ConnectionState::Closing: return "CLOSING";
        default: return "UNKNOWN";
    }
}

const char* SRTStreamer::socketStateToString(SRT_SOCKSTATUS status)
{
    switch (status) {
        case SRTS_INIT: return "INIT";
        case SRTS_OPENED: return "OPENED";
        case SRTS_LISTENING: return "LISTENING";
        case SRTS_CONNECTING: return "CONNECTING";
        case SRTS_CONNECTED: return "CONNECTED";
        case SRTS_BROKEN: return "BROKEN";
        case SRTS_CLOSING: return "CLOSING";
        case SRTS_CLOSED: return "CLOSED";
        case SRTS_NONEXIST: return "NONEXIST";
        default: return "UNKNOWN";
    }
}

void SRTStreamer::setState(ConnectionState state)
{
    state_.store(state, std::memory_order_release);
    publishConnectionState(connectionStateToString(state));
}

void SRTStreamer::setLastError(const std::string& error)
{
    std::lock_guard<std::mutex> lock(socket_mutex);
    last_error_ = error;
}

SRTStreamer::ConnectionState SRTStreamer::getState() const
{
    return state_.load(std::memory_order_acquire);
}

std::string SRTStreamer::getLastError() const
{
    std::lock_guard<std::mutex> lock(socket_mutex);
    return last_error_;
}

void SRTStreamer::resetPayloadizer()
{
    std::lock_guard<std::mutex> lk(tx_mutex_);
    pending_ts_bytes_.clear();
    send_iat_prev_ns_.store(0, std::memory_order_relaxed);
    send_burst_lt50_current_.store(0, std::memory_order_relaxed);
}

void SRTStreamer::closeSocket()
{
    SRTSOCKET socket_to_close = SRT_INVALID_SOCK;
    SRTSOCKET listener_to_close = SRT_INVALID_SOCK;

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        stop_requested_.store(true, std::memory_order_release);
        stats_running.store(false, std::memory_order_release);
        socket_to_close = srt_socket;
        listener_to_close = listener_socket_;
        srt_socket = SRT_INVALID_SOCK;
        listener_socket_ = SRT_INVALID_SOCK;
        last_error_.clear();
    }

    resetPayloadizer();
    setState(ConnectionState::Closing);
    stats_cv.notify_all();

    if (socket_to_close != SRT_INVALID_SOCK) {
        srt_close(socket_to_close);
    }
    if (listener_to_close != SRT_INVALID_SOCK && listener_to_close != socket_to_close) {
        srt_close(listener_to_close);
    }

    if (stats_thread.joinable()) {
        stats_thread.join();
    }

    setState(ConnectionState::Disconnected);
}

bool SRTStreamer::init(const std::string& address, int port, int latency)
{
    Config config;
    config.address = address;
    config.port = port;
    config.latency = latency;
    return init(config);
}

bool SRTStreamer::init(const Config& config)
{
    return initInternal(config);
}

// Create and configure a fresh SRT socket. Mode-specific connect/listen work
// happens after common latency, buffer, encryption, and timeout options are
// applied.
bool SRTStreamer::initInternal(const Config& config)
{
    closeSocket();
    stop_requested_.store(false, std::memory_order_release);
    current_config_ = config;
    resetPayloadizer();

    std::string validation_error;
    if (isFatalConfiguration(config, validation_error)) {
        setLastError(validation_error);
        setState(ConnectionState::Failed);
        std::cerr << "[SRT] Invalid configuration: " << validation_error << "\n";
        return false;
    }

    SRTSOCKET new_socket = srt_create_socket();
    if (new_socket == SRT_INVALID_SOCK) {
        const std::string err = srt_getlasterror_str();
        setLastError(err);
        setState(ConnectionState::Failed);
        std::cerr << "[SRT] Failed to create socket. error=" << err << "\n";
        return false;
    }

    setState(ConnectionState::Connecting);

    if (!applyCommonOptions(new_socket, config) || !applyModeOptions(new_socket, config)) {
        const std::string err = srt_getlasterror_str();
        setLastError(err);
        setState(ConnectionState::Failed);
        srt_close(new_socket);
        return false;
    }

    bool connected = false;
    SRTSOCKET connected_socket = new_socket;
    if (config.mode == Mode::Listener) {
        connected = resolveBindListenAccept(new_socket, config, connected_socket);
    } else {
        connected = resolveAndConnect(new_socket, config);
    }

    if (!connected) {
        const std::string err = getLastError().empty() ? srt_getlasterror_str() : getLastError();
        setLastError(err);
        setState(ConnectionState::Failed);
        if (connected_socket != SRT_INVALID_SOCK && connected_socket != new_socket) {
            srt_close(connected_socket);
        }
        if (new_socket != SRT_INVALID_SOCK) {
            srt_close(new_socket);
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        srt_socket = connected_socket;
        listener_socket_ = SRT_INVALID_SOCK;
        stats_running.store(true, std::memory_order_release);
        last_error_.clear();
    }

    setState(ConnectionState::Connected);

    if (stats_thread.joinable()) {
        stats_thread.join();
    }
    resetPayloadizer();
    stats_thread = std::thread(&SRTStreamer::logSRTStats, this);

    std::cout << "[SRT] Connected mode=" << modeToString(config.mode)
              << " target=" << config.address << ":" << config.port
              << " latency=" << config.latency
              << "ms payload=" << config.payload_size;
    if (!config.bind_address.empty()) {
        std::cout << " bind=" << config.bind_address;
    }
    if (!config.streamid.empty()) {
        std::cout << " streamid='" << config.streamid << "'";
    }
    std::cout << "\n";

    return true;
}

bool SRTStreamer::applyModeOptions(SRTSOCKET socket, const Config& config)
{
#ifdef SRTO_RENDEZVOUS
    if (config.mode == Mode::Rendezvous) {
        if (!setSockOptInt(socket, SRTO_RENDEZVOUS, 1, "SRTO_RENDEZVOUS")) {
            return false;
        }
    }
#endif
    return true;
}

// Apply transport options that are common to caller, listener, and rendezvous
// modes. Required options fail initialization; optional options log only when
// useful for diagnosis.
bool SRTStreamer::applyCommonOptions(SRTSOCKET socket, const Config& config)
{
    const int yes = 1;
    const int no = 0;
    const int transtype = SRTT_LIVE;
    const int payload = (config.payload_size > 0) ? config.payload_size : kDefaultPayloadSize;

    if (!setSockOptInt(socket, SRTO_TRANSTYPE, transtype, "SRTO_TRANSTYPE")) return false;
    if (!setSockOptInt(socket, SRTO_SENDER, config.sender ? yes : no, "SRTO_SENDER")) return false;
    if (!setSockOptInt(socket, SRTO_MESSAGEAPI, config.messageapi ? yes : no, "SRTO_MESSAGEAPI")) return false;
    if (!setSockOptInt(socket, SRTO_TLPKTDROP, config.tlpktdrop ? yes : no, "SRTO_TLPKTDROP")) return false;
    if (!setSockOptInt(socket, SRTO_PAYLOADSIZE, payload, "SRTO_PAYLOADSIZE")) return false;
    if (!setSockOptInt(socket, SRTO_LATENCY, config.latency, "SRTO_LATENCY")) return false;
    if (!setSockOptInt(socket, SRTO_CONNTIMEO, config.connect_timeout_ms, "SRTO_CONNTIMEO")) return false;
    if (!setSockOptInt(socket, SRTO_SNDTIMEO, config.send_timeout_ms, "SRTO_SNDTIMEO")) return false;
    if (!setSockOptInt(socket, SRTO_RCVTIMEO, config.recv_timeout_ms, "SRTO_RCVTIMEO")) return false;
    if (!setSockOptInt(socket, SRTO_SNDBUF, config.sndbuf, "SRTO_SNDBUF")) return false;
    if (!setSockOptInt(socket, SRTO_RCVBUF, config.rcvbuf, "SRTO_RCVBUF")) return false;
    if (!setSockOptInt(socket, SRTO_OHEADBW, config.oheadbw, "SRTO_OHEADBW")) return false;
    if (!setSockOptLinger(socket, config.linger, "SRTO_LINGER")) return false;

    if (config.maxbw != 0 && !setSockOptInt64(socket, SRTO_MAXBW, config.maxbw, "SRTO_MAXBW")) return false;
    if (config.inputbw > 0 && !setSockOptInt64(socket, SRTO_INPUTBW, config.inputbw, "SRTO_INPUTBW")) return false;

#ifdef SRTO_PEERLATENCY
    if (config.peer_latency > 0 && !setSockOptInt(socket, SRTO_PEERLATENCY, config.peer_latency, "SRTO_PEERLATENCY")) return false;
#endif
#ifdef SRTO_RCVLATENCY
    if (config.rcv_latency > 0 && !setSockOptInt(socket, SRTO_RCVLATENCY, config.rcv_latency, "SRTO_RCVLATENCY")) return false;
#endif
#ifdef SRTO_PEERIDLETIMEO
    if (config.peer_idle_timeout_ms > 0 &&
        !setSockOptInt(socket, SRTO_PEERIDLETIMEO, config.peer_idle_timeout_ms, "SRTO_PEERIDLETIMEO", false)) {
        std::cerr << "[SRT] Warning: SRTO_PEERIDLETIMEO not supported by this SRT build.\n";
    }
#endif
#ifdef SRTO_NAKREPORT
    if (!setSockOptInt(socket, SRTO_NAKREPORT, config.nakreport ? yes : no, "SRTO_NAKREPORT", false) && config.nakreport) {
        std::cerr << "[SRT] Warning: SRTO_NAKREPORT not supported by this SRT build.\n";
    }
#endif
#ifdef SRTO_SNDDROPDELAY
    if (config.snddropdelay > 0 &&
        !setSockOptInt(socket, SRTO_SNDDROPDELAY, config.snddropdelay, "SRTO_SNDDROPDELAY", false)) {
        std::cerr << "[SRT] Warning: SRTO_SNDDROPDELAY not supported by this SRT build.\n";
    }
#endif
#ifdef SRTO_STREAMID
    if (!config.streamid.empty() &&
        !setSockOptString(socket, SRTO_STREAMID, config.streamid, "SRTO_STREAMID", false)) {
        std::cerr << "[SRT] Warning: SRTO_STREAMID not supported by this SRT build.\n";
    }
#endif
#ifdef SRTO_PASSPHRASE
    if (!config.passphrase.empty()) {
        if (!setSockOptString(socket, SRTO_PASSPHRASE, config.passphrase, "SRTO_PASSPHRASE")) return false;
#ifdef SRTO_PBKEYLEN
        if (config.pbkeylen > 0 && !setSockOptInt(socket, SRTO_PBKEYLEN, config.pbkeylen, "SRTO_PBKEYLEN")) return false;
#endif
    }
#endif

    return true;
}

bool SRTStreamer::resolveAndConnect(SRTSOCKET socket, const Config& config)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    std::unique_ptr<struct addrinfo, decltype(&freeaddrinfo)> bind_list(nullptr, freeaddrinfo);
    const bool rendezvous = config.mode == Mode::Rendezvous;
    const std::string local_bind_host =
        rendezvous && config.bind_address.empty() ? "0.0.0.0" : config.bind_address;
    const std::string local_bind_port = rendezvous ? std::to_string(config.port) : "0";

    if (!local_bind_host.empty()) {
        struct addrinfo* bind_result = nullptr;
        const int gai_bind = getaddrinfo(local_bind_host.c_str(),
                                         local_bind_port.c_str(),
                                         &hints,
                                         &bind_result);
        if (gai_bind == 0 && bind_result != nullptr) {
            bind_list.reset(bind_result);
            bool bound = false;
            for (struct addrinfo* ai = bind_list.get(); ai != nullptr; ai = ai->ai_next) {
                if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
                    continue;
                }
                if (srt_bind(socket, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
                    bound = true;
                    break;
                }
            }
            if (!bound) {
                std::ostringstream oss;
                oss << "Failed to bind local SRT socket on " << local_bind_host
                    << ":" << local_bind_port
                    << " error=" << srt_getlasterror_str();
                setLastError(oss.str());
                std::cerr << "[SRT] " << oss.str() << "\n";
                return false;
            }
        } else {
            std::ostringstream oss;
            oss << "Failed to resolve local SRT bind address '" << local_bind_host
                << ":" << local_bind_port << "' error=" << gai_strerror(gai_bind);
            setLastError(oss.str());
            std::cerr << "[SRT] " << oss.str() << "\n";
            return false;
        }
    }

    const std::string port_str = std::to_string(config.port);
    struct addrinfo* result = nullptr;
    const int gai_rc = getaddrinfo(config.address.c_str(), port_str.c_str(), &hints, &result);
    if (gai_rc != 0 || result == nullptr) {
        std::ostringstream oss;
        oss << "Address resolution failed for " << config.address << ":" << config.port
            << " error=" << gai_strerror(gai_rc);
        setLastError(oss.str());
        std::cerr << "[SRT] " << oss.str() << "\n";
        return false;
    }

    std::unique_ptr<struct addrinfo, decltype(&freeaddrinfo)> addr_list(result, freeaddrinfo);

    for (struct addrinfo* ai = addr_list.get(); ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }

        if (srt_connect(socket, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == SRT_ERROR) {
            std::cerr << "[SRT] Connect attempt failed mode=" << modeToString(config.mode)
                      << " to " << config.address << ":" << config.port
                      << " error=" << srt_getlasterror_str() << "\n";
            continue;
        }

        return true;
    }

    std::ostringstream oss;
    oss << "Unable to connect mode=" << modeToString(config.mode)
        << " to " << config.address << ":" << config.port
        << " last_error=" << srt_getlasterror_str();
    setLastError(oss.str());
    std::cerr << "[SRT] " << oss.str() << "\n";
    return false;
}

bool SRTStreamer::resolveBindListenAccept(SRTSOCKET listen_socket, const Config& config, SRTSOCKET& accepted_socket)
{
    accepted_socket = SRT_INVALID_SOCK;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    const std::string bind_host = config.bind_address.empty() ? config.address : config.bind_address;
    const char* host = bind_host.empty() ? nullptr : bind_host.c_str();
    const std::string port_str = std::to_string(config.port);

    struct addrinfo* result = nullptr;
    const int gai_rc = getaddrinfo(host, port_str.c_str(), &hints, &result);
    if (gai_rc != 0 || result == nullptr) {
        std::ostringstream oss;
        oss << "Listen address resolution failed for "
            << (bind_host.empty() ? "0.0.0.0" : bind_host) << ":" << config.port
            << " error=" << gai_strerror(gai_rc);
        setLastError(oss.str());
        std::cerr << "[SRT] " << oss.str() << "\n";
        return false;
    }

    std::unique_ptr<struct addrinfo, decltype(&freeaddrinfo)> addr_list(result, freeaddrinfo);

    bool bound = false;
    for (struct addrinfo* ai = addr_list.get(); ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }
        if (srt_bind(listen_socket, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            bound = true;
            break;
        }
    }

    if (!bound) {
        std::ostringstream oss;
        oss << "Failed to bind listener socket on "
            << (bind_host.empty() ? "0.0.0.0" : bind_host) << ":" << config.port
            << " error=" << srt_getlasterror_str();
        setLastError(oss.str());
        std::cerr << "[SRT] " << oss.str() << "\n";
        return false;
    }

    const int no = 0;
    if (!setSockOptInt(listen_socket, SRTO_RCVSYN, no, "SRTO_RCVSYN(listener)")) {
        return false;
    }

    const int backlog = std::max(1, config.listen_backlog);
    if (srt_listen(listen_socket, backlog) == SRT_ERROR) {
        std::ostringstream oss;
        oss << "Failed to listen on "
            << (bind_host.empty() ? "0.0.0.0" : bind_host) << ":" << config.port
            << " error=" << srt_getlasterror_str();
        setLastError(oss.str());
        std::cerr << "[SRT] " << oss.str() << "\n";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        listener_socket_ = listen_socket;
    }

    setState(ConnectionState::Listening);
    std::cout << "[SRT] Listening on " << (bind_host.empty() ? "0.0.0.0" : bind_host)
              << ":" << config.port << " backlog=" << backlog << "\n";

    while (!shouldStop(stop_requested_, external_stop_flag_)) {
        sockaddr_storage peer_addr;
        int peer_len = sizeof(peer_addr);

        SRTSOCKET candidate = srt_accept(listen_socket, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (candidate != SRT_INVALID_SOCK) {
            accepted_socket = candidate;

            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                if (listener_socket_ == listen_socket) {
                    listener_socket_ = SRT_INVALID_SOCK;
                }
            }

            srt_close(listen_socket);

            char host_buf[NI_MAXHOST] = {0};
            char svc_buf[NI_MAXSERV] = {0};
            if (getnameinfo(reinterpret_cast<sockaddr*>(&peer_addr), peer_len,
                            host_buf, sizeof(host_buf), svc_buf, sizeof(svc_buf),
                            NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
                nxframe::senderDashboard().update([&](nxframe::DashboardState& d) { d.peer=std::string(host_buf)+":"+svc_buf; });
                std::cout << "[SRT] Accepted caller from " << host_buf << ":" << svc_buf << "\n";
            } else {
                std::cout << "[SRT] Accepted caller on port " << config.port << "\n";
            }

            return true;
        }

        const std::string err = srt_getlasterror_str();
        if (!shouldStop(stop_requested_, external_stop_flag_)) {
            std::string err_lower = err;
            std::transform(err_lower.begin(), err_lower.end(), err_lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            const bool no_pending =
                err_lower.find("no pending connection") != std::string::npos ||
                err_lower.find("non-blocking") != std::string::npos ||
                err_lower.find("no data available for reading") != std::string::npos ||
                err_lower.find("resource temporarily unavailable") != std::string::npos ||
                err_lower.find("no connection available") != std::string::npos;

            if (!no_pending) {
                std::ostringstream oss;
                oss << "Failed to accept incoming SRT connection on port " << config.port
                    << " error=" << err;
                setLastError(oss.str());
                std::cerr << "[SRT] " << oss.str() << "\n";

                {
                    std::lock_guard<std::mutex> lock(socket_mutex);
                    if (listener_socket_ == listen_socket) {
                        listener_socket_ = SRT_INVALID_SOCK;
                    }
                }

                srt_close(listen_socket);
                return false;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex);
        if (listener_socket_ == listen_socket) {
            listener_socket_ = SRT_INVALID_SOCK;
        }
    }

    srt_close(listen_socket);
    setLastError("Listener accept aborted due to shutdown request.");
    std::cerr << "[SRT] Listener accept aborted due to shutdown request.\n";
    return false;
}

// Send muxed MPEG-TS bytes through the active SRT socket. FFmpeg custom IO can
// provide arbitrary chunk sizes, so this function payloadizes the byte stream
// into TS-aligned SRT messages before sending.
bool SRTStreamer::sendPacket(const unsigned char* data, int size)
{
    if (!data || size <= 0) {
        return true;
    }

    SRTSOCKET socket_snapshot = SRT_INVALID_SOCK;
    Config config_snapshot;

    {
        // Hold the lifecycle mutex only long enough to take a stable snapshot.
        // The actual srt_sendmsg() calls and short retry sleeps must not hold
        // this mutex; otherwise a congested socket can block shutdown, stats,
        // closeSocket(), or reconnect state transitions.
        std::lock_guard<std::mutex> lock(socket_mutex);
        socket_snapshot = srt_socket;
        config_snapshot = current_config_;
    }

    if (socket_snapshot == SRT_INVALID_SOCK) {
        app_send_failures_.fetch_add(1, std::memory_order_relaxed);
        setLastError("socket is invalid");
        std::cerr << "[SRT] Cannot send data, socket is invalid.\n";
        return false;
    }

    const SRT_SOCKSTATUS initialState = srt_getsockstate(socket_snapshot);
    if (initialState != SRTS_CONNECTED) {
        app_send_failures_.fetch_add(1, std::memory_order_relaxed);
        const std::string err = std::string("socket not connected: ") + socketStateToString(initialState);
        setLastError(err);
        std::cerr << "[SRT] Cannot send, socket_state=" << socketStateToString(initialState) << "\n";
        setState(ConnectionState::Failed);
        return false;
    }

    // Production safety: FFmpeg custom IO delivers arbitrary byte chunks.
    // Keep SRT messages aligned to complete MPEG-TS packets so the transport is
    // stable and receiver-friendly. With the normal 1316-byte payload this emits
    // exactly 7 TS packets per SRT message. Any trailing partial data is kept for
    // the next mux chunk instead of being sent as a short TS fragment.
    const int payload = normalizeMpegTsPayloadSize(config_snapshot.payload_size);

    // Extract all complete SRT payloads in one contiguous block. The previous
    // implementation allocated one std::vector per 1316-byte message and then
    // erased that message from the front of pending_ts_bytes_. Front-erasing
    // repeatedly shifts the entire remaining buffer and scales very poorly when
    // a high-bitrate video frame produces a large mux burst.
    //
    // Keep the exact same wire behavior: complete TS-aligned payloads are sent
    // outside tx_mutex_, while fewer than `payload` trailing bytes remain staged
    // for the next FFmpeg custom-IO chunk. This reduces the hot path to one
    // contiguous copy plus at most one small remainder move per sendPacket().
    std::vector<uint8_t> send_bytes;
    {
        std::lock_guard<std::mutex> lk(tx_mutex_);
        pending_ts_bytes_.insert(pending_ts_bytes_.end(), data, data + size);

        const size_t payload_size = static_cast<size_t>(payload);
        const size_t sendable_bytes =
            (pending_ts_bytes_.size() / payload_size) * payload_size;

        if (sendable_bytes > 0) {
            send_bytes.assign(
                pending_ts_bytes_.begin(),
                pending_ts_bytes_.begin() + static_cast<std::ptrdiff_t>(sendable_bytes));

            const size_t remainder_bytes = pending_ts_bytes_.size() - sendable_bytes;
            if (remainder_bytes == 0) {
                pending_ts_bytes_.clear();
            } else {
                std::memmove(
                    pending_ts_bytes_.data(),
                    pending_ts_bytes_.data() + sendable_bytes,
                    remainder_bytes);
                pending_ts_bytes_.resize(remainder_bytes);
            }
        }
    }

    for (size_t offset = 0; offset < send_bytes.size(); offset += static_cast<size_t>(payload)) {
        if (shouldStop(stop_requested_, external_stop_flag_)) {
            setLastError("send aborted due to shutdown request");
            return false;
        }

        const int chunk = payload;
        int sent = SRT_ERROR;
        std::string lastSendErr;

        for (int attempt = 0; attempt < 4; ++attempt) {
            sent = srt_sendmsg(
                socket_snapshot,
                reinterpret_cast<const char*>(send_bytes.data() + offset),
                chunk,
                -1,
                1
            );

            if (sent != SRT_ERROR) {
                break;
            }

            lastSendErr = srt_getlasterror_str();
            const SRT_SOCKSTATUS retryState = srt_getsockstate(socket_snapshot);
            if (retryState != SRTS_CONNECTED || !isTransientSendError(lastSendErr) ||
                shouldStop(stop_requested_, external_stop_flag_)) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (sent == SRT_ERROR) {
            app_send_failures_.fetch_add(1, std::memory_order_relaxed);
            const std::string err = lastSendErr.empty() ? srt_getlasterror_str() : lastSendErr;
            setLastError(err);
            std::cerr << "[SRT] send failed: " << err
                      << " class=" << classifyErrorText(err) << "\n";
            const SRT_SOCKSTATUS st = srt_getsockstate(socket_snapshot);
            std::cerr << "[SRT] socket_state=" << socketStateToString(st) << "\n";
            setState(ConnectionState::Failed);
            return false;
        }

        if (sent != chunk) {
            app_send_failures_.fetch_add(1, std::memory_order_relaxed);
            setLastError("send returned a partial MPEG-TS payload");
            std::cerr << "[SRT] send returned " << sent << " / " << chunk
                      << " bytes; aborting packet send to preserve TS payload alignment.\n";
            setState(ConnectionState::Failed);
            return false;
        }

        app_bytes_sent_.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
        app_msgs_sent_.fetch_add(1, std::memory_order_relaxed);

        // Measure successful application-level SRT send cadence. This is kept
        // deliberately lightweight: one monotonic-clock sample and relaxed
        // atomic aggregates per message. Do not log here; per-message logging
        // would itself perturb the high-bitrate send cadence we are measuring.
        const uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const uint64_t previous_ns =
            send_iat_prev_ns_.exchange(now_ns, std::memory_order_relaxed);

        if (previous_ns != 0 && now_ns >= previous_ns) {
            const uint64_t gap_ns = now_ns - previous_ns;
            send_iat_count_.fetch_add(1, std::memory_order_relaxed);
            send_iat_sum_ns_.fetch_add(gap_ns, std::memory_order_relaxed);
            updateAtomicMin(send_iat_min_ns_, gap_ns);
            updateAtomicMax(send_iat_max_ns_, gap_ns);

            if (gap_ns < 50000ULL) {
                send_iat_lt50us_.fetch_add(1, std::memory_order_relaxed);

                uint64_t burst_messages =
                    send_burst_lt50_current_.load(std::memory_order_relaxed);
                burst_messages = (burst_messages == 0) ? 2 : (burst_messages + 1);
                send_burst_lt50_current_.store(burst_messages, std::memory_order_relaxed);
                updateAtomicMax(send_burst_lt50_max_, burst_messages);
            } else {
                send_burst_lt50_current_.store(0, std::memory_order_relaxed);
            }

            if (gap_ns < 100000ULL) {
                send_iat_lt100us_.fetch_add(1, std::memory_order_relaxed);
            }
            if (gap_ns > 1000000ULL) {
                send_iat_gt1ms_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    return true;
}

void SRTStreamer::logSRTStats()
{
    uint64_t prev_app_bytes = 0;
    uint64_t prev_app_reconnects = 0;
    auto prev_ts = std::chrono::steady_clock::now();

    while (true) {
        SRTSOCKET socket_snapshot = SRT_INVALID_SOCK;
        int stats_interval_ms = 2000;

        {
            std::unique_lock<std::mutex> lock(socket_mutex);
            stats_interval_ms = (current_config_.stats_interval_ms > 0) ? current_config_.stats_interval_ms : 2000;
            if (stats_cv.wait_for(
                    lock,
                    std::chrono::milliseconds(stats_interval_ms),
                    [this]() { return !stats_running.load(std::memory_order_acquire); })) {
                break;
            }
            socket_snapshot = srt_socket;
        }

        const uint64_t app_bytes_sent = app_bytes_sent_.load(std::memory_order_relaxed);
        const uint64_t app_msgs_sent = app_msgs_sent_.load(std::memory_order_relaxed);
        const uint64_t app_send_failures = app_send_failures_.load(std::memory_order_relaxed);
        const uint64_t app_reconnects = app_reconnects_.load(std::memory_order_relaxed);

        const uint64_t iat_count = send_iat_count_.exchange(0, std::memory_order_relaxed);
        const uint64_t iat_sum_ns = send_iat_sum_ns_.exchange(0, std::memory_order_relaxed);
        const uint64_t iat_min_ns = send_iat_min_ns_.exchange(UINT64_MAX, std::memory_order_relaxed);
        const uint64_t iat_max_ns = send_iat_max_ns_.exchange(0, std::memory_order_relaxed);
        const uint64_t iat_lt50us = send_iat_lt50us_.exchange(0, std::memory_order_relaxed);
        const uint64_t iat_lt100us = send_iat_lt100us_.exchange(0, std::memory_order_relaxed);
        const uint64_t iat_gt1ms = send_iat_gt1ms_.exchange(0, std::memory_order_relaxed);
        const uint64_t burst_lt50_max =
            send_burst_lt50_max_.exchange(0, std::memory_order_relaxed);

        const double iat_avg_us = (iat_count > 0)
            ? (static_cast<double>(iat_sum_ns) / static_cast<double>(iat_count) / 1000.0)
            : 0.0;
        const double iat_min_us = (iat_count > 0 && iat_min_ns != UINT64_MAX)
            ? (static_cast<double>(iat_min_ns) / 1000.0)
            : 0.0;
        const double iat_max_us = (iat_count > 0)
            ? (static_cast<double>(iat_max_ns) / 1000.0)
            : 0.0;
        const double iat_lt50_pct = (iat_count > 0)
            ? (100.0 * static_cast<double>(iat_lt50us) / static_cast<double>(iat_count))
            : 0.0;
        const double iat_lt100_pct = (iat_count > 0)
            ? (100.0 * static_cast<double>(iat_lt100us) / static_cast<double>(iat_count))
            : 0.0;

        const auto now = std::chrono::steady_clock::now();
        if (app_reconnects != prev_app_reconnects) {
            prev_app_bytes = app_bytes_sent;
            prev_ts = now;
            prev_app_reconnects = app_reconnects;
            continue;
        }
        const double seconds = std::chrono::duration<double>(now - prev_ts).count();
        const uint64_t delta_bytes = app_bytes_sent - prev_app_bytes;
        const double mbps = (seconds > 0.0) ? (static_cast<double>(delta_bytes) * 8.0 / 1000000.0 / seconds) : 0.0;

        uint64_t sent_pkts = 0;
        uint64_t rtx_pkts = 0;
        uint64_t lost_pkts = 0;
        uint64_t drop_pkts = 0;
        double rtt_ms = 0.0;
        double bandwidth_mbps = 0.0;
        uint64_t sndbuf_pkts = 0;
        uint64_t sndbuf_bytes = 0;
        uint64_t sndbuf_ms = 0;
        uint64_t sndbuf_avail_bytes = 0;
        bool dashboardSampleValid = false;
        const char* socket_state = "INVALID";

        if (socket_snapshot != SRT_INVALID_SOCK) {
            socket_state = socketStateToString(srt_getsockstate(socket_snapshot));

            SRT_TRACEBSTATS stats;
            std::memset(&stats, 0, sizeof(stats));
            if (srt_bstats(socket_snapshot, &stats, 0) != SRT_ERROR) {
                dashboardSampleValid = true;
                sent_pkts = static_cast<uint64_t>(stats.pktSentTotal ? stats.pktSentTotal : stats.pktSent);
                rtx_pkts = static_cast<uint64_t>(stats.pktRetransTotal ? stats.pktRetransTotal : stats.pktRetrans);
                lost_pkts = static_cast<uint64_t>(stats.pktSndLossTotal ? stats.pktSndLossTotal : stats.pktSndLoss);
                drop_pkts = static_cast<uint64_t>(stats.pktSndDropTotal ? stats.pktSndDropTotal : stats.pktSndDrop);
                rtt_ms = stats.msRTT;
                bandwidth_mbps = stats.mbpsBandwidth;
                sndbuf_pkts = static_cast<uint64_t>(std::max(0, stats.pktSndBuf));
                sndbuf_bytes = static_cast<uint64_t>(std::max(0, stats.byteSndBuf));
                sndbuf_ms = static_cast<uint64_t>(std::max(0, stats.msSndBuf));
                sndbuf_avail_bytes = static_cast<uint64_t>(std::max(0, stats.byteAvailSndBuf));
            }
        }

        nxframe::senderDashboard().update([&](nxframe::DashboardState& d) {
            d.network=connectionStateToString(getState()); d.networkSample=dashboardSampleValid;
            d.networkUpdated=now; d.rtt=rtt_ms; d.sendBufferMs=sndbuf_ms;
            d.lost=lost_pkts; d.retransmitted=rtx_pkts; d.dropped=drop_pkts; d.reconnects=app_reconnects;
        });
        if (!nxframe::senderDashboard().active() || nxframe::senderDashboard().verbose()) std::cout << "[SRT] stats"
                  << " bitrate_mbps=" << mbps
                  << " bytes_sent=" << app_bytes_sent
                  << " msgs_sent=" << app_msgs_sent
                  << " sent_pkts=" << sent_pkts
                  << " rtx_pkts=" << rtx_pkts
                  << " lost_pkts=" << lost_pkts
                  << " drop_pkts=" << drop_pkts
                  << " rtt_ms=" << rtt_ms
                  << " bandwidth_mbps=" << bandwidth_mbps
                  << " sndbuf_pkts=" << sndbuf_pkts
                  << " sndbuf_bytes=" << sndbuf_bytes
                  << " sndbuf_ms=" << sndbuf_ms
                  << " sndbuf_avail_bytes=" << sndbuf_avail_bytes
                  << " tx_iat_avg_us=" << iat_avg_us
                  << " tx_iat_min_us=" << iat_min_us
                  << " tx_iat_max_us=" << iat_max_us
                  << " tx_iat_lt50_pct=" << iat_lt50_pct
                  << " tx_iat_lt100_pct=" << iat_lt100_pct
                  << " tx_iat_gt1ms=" << iat_gt1ms
                  << " tx_burst_lt50_max=" << burst_lt50_max
                  << " send_failures=" << app_send_failures
                  << " reconnects=" << app_reconnects
                  << " state=" << connectionStateToString(getState())
                  << " socket_state=" << socket_state
                  << "\n";

        publishTelemetry(mbps,
                         app_bytes_sent,
                         app_msgs_sent,
                         sent_pkts,
                         rtx_pkts,
                         lost_pkts,
                         drop_pkts,
                         rtt_ms,
                         bandwidth_mbps,
                         sndbuf_pkts,
                         sndbuf_bytes,
                         sndbuf_ms,
                         sndbuf_avail_bytes,
                         app_send_failures,
                         app_reconnects,
                         connectionStateToString(getState()),
                         socket_state);

        prev_app_bytes = app_bytes_sent;
        prev_app_reconnects = app_reconnects;
        prev_ts = now;
    }
}

bool SRTStreamer::reconnect(const std::string& address, int port, int attempts, int latency)
{
    Config config = current_config_;
    config.address = address;
    config.port = port;
    config.latency = latency;
    return reconnect(config, attempts);
}

// Reconnect by creating a new socket from the stored runtime configuration.
// Existing sockets are closed first so recovery never writes to a stale handle.
bool SRTStreamer::reconnect(const Config& config, int attempts)
{
    if (attempts <= 0) {
        attempts = (config.reconnect_attempts > 0) ? config.reconnect_attempts : 1;
    }

    const bool reconnect_forever = config.reconnect_forever;
    int delay_ms = std::max(250, config.reconnect_backoff_ms);
    const int max_delay_ms = std::max(delay_ms, config.reconnect_backoff_max_ms);

    int i = 0;
    while (!shouldStop(stop_requested_, external_stop_flag_) && (reconnect_forever || i < attempts)) {
        setState(ConnectionState::Reconnecting);

        {
            std::ostringstream reconnect_log;
            reconnect_log << "[SRT] Reconnect attempt " << (i + 1);
            if (!reconnect_forever) {
                reconnect_log << " / " << attempts;
            }
            reconnect_log << " mode=" << modeToString(config.mode)
                          << " target=" << config.address << ":" << config.port;
            std::cerr << reconnect_log.str() << "\n";
        }

        if (init(config)) {
            app_reconnects_.fetch_add(1, std::memory_order_relaxed);
            std::cout << "[SRT] Reconnected successfully.\n";
            return true;
        }

        ++i;
        if (!reconnect_forever && i >= attempts) {
            break;
        }

        if (shouldStop(stop_requested_, external_stop_flag_)) {
            break;
        }

        std::cerr << "[SRT] Reconnect failed, waiting " << delay_ms << " ms"
                  << " last_error='" << getLastError() << "'\n";

        int waited_ms = 0;
        while (!shouldStop(stop_requested_, external_stop_flag_) && waited_ms < delay_ms) {
            const int slice_ms = std::min(100, delay_ms - waited_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice_ms));
            waited_ms += slice_ms;
        }

        delay_ms = std::min(delay_ms * 2, max_delay_ms);
    }

    if (shouldStop(stop_requested_, external_stop_flag_)) {
        setState(ConnectionState::Disconnected);
        std::cerr << "[SRT] Reconnect aborted due to shutdown request.\n";
        return false;
    }

    setState(ConnectionState::Failed);
    if (reconnect_forever) {
        std::cerr << "[SRT] Reconnect loop ended unexpectedly. last_error='"
                  << getLastError() << "'\n";
    } else {
        std::cerr << "[SRT] Failed to reconnect after " << attempts
                  << " attempts. last_error='" << getLastError() << "'\n";
    }
    return false;
}