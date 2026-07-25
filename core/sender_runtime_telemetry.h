/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared-memory sender telemetry record used between an NxFrame worker and
 * NxFrameWeb. The worker overwrites one fixed record; the GUI reads it only
 * when its existing sender status endpoint is requested.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace nxframe {

constexpr std::uint32_t kSenderTelemetryMagic = 0x4E585354U; // "NXST"
constexpr std::uint16_t kSenderTelemetryVersion = 1;

struct SenderRuntimeTelemetry {
    // Seqlock value: odd while the writer updates, even when stable.
    std::uint64_t sequence = 0;

    std::uint32_t magic = kSenderTelemetryMagic;
    std::uint16_t version = kSenderTelemetryVersion;
    std::uint16_t record_size = 0;

    std::uint64_t updated_monotonic_ms = 0;
    double bitrate_mbps = 0.0;

    std::uint64_t bytes_sent = 0;
    std::uint64_t messages_sent = 0;
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_retransmitted = 0;
    std::uint64_t packets_lost = 0;
    std::uint64_t packets_dropped = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t reconnects = 0;

    char connection_state[20]{};
    char socket_state[20]{};
};

static_assert(sizeof(SenderRuntimeTelemetry) < 256,
              "sender telemetry record must remain compact");

} // namespace nxframe
