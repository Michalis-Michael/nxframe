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
 * DeckLink SDI input implementation. The hot path receives Blackmagic SDK
 * video/audio callbacks, normalizes video into NxFrame's planar 10-bit 4:2:2
 * internal bus, extracts metadata, and pushes owned frames into the live
 * sender queues.
 */

#include "decklink.h"
#include "simd_v210_avx2.h"
#include "stage_timing.h"
#include "DeckLinkAPIVideoFrame_v14_2_1.h"

#include <chrono>
#include <atomic>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <cstdlib>


static inline void updateMaxAtomic(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t old = target.load(std::memory_order_relaxed);
    while (value > old &&
           !target.compare_exchange_weak(old, value, std::memory_order_relaxed)) {
    }
}

// Lightweight periodic timing report for the required v210 -> YUV422P10LE
// normalization step. This helps identify when the input conversion becomes a
// frame-time risk without logging once per frame.
static void reportDeckLinkV210UnpackTiming(uint64_t elapsedNs, bool avx2Path)
{
    static std::atomic<uint64_t> calls{0};
    static std::atomic<uint64_t> totalNs{0};
    static std::atomic<uint64_t> maxNs{0};
    static std::atomic<int64_t> lastReportNs{0};

    calls.fetch_add(1, std::memory_order_relaxed);
    totalNs.fetch_add(elapsedNs, std::memory_order_relaxed);
    updateMaxAtomic(maxNs, elapsedNs);

    const auto now = std::chrono::steady_clock::now();
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    int64_t last = lastReportNs.load(std::memory_order_relaxed);
    if (last == 0) {
        lastReportNs.compare_exchange_strong(last, nowNs, std::memory_order_relaxed);
        return;
    }

    const int64_t intervalNs = 5LL * 1000LL * 1000LL * 1000LL;
    if ((nowNs - last) < intervalNs) {
        return;
    }

    if (!lastReportNs.compare_exchange_strong(last, nowNs, std::memory_order_relaxed)) {
        return;
    }

    const uint64_t n = calls.exchange(0, std::memory_order_relaxed);
    const uint64_t total = totalNs.exchange(0, std::memory_order_relaxed);
    const uint64_t maxv = maxNs.exchange(0, std::memory_order_relaxed);

    if (n == 0) {
        return;
    }

    const double avgUs = static_cast<double>(total) / static_cast<double>(n) / 1000.0;
    const double maxUs = static_cast<double>(maxv) / 1000.0;

    std::cout << "[DeckLink] decklink_unpack_v210 avg_us="
              << std::fixed << std::setprecision(2) << avgUs
              << " max_us=" << maxUs
              << " frames=" << n
              << " path=" << (avx2Path ? "avx2" : "scalar")
              << std::defaultfloat
              << "\n";
}

static inline bool iidEqualLocal(REFIID a, REFIID b)
{
    return std::memcmp(&a, &b, sizeof(b)) == 0;
}

static const char* pixelFormatToString(BMDPixelFormat pf);

// Access Blackmagic-owned frame memory across Desktop Video SDK versions.
// Newer headers use IDeckLinkVideoBuffer, while older-compatible runtime
// objects may still expose GetBytes() through the v14.2.1 frame interface.
static HRESULT getDeckLinkFrameBytes(IDeckLinkVideoFrame* frame, void** bytes)
{
    if (!frame || !bytes) {
        return E_INVALIDARG;
    }
    *bytes = nullptr;

    // SDK 15.x exposes frame memory via IDeckLinkVideoBuffer when the
    // frame was created with the newer buffer API.
    IDeckLinkVideoBuffer* buffer = nullptr;
    HRESULT qi = frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buffer));
    if (qi == S_OK && buffer) {
        const HRESULT startHr = buffer->StartAccess(bmdBufferAccessRead);
        const HRESULT hr = buffer->GetBytes(bytes);
        buffer->EndAccess(bmdBufferAccessRead);
        buffer->Release();
        return (startHr == S_OK) ? hr : startHr;
    }

    // Desktop Video 15 headers removed GetBytes() from the current
    // IDeckLinkVideoFrame interface, but the runtime video input frame can
    // still expose the v14.2.1 compatibility interface. This keeps capture
    // on Blackmagic-owned input buffers instead of replacing them with a
    // caller allocator that may return zero-filled buffers on some systems.
    IDeckLinkVideoFrame_v14_2_1* legacyFrame = nullptr;
    qi = frame->QueryInterface(IID_IDeckLinkVideoFrame_v14_2_1, reinterpret_cast<void**>(&legacyFrame));
    if (qi == S_OK && legacyFrame) {
        const HRESULT hr = legacyFrame->GetBytes(bytes);
        legacyFrame->Release();
        return hr;
    }

    return qi;
}

static const char* fieldDominanceToString(BMDFieldDominance d)
{
    switch (d) {
        case bmdUnknownFieldDominance:        return "unknown";
        case bmdLowerFieldFirst:              return "lower-field-first";
        case bmdUpperFieldFirst:              return "upper-field-first";
        case bmdProgressiveFrame:             return "progressive-frame";
        case bmdProgressiveSegmentedFrame:    return "psf";
        default:                              return "other";
    }
}

