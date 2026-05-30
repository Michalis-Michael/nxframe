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
 * Sender-side input coordinator. InputManager selects the active source, exposes
 * the normalized input format/timing contract, and attaches that source to the
 * live sender queues. DeckLink capture uses the callback-attached path; the
 * generated test source uses a local producer thread.
 */

#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include "decklink.h"
#include "test_signal_generator.h"

#include "../core/frame.h"
#include "../core/bounded_queue.h"
#include "../core/stop_token.h"
#include "../core/pipeline_telemetry.h"

// Input source selector and sender-facing coordinator. The production SDI path
// is DeckLink -> normalized YUV422P10LE -> live queues. Test signal generation
// is intentionally separate and is used only when selected or explicitly allowed
// as a development fallback.
class InputManager
{
public:
    InputManager();
    ~InputManager();

    // Initialize the requested source. For DeckLink, fallback to the generated
    // source is disabled unless the caller explicitly allows it.
    bool init(const std::string& inputType, const std::string& cardInput, bool allowTestFallback = false);

    // Stops any active source and joins the synthetic producer thread when used.
    void stopCapture();

    // Legacy pull API used by older call sites. The queue-attached producer path
    // below is the preferred sender path for live operation.
    uint8_t* getFrame();
    uint8_t* getAudioFrame();
    int getAudioFrameBytes() const;

    AVPixelFormat getPixelFormat() const;

    bool isInitialized() const;
    bool videoFeedDetected() const;

    int getWidth() const { return width; }
    int getHeight() const { return height; }
    int getFPS() const { return fps; }
    AVRational getTimeBase() const { return timeBase; }
    bool isInterlaced() const { return interlaced; }
    bool isTopFieldFirst() const { return tff; }

    int getAudioSampleRate() const { return audioSampleRate; }
    int getAudioChannels() const { return audioChannels; }

    // Attach the selected source to the sender queues. DeckLink attaches its SDK
    // callback directly to the queues; test signal starts a controlled producer
    // thread that generates synthetic video/audio at the configured cadence.
    bool startProducer(StopToken& stop,
                       BoundedQueue<VideoFrame>& videoQ,
                       BoundedQueue<AudioFrame>& audioQ,
                       PipelineTelemetry* telemetry = nullptr);

private:
    void producerLoop(StopToken* stop,
                      BoundedQueue<VideoFrame>* videoQ,
                      BoundedQueue<AudioFrame>* audioQ,
                      PipelineTelemetry* telemetry);

    int parseDeviceIndex(const std::string& cardInput) const;

private:
    std::string inputType;
    bool useDecklink{false};

    std::unique_ptr<DeckLinkCapture> decklinkInput;
    std::unique_ptr<TestSignalGenerator> testSignal;

    AVPixelFormat detectedFormat{AV_PIX_FMT_NONE};

    int width{1920};
    int height{1080};
    int fps{50};
    AVRational timeBase{1, 50};
    bool interlaced{false};
    bool tff{true};

    int audioSampleRate{48000};
    int audioChannels{2};

    // Storage for the legacy pull API. The queue-attached path owns frame/audio
    // memory through VideoFrame / AudioFrame shared buffers instead.
    std::unique_ptr<uint8_t[]> lastVideoFrame;
    int lastVideoSize{0};

    std::vector<int16_t> audioBufS16;
    int lastAudioBytes{0};
    double audioFrac{0.0};

    std::thread producerThread;
    std::atomic<bool> producerRunning{false};
};

#endif