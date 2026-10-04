#include "core/sender_dashboard.h"
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
#include "core/metadata_tracker.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
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
                                     std::atomic<bool>& encodedVideoDiscontinuity,
                                     const Config& config)
    : encoder_(encoder),
      videoQ_(videoQ),
      audioQ_(audioQ),
      videoPktQ_(videoPktQ),
      audioPktQ_(audioPktQ),
      telemetry_(telemetry),
      stop_(stop),
      transportRecovering_(transportRecovering),
      encodedVideoDiscontinuity_(encodedVideoDiscontinuity),
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
    nxframe::FrameMetadataTracker metadataTracker;
    uint64_t captionMetadataAtEncodedBoundary = 0;
    bool metadataAssociationWarningLogged = false;

    using diag_clock = std::chrono::steady_clock;
    diag_clock::time_point diagLastInputFrame{};
    diag_clock::time_point diagLastEncodedPacket{};
    double diagMaxInputGapMs = 0.0;
    double diagMaxEncodeMs = 0.0;
    double diagMaxEncodedGapMs = 0.0;
    uint64_t diagInputGapGt40 = 0;
    uint64_t diagEncodeGt40 = 0;
    uint64_t diagEncodedGapGt40 = 0;
    uint64_t diagFrames = 0;
    uint64_t diagPackets = 0;
    auto diagPeriodStart = diag_clock::now();

    std::vector<AVPacketPtr> vpkts;
    vpkts.reserve(4);
    while (!stop_.stop_requested()) {
        VideoFrame vf;
        {
            stage_timing::ScopedTimer timer(waitStat);
            if (!videoQ_.pop(vf)) {
                break;
            }
        }

        const auto diagInputNow = diag_clock::now();
        if (diagLastInputFrame != diag_clock::time_point{}) {
            const double gapMs = std::chrono::duration<double, std::milli>(diagInputNow - diagLastInputFrame).count();
            diagMaxInputGapMs = std::max(diagMaxInputGapMs, gapMs);
            if (gapMs > 40.0) {
                ++diagInputGapGt40;
                if (stage_timing::verbose_enabled() || nxframe::senderDashboard().diagnosticsEnabled()) std::cerr << "[VideoEncodeWorker][DIAG] input frame gap_ms=" << gapMs
                          << " input_pts=" << vf.pts
                          << " raw_vq=" << videoQ_.size()
                          << " enc_vq=" << videoPktQ_.size()
                          << " enc_aq=" << audioPktQ_.size() << "\n";
            }
        }
        diagLastInputFrame = diagInputNow;
        ++diagFrames;
        nxframe::senderDashboard().update([&](nxframe::DashboardState& d) {
            if(d.source=="Test generator") {
                d.inputSample=true; d.inputUpdated=std::chrono::steady_clock::now();
                d.input=std::to_string(vf.width)+"x"+std::to_string(vf.height)+(vf.interlaced?" interlaced":" progressive");
                const char* pf=av_get_pix_fmt_name(vf.pix_fmt); d.internal=pf?pf:"--";
            }
        });

        stage_timing::ScopedTimer stageTimer(stageStat);
        telemetry_.observeQueues(videoQ_.size(), audioQ_.size(), videoPktQ_.size(), audioPktQ_.size());

        validateVideoInputAgainstEncoderOnce(vf, encoder_.getVideoCodecContext(), timingValidationDone_);

        // Preserve metadata by input PTS because the encoder may buffer pictures.
        metadataTracker.remember(vf.pts, vf.metadata);

        vpkts.clear();
        const auto diagEncodeStart = diag_clock::now();
        if (vf.queueEntered != diag_clock::time_point{}) {
            telemetry_.videoQueueDelay.record(
                std::chrono::duration<double,std::milli>(diagEncodeStart-vf.queueEntered).count());
        }

        if (!config_.forceCopy && vf.buffer && vf.buffer_size > 0) {
            {
                stage_timing::ScopedTimer timer(zcStat);
                encoder_.encodeVideoFramePackets(vf, vpkts);
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

        const auto diagEncodeEnd = diag_clock::now();
        const double encodeMs = std::chrono::duration<double, std::milli>(diagEncodeEnd - diagEncodeStart).count();
        diagMaxEncodeMs = std::max(diagMaxEncodeMs, encodeMs);
        telemetry_.encodeCalls.fetch_add(1,std::memory_order_relaxed);
        const auto* ctx=encoder_.getVideoCodecContext();
        if(ctx && ctx->framerate.num>0 && encodeMs>1000.0*ctx->framerate.den/ctx->framerate.num)
            telemetry_.encodeOverBudget.fetch_add(1,std::memory_order_relaxed);
        if (encodeMs > 40.0) {
            ++diagEncodeGt40;
            if (stage_timing::verbose_enabled() || nxframe::senderDashboard().diagnosticsEnabled()) std::cerr << "[VideoEncodeWorker][DIAG] slow encode_ms=" << encodeMs
                      << " input_pts=" << vf.pts
                      << " packets=" << vpkts.size()
                      << " raw_vq=" << videoQ_.size()
                      << " enc_vq=" << videoPktQ_.size() << "\n";
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

            const auto diagPacketNow = diag_clock::now();
            if (diagLastEncodedPacket != diag_clock::time_point{}) {
                const double gapMs = std::chrono::duration<double, std::milli>(diagPacketNow - diagLastEncodedPacket).count();
                diagMaxEncodedGapMs = std::max(diagMaxEncodedGapMs, gapMs);
                if (gapMs > 40.0) {
                    ++diagEncodedGapGt40;
                    if (stage_timing::verbose_enabled() || nxframe::senderDashboard().diagnosticsEnabled()) std::cerr << "[VideoEncodeWorker][DIAG] encoded packet gap_ms=" << gapMs
                              << " pkt_pts=" << vpkt->pts
                              << " pkt_dts=" << vpkt->dts
                              << " key=" << ((vpkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0)
                              << " raw_vq=" << videoQ_.size()
                              << " enc_vq=" << videoPktQ_.size() << "\n";
                }
            }
            diagLastEncodedPacket = diagPacketNow;
            ++diagPackets;

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
            const int64_t packetPts = out.pkt ? out.pkt->pts : AV_NOPTS_VALUE;
            if (packetPts == AV_NOPTS_VALUE ||
                !metadataTracker.take(packetPts, out.metadata)) {
                out.metadata.clear();
                if (!metadataAssociationWarningLogged) {
                    metadataAssociationWarningLogged = true;
                    std::cerr << "[VideoEncodeWorker][Metadata] WARNING: unable to associate "
                                 "encoded packet with input metadata"
                              << " packet_pts=" << packetPts
                              << " current_input_pts=" << vf.pts
                              << ". Metadata omitted rather than attached to the wrong picture.\n";
                }
            }

            if (out.metadata.hasCaption()) {
                ++captionMetadataAtEncodedBoundary;
                if (captionMetadataAtEncodedBoundary == 1u ||
                    (captionMetadataAtEncodedBoundary % 250u) == 0u) {
                    std::cout << "[VideoEncodeWorker][CC] metadata reached encoded boundary"
                              << " packets=" << captionMetadataAtEncodedBoundary
                              << " pts=" << packetPts
                              << " sequence=" << out.metadata.caption.sequence
                              << " cdp_bytes=" << out.metadata.caption.cdp_bytes.size()
                              << " cc_count=" << out.metadata.caption.cc_data.size()
                              << " pending_metadata=" << metadataTracker.size()
                              << "\n";
                }
            }

            if (transportRecovering_.load(std::memory_order_acquire)) {
                telemetry_.dropVideoWhileRecovering.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            {
                stage_timing::ScopedTimer timer(pushStat);
                // Never remove an already-queued compressed video packet. A queued
                // P/B packet can be a reference for later pictures, so DropOldest
                // can create a syntactically transportable but undecodable GOP.
                //
                // Give a short pacing burst time to clear. If the encoded queue is
                // still full, reject this newest packet and request a clean local
                // live-session recovery. The output thread will drain both encoded
                // queues, reset MPEG-TS timestamp/session state, request a fresh
                // keyframe, and resume only from that keyframe.
                const QueuePushResult pushResult =
                    videoPktQ_.push_for_with_policy(std::move(out),
                                                    std::chrono::milliseconds(100),
                                                    QueueOverflowPolicy::DropNewest);
                if (pushResult == QueuePushResult::Stopped) {
                    telemetry_.pushFailVideoPkt.fetch_add(1, std::memory_order_relaxed);
                    videoPushFailed = true;
                    break;
                }
                if (pushResult == QueuePushResult::DroppedNewest) {
                    telemetry_.dropVideoPktBackpressure.fetch_add(1, std::memory_order_relaxed);

                    // Gate encoder workers immediately so no more compressed video
                    // or audio is queued behind a known video discontinuity. The
                    // dedicated flag tells OutputManager this is a local encoded
                    // backpressure recovery, not a transport reconnect.
                    transportRecovering_.store(true, std::memory_order_release);
                    encodedVideoDiscontinuity_.store(true, std::memory_order_release);
                    std::cerr << "[VideoEncodeWorker] Encoded video queue saturated. "
                              << "Dropping newest packet and requesting clean keyframe recovery.\n";
                    break;
                }
            }
        }

        if (videoPushFailed) {
            break;
        }

        telemetry_.observeQueues(videoQ_.size(), audioQ_.size(), videoPktQ_.size(), audioPktQ_.size());

        const auto diagNow = diag_clock::now();
        if (diagNow - diagPeriodStart >= std::chrono::seconds(2)) {
            if (stage_timing::verbose_enabled() || nxframe::senderDashboard().diagnosticsEnabled()) std::cout << "[VideoEncodeWorker][DIAG] cadence frames=" << diagFrames
                      << " packets=" << diagPackets
                      << " max_input_gap_ms=" << diagMaxInputGapMs
                      << " input_gap_gt40=" << diagInputGapGt40
                      << " max_encode_ms=" << diagMaxEncodeMs
                      << " encode_gt40=" << diagEncodeGt40
                      << " max_encoded_gap_ms=" << diagMaxEncodedGapMs
                      << " encoded_gap_gt40=" << diagEncodedGapGt40
                      << " raw_vq=" << videoQ_.size()
                      << " enc_vq=" << videoPktQ_.size() << "\n";
            diagPeriodStart = diagNow;
            diagFrames = 0;
            diagPackets = 0;
            diagMaxInputGapMs = 0.0;
            diagMaxEncodeMs = 0.0;
            diagMaxEncodedGapMs = 0.0;
            diagInputGapGt40 = 0;
            diagEncodeGt40 = 0;
            diagEncodedGapGt40 = 0;
        }
    }

    // libx264 may retain delayed pictures when frame reordering is enabled
    // (for example B-frames). Always drain the codec before the worker exits
    // so it is left in a clean state. During a natural end-of-stream, forward
    // those delayed packets to the muxer. During an externally requested live
    // shutdown the packet queues are already being stopped, so drain and
    // discard instead of trying to publish packets into a closing transport.
    std::vector<AVPacketPtr> flushed = encoder_.flushVideo();
    const bool publishFlushed = !stop_.stop_requested();
    const std::size_t flushedPacketCount = flushed.size();
    std::size_t publishedFlushedPacketCount = 0;
    for (AVPacketPtr& vpkt : flushed) {
        if (!vpkt) {
            continue;
        }

        if (!publishFlushed) {
            continue;
        }

        telemetry_.encVideo.fetch_add(1, std::memory_order_relaxed);

        EncodedPacket out;
        out.pkt = std::move(vpkt);
        out.isVideo = true;
        out.pts = out.pkt->pts;
        out.dts = out.pkt->dts;
        out.duration = (out.pkt->duration > 0) ? out.pkt->duration : 1;
        if (encoder_.getVideoCodecContext()) {
            out.time_base = encoder_.getVideoCodecContext()->time_base;
        }

        if (!metadataTracker.take(out.pts, out.metadata)) {
            out.metadata.clear();
        }

        if (transportRecovering_.load(std::memory_order_acquire)) {
            telemetry_.dropVideoWhileRecovering.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const QueuePushResult pushResult =
            videoPktQ_.push_for_with_policy(std::move(out),
                                            std::chrono::milliseconds(100),
                                            QueueOverflowPolicy::DropNewest);
        if (pushResult == QueuePushResult::Stopped) {
            telemetry_.pushFailVideoPkt.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (pushResult == QueuePushResult::DroppedNewest) {
            telemetry_.dropVideoPktBackpressure.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        ++publishedFlushedPacketCount;
    }

    if (flushedPacketCount > 0 ||
        (encoder_.getVideoCodecContext() && encoder_.getVideoCodecContext()->max_b_frames > 0)) {
        std::cout << "[VideoEncodeWorker] Video encoder drain complete"
                  << " packets=" << flushedPacketCount
                  << " published=" << publishedFlushedPacketCount
                  << " discarded=" << (flushedPacketCount - publishedFlushedPacketCount)
                  << " stop_requested=" << (stop_.stop_requested() ? "yes" : "no")
                  << " max_b_frames="
                  << (encoder_.getVideoCodecContext()
                          ? encoder_.getVideoCodecContext()->max_b_frames
                          : 0)
                  << "\n";
    }

    done_.store(true, std::memory_order_release);
}
