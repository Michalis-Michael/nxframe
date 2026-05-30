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
 * Encoded packet ownership and pooling utilities. This file wraps FFmpeg AVPacket objects for queue-safe RAII handoff between encoders, muxers, and transports.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "core/metadata.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

// AVPacket pool used by hot paths to avoid allocating/freeing packets for every frame.
class AVPacketPool;

struct AVPacketDeleter
{
    AVPacketPool* pool = nullptr;

    void operator()(AVPacket* pkt) const noexcept;
};

using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

// AVPacket pool used by hot paths to avoid allocating/freeing packets for every frame.
class AVPacketPool
{
public:
    // Lifetime invariant: every AVPacketPtr acquired from this pool must be
    // destroyed before the AVPacketPool itself is destroyed.  The custom
    // deleter intentionally returns packets to this pool to avoid hot-path
    // allocations in the live encoder/demuxer loops.
    explicit AVPacketPool(size_t preallocate = 0)
    {
        for (size_t i = 0; i < preallocate; ++i) {
            AVPacket* pkt = av_packet_alloc();
            if (pkt) {
                freePackets_.push(pkt);
                allPackets_.push_back(pkt);
            }
        }
    }

    ~AVPacketPool()
    {
        while (!freePackets_.empty()) {
            freePackets_.pop();
        }
        for (AVPacket* pkt : allPackets_) {
            if (pkt) {
                av_packet_free(&pkt);
            }
        }
        allPackets_.clear();
    }

    AVPacketPtr acquire()
    {
        std::lock_guard<std::mutex> lk(mtx_);

        AVPacket* pkt = nullptr;
        if (!freePackets_.empty()) {
            pkt = freePackets_.front();
            freePackets_.pop();
            av_packet_unref(pkt);
            pkt->pts = AV_NOPTS_VALUE;
            pkt->dts = AV_NOPTS_VALUE;
            pkt->duration = 0;
            pkt->pos = -1;
            pkt->stream_index = 0;
        } else {
            pkt = av_packet_alloc();
            if (pkt) {
                allPackets_.push_back(pkt);
            }
        }

        return AVPacketPtr(pkt, AVPacketDeleter{this});
    }

    void release(AVPacket* pkt) noexcept
    {
        if (!pkt) {
            return;
        }

        av_packet_unref(pkt);
        pkt->pts = AV_NOPTS_VALUE;
        pkt->dts = AV_NOPTS_VALUE;
        pkt->duration = 0;
        pkt->pos = -1;
        pkt->stream_index = 0;

        std::lock_guard<std::mutex> lk(mtx_);
        freePackets_.push(pkt);
    }

private:
    std::mutex mtx_;
    std::queue<AVPacket*> freePackets_;
    std::vector<AVPacket*> allPackets_;
};

inline void AVPacketDeleter::operator()(AVPacket* pkt) const noexcept
{
    if (!pkt) {
        return;
    }

    if (pool) {
        pool->release(pkt);
    } else {
        av_packet_free(&pkt);
    }
}

inline AVPacketPtr makeOwnedAVPacket(AVPacket* pkt) noexcept
{
    return AVPacketPtr(pkt, AVPacketDeleter{nullptr});
}

// Queue item passed from encoder workers to mux/output stages.
struct EncodedPacket
{
    AVPacketPtr pkt;
    bool isVideo = false;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    int64_t duration = 0;
    AVRational time_base{1, 90000};

    // Internal metadata sidecar. This is preserved across the sender
    // encode/output queues so muxers/transports can later map it to a
    // standards-compliant carriage instead of reparsing raw video.
    FrameMetadata metadata;
};