/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: sender/video_encode_worker.h
 * Description: Declares the video encode worker and queue contracts.
 */

#pragma once

#include <atomic>
#include <thread>

#include "core/bounded_queue.h"
#include "core/frame.h"
#include "core/packet_item.h"
#include "core/pipeline_telemetry.h"
#include "core/stop_token.h"
#include "encoders/encoder_manager.h"

// Live video worker. The normal path preserves the VideoFrame shared buffer
// so the encoder can use the internal zero-copy-oriented frame handoff.
class VideoEncodeWorker {
public:
    struct Config {
        bool forceCopy = false;
    };

    VideoEncodeWorker(EncoderManager& encoder,
                      BoundedQueue<VideoFrame>& videoQ,
                      BoundedQueue<AudioFrame>& audioQ,
                      BoundedQueue<EncodedPacket>& videoPktQ,
                      BoundedQueue<EncodedPacket>& audioPktQ,
                      PipelineTelemetry& telemetry,
                      StopToken& stop,
                      std::atomic<bool>& transportRecovering,
                      const Config& config);

    VideoEncodeWorker(const VideoEncodeWorker&) = delete;
    VideoEncodeWorker& operator=(const VideoEncodeWorker&) = delete;

    ~VideoEncodeWorker();

    void start();
    void join();
    bool done() const;
    std::atomic<bool>& doneFlag() noexcept { return done_; }

private:
    void run();

    EncoderManager& encoder_;
    BoundedQueue<VideoFrame>& videoQ_;
    BoundedQueue<AudioFrame>& audioQ_;
    BoundedQueue<EncodedPacket>& videoPktQ_;
    BoundedQueue<EncodedPacket>& audioPktQ_;
    PipelineTelemetry& telemetry_;
    StopToken& stop_;
    std::atomic<bool>& transportRecovering_;
    Config config_;
    std::atomic<bool> timingValidationDone_{false};
    std::atomic<bool> done_{false};
    std::thread thread_;
};
