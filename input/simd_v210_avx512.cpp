/*
 * NxFrame - broadcast contribution encoder/decoder
 *
 * Copyright (C) 2026 Michalis Michael
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Description:
 * AVX-512 accelerated v210 unpack implementation used by the DeckLink input
 * path. Runtime dispatch lives outside this translation unit so AVX-512 code is
 * never executed on unsupported CPUs/OSes. Falls back to AVX2 if this source
 * file is built without the required AVX-512 feature set.
 */

#include "simd_v210_avx2.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
  #include <immintrin.h>
#endif

namespace {

static inline unsigned env_u32(const char* name, unsigned fallback)
{
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(v, &end, 10);
    if (end == v || *end != '\0') return fallback;
    return static_cast<unsigned>(parsed);
}

static inline unsigned choose_parallel_workers()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 2) return 0;
    unsigned auto_workers = 1;
    if (hw >= 8) auto_workers = 2;
    return env_u32("NXFRAME_V210_THREADS", auto_workers);
}

static inline unsigned choose_rows_per_task(int height)
{
    const unsigned auto_rows = (height >= 1080) ? 64u : 32u;
    return std::max(1u, env_u32("NXFRAME_V210_ROWS_PER_TASK", auto_rows));
}

static inline bool should_parallelize(int width, int height, unsigned workers)
{
    return workers != 0 && width >= 1280 && height >= 200;
}

#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)

static inline void write_group6_visible(const uint32_t* row,
                                        int visiblePixels,
                                        uint16_t*& yPtr,
                                        uint16_t*& uPtr,
                                        uint16_t*& vPtr)
{
    const uint32_t a = row[0];
    const uint32_t b = row[1];
    const uint32_t c = row[2];
    const uint32_t d = row[3];

    const uint16_t s0  = static_cast<uint16_t>((a >>  0) & 0x3FFu);
    const uint16_t s1  = static_cast<uint16_t>((a >> 10) & 0x3FFu);
    const uint16_t s2  = static_cast<uint16_t>((a >> 20) & 0x3FFu);
    const uint16_t s3  = static_cast<uint16_t>((b >>  0) & 0x3FFu);
    const uint16_t s4  = static_cast<uint16_t>((b >> 10) & 0x3FFu);
    const uint16_t s5  = static_cast<uint16_t>((b >> 20) & 0x3FFu);
    const uint16_t s6  = static_cast<uint16_t>((c >>  0) & 0x3FFu);
    const uint16_t s7  = static_cast<uint16_t>((c >> 10) & 0x3FFu);
    const uint16_t s8  = static_cast<uint16_t>((c >> 20) & 0x3FFu);
    const uint16_t s9  = static_cast<uint16_t>((d >>  0) & 0x3FFu);
    const uint16_t s10 = static_cast<uint16_t>((d >> 10) & 0x3FFu);
    const uint16_t s11 = static_cast<uint16_t>((d >> 20) & 0x3FFu);

    const uint16_t yy[6] = { s1, s3, s5, s7, s9, s11 };
    const uint16_t uu[3] = { s0, s4, s8 };
    const uint16_t vv[3] = { s2, s6, s10 };

    const int yCount = std::max(0, std::min(visiblePixels, 6));
    const int cCount = yCount / 2;
    for (int i = 0; i < yCount; ++i) yPtr[i] = yy[i];
    for (int i = 0; i < cCount; ++i) {
        uPtr[i] = uu[i];
        vPtr[i] = vv[i];
    }
    yPtr += yCount;
    uPtr += cCount;
    vPtr += cCount;
}

struct V210Avx512Consts {
    __m512i mask10;
    __m512i y0_f0_idx, y0_f1_idx, y0_f2_idx;
    __m512i y1_f0_idx, y1_f1_idx, y1_f2_idx;
    __m512i u_f0_idx, u_f1_idx, u_f2_idx;
    __m512i v_f0_idx, v_f1_idx, v_f2_idx;

    V210Avx512Consts()
        : mask10(_mm512_set1_epi32(0x3FF)),
          // Y0..Y15 for the first 16 output luma samples.
          y0_f0_idx(_mm512_setr_epi32(0,1,0,0,3,0,0,5,0,0,7,0,0,9,0,0)),
          y0_f1_idx(_mm512_setr_epi32(0,0,0,2,0,0,4,0,0,6,0,0,8,0,0,10)),
          y0_f2_idx(_mm512_setr_epi32(0,0,1,0,0,3,0,0,5,0,0,7,0,0,9,0)),
          // Y16..Y23 in lanes 0..7.
          y1_f0_idx(_mm512_setr_epi32(11,0,0,13,0,0,15,0,0,0,0,0,0,0,0,0)),
          y1_f1_idx(_mm512_setr_epi32(0,0,12,0,0,14,0,0,0,0,0,0,0,0,0,0)),
          y1_f2_idx(_mm512_setr_epi32(0,11,0,0,13,0,0,15,0,0,0,0,0,0,0,0)),
          // U0..U11 in lanes 0..11.
          u_f0_idx(_mm512_setr_epi32(0,0,0,4,0,0,8,0,0,12,0,0,0,0,0,0)),
          u_f1_idx(_mm512_setr_epi32(0,1,0,0,5,0,0,9,0,0,13,0,0,0,0,0)),
          u_f2_idx(_mm512_setr_epi32(0,0,2,0,0,6,0,0,10,0,0,14,0,0,0,0)),
          // V0..V11 in lanes 0..11.
          v_f0_idx(_mm512_setr_epi32(0,2,0,0,6,0,0,10,0,0,14,0,0,0,0,0)),
          v_f1_idx(_mm512_setr_epi32(0,0,3,0,0,7,0,0,11,0,0,15,0,0,0,0)),
          v_f2_idx(_mm512_setr_epi32(0,0,0,4,0,0,8,0,0,12,0,0,0,0,0,0))
    {}
};

