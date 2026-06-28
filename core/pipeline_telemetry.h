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
 * Low-overhead live pipeline telemetry counters for frame rates, queue depth, drops, mux failures, recovery, and transport activity.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>
#include <algorithm>

// Shared counters are updated from multiple worker threads; use relaxed atomics because telemetry is observational only.
struct PipelineTelemetry
{
    std::atomic<uint64_t> inVideo{0};
    std::atomic<uint64_t> encVideo{0};
    std::atomic<uint64_t> inAudio{0};
    std::atomic<uint64_t> encAudio{0};
    // Successful payload bytes sent to the transport. Used for PERF send Mbps.
    std::atomic<uint64_t> sendBytes{0};
    // Bytes attempted before transport success/failure is known. Useful for diagnosing reconnects.
    std::atomic<uint64_t> attemptedSendBytes{0};

    // Encoder produced no packet for an input frame/chunk. This can be normal
    // during startup, codec lookahead/reordering, or buffered audio packetization,
    // so PERF reports it as per-interval enc_empty plus a cumulative total rather
    // than a live-frame-loss counter.
    std::atomic<uint64_t> missVideo{0};
    std::atomic<uint64_t> missAudio{0};

    std::atomic<uint64_t> pushFailVideo{0};
    std::atomic<uint64_t> pushFailAudio{0};
    std::atomic<uint64_t> pushFailVideoPkt{0};
    std::atomic<uint64_t> pushFailAudioPkt{0};

    std::atomic<uint64_t> muxFail{0};
    std::atomic<uint64_t> sendFail{0};

    // Recovery / keyframe-gating observability. These should stay low in normal operation.
    std::atomic<uint64_t> liveSessionResets{0};
    std::atomic<uint64_t> freshKeyframesAccepted{0};
    std::atomic<uint64_t> dropVideoWhileWaitingKeyframe{0};
    std::atomic<uint64_t> dropAudioWhileWaitingKeyframe{0};
    std::atomic<uint64_t> dropVideoWhileRecovering{0};
    std::atomic<uint64_t> dropAudioWhileRecovering{0};

    // Explicit live backpressure policy counters. Non-zero values mean NxFrame
    // preserved live latency by discarding stale encoded payload rather than
    // blocking an encoder worker indefinitely.
    std::atomic<uint64_t> dropVideoPktBackpressure{0};
    std::atomic<uint64_t> dropAudioPktBackpressure{0};

    // Muxer timestamp repair counters. Non-zero values indicate an upstream timing bug.
    std::atomic<uint64_t> muxVideoDtsRepairs{0};
    std::atomic<uint64_t> muxVideoPtsRepairs{0};

    std::atomic<uint64_t> idleVideo{0};
    std::atomic<uint64_t> idleAudio{0};
    std::atomic<uint64_t> idleMux{0};

    std::atomic<size_t> peakVideoQ{0};
    std::atomic<size_t> peakAudioQ{0};
    std::atomic<size_t> peakVideoPktQ{0};
    std::atomic<size_t> peakAudioPktQ{0};

    void observeQueues(size_t vq, size_t aq, size_t vpq, size_t apq)
    {
        updatePeak(peakVideoQ, vq);
        updatePeak(peakAudioQ, aq);
        updatePeak(peakVideoPktQ, vpq);
        updatePeak(peakAudioPktQ, apq);
    }

