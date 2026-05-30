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
 * Implementation of the sender-side input coordinator. This module keeps source
 * selection, input timing/format state, DeckLink queue attachment, and generated
 * test-signal production behind one small API for the sender pipeline.
 */

#include "input_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

InputManager::InputManager()
    : useDecklink(false),
      width(1920),
      height(1080),
      fps(50)
{
}

InputManager::~InputManager()
{
    stopCapture();
}

int InputManager::parseDeviceIndex(const std::string& cardInput) const
{
    if (cardInput.empty()) {
        return 0;
    }
    return std::atoi(cardInput.c_str());
}

bool InputManager::init(const std::string& inputType, const std::string& cardInput, bool allowTestFallback)
{
    // Reinitialization is allowed, but the previous source must be fully stopped
    // first so callbacks/producer threads cannot publish into stale queues.
    stopCapture();

    this->inputType = inputType;

    lastVideoFrame.reset();
    lastVideoSize = 0;
    audioBufS16.clear();
    lastAudioBytes = 0;
    audioFrac = 0.0;

    if (inputType == "decklink") {
        const int deviceIndex = parseDeviceIndex(cardInput);
        std::cout << "[InputManager] Initializing DeckLink input, device " << deviceIndex << "\n";

        decklinkInput = std::make_unique<DeckLinkCapture>();
        if (!decklinkInput->init(deviceIndex) || !decklinkInput->startCapture(bmdModeHD1080i50)) {
            std::cerr << "[InputManager] DeckLink init failed.\n";
            decklinkInput.reset();
            useDecklink = false;

            if (!allowTestFallback) {
                std::cerr << "[InputManager] Test signal fallback is disabled by default for decklink input. "
                          << "Use --allow-test-fallback to explicitly allow fallback during development.\n";
                return false;
            }

            std::cerr << "[InputManager] --allow-test-fallback enabled. Falling back to test signal.\n";
        } else {
            useDecklink = true;

            // DeckLinkCapture normalizes supported SDI input formats to the
            // sender-facing internal bus. Downstream stages should treat this
            // as the input contract, not as the raw card pixel format.
            detectedFormat = AV_PIX_FMT_YUV422P10LE;

            width      = decklinkInput->getWidth();
            height     = decklinkInput->getHeight();
            fps        = decklinkInput->getFPS();
            timeBase   = decklinkInput->getTimeBase();
            interlaced = decklinkInput->isInterlaced();
            tff        = decklinkInput->isTopFieldFirst();

            audioSampleRate = decklinkInput->getAudioSampleRate();
            audioChannels   = decklinkInput->getAudioChannels();

            std::cout << "[InputManager] DeckLink normalized to internal bus AV_PIX_FMT_YUV422P10LE\n";
            return true;
        }
    }

    std::cout << "[InputManager] Using test signal generator.\n";

    testSignal = std::make_unique<TestSignalGenerator>();

    // The generated source uses the same internal bus as DeckLink so encoder
    // tests exercise the same sender-facing frame layout.
    detectedFormat = AV_PIX_FMT_YUV422P10LE;

    timeBase   = AVRational{1, fps > 0 ? fps : 50};
    interlaced = false;
    tff        = true;

    audioSampleRate = 48000;
    audioChannels   = 2;

    return true;
}

AVPixelFormat InputManager::getPixelFormat() const
{
    return detectedFormat;
}

uint8_t* InputManager::getFrame()
{
    // Legacy pull API. The current sender hot path should use startProducer(),
    // which preserves frame ownership through the queue-attached VideoFrame API.
    if (useDecklink && decklinkInput) {
        width      = decklinkInput->getWidth();
        height     = decklinkInput->getHeight();
        fps        = decklinkInput->getFPS();
        timeBase   = decklinkInput->getTimeBase();
        interlaced = decklinkInput->isInterlaced();
        tff        = decklinkInput->isTopFieldFirst();

        return decklinkInput->getFrame();
    }

    if (testSignal) {
        // YUV422P10LE internal bus: Y + U + V planes total width*height*4 bytes.
        const int frameSize = width * height * 4;

        if (!lastVideoFrame || frameSize != lastVideoSize) {
            lastVideoFrame = std::make_unique<uint8_t[]>(frameSize);
            lastVideoSize = frameSize;
        }

        testSignal->generateFrame(lastVideoFrame.get(), width, height, 0);
        return lastVideoFrame.get();
    }

    return nullptr;
}

