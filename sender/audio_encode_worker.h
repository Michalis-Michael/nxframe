/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: sender/audio_encode_worker.h
 * Description: Declares the audio encode worker and queue contracts.
 */

#pragma once

#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

#include "core/bounded_queue.h"
#include "core/frame.h"
#include "core/packet_item.h"
#include "core/pipeline_telemetry.h"
#include "core/stop_token.h"
#include "encoders/encoder_manager.h"

// Live audio worker. Audio encoders may emit zero, one, or many packets for
// each input chunk depending on codec framing and internal buffering.
class AudioEncodeWorker {
public:
    AudioEncodeWorker(EncoderManager& encoder,
                      BoundedQueue<VideoFrame>& videoQ,
                      BoundedQueue<AudioFrame>& audioQ,
                      BoundedQueue<EncodedPacket>& videoPktQ,
                      BoundedQueue<EncodedPacket>& audioPktQ,
                      PipelineTelemetry& telemetry,
                      StopToken& stop,
                      std::atomic<bool>& transportRecovering);

    AudioEncodeWorker(const AudioEncodeWorker&) = delete;
    AudioEncodeWorker& operator=(const AudioEncodeWorker&) = delete;

    ~AudioEncodeWorker();

    void start();
    void join();
    bool done() const;
    std::atomic<bool>& doneFlag() noexcept { return done_; }

private:
    AVRational audioTimeBaseForPacket(const AVPacket* pkt) const;
    bool pushAudioPacket(AVPacket* rawPkt, bool countEncoded = true);
    void flushAudio(bool pushPackets);
    void run();

    EncoderManager& encoder_;
    BoundedQueue<VideoFrame>& videoQ_;
    BoundedQueue<AudioFrame>& audioQ_;
    BoundedQueue<EncodedPacket>& videoPktQ_;
    BoundedQueue<EncodedPacket>& audioPktQ_;
    PipelineTelemetry& telemetry_;
    StopToken& stop_;
    std::atomic<bool>& transportRecovering_;
    std::vector<AVCodecContext*> audioCodecContexts_;
    bool audioOutputMayBuffer_ = false;
    std::atomic<bool> done_{false};
    std::thread thread_;
};
