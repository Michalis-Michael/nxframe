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
 * DeckLink SDI input declarations. This module owns Blackmagic DeckLink
 * capture setup, callback forwarding, input-mode state, frame/audio buffer
 * ownership, and the normalized sender-facing YUV422P10LE input contract.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <vector>
#include <chrono>
#include <condition_variable>
#include <thread>

extern "C" {
#include <libavutil/rational.h>
#include <libavutil/pixfmt.h>
}

#include "DeckLinkAPI.h"
#include "../core/frame.h"
#include "../core/frame_pool.h"
#include "../core/bounded_queue.h"
#include "../core/stop_token.h"
#include "../core/pipeline_telemetry.h"

class DeckLinkCapture;

// Thin COM callback wrapper. It keeps DeckLink SDK callbacks separated from
// the capture implementation and forwards events to DeckLinkCapture.
class DeckLinkCaptureCallback : public IDeckLinkInputCallback
{
public:
    explicit DeckLinkCaptureCallback(DeckLinkCapture* parent);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override;
    ULONG   STDMETHODCALLTYPE AddRef(void) override;
    ULONG   STDMETHODCALLTYPE Release(void) override;

    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(
        BMDVideoInputFormatChangedEvents events,
        IDeckLinkDisplayMode* mode,
        BMDDetectedVideoInputFormatFlags flags) override;

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
        IDeckLinkVideoInputFrame* videoFrame,
        IDeckLinkAudioInputPacket* audioPacket) override;

private:
    std::atomic<ULONG> m_refCount{1};
    DeckLinkCapture*   m_parent{nullptr};
};

// DeckLink SDI input source. Public methods expose the legacy latest-frame API
// and the current queue-attached sender API. The queue-attached path is the
// production path for low-latency sender operation.
class DeckLinkCapture
{
public:
    DeckLinkCapture();
    ~DeckLinkCapture();

    bool init(int deviceIndex);
    bool startCapture(BMDDisplayMode preferred);
    void stopCapture();

    uint8_t* getFrame();
    int      getFPS();

    uint8_t* getAudioFrame();
    int      getAudioFrameBytes();

    bool videoFeedDetected() const;
    int  getWidth() const;
    int  getHeight() const;

    AVRational getTimeBase() const;
    bool isInterlaced() const;
    bool isTopFieldFirst() const;

    int getAudioSampleRate() const { return 48000; }
    int getAudioChannels() const { return 16; }

    void attachOutputQueues(BoundedQueue<VideoFrame>* videoQ,
                            BoundedQueue<AudioFrame>* audioQ,
                            StopToken* stop,
                            PipelineTelemetry* telemetry);
    void detachOutputQueues();

    void requestReconfigure(BMDDisplayMode mode, BMDPixelFormat pixFmt, IDeckLinkDisplayMode* displayMode);
    // Compatibility hook for older callers. Reconfiguration itself is handled
    // by the worker thread, not directly inside the DeckLink callback.
    void applyPendingReconfigureIfNeeded();

    void onVideoFrameArrived(IDeckLinkVideoInputFrame* frame);
    void onAudioPacketArrived(IDeckLinkAudioInputPacket* pkt);

private:
    friend class DeckLinkCaptureCallback;

    // Debounced input-mode candidate. Format changes are detected by the
    // callback but applied outside the callback path.
    struct PendingDetectedMode
    {
        BMDDisplayMode mode = bmdModeUnknown;
        BMDPixelFormat pixelFormat = bmdFormat10BitYUV;
        int width = 0;
        int height = 0;
        int tbNum = 1;
        int tbDen = 25;
        BMDFieldDominance fieldDominance = bmdUnknownFieldDominance;
        std::chrono::steady_clock::time_point firstSeen;
        std::chrono::steady_clock::time_point lastSeen;
        int hitCount = 0;
        bool valid = false;
    };

    std::atomic<bool> m_inputFormatLogged{false};

    std::mutex m_detectMtx;
    PendingDetectedMode m_candidateMode;
    bool m_candidateReadyToApply = false;

    IDeckLink*       m_deckLink{nullptr};
    IDeckLinkInput*  m_deckLinkInput{nullptr};
    DeckLinkCaptureCallback* m_callback{nullptr};