uint8_t* InputManager::getAudioFrame()
{
    // Legacy pull API. Audio data returned here remains owned by InputManager or
    // DeckLinkCapture until the next call. Queue-attached audio uses AudioFrame
    // ownership instead.
    lastAudioBytes = 0;

    if (useDecklink && decklinkInput) {
        uint8_t* p = decklinkInput->getAudioFrame();
        lastAudioBytes = decklinkInput->getAudioFrameBytes();
        return p;
    }

    if (testSignal) {
        if (fps <= 0) {
            fps = 50;
        }

        const double ideal = static_cast<double>(audioSampleRate) / static_cast<double>(fps);
        audioFrac += ideal;

        const int samplesThisFrame = static_cast<int>(audioFrac);
        audioFrac -= samplesThisFrame;

        if (samplesThisFrame <= 0) {
            return nullptr;
        }

        audioBufS16.resize(static_cast<size_t>(samplesThisFrame) * static_cast<size_t>(audioChannels));
        testSignal->generateAudioFrame(audioBufS16.data(), audioChannels, samplesThisFrame);

        lastAudioBytes = samplesThisFrame * audioChannels * 2;
        return reinterpret_cast<uint8_t*>(audioBufS16.data());
    }

    return nullptr;
}

int InputManager::getAudioFrameBytes() const
{
    return lastAudioBytes;
}

void InputManager::stopCapture()
{
    // Stop the synthetic producer first, then detach DeckLink queues so no source
    // can push frames after the caller begins tearing the pipeline down.
    producerRunning.store(false, std::memory_order_release);

    if (producerThread.joinable()) {
        producerThread.join();
    }

    if (decklinkInput) {
        decklinkInput->detachOutputQueues();
    }

    if (useDecklink && decklinkInput) {
        decklinkInput->stopCapture();
        decklinkInput.reset();
        useDecklink = false;
        std::cout << "[InputManager] DeckLink capture stopped.\n";
    }

    if (testSignal) {
        testSignal.reset();
        std::cout << "[InputManager] Test signal stopped.\n";
    }
}

bool InputManager::isInitialized() const
{
    return useDecklink ? (decklinkInput != nullptr) : (testSignal != nullptr);
}

bool InputManager::videoFeedDetected() const
{
    if (useDecklink && decklinkInput) {
        return decklinkInput->videoFeedDetected();
    }

    if (testSignal) {
        return true;
    }

    return false;
}

bool InputManager::startProducer(StopToken& stop,
                                 BoundedQueue<VideoFrame>& videoQ,
                                 BoundedQueue<AudioFrame>& audioQ,
                                 PipelineTelemetry* telemetry)
{
    if (producerThread.joinable()) {
        producerThread.join();
    }

    producerRunning.store(true, std::memory_order_release);

    if (useDecklink && decklinkInput) {
        // Production SDI path: no extra InputManager producer thread. The
        // Blackmagic callback publishes normalized, owned VideoFrame/AudioFrame
        // objects directly into the live queues.
        decklinkInput->attachOutputQueues(&videoQ, &audioQ, &stop, telemetry);
        std::cout << "[InputManager] DeckLink callback path attached directly to pipeline queues.\n";
        return true;
    }

    // Generated-source path: produce deterministic video/audio frames on a local
    // cadence thread. This path is useful for development and regression tests.
    producerThread = std::thread(&InputManager::producerLoop, this, &stop, &videoQ, &audioQ, telemetry);
    return true;
}

