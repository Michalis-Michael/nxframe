/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: sender/video_encode_worker.cpp
 * Description: Implements the video encode worker and zero-copy-oriented frame handoff.
 */

#include "sender/video_encode_worker.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

#include "stage_timing.h"

namespace {

bool isCodecInterlaced(const AVCodecContext* ctx)
{
    if (!ctx) {
        return false;
    }
    return ctx->field_order == AV_FIELD_TT ||
           ctx->field_order == AV_FIELD_BB ||
           ctx->field_order == AV_FIELD_TB ||
           ctx->field_order == AV_FIELD_BT;
}

const char* fieldOrderName(AVFieldOrder order)
{
    switch (order) {
        case AV_FIELD_PROGRESSIVE: return "progressive";
        case AV_FIELD_TT: return "interlaced-tt";
        case AV_FIELD_BB: return "interlaced-bb";
        case AV_FIELD_TB: return "interlaced-tff";
        case AV_FIELD_BT: return "interlaced-bff";
        case AV_FIELD_UNKNOWN:
        default: return "unknown";
    }
}

void validateVideoInputAgainstEncoderOnce(const VideoFrame& vf,
                                          const AVCodecContext* ctx,
                                          std::atomic<bool>& done)
{
    if (!ctx) {
        return;
    }

    // DeckLink can publish a few startup frames before format detection/reconfiguration
    // has settled. Validate after the live frame clock has advanced a little so the
    // check compares the actual active SDI mode, not only the initial preferred mode.
    if (vf.pts >= 0 && vf.pts < 25) {
        return;
    }

    bool expected = false;
    if (!done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    const double inputFps = (vf.time_base.num > 0 && vf.time_base.den > 0)
        ? 1.0 / av_q2d(vf.time_base)
        : 0.0;
    double encoderFps = 0.0;
    if (ctx->framerate.num > 0 && ctx->framerate.den > 0) {
        encoderFps = av_q2d(ctx->framerate);
    } else if (ctx->time_base.num > 0 && ctx->time_base.den > 0) {
        encoderFps = 1.0 / av_q2d(ctx->time_base);
    }

    const bool sizeOk = (vf.width == ctx->width && vf.height == ctx->height);
    const bool fpsOk = (inputFps <= 0.0 || encoderFps <= 0.0)
        ? true
        : (std::abs(inputFps - encoderFps) < 0.01);
    const bool inputInterlaced = vf.interlaced;
    const bool encoderInterlaced = isCodecInterlaced(ctx);
    const bool scanOk = (inputInterlaced == encoderInterlaced) ||
                        (!inputInterlaced && ctx->field_order == AV_FIELD_UNKNOWN);

    if (sizeOk && fpsOk && scanOk) {
        std::cout << "[TimingValidation] Input matches encoder timing: "
                  << vf.width << "x" << vf.height
                  << " fps~" << inputFps
                  << " scan=" << (inputInterlaced ? (vf.tff ? "interlaced-tff" : "interlaced-bff") : "progressive")
                  << " encoder_tb=" << ctx->time_base.num << "/" << ctx->time_base.den
                  << " field_order=" << fieldOrderName(ctx->field_order)
                  << "\n";
        return;
    }

    std::cerr << "[TimingValidation] WARNING: input timing/format differs from encoder preset. "
              << "input=" << vf.width << "x" << vf.height
              << " fps~" << inputFps
              << " scan=" << (inputInterlaced ? (vf.tff ? "interlaced-tff" : "interlaced-bff") : "progressive")
              << " encoder=" << ctx->width << "x" << ctx->height
              << " fps~" << encoderFps
              << " field_order=" << fieldOrderName(ctx->field_order)
              << ". NxFrame will continue, but broadcast-safe operation requires the preset to match the SDI mode.\n";
}

} // namespace

VideoEncodeWorker::VideoEncodeWorker(EncoderManager& encoder,
                                     BoundedQueue<VideoFrame>& videoQ,
                                     BoundedQueue<AudioFrame>& audioQ,
                                     BoundedQueue<EncodedPacket>& videoPktQ,
                                     BoundedQueue<EncodedPacket>& audioPktQ,
                                     PipelineTelemetry& telemetry,
                                     StopToken& stop,
                                     std::atomic<bool>& transportRecovering,
                                     const Config& config)
    : encoder_(encoder),
      videoQ_(videoQ),
      audioQ_(audioQ),
      videoPktQ_(videoPktQ),
      audioPktQ_(audioPktQ),
      telemetry_(telemetry),
      stop_(stop),
      transportRecovering_(transportRecovering),
      config_(config)
{
}

VideoEncodeWorker::~VideoEncodeWorker()
{
    join();
}

void VideoEncodeWorker::start()
{
    done_.store(false, std::memory_order_release);
    timingValidationDone_.store(false, std::memory_order_release);
    thread_ = std::thread(&VideoEncodeWorker::run, this);
}

void VideoEncodeWorker::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool VideoEncodeWorker::done() const
{
    return done_.load(std::memory_order_acquire);
}

void VideoEncodeWorker::run()
{
    static stage_timing::StageStats& waitStat = stage_timing::get("video_q_wait");
    static stage_timing::StageStats& stageStat = stage_timing::get("video_stage");
    static stage_timing::StageStats& zcStat = stage_timing::get("video_encode_zc");
    static stage_timing::StageStats& copyStat = stage_timing::get("video_encode_copy");
    static stage_timing::StageStats& pushStat = stage_timing::get("video_pkt_push");

    while (!stop_.stop_requested()) {
        VideoFrame vf;
        {
            stage_timing::ScopedTimer timer(waitStat);
            if (!videoQ_.pop(vf)) {
                break;
            }
        }

        stage_timing::ScopedTimer stageTimer(stageStat);
        telemetry_.observeQueues(videoQ_.size(), audioQ_.size(), videoPktQ_.size(), audioPktQ_.size());

        validateVideoInputAgainstEncoderOnce(vf, encoder_.getVideoCodecContext(), timingValidationDone_);

        std::vector<AVPacketPtr> vpkts;

        if (!config_.forceCopy && vf.buffer && vf.buffer_size > 0) {
            {
                stage_timing::ScopedTimer timer(zcStat);
                vpkts = encoder_.encodeVideoFramePackets(vf);
            }
            // Do not re-submit the same frame through the copy path when the
            // encoder returns no packet (for example AVERROR(EAGAIN)).  Re-encoding
            // the same PTS can create duplicate input to x264 and hide timing bugs.
            // The explicit --copy path below remains available for debugging.
        } else {
            uint8_t* src = vf.buffer ? vf.buffer.get() : nullptr;
            if (!src) {
                src = encoder_.getBlackFrame();
            }
            stage_timing::ScopedTimer timer(copyStat);
            vpkts = encoder_.encodeFramePackets(src, vf.pts);
        }

        if (vpkts.empty()) {
            telemetry_.missVideo.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        bool videoPushFailed = false;
        for (AVPacketPtr& vpkt : vpkts) {
            if (!vpkt) {
                continue;
            }

            telemetry_.encVideo.fetch_add(1, std::memory_order_relaxed);

            EncodedPacket out;
            out.pkt = std::move(vpkt);
            out.isVideo = true;
            if (out.pkt) {
                out.pts = out.pkt->pts;
                out.dts = out.pkt->dts;
                out.duration = (out.pkt->duration > 0) ? out.pkt->duration : 1;
            }
            if (encoder_.getVideoCodecContext()) {
                out.time_base = encoder_.getVideoCodecContext()->time_base;
            }
            out.metadata = vf.metadata;

            if (transportRecovering_.load(std::memory_order_acquire)) {
                telemetry_.dropVideoWhileRecovering.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            {
                stage_timing::ScopedTimer timer(pushStat);
                // Do not block the live encoder indefinitely, but also do not
                // drop a video packet on a transient startup/pacing burst. Wait
                // briefly, then apply the live drop-oldest policy only if the
                // muxer/output side remains saturated.
                const QueuePushResult pushResult =
                    videoPktQ_.push_for_with_policy(std::move(out),
                                                    std::chrono::milliseconds(100),
                                                    QueueOverflowPolicy::DropOldest);
                if (pushResult == QueuePushResult::Stopped) {
                    telemetry_.pushFailVideoPkt.fetch_add(1, std::memory_order_relaxed);
                    videoPushFailed = true;
                    break;
                }
                if (pushResult == QueuePushResult::DroppedOldestAndPushed ||
                    pushResult == QueuePushResult::DroppedNewest) {
                    telemetry_.dropVideoPktBackpressure.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        if (videoPushFailed) {
            break;
        }

        telemetry_.observeQueues(videoQ_.size(), audioQ_.size(), videoPktQ_.size(), audioPktQ_.size());
    }

    done_.store(true, std::memory_order_release);
}
