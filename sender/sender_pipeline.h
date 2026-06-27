/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: sender/sender_pipeline.h
 * Description: Declares the sender pipeline configuration and runtime ownership model.
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "core/bounded_queue.h"
#include "core/frame.h"
#include "core/packet_item.h"
#include "core/pipeline_telemetry.h"
#include "core/stop_token.h"
#include "encoders/encoder_manager.h"
#include "input/input_manager.h"
#include "output/output_manager.h"
#include "sender/audio_encode_worker.h"
#include "sender/video_encode_worker.h"

// Owns sender-mode startup, worker coordination and shutdown. Encoding,
// muxing and transport are delegated to their modules; this class only
// connects the live queues and lifecycle boundaries.
class SenderPipeline {
public:
    struct Config {
        std::string inputType;
        std::string cardInput;
        bool allowTestFallback = false;
        bool forceCopy = false;
        std::string presetFile;
        OutputManager::SenderTransport transport = OutputManager::SenderTransport::SRT;
        std::string transportAddress;
        int transportPort = 0;
        bool tsDebug = false;
        std::string tsCapturePath;
        const std::atomic<bool>* externalStopFlag = nullptr;
    };

    SenderPipeline();
    ~SenderPipeline();

    SenderPipeline(const SenderPipeline&) = delete;
    SenderPipeline& operator=(const SenderPipeline&) = delete;

    bool initialize(const Config& config);
    int run();
    void shutdown();

private:
    void startPerfThread();
    void stopQueues();
    void joinWorkers();

    Config config_;
    InputManager inputManager_;
    std::unique_ptr<EncoderManager> encoder_;
    OutputManager outputManager_;
    PipelineTelemetry telemetry_;

    BoundedQueue<VideoFrame> videoQ_;
    BoundedQueue<AudioFrame> audioQ_;
    BoundedQueue<EncodedPacket> videoPktQ_;
    BoundedQueue<EncodedPacket> audioPktQ_;

    StopToken stop_;
    std::atomic<bool> transportRecovering_{false};
    std::atomic<bool> waitForFreshKeyframe_{false};

    std::unique_ptr<VideoEncodeWorker> videoWorker_;
    std::unique_ptr<AudioEncodeWorker> audioWorker_;
    std::thread perfThread_;
    bool initialized_ = false;
    bool producerStarted_ = false;
    bool outputRuntimeStarted_ = false;
};
