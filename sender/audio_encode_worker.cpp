/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: sender/audio_encode_worker.cpp
 * Description: Implements the audio encode worker and audio packet handoff.
 */

#include "sender/audio_encode_worker.h"

#include <exception>
#include <chrono>
#include <iostream>

#include "stage_timing.h"

AudioEncodeWorker::AudioEncodeWorker(EncoderManager& encoder,
                                     BoundedQueue<VideoFrame>& videoQ,
                                     BoundedQueue<AudioFrame>& audioQ,
                                     BoundedQueue<EncodedPacket>& videoPktQ,
                                     BoundedQueue<EncodedPacket>& audioPktQ,
                                     PipelineTelemetry& telemetry,
                                     StopToken& stop,
                                     std::atomic<bool>& transportRecovering)
    : encoder_(encoder),
      videoQ_(videoQ),
      audioQ_(audioQ),
      videoPktQ_(videoPktQ),
      audioPktQ_(audioPktQ),
      telemetry_(telemetry),
      stop_(stop),
      transportRecovering_(transportRecovering),
      audioCodecContexts_(encoder.getAudioCodecContexts()),
      audioOutputMayBuffer_(encoder.audioOutputMayBuffer())
{
}

AudioEncodeWorker::~AudioEncodeWorker()
{
    join();
}

void AudioEncodeWorker::start()
{
    done_.store(false, std::memory_order_release);
    thread_ = std::thread(&AudioEncodeWorker::run, this);
}

void AudioEncodeWorker::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool AudioEncodeWorker::done() const
{
    return done_.load(std::memory_order_acquire);
}

AVRational AudioEncodeWorker::audioTimeBaseForPacket(const AVPacket* pkt) const
{
    if (!pkt) {
        return AVRational{1, 48000};
    }
    const int logicalIndex = pkt->stream_index;
    if (logicalIndex >= 0 &&
        logicalIndex < static_cast<int>(audioCodecContexts_.size()) &&
        audioCodecContexts_[logicalIndex]) {
        return audioCodecContexts_[logicalIndex]->time_base;
    }
    if (AVCodecContext* actx = encoder_.getAudioCodecContext()) {
        return actx->time_base;
    }
    return AVRational{1, 48000};
}