void InputManager::producerLoop(StopToken* stop,
                                BoundedQueue<VideoFrame>* videoQ,
                                BoundedQueue<AudioFrame>* audioQ,
                                PipelineTelemetry* telemetry)
{
    int64_t v_pts = 0;
    int64_t a_pts = 0;
    double audioFracLocal = 0.0;
    auto nextFrameAt = std::chrono::steady_clock::now();

    while (producerRunning.load(std::memory_order_acquire) && stop && !stop->stop_requested()) {
        if (fps <= 0) {
            fps = 25;
        }

        VideoFrame vf;
        vf.width      = width;
        vf.height     = height;
        vf.pix_fmt    = AV_PIX_FMT_YUV422P10LE;
        vf.time_base  = AVRational{1, fps};
        vf.pts        = v_pts++;
        vf.interlaced = interlaced;
        vf.tff        = tff;

        // Allocate one owned internal-bus buffer for the generated frame. This
        // mirrors the DeckLink normalized layout so downstream code sees the
        // same VideoFrame contract in both input modes.
        const size_t v_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        vf.buffer = make_shared_u8(v_bytes);
        vf.buffer_size = v_bytes;

        vf.data[0] = vf.buffer.get();
        vf.data[1] = vf.data[0] + static_cast<size_t>(width) * static_cast<size_t>(height) * 2u;
        vf.data[2] = vf.data[1] + static_cast<size_t>(width / 2) * static_cast<size_t>(height) * 2u;

        vf.linesize[0] = width * 2;
        vf.linesize[1] = (width / 2) * 2;
        vf.linesize[2] = (width / 2) * 2;

        bool gotVideo = false;

        if (testSignal) {
            testSignal->generateFrame(vf.buffer.get(), width, height, vf.linesize[0]);
            gotVideo = true;
        }

        if (gotVideo) {
            if (videoQ->push(std::move(vf))) {
                if (telemetry) {
                    telemetry->inVideo.fetch_add(1, std::memory_order_relaxed);
                    telemetry->observeQueues(videoQ->size(), audioQ->size(), 0, 0);
                }
            } else {
                if (telemetry) {
                    telemetry->pushFailVideo.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        } else {
            if (telemetry) {
                telemetry->idleVideo.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        AudioFrame af;
        bool gotAudio = false;

        if (testSignal) {
            const int sampleRate = audioSampleRate > 0 ? audioSampleRate : 48000;
            const int channels = audioChannels > 0 ? audioChannels : 2;
            const double ideal = static_cast<double>(sampleRate) / static_cast<double>(fps);
            audioFracLocal += ideal;

            const int samplesThisFrame = static_cast<int>(audioFracLocal);
            audioFracLocal -= samplesThisFrame;

            if (samplesThisFrame > 0) {
                // Test-signal audio is interleaved signed 16-bit PCM. The DeckLink
                // path may expose a different sample container, but both paths
                // publish explicit format metadata in AudioFrame.
                const int bytes = samplesThisFrame * channels * 2;

                af.buffer = make_shared_u8(static_cast<size_t>(bytes));
                af.buffer_size = static_cast<size_t>(bytes);
                af.sample_rate = sampleRate;
                af.channels = channels;
                af.bytes_per_sample = 2;
                af.num_samples = samplesThisFrame;
                af.pts = a_pts;
                af.time_base = AVRational{1, sampleRate};

                testSignal->generateAudioFrame(
                    reinterpret_cast<int16_t*>(af.buffer.get()),
                    channels,
                    samplesThisFrame
                );

                a_pts += samplesThisFrame;
                gotAudio = true;
            }
        }

        if (gotAudio) {
            if (audioQ->push(std::move(af))) {
                if (telemetry) {
                    telemetry->inAudio.fetch_add(1, std::memory_order_relaxed);
                    telemetry->observeQueues(videoQ->size(), audioQ->size(), 0, 0);
                }
            } else {
                if (telemetry) {
                    telemetry->pushFailAudio.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        } else {
            if (telemetry) {
                telemetry->idleAudio.fetch_add(1, std::memory_order_relaxed);
            }
        }

        const double fpsLocal = static_cast<double>(std::max(1, fps));
        const auto framePeriod = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / fpsLocal));
        nextFrameAt += framePeriod;

        const auto now = std::chrono::steady_clock::now();
        if (nextFrameAt > now) {
            std::this_thread::sleep_until(nextFrameAt);
        } else {
            // If frame generation or queue pressure overruns the cadence, resync
            // instead of accumulating a growing backlog of immediate wakeups.
            nextFrameAt = now;
        }
    }
}