    std::atomic<bool> m_signalDetected{false};

    std::atomic<int>  m_width{1920};
    std::atomic<int>  m_height{1080};
    std::atomic<int>  m_fpsRounded{25};
    std::atomic<bool> m_interlaced{true};
    std::atomic<bool> m_tff{true};
    std::atomic<int>  m_tbNum{1};
    std::atomic<int>  m_tbDen{25};
    std::atomic<int64_t> m_videoPtsCounter{0};
    std::atomic<int64_t> m_audioPtsCounter{0};

    std::mutex m_timecodeLogMtx;
    std::string m_lastLoggedTimecode;

    std::atomic<bool> m_reconfigRequested{false};
    std::mutex              m_reconfigMtx;
    std::condition_variable m_reconfigCv;
    std::thread             m_reconfigThread;
    std::atomic<bool>       m_reconfigWorkerRunning{false};
    std::atomic<bool>       m_reconfigWorkerStop{false};
    BMDDisplayMode          m_pendingMode{bmdModeUnknown};
    BMDPixelFormat          m_pendingPixFmt{bmdFormat10BitYUV};

    std::mutex               m_latestFrameMtx;
    std::shared_ptr<uint8_t> m_latestVideoShared;
    std::shared_ptr<uint8_t> m_latestAudioShared;
    std::shared_ptr<uint8_t> m_blackVideoShared;
    std::atomic<uint8_t*>    m_videoPublished{nullptr};
    std::atomic<int>         m_frameBytes{0};
    std::atomic<uint8_t*>    m_audioPublished{nullptr};
    std::atomic<int>         m_audioPublishedBytes{0};

    std::mutex                 m_outputMtx;
    BoundedQueue<VideoFrame>*  m_videoOutQ{nullptr};
    BoundedQueue<AudioFrame>*  m_audioOutQ{nullptr};
    StopToken*                 m_stopToken{nullptr};
    PipelineTelemetry*         m_telemetry{nullptr};
    std::atomic<uint64_t>      m_audioQueueDropCounter{0};

    // Pooled owned buffers used after DeckLink-owned callback memory is read.
    // Video buffers carry the normalized YUV422P10LE internal bus.
    SharedBufferPool m_videoPool;
    SharedBufferPool m_audioPool;

    bool m_hasAvx2{false};

    void ensureVideoBuffers(int w, int h);
    void ensureAudioBuffers(size_t bytes);
    void rebuildBlackFrameLocked(int w, int h);

    std::shared_ptr<uint8_t> acquireVideoBuffer(size_t bytes);
    std::shared_ptr<uint8_t> acquireAudioBuffer(size_t bytes);

    // Maps VideoFrame plane pointers into an already-owned shared buffer. This
    // does not copy image data.
    void buildVideoFrameMetadata(VideoFrame& vf,
                                 const std::shared_ptr<uint8_t>& buf,
                                 size_t bytes,
                                 int frameWidth,
                                 int frameHeight,
                                 int64_t pts = 0,
                                 IDeckLinkVideoInputFrame* sourceFrame = nullptr);
    SmpteTimecode extractTimecode(IDeckLinkVideoInputFrame* frame);
    void logTimecodeIfChanged(const SmpteTimecode& tc);
    void reconfigureWorkerLoop();
    void performPendingReconfigure(BMDDisplayMode mode, BMDPixelFormat pf);
    void publishLatestVideoFrame(const std::shared_ptr<uint8_t>& buf, size_t bytes);
    void publishLatestAudioFrame(const std::shared_ptr<uint8_t>& buf, size_t bytes);
    PipelineTelemetry* telemetrySnapshot();
    void pushVideoToOutput(const VideoFrame& vf);
    void pushAudioToOutput(const AudioFrame& af);

    void fillBlackFrame(uint8_t* dst, int w, int h);
    // Normalize supported DeckLink input pixel formats into the sender-facing
    // planar 10-bit 4:2:2 bus. v210 uses AVX2 when available.
    void v210_to_yuv422p10le_dispatch(const uint8_t* src, int srcRowBytes, int w, int h, uint8_t* dst);
    void uyvy_to_yuv422p10le(const uint8_t* src, int srcRowBytes, int w, int h, uint8_t* dst);
};