static inline __m512i compose3(__m512i a, __m512i b, __m512i c,
                               __m512i ia, __m512i ib, __m512i ic,
                               __mmask16 maskB, __mmask16 maskC)
{
    __m512i out = _mm512_permutexvar_epi32(ia, a);
    out = _mm512_mask_mov_epi32(out, maskB, _mm512_permutexvar_epi32(ib, b));
    out = _mm512_mask_mov_epi32(out, maskC, _mm512_permutexvar_epi32(ic, c));
    return out;
}

static inline void store12_u16(uint16_t* dst, __m512i v32)
{
    const __m256i v16 = _mm512_cvtepi32_epi16(v32);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(v16));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + 8), _mm256_extracti128_si256(v16, 1));
}

static inline void unpack24_avx512_from16words(const uint32_t* row,
                                               uint16_t*& yPtr,
                                               uint16_t*& uPtr,
                                               uint16_t*& vPtr,
                                               const V210Avx512Consts& cst)
{
    const __m512i v = _mm512_loadu_si512(reinterpret_cast<const void*>(row));
    const __m512i f0 = _mm512_and_si512(v, cst.mask10);
    const __m512i f1 = _mm512_and_si512(_mm512_srli_epi32(v, 10), cst.mask10);
    const __m512i f2 = _mm512_and_si512(_mm512_srli_epi32(v, 20), cst.mask10);

    // Source masks repeat the six-pixel v210 mapping:
    // Y pattern by output lane: f1,f0,f2,f1,f0,f2...
    const __m512i y0 = compose3(f0, f1, f2,
                                cst.y0_f0_idx, cst.y0_f1_idx, cst.y0_f2_idx,
                                0x9249u, 0x4924u);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(yPtr), _mm512_cvtepi32_epi16(y0));

    const __m512i y1 = compose3(f0, f1, f2,
                                cst.y1_f0_idx, cst.y1_f1_idx, cst.y1_f2_idx,
                                0x0024u, 0x0092u);
    const __m256i y1_16 = _mm512_cvtepi32_epi16(y1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(yPtr + 16), _mm256_castsi256_si128(y1_16));

    // U pattern: f0,f1,f2 repeated. V pattern: f2,f0,f1 repeated.
    const __m512i u = compose3(f0, f1, f2,
                               cst.u_f0_idx, cst.u_f1_idx, cst.u_f2_idx,
                               0x0492u, 0x0924u);
    store12_u16(uPtr, u);

    const __m512i vv = compose3(f2, f0, f1,
                                cst.v_f2_idx, cst.v_f0_idx, cst.v_f1_idx,
                                0x0492u, 0x0924u);
    store12_u16(vPtr, vv);

    yPtr += 24;
    uPtr += 12;
    vPtr += 12;
}

static inline void unpack_row_avx512(const uint8_t* srcRow,
                                     int width,
                                     uint16_t* yRow,
                                     uint16_t* uRow,
                                     uint16_t* vRow)
{
    const uint32_t* row = reinterpret_cast<const uint32_t*>(srcRow);
    uint16_t* yPtr = yRow;
    uint16_t* uPtr = uRow;
    uint16_t* vPtr = vRow;
    const V210Avx512Consts cst;

    int x = 0;
    // 1920 is exactly 80 x 24 pixels: no tail on the primary HD hot path.
    if (width == 1920) {
        for (int i = 0; i < 80; ++i) {
            _mm_prefetch(reinterpret_cast<const char*>(row + 32), _MM_HINT_T0);
            unpack24_avx512_from16words(row, yPtr, uPtr, vPtr, cst);
            row += 16;
        }
        return;
    }

    while (x + 23 < width) {
        _mm_prefetch(reinterpret_cast<const char*>(row + 32), _MM_HINT_T0);
        unpack24_avx512_from16words(row, yPtr, uPtr, vPtr, cst);
        row += 16;
        x += 24;
    }
    while (x + 5 < width) {
        write_group6_visible(row, 6, yPtr, uPtr, vPtr);
        row += 4;
        x += 6;
    }
    const int remaining = width - x;
    if (remaining > 0) write_group6_visible(row, remaining, yPtr, uPtr, vPtr);
}

