/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: sender/sender_pipeline.cpp
 * Description: Implements the sender pipeline lifecycle and worker coordination.
 */

#include "sender/sender_pipeline.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <thread>

#include "stage_timing.h"

SenderPipeline::SenderPipeline()
    : videoQ_(2),
      audioQ_(128),
      videoPktQ_(32),
      audioPktQ_(64)
{
}

SenderPipeline::~SenderPipeline()
{
    shutdown();
}

bool SenderPipeline::initialize(const Config& config)
{
    config_ = config;

    if (!inputManager_.init(config_.inputType, config_.cardInput, config_.allowTestFallback)) {
        std::cerr << "[Main] Failed to initialize input source.\n";
        return false;
    }

    encoder_ = EncoderManager::createEncoder(config_.presetFile);
    bool encoderReady = false;
    try {
        encoderReady = encoder_ && encoder_->initialize();
    } catch (const std::exception& e) {
        std::cerr << "[Main] Encoder initialization exception: " << e.what() << "\n";
        encoderReady = false;
    }
    if (!encoderReady) {
        std::cerr << "[Main] Encoder initialization failed.\n";
        inputManager_.stopCapture();
        return false;
    }

    OutputManager::SenderInitOptions outputOptions;
    outputOptions.tsDebug = config_.tsDebug;
    outputOptions.tsCapturePath = config_.tsCapturePath;
    outputOptions.externalStopFlag = config_.externalStopFlag;

    if (!outputManager_.initializeSender(config_.presetFile,
                                         config_.transport,
                                         config_.transportAddress,
                                         config_.transportPort,
                                         *encoder_,
                                         outputOptions)) {
        inputManager_.stopCapture();
        return false;
    }

    VideoEncodeWorker::Config videoConfig;
    videoConfig.forceCopy = config_.forceCopy;

    videoWorker_.reset(new VideoEncodeWorker(*encoder_,
                                             videoQ_,
                                             audioQ_,
                                             videoPktQ_,
                                             audioPktQ_,
                                             telemetry_,
                                             stop_,
                                             transportRecovering_,
                                             videoConfig));
    audioWorker_.reset(new AudioEncodeWorker(*encoder_,
                                             videoQ_,
                                             audioQ_,
                                             videoPktQ_,
                                             audioPktQ_,
                                             telemetry_,
                                             stop_,
                                             transportRecovering_));

    initialized_ = true;
    return true;
}

int SenderPipeline::run()
{
    if (!initialized_ || !encoder_) {
        return -1;
    }

    // The transport may not be connected yet, especially in SRT listener mode.
    // Start capture/encoding immediately, but gate encoded packet output until
    // OutputManager has a connected socket and has requested a fresh keyframe.
    transportRecovering_.store(true, std::memory_order_release);
    waitForFreshKeyframe_.store(true, std::memory_order_release);

    if (!inputManager_.startProducer(stop_, videoQ_, audioQ_, &telemetry_)) {
        std::cerr << "[Main] Failed to start producer thread.\n";
        outputManager_.shutdownSender();
        inputManager_.stopCapture();
        return -1;
    }
    producerStarted_ = true;

    videoWorker_->start();
    audioWorker_->start();

    if (!outputManager_.startSenderRuntime(videoPktQ_,
                                           audioPktQ_,
                                           *encoder_,
                                           telemetry_,
                                           stop_,
                                           videoWorker_->doneFlag(),
                                           audioWorker_->doneFlag(),
                                           transportRecovering_,
                                           waitForFreshKeyframe_)) {
        stop_.request_stop();
    } else {
        outputRuntimeStarted_ = true;
    }

    startPerfThread();

    while (!(config_.externalStopFlag && config_.externalStopFlag->load(std::memory_order_acquire)) &&
           !stop_.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    shutdown();
    return 0;
}

void SenderPipeline::startPerfThread()
{
    perfThread_ = std::thread([this]() {
        while (!stop_.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            telemetry_.report(videoQ_.size(), audioQ_.size(), videoPktQ_.size(), audioPktQ_.size());
            if (stage_timing::enabled()) {
                const std::string timing = stage_timing::report_and_reset();
                if (!timing.empty()) {
                    std::cout << "[TIMING] " << timing << std::endl;
                }
            }
        }
    });
}

void SenderPipeline::stopQueues()
{
    videoQ_.stop();
    audioQ_.stop();
    videoPktQ_.stop();
    audioPktQ_.stop();
}

void SenderPipeline::joinWorkers()
{
    if (videoWorker_) {
        videoWorker_->join();
    }
    if (audioWorker_) {
        audioWorker_->join();
    }
    if (perfThread_.joinable()) {
        perfThread_.join();
    }
}

void SenderPipeline::shutdown()
{
    stop_.request_stop();
    outputManager_.srtStreamer().requestStop();

    stopQueues();

    if (producerStarted_) {
        inputManager_.stopCapture();
        producerStarted_ = false;
    } else {
        inputManager_.stopCapture();
    }

    joinWorkers();

    if (outputRuntimeStarted_) {
        outputManager_.stopSenderRuntime();
        outputRuntimeStarted_ = false;
    }

    if (initialized_) {
        outputManager_.shutdownSender();
        initialized_ = false;
    }
}