    // Print one interval report. The caller controls cadence, normally once per second.
    void report(size_t vq, size_t aq, size_t vpq, size_t apq)
    {
        const uint64_t curInV = inVideo.load(std::memory_order_relaxed);
        const uint64_t curEncV = encVideo.load(std::memory_order_relaxed);
        const uint64_t curInA = inAudio.load(std::memory_order_relaxed);
        const uint64_t curEncA = encAudio.load(std::memory_order_relaxed);
        const uint64_t curSendB = sendBytes.load(std::memory_order_relaxed);
        const uint64_t curMissV = missVideo.load(std::memory_order_relaxed);
        const uint64_t curMissA = missAudio.load(std::memory_order_relaxed);

        const uint64_t dInV = curInV - lastInVideo_;
        const uint64_t dEncV = curEncV - lastEncVideo_;
        const uint64_t dInA = curInA - lastInAudio_;
        const uint64_t dEncA = curEncA - lastEncAudio_;
        const uint64_t dMissV = curMissV - lastMissVideo_;
        const uint64_t dMissA = curMissA - lastMissAudio_;
        const double mbps = static_cast<double>(curSendB - lastSendBytes_) * 8.0 / 1000000.0;

        lastInVideo_ = curInV;
        lastEncVideo_ = curEncV;
        lastInAudio_ = curInA;
        lastEncAudio_ = curEncA;
        lastSendBytes_ = curSendB;
        lastMissVideo_ = curMissV;
        lastMissAudio_ = curMissA;

        std::cout
            << "[PERF] "
            << "in_vfps=" << dInV
            << " enc_vpps=" << dEncV
            << " in_afps=" << dInA
            << " enc_apps=" << dEncA
            << " send=" << mbps << "Mbps"
            << " q[v/a/vp/ap]="
            << vq << "/" << aq << "/" << vpq << "/" << apq
            << " peak="
            << peakVideoQ.load(std::memory_order_relaxed) << "/"
            << peakAudioQ.load(std::memory_order_relaxed) << "/"
            << peakVideoPktQ.load(std::memory_order_relaxed) << "/"
            << peakAudioPktQ.load(std::memory_order_relaxed)
            << " enc_empty[v/a]="
            << dMissV << "/"
            << dMissA
            << " enc_empty_total[v/a]="
            << curMissV << "/"
            << curMissA
            << " pushfail[v/a/vp/ap]="
            << pushFailVideo.load(std::memory_order_relaxed) << "/"
            << pushFailAudio.load(std::memory_order_relaxed) << "/"
            << pushFailVideoPkt.load(std::memory_order_relaxed) << "/"
            << pushFailAudioPkt.load(std::memory_order_relaxed)
            << " muxfail=" << muxFail.load(std::memory_order_relaxed)
            << " sendfail=" << sendFail.load(std::memory_order_relaxed)
            << " recovery[resets/key/dropv/dropa/recv/reca]="
            << liveSessionResets.load(std::memory_order_relaxed) << "/"
            << freshKeyframesAccepted.load(std::memory_order_relaxed) << "/"
            << dropVideoWhileWaitingKeyframe.load(std::memory_order_relaxed) << "/"
            << dropAudioWhileWaitingKeyframe.load(std::memory_order_relaxed) << "/"
            << dropVideoWhileRecovering.load(std::memory_order_relaxed) << "/"
            << dropAudioWhileRecovering.load(std::memory_order_relaxed)
            << " backpressure[v/a]="
            << dropVideoPktBackpressure.load(std::memory_order_relaxed) << "/"
            << dropAudioPktBackpressure.load(std::memory_order_relaxed)
            << " tsrepair[v_dts/v_pts]="
            << muxVideoDtsRepairs.load(std::memory_order_relaxed) << "/"
            << muxVideoPtsRepairs.load(std::memory_order_relaxed)
            << " attempted=" << attemptedSendBytes.load(std::memory_order_relaxed) << "B"
            << " idle[v/a/m]="
            << idleVideo.load(std::memory_order_relaxed) << "/"
            << idleAudio.load(std::memory_order_relaxed) << "/"
            << idleMux.load(std::memory_order_relaxed)
            << std::endl;
    }

private:
    uint64_t lastInVideo_{0};
    uint64_t lastEncVideo_{0};
    uint64_t lastInAudio_{0};
    uint64_t lastEncAudio_{0};
    uint64_t lastSendBytes_{0};
    uint64_t lastMissVideo_{0};
    uint64_t lastMissAudio_{0};

    static void updatePeak(std::atomic<size_t>& peak, size_t value)
    {
        size_t cur = peak.load(std::memory_order_relaxed);
        while (value > cur &&
               !peak.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
        }
    }
};