static inline void process_rows_avx512(const uint8_t* src,
                                       int srcRowBytes,
                                       int width,
                                       int startRow,
                                       int endRow,
                                       uint16_t* dstY,
                                       uint16_t* dstU,
                                       uint16_t* dstV)
{
    const int chromaWidth = width / 2;
    for (int y = startRow; y < endRow; ++y) {
        const uint8_t* srcRow = src + static_cast<size_t>(y) * static_cast<size_t>(srcRowBytes);
        uint16_t* yRow = dstY + static_cast<size_t>(y) * static_cast<size_t>(width);
        uint16_t* uRow = dstU + static_cast<size_t>(y) * static_cast<size_t>(chromaWidth);
        uint16_t* vRow = dstV + static_cast<size_t>(y) * static_cast<size_t>(chromaWidth);
        unpack_row_avx512(srcRow, width, yRow, uRow, vRow);
    }
}
#endif

class RowThreadPool512 {
public:
    RowThreadPool512()
        : shutdown_(false), active_(false), generation_(0), nextRow_(0), endRow_(0), chunkRows_(1), remaining_(0),
          src_(nullptr), srcRowBytes_(0), width_(0), dstY_(nullptr), dstU_(nullptr), dstV_(nullptr)
    {
        const unsigned workerCount = choose_parallel_workers();
        workers_.reserve(workerCount);
        for (unsigned i = 0; i < workerCount; ++i) workers_.emplace_back(&RowThreadPool512::workerLoop, this);
    }

    ~RowThreadPool512()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            shutdown_ = true;
            active_ = false;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) if (t.joinable()) t.join();
    }

    unsigned workerCount() const { return static_cast<unsigned>(workers_.size()); }

    void run(const uint8_t* src, int srcRowBytes, int width, int height,
             uint16_t* dstY, uint16_t* dstU, uint16_t* dstV)
    {
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
        // A second capture instance uses the serial kernel rather than waiting
        // for, or overwriting, the shared pool's current job.
        std::unique_lock<std::mutex> jobLock(jobMutex_, std::try_to_lock);
        if (!jobLock.owns_lock() || workers_.empty() || !should_parallelize(width, height, workerCount())) {
            process_rows_avx512(src, srcRowBytes, width, 0, height, dstY, dstU, dstV);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            src_ = src; srcRowBytes_ = srcRowBytes; width_ = width;
            dstY_ = dstY; dstU_ = dstU; dstV_ = dstV;
            nextRow_.store(0, std::memory_order_release);
            endRow_ = height;
            chunkRows_ = static_cast<int>(choose_rows_per_task(height));
            remaining_.store(static_cast<int>(workers_.size()), std::memory_order_release);
            active_ = true;
            ++generation_;
        }
        cv_.notify_all();
        processChunks();
        std::unique_lock<std::mutex> lk(doneMtx_);
        doneCv_.wait(lk, [this] { return remaining_.load(std::memory_order_acquire) == 0; });
        {
            std::lock_guard<std::mutex> lk2(mtx_);
            active_ = false;
        }
#else
        (void)src; (void)srcRowBytes; (void)width; (void)height; (void)dstY; (void)dstU; (void)dstV;
#endif
    }

private:
    void processOneRange(int startRow, int endRow)
    {
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
        process_rows_avx512(src_, srcRowBytes_, width_, startRow, endRow, dstY_, dstU_, dstV_);
#else
        (void)startRow; (void)endRow;
#endif
    }

    void processChunks()
    {
        for (;;) {
            const int start = nextRow_.fetch_add(chunkRows_, std::memory_order_acq_rel);
            if (start >= endRow_) break;
            processOneRange(start, std::min(start + chunkRows_, endRow_));
        }
    }

    void workerLoop()
    {
        uint64_t seenGeneration = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this, &seenGeneration] { return shutdown_ || (active_ && generation_ != seenGeneration); });
                if (shutdown_) return;
                seenGeneration = generation_;
            }
            processChunks();
            if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lk(doneMtx_);
                doneCv_.notify_one();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::mutex jobMutex_;
    std::mutex mtx_, doneMtx_;
    std::condition_variable cv_, doneCv_;
    bool shutdown_, active_;
    uint64_t generation_;
    std::atomic<int> nextRow_;
    int endRow_, chunkRows_;
    std::atomic<int> remaining_;
    const uint8_t* src_;
    int srcRowBytes_, width_;
    uint16_t *dstY_, *dstU_, *dstV_;
};

static RowThreadPool512& row_pool512()
{
    static RowThreadPool512 pool;
    return pool;
}

} // namespace

void v210_to_yuv422p10le_avx512(
    const uint8_t* src, int srcRowBytes, int w, int h,
    uint16_t* dstY, uint16_t* dstU, uint16_t* dstV)
{
#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
    row_pool512().run(src, srcRowBytes, w, h, dstY, dstU, dstV);
#else
    v210_to_yuv422p10le_avx2(src, srcRowBytes, w, h, dstY, dstU, dstV);
#endif
}