static const char* pixelFormatToString(BMDPixelFormat pf)
{
    switch (pf) {
        case bmdFormat8BitYUV:   return "bmdFormat8BitYUV";
        case bmdFormat10BitYUV:  return "bmdFormat10BitYUV";
        case bmdFormat8BitARGB:  return "bmdFormat8BitARGB";
        case bmdFormat8BitBGRA:  return "bmdFormat8BitBGRA";
        default:                 return "unknown";
    }
}

static constexpr int kDeckLinkAudioSampleRate = 48000;
static constexpr int kDeckLinkAudioChannels = 16;
// Capture embedded SDI audio as 32-bit integer samples. This preserves
// 24-bit embedded audio payloads at the input boundary instead of truncating
// them to 16-bit before the encoder/muxer path can make a policy decision.
static constexpr int kDeckLinkAudioBytesPerSample = 4;
static constexpr int kDeckLinkAudioValidBitsPerSample = 24;

DeckLinkCaptureCallback::DeckLinkCaptureCallback(DeckLinkCapture* parent)
    : m_parent(parent)
{
}

HRESULT STDMETHODCALLTYPE DeckLinkCaptureCallback::QueryInterface(REFIID iid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (iidEqualLocal(iid, IID_IUnknown) || iidEqualLocal(iid, IID_IDeckLinkInputCallback)) {
        *ppv = static_cast<IDeckLinkInputCallback*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkCaptureCallback::AddRef(void)
{
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE DeckLinkCaptureCallback::Release(void)
{
    ULONG v = --m_refCount;
    if (v == 0) delete this;
    return v;
}

HRESULT STDMETHODCALLTYPE DeckLinkCaptureCallback::VideoInputFormatChanged(
    BMDVideoInputFormatChangedEvents /*events*/,
    IDeckLinkDisplayMode* mode,
    BMDDetectedVideoInputFormatFlags /*flags*/)
{
    if (!m_parent || !mode) return S_OK;

    const int newW = static_cast<int>(mode->GetWidth());
    const int newH = static_cast<int>(mode->GetHeight());

    BMDTimeValue dur = 0;
    BMDTimeScale scale = 0;
    mode->GetFrameRate(&dur, &scale);

    if (dur <= 0 || scale <= 0) {
        dur = 1;
        scale = 25;
    }

    const BMDFieldDominance dom = mode->GetFieldDominance();
    const bool detectedInterlaced =
        (dom == bmdLowerFieldFirst || dom == bmdUpperFieldFirst);
    const auto now = std::chrono::steady_clock::now();

    bool needsApply = false;

    {
        std::lock_guard<std::mutex> lk(m_parent->m_detectMtx);

        m_parent->m_candidateMode.mode = mode->GetDisplayMode();
        m_parent->m_candidateMode.pixelFormat = bmdFormat10BitYUV;
        m_parent->m_candidateMode.width = newW;
        m_parent->m_candidateMode.height = newH;
        m_parent->m_candidateMode.tbNum = static_cast<int>(dur);
        m_parent->m_candidateMode.tbDen = static_cast<int>(scale);
        m_parent->m_candidateMode.fieldDominance = dom;
        m_parent->m_candidateMode.firstSeen = now;
        m_parent->m_candidateMode.lastSeen = now;
        m_parent->m_candidateMode.hitCount = 1;
        m_parent->m_candidateMode.valid = true;

        std::cout << "[DeckLink] Candidate mode detected: "
                  << newW << "x" << newH
                  << " " << fieldDominanceToString(dom)
                  << " tb=" << dur << "/" << scale
                  << "\n";

        const bool activeMatchesDetected =
            m_parent->m_width.load(std::memory_order_acquire) == newW &&
            m_parent->m_height.load(std::memory_order_acquire) == newH &&
            m_parent->m_tbNum.load(std::memory_order_acquire) == static_cast<int>(dur) &&
            m_parent->m_tbDen.load(std::memory_order_acquire) == static_cast<int>(scale) &&
            m_parent->m_interlaced.load(std::memory_order_acquire) == detectedInterlaced;

        if (!activeMatchesDetected) {
            m_parent->m_candidateReadyToApply = true;
            needsApply = true;
        }
    }

    if (needsApply) {
        std::cout << "[DeckLink] Queuing detected input mode for reconfiguration."
                  << " Source=" << newW << "x" << newH
                  << " " << fieldDominanceToString(dom)
                  << " tb=" << dur << "/" << scale
                  << "\n";
        m_parent->requestReconfigure(mode->GetDisplayMode(), bmdFormat10BitYUV, mode);
    }

    return S_OK;
}


HRESULT STDMETHODCALLTYPE DeckLinkCaptureCallback::VideoInputFrameArrived(
    IDeckLinkVideoInputFrame* videoFrame,
    IDeckLinkAudioInputPacket* audioPacket)
{
    if (!m_parent) return S_OK;

    if (videoFrame && (videoFrame->GetFlags() & bmdFrameHasNoInputSource)) {
        m_parent->m_signalDetected.store(false, std::memory_order_release);
    } else if (videoFrame) {
        m_parent->m_signalDetected.store(true, std::memory_order_release);
    }

    if (videoFrame)  m_parent->onVideoFrameArrived(videoFrame);
    if (audioPacket) m_parent->onAudioPacketArrived(audioPacket);

    return S_OK;
}

DeckLinkCapture::DeckLinkCapture() = default;

DeckLinkCapture::~DeckLinkCapture()
{
    stopCapture();

    if (m_deckLinkInput) {
        m_deckLinkInput->SetCallback(nullptr);
        m_deckLinkInput->Release();
        m_deckLinkInput = nullptr;
    }


    if (m_deckLink) {
        m_deckLink->Release();
        m_deckLink = nullptr;
    }

    if (m_callback) {
        m_callback->Release();
        m_callback = nullptr;
    }
}

bool DeckLinkCapture::init(int deviceIndex)
{
    IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
    if (!it) {
        std::cerr << "[DeckLink] ERROR: No DeckLink iterator available.\n";
        return false;
    }

    int idx = 0;
    IDeckLink* dl = nullptr;
    while (it->Next(&dl) == S_OK) {
        if (idx++ == deviceIndex) {
            m_deckLink = dl;
            break;
        }
        dl->Release();
    }
    it->Release();

    if (!m_deckLink) {
        std::cerr << "[DeckLink] ERROR: Device index " << deviceIndex << " not found.\n";
        return false;
    }

    if (m_deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&m_deckLinkInput) != S_OK) {
        std::cerr << "[DeckLink] ERROR: QueryInterface(IID_IDeckLinkInput) failed.\n";
        return false;
    }

    m_hasAvx2 = cpu_has_avx2();
    std::cout << "[DeckLink] AVX2 support: " << (m_hasAvx2 ? "YES" : "NO") << "\n";


    m_callback = new DeckLinkCaptureCallback(this);
    m_deckLinkInput->SetCallback(m_callback);

    ensureVideoBuffers(m_width.load(), m_height.load());
    ensureAudioBuffers(static_cast<size_t>(kDeckLinkAudioChannels) * 1920u *
                       static_cast<size_t>(kDeckLinkAudioBytesPerSample));
    return true;
}

bool DeckLinkCapture::startCapture(BMDDisplayMode preferred)
{
    if (!m_deckLinkInput) return false;

    HRESULT r = m_deckLinkInput->EnableVideoInput(
        preferred,
        bmdFormat10BitYUV,
        bmdVideoInputEnableFormatDetection
    );

    if (r != S_OK) {
        std::cerr << "[DeckLink] WARN: 10-bit input enable failed, trying 8-bit YUV.\n";
        r = m_deckLinkInput->EnableVideoInput(
            preferred,
            bmdFormat8BitYUV,
            bmdVideoInputEnableFormatDetection
        );
        if (r != S_OK) {
            std::cerr << "[DeckLink] ERROR: EnableVideoInput failed.\n";
            return false;
        }
    }

    r = m_deckLinkInput->EnableAudioInput(
        bmdAudioSampleRate48kHz,
        bmdAudioSampleType32bitInteger,
        kDeckLinkAudioChannels
    );

    if (r != S_OK) {
        std::cerr << "[DeckLink] WARN: EnableAudioInput failed. Continuing without audio.\n";
    }

    if (m_deckLinkInput->StartStreams() != S_OK) {
        std::cerr << "[DeckLink] ERROR: StartStreams failed.\n";
        return false;
    }

    m_reconfigWorkerStop.store(false, std::memory_order_release);
    if (!m_reconfigWorkerRunning.exchange(true, std::memory_order_acq_rel)) {
        m_reconfigThread = std::thread(&DeckLinkCapture::reconfigureWorkerLoop, this);
    }

    std::cout << "[DeckLink] Capture started with format detection enabled"
              << " audio=s32/24-valid x" << kDeckLinkAudioChannels
              << ".\n";
    return true;
}

void DeckLinkCapture::stopCapture()
{
    detachOutputQueues();

    {
        std::lock_guard<std::mutex> lk(m_reconfigMtx);
        m_reconfigWorkerStop.store(true, std::memory_order_release);
        m_reconfigRequested.store(false, std::memory_order_release);
    }
    m_reconfigCv.notify_all();

    if (m_reconfigThread.joinable()) {
        m_reconfigThread.join();
    }
    m_reconfigWorkerRunning.store(false, std::memory_order_release);

    if (!m_deckLinkInput) return;

    m_deckLinkInput->StopStreams();
    m_deckLinkInput->FlushStreams();
    m_deckLinkInput->DisableAudioInput();
    m_deckLinkInput->DisableVideoInput();
}

uint8_t* DeckLinkCapture::getFrame()
{
    return m_videoPublished.load(std::memory_order_acquire);
}

int DeckLinkCapture::getFPS()
{
    return m_fpsRounded.load(std::memory_order_acquire);
}

uint8_t* DeckLinkCapture::getAudioFrame()
{
    return m_audioPublished.load(std::memory_order_acquire);
}

int DeckLinkCapture::getAudioFrameBytes()
{
    return m_audioPublishedBytes.load(std::memory_order_acquire);
}

void DeckLinkCapture::attachOutputQueues(BoundedQueue<VideoFrame>* videoQ,
                                         BoundedQueue<AudioFrame>* audioQ,
                                         StopToken* stop,
                                         PipelineTelemetry* telemetry)
{
    std::lock_guard<std::mutex> lk(m_outputMtx);
    m_videoOutQ = videoQ;
    m_audioOutQ = audioQ;
    m_stopToken = stop;
    m_telemetry = telemetry;
}

void DeckLinkCapture::detachOutputQueues()
{
    std::lock_guard<std::mutex> lk(m_outputMtx);
    m_videoOutQ = nullptr;
    m_audioOutQ = nullptr;
    m_stopToken = nullptr;
    m_telemetry = nullptr;
}

bool DeckLinkCapture::videoFeedDetected() const
{
    return m_signalDetected.load(std::memory_order_acquire);
}

int DeckLinkCapture::getWidth() const
{
    return m_width.load(std::memory_order_acquire);
}

int DeckLinkCapture::getHeight() const
{
    return m_height.load(std::memory_order_acquire);
}

AVRational DeckLinkCapture::getTimeBase() const
{
    AVRational tb;
    tb.num = m_tbNum.load(std::memory_order_acquire);
    tb.den = m_tbDen.load(std::memory_order_acquire);
    if (tb.num <= 0 || tb.den <= 0) return AVRational{1, 25};
    return tb;
}

bool DeckLinkCapture::isInterlaced() const
{
    return m_interlaced.load(std::memory_order_acquire);
}

bool DeckLinkCapture::isTopFieldFirst() const
{
    return m_tff.load(std::memory_order_acquire);
}

void DeckLinkCapture::requestReconfigure(BMDDisplayMode mode,
                                         BMDPixelFormat pixFmt,
                                         IDeckLinkDisplayMode* displayMode)
{
    // Store the detected mode immediately for downstream metadata, then ask the
    // worker to restart DeckLink streams outside the SDK callback thread.
    if (displayMode) {
        const int w = static_cast<int>(displayMode->GetWidth());
        const int h = static_cast<int>(displayMode->GetHeight());

        m_width.store(w, std::memory_order_release);
        m_height.store(h, std::memory_order_release);

        const BMDFieldDominance dom = displayMode->GetFieldDominance();
        const bool interlaced =
            (dom == bmdLowerFieldFirst || dom == bmdUpperFieldFirst);
        const bool tff =
            (dom == bmdUpperFieldFirst);

        m_interlaced.store(interlaced, std::memory_order_release);
        m_tff.store(tff, std::memory_order_release);

        BMDTimeValue dur = 0;
        BMDTimeScale scale = 0;
        displayMode->GetFrameRate(&dur, &scale);

        if (dur > 0 && scale > 0) {
            const double fpsD = static_cast<double>(scale) / static_cast<double>(dur);
            int fpsR = static_cast<int>(std::lround(fpsD));
            if (fpsR <= 0) fpsR = 25;

            m_fpsRounded.store(fpsR, std::memory_order_release);
            m_tbNum.store(static_cast<int>(dur), std::memory_order_release);
            m_tbDen.store(static_cast<int>(scale), std::memory_order_release);
        } else {
            m_fpsRounded.store(25, std::memory_order_release);
            m_tbNum.store(1, std::memory_order_release);
            m_tbDen.store(25, std::memory_order_release);
        }

        std::cout << "[DeckLink] Accepted stable mode: "
                  << w << "x" << h
                  << " " << fieldDominanceToString(dom)
                  << " fps~" << m_fpsRounded.load(std::memory_order_acquire)
                  << "\n";
    }

    {
        std::lock_guard<std::mutex> lk(m_reconfigMtx);
        m_pendingMode = mode;
        m_pendingPixFmt = pixFmt;
    }

    m_inputFormatLogged.store(false, std::memory_order_release);
    m_reconfigRequested.store(true, std::memory_order_release);
    m_reconfigCv.notify_one();
}

void DeckLinkCapture::applyPendingReconfigureIfNeeded()
{
    // Kept for compatibility with older callers. Real reconfiguration is now
    // performed by reconfigureWorkerLoop(), outside the DeckLink callback path.
    m_reconfigCv.notify_one();
}

void DeckLinkCapture::reconfigureWorkerLoop()
{
    for (;;) {
        BMDDisplayMode mode = bmdModeUnknown;
        BMDPixelFormat pf = bmdFormat10BitYUV;

        {
            std::unique_lock<std::mutex> lk(m_reconfigMtx);
            m_reconfigCv.wait(lk, [this]() {
                return m_reconfigWorkerStop.load(std::memory_order_acquire) ||
                       m_reconfigRequested.load(std::memory_order_acquire);
            });

            if (m_reconfigWorkerStop.load(std::memory_order_acquire)) {
                break;
            }

            mode = m_pendingMode;
            pf = m_pendingPixFmt;
            m_reconfigRequested.store(false, std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> lk(m_detectMtx);
            m_candidateReadyToApply = false;
        }

        performPendingReconfigure(mode, pf);
    }
}

void DeckLinkCapture::performPendingReconfigure(BMDDisplayMode mode, BMDPixelFormat pf)
{
    if (!m_deckLinkInput || mode == bmdModeUnknown)
        return;

    m_deckLinkInput->StopStreams();
    m_deckLinkInput->FlushStreams();
    m_deckLinkInput->DisableVideoInput();

    HRESULT r = m_deckLinkInput->EnableVideoInput(
        mode,
        pf,
        bmdVideoInputEnableFormatDetection
    );

    if (r != S_OK) {
        std::cerr << "[DeckLink] WARN: Re-enable with 10-bit failed, trying 8-bit.\n";

        r = m_deckLinkInput->EnableVideoInput(
            mode,
            bmdFormat8BitYUV,
            bmdVideoInputEnableFormatDetection
        );

        if (r != S_OK) {
            std::cerr << "[DeckLink] ERROR: Re-enable video input failed.\n";
            return;
        }
    }

    ensureVideoBuffers(
        m_width.load(std::memory_order_acquire),
        m_height.load(std::memory_order_acquire)
    );

    if (m_deckLinkInput->StartStreams() != S_OK) {
        std::cerr << "[DeckLink] ERROR: StartStreams after reconfigure failed.\n";
        return;
    }

    std::cout << "[DeckLink] Streams restarted with detected mode.\n";
}


void DeckLinkCapture::ensureVideoBuffers(int w, int h)
{
    // Internal video bus is tightly packed YUV422P10LE:
    // Y = w*h*2, U = w/2*h*2, V = w/2*h*2 => w*h*4 bytes.
    const int needed = w * h * 4;
    if (needed <= 0) {
        return;
    }

    const bool sizeChanged = (m_frameBytes.load(std::memory_order_acquire) != needed);
    if (!sizeChanged && m_blackVideoShared) {
        return;
    }

    m_videoPool.reset(static_cast<size_t>(needed), 8);
    m_frameBytes.store(needed, std::memory_order_release);

    std::lock_guard<std::mutex> lk(m_latestFrameMtx);
    rebuildBlackFrameLocked(w, h);
    m_latestVideoShared = m_blackVideoShared;
    m_videoPublished.store(m_blackVideoShared ? m_blackVideoShared.get() : nullptr, std::memory_order_release);
}

void DeckLinkCapture::ensureAudioBuffers(size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    if (m_audioPool.blockSize() == bytes) {
        return;
    }
    m_audioPool.reset(bytes, 32);
}

void DeckLinkCapture::rebuildBlackFrameLocked(int w, int h)
{
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    m_blackVideoShared = make_shared_u8(bytes);
    fillBlackFrame(m_blackVideoShared.get(), w, h);
}

std::shared_ptr<uint8_t> DeckLinkCapture::acquireVideoBuffer(size_t bytes)
{
    if (m_videoPool.blockSize() != bytes) {
        ensureVideoBuffers(m_width.load(std::memory_order_acquire), m_height.load(std::memory_order_acquire));
    }
    auto buf = m_videoPool.acquire();
    if (!buf) {
        buf = make_shared_u8(bytes);
    }
    return buf;
}

std::shared_ptr<uint8_t> DeckLinkCapture::acquireAudioBuffer(size_t bytes)
{
    if (m_audioPool.blockSize() != bytes) {
        ensureAudioBuffers(bytes);
    }
    auto buf = m_audioPool.acquire();
    if (!buf) {
        buf = make_shared_u8(bytes);
    }
    return buf;
}

void DeckLinkCapture::onVideoFrameArrived(IDeckLinkVideoInputFrame* frame)
{
    // Video hot path. The only unavoidable image copy/conversion here is the
    // normalization from DeckLink-owned packed input memory into NxFrame-owned
    // planar YUV422P10LE memory. The queued VideoFrame shares that owned buffer.
    stage_timing::ScopedTimer timer(stage_timing::get("decklink_video_total"));

    if (!frame) return;

    const int w = static_cast<int>(frame->GetWidth());
    const int h = static_cast<int>(frame->GetHeight());
    const size_t frameBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

    ensureVideoBuffers(w, h);

    if (frame->GetFlags() & bmdFrameHasNoInputSource) {
        static std::atomic<bool> warnedNoInputSource{false};
        if (!warnedNoInputSource.exchange(true, std::memory_order_acq_rel)) {
            std::cerr << "[DeckLink] WARN: Frame has no input source flag. Publishing black fallback frames.\n";
        }
        std::shared_ptr<uint8_t> black;
        {
            std::lock_guard<std::mutex> lk(m_latestFrameMtx);
            if (!m_blackVideoShared || m_frameBytes.load(std::memory_order_acquire) != static_cast<int>(frameBytes)) {
                rebuildBlackFrameLocked(w, h);
            }
            black = m_blackVideoShared;
        }

        {
            stage_timing::ScopedTimer t(stage_timing::get("decklink_video_publish"));
            publishLatestVideoFrame(black, frameBytes);
        }

        VideoFrame vf;
        buildVideoFrameMetadata(vf, black, frameBytes, w, h, m_videoPtsCounter.fetch_add(1, std::memory_order_relaxed), frame);
        {
            stage_timing::ScopedTimer t(stage_timing::get("decklink_video_queue_push"));
            pushVideoToOutput(vf);
        }
        if (PipelineTelemetry* telemetry = telemetrySnapshot()) {
            telemetry->inVideo.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    void* bytes = nullptr;
    const HRESULT bufferHr = getDeckLinkFrameBytes(frame, &bytes);
    if (bufferHr != S_OK || !bytes) {
        static std::atomic<bool> warnedVideoBufferFail{false};
        if (!warnedVideoBufferFail.exchange(true, std::memory_order_acq_rel)) {
            std::cerr << "[DeckLink] ERROR: Video input buffer access failed hr=0x"
                      << std::hex << static_cast<unsigned long>(bufferHr) << std::dec
                      << ". Publishing black fallback frames.\n";
        }
        std::shared_ptr<uint8_t> black;
        {
            std::lock_guard<std::mutex> lk(m_latestFrameMtx);
            if (!m_blackVideoShared || m_frameBytes.load(std::memory_order_acquire) != static_cast<int>(frameBytes)) {
                rebuildBlackFrameLocked(w, h);
            }
            black = m_blackVideoShared;
        }

        {
            stage_timing::ScopedTimer t(stage_timing::get("decklink_video_publish"));
            publishLatestVideoFrame(black, frameBytes);
        }

        VideoFrame vf;
        buildVideoFrameMetadata(vf, black, frameBytes, w, h, m_videoPtsCounter.fetch_add(1, std::memory_order_relaxed), frame);
        {
            stage_timing::ScopedTimer t(stage_timing::get("decklink_video_queue_push"));
            pushVideoToOutput(vf);
        }
        if (PipelineTelemetry* telemetry = telemetrySnapshot()) {
            telemetry->inVideo.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    const int rowBytes = static_cast<int>(frame->GetRowBytes());
    const BMDPixelFormat pf = frame->GetPixelFormat();

    if (!m_inputFormatLogged.load(std::memory_order_acquire)) {
        const bool interlaced = m_interlaced.load(std::memory_order_acquire);
        const bool tff = m_tff.load(std::memory_order_acquire);
        const AVRational tb = getTimeBase();

        double fps = 0.0;
        if (tb.num > 0) {
            fps = static_cast<double>(tb.den) / static_cast<double>(tb.num);
        }

        double displayRate = fps;
        if (interlaced && fps > 0.0 && fps <= 30.0) {
            displayRate = fps * 2.0;
        }

        const int roundedDisplayRate = static_cast<int>(std::lround(displayRate));

        std::cout << "[DeckLink] Active input locked: "
                  << w << "x" << h
                  << (interlaced ? "i" : "p")
                  << roundedDisplayRate;

        if (interlaced) {
            std::cout << " " << (tff ? "TFF" : "BFF");
        }

        std::cout << " pf=" << pixelFormatToString(pf)
                  << " internal=AV_PIX_FMT_YUV422P10LE"
                  << "\n";

        m_inputFormatLogged.store(true, std::memory_order_release);
    }

    auto frameBuf = acquireVideoBuffer(frameBytes);

    // Normalize every supported SDI input format to the same internal bus.
    // Downstream encoder workers can then make a simple zero-copy decision based
    // on whether their target format is also YUV422P10LE.
    if (pf == bmdFormat10BitYUV) {
        stage_timing::ScopedTimer t(stage_timing::get("decklink_unpack_v210"));
        const auto unpackStart = std::chrono::steady_clock::now();
        v210_to_yuv422p10le_dispatch(reinterpret_cast<const uint8_t*>(bytes), rowBytes, w, h, frameBuf.get());
        const auto unpackEnd = std::chrono::steady_clock::now();
        const uint64_t unpackNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(unpackEnd - unpackStart).count());
        reportDeckLinkV210UnpackTiming(unpackNs, m_hasAvx2);
    } else if (pf == bmdFormat8BitYUV) {
        stage_timing::ScopedTimer t(stage_timing::get("decklink_unpack_uyvy"));
        uyvy_to_yuv422p10le(reinterpret_cast<const uint8_t*>(bytes), rowBytes, w, h, frameBuf.get());
    } else {
        std::cerr << "[DeckLink] WARN: Unsupported input pixel format. Publishing black frame.\n";
        stage_timing::ScopedTimer t(stage_timing::get("decklink_fill_black"));
        fillBlackFrame(frameBuf.get(), w, h);
    }


    {
        stage_timing::ScopedTimer t(stage_timing::get("decklink_video_publish"));
        publishLatestVideoFrame(frameBuf, frameBytes);
    }

    VideoFrame vf;
    {
        stage_timing::ScopedTimer t(stage_timing::get("decklink_video_metadata"));
        buildVideoFrameMetadata(vf, frameBuf, frameBytes, w, h, m_videoPtsCounter.fetch_add(1, std::memory_order_relaxed), frame);
    }
    {
        stage_timing::ScopedTimer t(stage_timing::get("decklink_video_queue_push"));
        pushVideoToOutput(vf);
    }
    if (PipelineTelemetry* telemetry = telemetrySnapshot()) {
        telemetry->inVideo.fetch_add(1, std::memory_order_relaxed);
    }
}

void DeckLinkCapture::onAudioPacketArrived(IDeckLinkAudioInputPacket* pkt)
{
    // Audio packet memory belongs to the DeckLink callback lifetime. Copy it
    // into an owned pooled buffer before queueing so downstream stages can run
    // independently of the SDK callback.
    stage_timing::ScopedTimer timer(stage_timing::get("decklink_audio_total"));

    if (!pkt) return;

    void* audioBytes = nullptr;
    if (pkt->GetBytes(&audioBytes) != S_OK || !audioBytes) return;

    const long frames = pkt->GetSampleFrameCount();
    if (frames <= 0) return;

    const size_t bytes = static_cast<size_t>(frames) *
                         static_cast<size_t>(kDeckLinkAudioChannels) *
                         static_cast<size_t>(kDeckLinkAudioBytesPerSample);
    auto audioBuf = acquireAudioBuffer(bytes);
    {
        stage_timing::ScopedTimer t(stage_timing::get("decklink_audio_copy"));
        std::memcpy(audioBuf.get(), audioBytes, bytes);
    }

    {
        stage_timing::ScopedTimer t(stage_timing::get("decklink_audio_publish"));
        publishLatestAudioFrame(audioBuf, bytes);
    }

    AudioFrame af;
    af.buffer = audioBuf;
    af.buffer_size = bytes;
    af.sample_rate = kDeckLinkAudioSampleRate;
    af.channels = kDeckLinkAudioChannels;
    af.bytes_per_sample = kDeckLinkAudioBytesPerSample;
    af.valid_bits_per_sample = kDeckLinkAudioValidBitsPerSample;
    af.num_samples = static_cast<int>(frames);
    af.time_base = AVRational{1, 48000};
    af.pts = m_audioPtsCounter.fetch_add(static_cast<int64_t>(frames), std::memory_order_relaxed);
    {
        stage_timing::ScopedTimer t(stage_timing::get("decklink_audio_queue_push"));
        pushAudioToOutput(af);
    }
    if (PipelineTelemetry* telemetry = telemetrySnapshot()) {
        telemetry->inAudio.fetch_add(1, std::memory_order_relaxed);
    }
}

void DeckLinkCapture::v210_to_yuv422p10le_dispatch(const uint8_t* src,
                                                   int srcRowBytes,
                                                   int w,
                                                   int h,
                                                   uint8_t* dst)
{
    uint16_t* Y = reinterpret_cast<uint16_t*>(dst);
    uint16_t* U = Y + static_cast<size_t>(w) * static_cast<size_t>(h);
    uint16_t* V = U + static_cast<size_t>(w / 2) * static_cast<size_t>(h);

    if (m_hasAvx2) {
        v210_to_yuv422p10le_avx2(src, srcRowBytes, w, h, Y, U, V);
    } else {
        v210_to_yuv422p10le_scalar(src, srcRowBytes, w, h, Y, U, V);
    }
}

void DeckLinkCapture::uyvy_to_yuv422p10le(const uint8_t* src,
                                          int srcRowBytes,
                                          int w,
                                          int h,
                                          uint8_t* dst)
{
    uint16_t* Y = reinterpret_cast<uint16_t*>(dst);
    uint16_t* U = Y + static_cast<size_t>(w) * static_cast<size_t>(h);
    uint16_t* V = U + static_cast<size_t>(w / 2) * static_cast<size_t>(h);

    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(srcRowBytes);
        const size_t yOff = static_cast<size_t>(y) * static_cast<size_t>(w);
        const size_t cOff = static_cast<size_t>(y) * static_cast<size_t>(w / 2);

        for (int x = 0; x < w; x += 2) {
            const uint8_t u  = row[0];
            const uint8_t y0 = row[1];
            const uint8_t v  = row[2];
            const uint8_t y1 = row[3];

            Y[yOff + static_cast<size_t>(x + 0)] = static_cast<uint16_t>(y0) << 2;
            Y[yOff + static_cast<size_t>(x + 1)] = static_cast<uint16_t>(y1) << 2;
            U[cOff + static_cast<size_t>(x / 2)] = static_cast<uint16_t>(u)  << 2;
            V[cOff + static_cast<size_t>(x / 2)] = static_cast<uint16_t>(v)  << 2;
            row += 4;
        }
    }
}


SmpteTimecode DeckLinkCapture::extractTimecode(IDeckLinkVideoInputFrame* frame)
{
    SmpteTimecode out;
    if (!frame) {
        return out;
    }

    struct Candidate {
        BMDTimecodeFormat format;
        const char* label;
    };

    const Candidate candidates[] = {
        { bmdTimecodeRP188Any, "rp188-any" },
        { bmdTimecodeRP188HighFrameRate, "rp188-hfr" },
        { bmdTimecodeRP188VITC1, "rp188-vitc1" },
        { bmdTimecodeRP188VITC2, "rp188-vitc2" },
        { bmdTimecodeRP188LTC, "rp188-ltc" },
        { bmdTimecodeVITC, "vitc" },
        { bmdTimecodeVITCField2, "vitc-field2" },
        { bmdTimecodeSerial, "serial" }
    };

    for (const Candidate& c : candidates) {
        IDeckLinkTimecode* tc = nullptr;
        if (frame->GetTimecode(c.format, &tc) != S_OK || !tc) {
            continue;
        }

        uint8_t hh = 0, mm = 0, ss = 0, ff = 0;
        const HRESULT compHr = tc->GetComponents(&hh, &mm, &ss, &ff);
        if (compHr == S_OK) {
            out.valid = true;
            out.hours = hh;
            out.minutes = mm;
            out.seconds = ss;
            out.frames = ff;
            out.flags = static_cast<uint32_t>(tc->GetFlags());
            out.source = c.label ? c.label : "unknown";

            const char* tcString = nullptr;
            if (tc->GetString(&tcString) == S_OK && tcString) {
                out.text = tcString;
                free(const_cast<char*>(tcString));
            }

            BMDTimecodeUserBits userBits = 0;
            if (tc->GetTimecodeUserBits(&userBits) == S_OK) {
                out.user_bits = static_cast<uint32_t>(userBits);
                out.has_user_bits = true;
            }

            tc->Release();
            break;
        }

        tc->Release();
    }

    return out;
}

void DeckLinkCapture::logTimecodeIfChanged(const SmpteTimecode& tc)
{
    if (!tc.valid) {
        return;
    }

    const std::string text = tc.toString();
    if (text.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lk(m_timecodeLogMtx);
    if (text == m_lastLoggedTimecode) {
        return;
    }

    const bool first = m_lastLoggedTimecode.empty();
    m_lastLoggedTimecode = text;

    if (first || tc.frames == 0) {
        std::cout << "[DeckLink] SMPTE timecode locked"
                  << " source=" << (tc.source.empty() ? "unknown" : tc.source)
                  << " tc=" << text;
        if (tc.has_user_bits) {
            std::cout << " user_bits=0x" << std::hex << tc.user_bits << std::dec;
        }
        std::cout << "\n";
    }
}

void DeckLinkCapture::buildVideoFrameMetadata(VideoFrame& vf,
                                              const std::shared_ptr<uint8_t>& buf,
                                              size_t bytes,
                                              int frameWidth,
                                              int frameHeight,
                                              int64_t pts,
                                              IDeckLinkVideoInputFrame* sourceFrame)
{
    const int w = frameWidth;
    const int h = frameHeight;
    const AVRational tb = getTimeBase();

    vf.buffer = buf;
    vf.buffer_size = bytes;
    vf.width = w;
    vf.height = h;
    vf.pix_fmt = AV_PIX_FMT_YUV422P10LE;
    vf.interlaced = m_interlaced.load(std::memory_order_acquire);
    vf.tff = m_tff.load(std::memory_order_acquire);
    vf.time_base = (tb.num > 0 && tb.den > 0) ? tb : AVRational{1, 25};
    vf.pts = pts;
    vf.metadata.timecode = extractTimecode(sourceFrame);
    logTimecodeIfChanged(vf.metadata.timecode);

    vf.data[0] = vf.buffer.get();
    vf.data[1] = vf.data[0] + static_cast<size_t>(w) * static_cast<size_t>(h) * 2u;
    vf.data[2] = vf.data[1] + static_cast<size_t>(w / 2) * static_cast<size_t>(h) * 2u;
    vf.linesize[0] = w * 2;
    vf.linesize[1] = (w / 2) * 2;
    vf.linesize[2] = (w / 2) * 2;
}

void DeckLinkCapture::publishLatestVideoFrame(const std::shared_ptr<uint8_t>& buf, size_t bytes)
{
    std::lock_guard<std::mutex> lk(m_latestFrameMtx);
    m_latestVideoShared = buf;
    m_videoPublished.store(buf ? buf.get() : nullptr, std::memory_order_release);
    m_frameBytes.store(static_cast<int>(bytes), std::memory_order_release);
}

void DeckLinkCapture::publishLatestAudioFrame(const std::shared_ptr<uint8_t>& buf, size_t bytes)
{
    std::lock_guard<std::mutex> lk(m_latestFrameMtx);
    m_latestAudioShared = buf;
    m_audioPublished.store(buf ? buf.get() : nullptr, std::memory_order_release);
    m_audioPublishedBytes.store(static_cast<int>(bytes), std::memory_order_release);
}

PipelineTelemetry* DeckLinkCapture::telemetrySnapshot()
{
    std::lock_guard<std::mutex> lk(m_outputMtx);
    return m_telemetry;
}

void DeckLinkCapture::pushVideoToOutput(const VideoFrame& vf)
{
    // Copying the VideoFrame object copies metadata and shared_ptr ownership,
    // not the underlying video image.
    BoundedQueue<VideoFrame>* videoQ = nullptr;
    StopToken* stop = nullptr;
    PipelineTelemetry* telemetry = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_outputMtx);
        videoQ = m_videoOutQ;
        stop = m_stopToken;
        telemetry = m_telemetry;
    }

    if (!videoQ || (stop && stop->stop_requested())) {
        return;
    }

    VideoFrame copy = vf;
    if (!videoQ->push_drop_oldest(std::move(copy)) && telemetry) {
        telemetry->pushFailVideo.fetch_add(1, std::memory_order_relaxed);
    }
}

void DeckLinkCapture::pushAudioToOutput(const AudioFrame& af)
{
    BoundedQueue<AudioFrame>* audioQ = nullptr;
    StopToken* stop = nullptr;
    PipelineTelemetry* telemetry = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_outputMtx);
        audioQ = m_audioOutQ;
        stop = m_stopToken;
        telemetry = m_telemetry;
    }

    if (!audioQ || (stop && stop->stop_requested())) {
        return;
    }

    AudioFrame copy = af;
    const QueuePushResult pushResult =
        audioQ->push_with_policy(std::move(copy), QueueOverflowPolicy::DropOldest);

    if (pushResult == QueuePushResult::Stopped) {
        if (telemetry) {
            telemetry->pushFailAudio.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    if (pushResult == QueuePushResult::DroppedOldestAndPushed) {
        const uint64_t dropped = m_audioQueueDropCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        if (telemetry) {
            telemetry->pushFailAudio.fetch_add(1, std::memory_order_relaxed);
        }
        if (dropped == 1 || (dropped % 100) == 0) {
            std::cerr << "[DeckLink] WARNING: audio output queue full; dropped oldest audio packet "
                      << "to keep live audio current. drops=" << dropped
                      << " newest_pts=" << af.pts << "\n";
        }
    }
}

void DeckLinkCapture::fillBlackFrame(uint8_t* dst, int w, int h)
{
    // Broadcast-range black in 10-bit limited range: Y=64, Cb/Cr=512.
    if (!dst || w <= 0 || h <= 0) return;

    const size_t ySamples = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t cSamples = static_cast<size_t>(w / 2) * static_cast<size_t>(h);

    uint16_t* p = reinterpret_cast<uint16_t*>(dst);
    const uint16_t yBlack = 64;
    const uint16_t cNeutral = 512;

    for (size_t i = 0; i < ySamples; ++i) p[i] = yBlack;
    for (size_t i = 0; i < cSamples; ++i) p[ySamples + i] = cNeutral;
    for (size_t i = 0; i < cSamples; ++i) p[ySamples + cSamples + i] = cNeutral;
}