bool AudioEncodeWorker::pushAudioPacket(AVPacket* rawPkt, bool countEncoded)
{
    if (!rawPkt) {
        return true;
    }

    if (countEncoded) {
        telemetry_.encAudio.fetch_add(1, std::memory_order_relaxed);
    }

    EncodedPacket out;
    out.pkt = makeOwnedAVPacket(rawPkt);
    out.isVideo = false;
    if (rawPkt->pts != AV_NOPTS_VALUE) out.pts = rawPkt->pts;
    if (rawPkt->dts != AV_NOPTS_VALUE) out.dts = rawPkt->dts;
    out.duration = rawPkt->duration;
    out.time_base = audioTimeBaseForPacket(rawPkt);
    const int logicalAudioIndex = rawPkt->stream_index;
    if (out.duration <= 0 &&
        logicalAudioIndex >= 0 &&
        logicalAudioIndex < static_cast<int>(audioCodecContexts_.size()) &&
        audioCodecContexts_[logicalAudioIndex] &&
        audioCodecContexts_[logicalAudioIndex]->frame_size > 0) {
        out.duration = audioCodecContexts_[logicalAudioIndex]->frame_size;
    }

    if (transportRecovering_.load(std::memory_order_acquire)) {
        telemetry_.dropAudioWhileRecovering.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    static stage_timing::StageStats& pushStat = stage_timing::get("audio_pkt_push");
    stage_timing::ScopedTimer timer(pushStat);
    // Encoded packet queues are live queues, but clean local startup/pacing
    // bursts should not immediately discard audio. Wait briefly for the muxer
    // to catch up, then apply the live drop-oldest policy only if the queue is
    // still saturated. This keeps shutdown/recovery bounded without creating
    // avoidable startup holes.
    const QueuePushResult pushResult =
        audioPktQ_.push_for_with_policy(std::move(out),
                                        std::chrono::milliseconds(100),
                                        QueueOverflowPolicy::DropOldest);
    if (pushResult == QueuePushResult::Stopped) {
        telemetry_.pushFailAudioPkt.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (pushResult == QueuePushResult::DroppedOldestAndPushed ||
        pushResult == QueuePushResult::DroppedNewest) {
        telemetry_.dropAudioPktBackpressure.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void AudioEncodeWorker::flushAudio(bool pushPackets)
{
    static stage_timing::StageStats& flushStat = stage_timing::get("audio_flush");
    stage_timing::ScopedTimer timer(flushStat);

    // Always flush the codec itself before the worker exits.  During normal
    // end-of-stream we forward delayed packets to the muxer.  During an
    // external shutdown the packet queues may already be stopped, so drain
    // libavcodec/libfdk-aac and discard the delayed packets instead of
    // leaving frames queued inside the encoder.
    std::vector<AVPacket*> flushed = encoder_.flushAudio();
    for (AVPacket* rawPkt : flushed) {
        if (!rawPkt) {
            continue;
        }

        if (!pushPackets) {
            av_packet_free(&rawPkt);
            continue;
        }

        if (!pushAudioPacket(rawPkt, false)) {
            av_packet_free(&rawPkt);
            break;
        }
    }
}

void AudioEncodeWorker::run()
{
    static stage_timing::StageStats& waitStat = stage_timing::get("audio_q_wait");
    static stage_timing::StageStats& stageStat = stage_timing::get("audio_stage");
    static stage_timing::StageStats& encodeStat = stage_timing::get("audio_encode");

    while (!stop_.stop_requested()) {
        AudioFrame af;
        {
            stage_timing::ScopedTimer timer(waitStat);
            if (!audioQ_.pop(af)) {
                break;
            }
        }

        stage_timing::ScopedTimer stageTimer(stageStat);
        telemetry_.observeQueues(videoQ_.size(), audioQ_.size(), videoPktQ_.size(), audioPktQ_.size());

        if (!af.buffer || af.buffer_size == 0) {
            telemetry_.idleAudio.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        bool producedAnyAudio = false;
        bool audioPushFailed = false;

        AVPacket* apkt = nullptr;
        {
            stage_timing::ScopedTimer timer(encodeStat);
            // Pass the full AudioFrame so audio encoders can use the real
            // SDI metadata: input channel count, container bytes/sample,
            // valid bits/sample and samples-per-channel.  This is critical for
            // AAC, which must convert DeckLink s32/24-valid PCM to normal PCM
            // instead of guessing from byte size.
            apkt = encoder_.encodeAudioFrame(af);
        }

        if (apkt) {
            producedAnyAudio = true;
            if (!pushAudioPacket(apkt)) {
                audioPushFailed = true;
            }
        }

        while (!audioPushFailed) {
            AVPacket* extra = nullptr;
            {
                stage_timing::ScopedTimer timer(encodeStat);
                extra = encoder_.drainAudioPacket();
            }
            if (!extra) {
                break;
            }
            producedAnyAudio = true;
            if (!pushAudioPacket(extra)) {
                audioPushFailed = true;
                break;
            }
        }

        if (audioPushFailed) {
            break;
        }

        if (!producedAnyAudio) {
            // AAC-LC and other frame-based audio encoders legitimately buffer
            // input chunks and emit packets only when a full codec access unit
            // is available (AAC-LC is 1024 samples).  Do not report these
            // normal buffered chunks as missing audio; reserve missAudio for
            // packetizers/codecs that should produce output on every input
            // audio chunk.
            if (!audioOutputMayBuffer_) {
                telemetry_.missAudio.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        telemetry_.observeQueues(videoQ_.size(), audioQ_.size(), videoPktQ_.size(), audioPktQ_.size());
    }

    // Flush even on external shutdown so frame-based audio encoders,
    // especially libfdk_aac, do not close with delayed frames still queued.
    // Only publish flushed packets when this is a natural queue drain rather
    // than a requested shutdown.
    flushAudio(!stop_.stop_requested());

    done_.store(true, std::memory_order_release);
}
