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
 * AVX2-accelerated v210 unpack implementation used by the DeckLink input path.
 * It converts packed 10-bit v210 rows into planar YUV422P10LE buffers, with a
 * scalar fallback when the translation unit is built without AVX2 support.
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

#if defined(__AVX2__)
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

    // Keep the default v210 worker count conservative. On 1080p50, using too
    // many unpack workers can compete with x264 and increase worst-case latency.
    // NXFRAME_V210_THREADS remains available for machine-specific tuning.
    unsigned auto_workers = 1;
    if (hw >= 8) auto_workers = 2;

    return env_u32("NXFRAME_V210_THREADS", auto_workers);
}

static inline unsigned choose_rows_per_task(int height)
{
    // Row chunks are intentionally coarse. The v210 unpacker is memory-bandwidth
    // sensitive, so tiny tasks add scheduling overhead without improving the hot
    // path on typical single-channel 1080i/p deployments.
    const unsigned auto_rows = (height >= 1080) ? 64u : 32u;
    return std::max(1u, env_u32("NXFRAME_V210_ROWS_PER_TASK", auto_rows));
}

static inline bool should_parallelize(int width, int height, unsigned workers)
{
    if (workers == 0) return false;
    if (width < 1280) return false;
    if (height < 200) return false;
    return true;
}

#if defined(__AVX2__)
struct V210Avx2Consts {
    __m256i mask10;
    __m256i y0_f1_idx;
    __m256i y0_f0_idx;
    __m256i y0_f2_idx;
    __m256i y8_f2_idx;
    __m256i y8_f1_idx;
    __m256i y8_f0_idx;
    __m256i u_f0_idx;
    __m256i u_f1_idx;
    __m256i u_f2_idx;
    __m256i v_f2_idx;
    __m256i v_f0_idx;
    __m256i v_f1_idx;

    V210Avx2Consts()
        : mask10(_mm256_set1_epi32(0x3FF)),
          y0_f1_idx(_mm256_setr_epi32(0, 0, 0, 2, 0, 0, 4, 0)),
          y0_f0_idx(_mm256_setr_epi32(0, 1, 0, 0, 3, 0, 0, 5)),
          y0_f2_idx(_mm256_setr_epi32(0, 0, 1, 0, 0, 3, 0, 0)),
          y8_f2_idx(_mm256_setr_epi32(5, 0, 0, 7, 0, 0, 0, 0)),
          y8_f1_idx(_mm256_setr_epi32(0, 6, 0, 0, 0, 0, 0, 0)),
          y8_f0_idx(_mm256_setr_epi32(0, 0, 7, 0, 0, 0, 0, 0)),
          u_f0_idx(_mm256_setr_epi32(0, 0, 0, 4, 0, 0, 0, 0)),
          u_f1_idx(_mm256_setr_epi32(0, 1, 0, 0, 5, 0, 0, 0)),
          u_f2_idx(_mm256_setr_epi32(0, 0, 2, 0, 0, 6, 0, 0)),
          v_f2_idx(_mm256_setr_epi32(0, 0, 0, 4, 0, 0, 0, 0)),
          v_f0_idx(_mm256_setr_epi32(0, 2, 0, 0, 6, 0, 0, 0)),
          v_f1_idx(_mm256_setr_epi32(0, 0, 3, 0, 0, 7, 0, 0))
    {}
};

static inline void write_group6_visible(const uint32_t* row,
                                        int visiblePixels,
                                        uint16_t*& yPtr,
                                        uint16_t*& uPtr,
                                        uint16_t*& vPtr)
{
    // Scalar tail path used for non-48/12-pixel remainders. Keeping the tail in
    // the AVX2 file avoids a dependency on private helpers from the scalar file
    // while preserving identical visible-pixel handling.
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

template<int Lane>
static inline uint16_t lane16(const __m256i& v)
{
    static_assert(Lane >= 0 && Lane < 8, "AVX2 lane index out of range");
    return static_cast<uint16_t>(_mm256_extract_epi32(v, Lane));
}

static inline void store4x32_as_u16(uint16_t* dst, const __m128i v32)
{
    const __m128i z = _mm_setzero_si128();
    const __m128i packed = _mm_packus_epi32(v32, z);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), packed);
}

static inline void store8x32_as_u16(uint16_t* dst, const __m256i v32)
{
    const __m256i z = _mm256_setzero_si256();
    const __m256i packed = _mm256_packus_epi32(v32, z);
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), _mm256_castsi256_si128(packed));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + 4), _mm256_extracti128_si256(packed, 1));
}

static inline __m256i select_y0_7(const __m256i f0, const __m256i f1, const __m256i f2, const V210Avx2Consts& cst)
{
    // Y0..Y7 = f1[0], f0[1], f2[1], f1[2], f0[3], f2[3], f1[4], f0[5]
    const __m256i a = _mm256_permutevar8x32_epi32(f1, cst.y0_f1_idx);
    const __m256i b = _mm256_permutevar8x32_epi32(f0, cst.y0_f0_idx);
    const __m256i c = _mm256_permutevar8x32_epi32(f2, cst.y0_f2_idx);
    return _mm256_blend_epi32(_mm256_blend_epi32(a, b, 0x92), c, 0x24);
}

static inline __m128i select_y8_11(const __m256i f0, const __m256i f1, const __m256i f2, const V210Avx2Consts& cst)
{
    // Y8..Y11 = f2[5], f1[6], f0[7], f2[7]
    const __m256i a = _mm256_permutevar8x32_epi32(f2, cst.y8_f2_idx);
    const __m256i b = _mm256_permutevar8x32_epi32(f1, cst.y8_f1_idx);
    const __m256i c = _mm256_permutevar8x32_epi32(f0, cst.y8_f0_idx);
    const __m256i y = _mm256_blend_epi32(_mm256_blend_epi32(a, b, 0x02), c, 0x04);
    return _mm256_castsi256_si128(y);
}

static inline __m256i select_u0_5(const __m256i f0, const __m256i f1, const __m256i f2, const V210Avx2Consts& cst)
{
    // U0..U5 = f0[0], f1[1], f2[2], f0[4], f1[5], f2[6]
    const __m256i a = _mm256_permutevar8x32_epi32(f0, cst.u_f0_idx);
    const __m256i b = _mm256_permutevar8x32_epi32(f1, cst.u_f1_idx);
    const __m256i c = _mm256_permutevar8x32_epi32(f2, cst.u_f2_idx);
    return _mm256_blend_epi32(_mm256_blend_epi32(a, b, 0x12), c, 0x24);
}

static inline __m256i select_v0_5(const __m256i f0, const __m256i f1, const __m256i f2, const V210Avx2Consts& cst)
{
    // V0..V5 = f2[0], f0[2], f1[3], f2[4], f0[6], f1[7]
    const __m256i a = _mm256_permutevar8x32_epi32(f2, cst.v_f2_idx);
    const __m256i b = _mm256_permutevar8x32_epi32(f0, cst.v_f0_idx);
    const __m256i c = _mm256_permutevar8x32_epi32(f1, cst.v_f1_idx);
    return _mm256_blend_epi32(_mm256_blend_epi32(a, b, 0x12), c, 0x24);
}

static inline void unpack12_avx2_from8words(const uint32_t* row,
                                            uint16_t*& yPtr,
                                            uint16_t*& uPtr,
                                            uint16_t*& vPtr,
                                            const V210Avx2Consts& cst)
{
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row));

    const __m256i f0 = _mm256_and_si256(v, cst.mask10);
    const __m256i f1 = _mm256_and_si256(_mm256_srli_epi32(v, 10), cst.mask10);
    const __m256i f2 = _mm256_and_si256(_mm256_srli_epi32(v, 20), cst.mask10);

    // v210 group mapping, two 6-pixel groups per 8 DWORDs:
    // word0: U0 Y0 V0 | word1: Y1 U2 Y2 | word2: V2 Y3 U4 | word3: Y4 V4 Y5
    // word4..7 repeat the same pattern for pixels 6..11.
    store8x32_as_u16(yPtr, select_y0_7(f0, f1, f2, cst));
    store4x32_as_u16(yPtr + 8, select_y8_11(f0, f1, f2, cst));

    const __m256i u = select_u0_5(f0, f1, f2, cst);
    store4x32_as_u16(uPtr, _mm256_castsi256_si128(u));
    uPtr[4] = lane16<4>(u);
    uPtr[5] = lane16<5>(u);

    const __m256i vv = select_v0_5(f0, f1, f2, cst);
    store4x32_as_u16(vPtr, _mm256_castsi256_si128(vv));
    vPtr[4] = lane16<4>(vv);
    vPtr[5] = lane16<5>(vv);

    yPtr += 12;
    uPtr += 6;
    vPtr += 6;
}

static inline void unpack48_avx2_from32words(const uint32_t* row,
                                             uint16_t*& yPtr,
                                             uint16_t*& uPtr,
                                             uint16_t*& vPtr,
                                             const V210Avx2Consts& cst)
{
    unpack12_avx2_from8words(row +  0, yPtr, uPtr, vPtr, cst);
    unpack12_avx2_from8words(row +  8, yPtr, uPtr, vPtr, cst);
    unpack12_avx2_from8words(row + 16, yPtr, uPtr, vPtr, cst);
    unpack12_avx2_from8words(row + 24, yPtr, uPtr, vPtr, cst);
}

static inline void unpack_row_avx2(const uint8_t* srcRow,
                                   int width,
                                   uint16_t* yRow,
                                   uint16_t* uRow,
                                   uint16_t* vRow)
{
    // The output plane layout matches NxFrame's internal bus:
    //   Y: width samples per row
    //   U/V: width / 2 samples per row
    // Each sample is a 10-bit value stored in a uint16_t container.
    const uint32_t* row = reinterpret_cast<const uint32_t*>(srcRow);
    uint16_t* yPtr = yRow;
    uint16_t* uPtr = uRow;
    uint16_t* vPtr = vRow;

    int x = 0;
    const V210Avx2Consts cst;

    // Main 1080p50/HD hot path. 1920 pixels is exactly 40 x 48-pixel chunks,
    // so avoid the generic remainder checks inside the row loop.
    if (width == 1920) {
        for (int i = 0; i < 40; ++i) {
            _mm_prefetch(reinterpret_cast<const char*>(row + 64), _MM_HINT_T0);
            unpack48_avx2_from32words(row, yPtr, uPtr, vPtr, cst);
            row += 32;
        }
        return;
    }

    while (x + 47 < width) {
        _mm_prefetch(reinterpret_cast<const char*>(row + 64), _MM_HINT_T0);
        unpack48_avx2_from32words(row, yPtr, uPtr, vPtr, cst);
        row += 32;
        x += 48;
    }
    while (x + 11 < width) {
        unpack12_avx2_from8words(row, yPtr, uPtr, vPtr, cst);
        row += 8;
        x += 12;
    }
    while (x + 5 < width) {
        write_group6_visible(row, 6, yPtr, uPtr, vPtr);
        row += 4;
        x += 6;
    }

    const int remaining = width - x;
    if (remaining > 0) {
        write_group6_visible(row, remaining, yPtr, uPtr, vPtr);
    }
}

static inline void process_rows_avx2(const uint8_t* src,
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
        unpack_row_avx2(srcRow, width, yRow, uRow, vRow);
    }
}
#endif

class RowThreadPool {
public:
    RowThreadPool()
        : shutdown_(false), active_(false), generation_(0), nextRow_(0), endRow_(0), chunkRows_(1), remaining_(0),
          src_(nullptr), srcRowBytes_(0), width_(0), dstY_(nullptr), dstU_(nullptr), dstV_(nullptr)
    {
        const unsigned workerCount = choose_parallel_workers();
        workers_.reserve(workerCount);
        for (unsigned i = 0; i < workerCount; ++i) {
            workers_.emplace_back(&RowThreadPool::workerLoop, this);
        }
    }

    ~RowThreadPool()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            shutdown_ = true;
            active_ = false;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    unsigned workerCount() const { return static_cast<unsigned>(workers_.size()); }

    void run(const uint8_t* src,
             int srcRowBytes,
             int width,
             int height,
             uint16_t* dstY,
             uint16_t* dstU,
             uint16_t* dstV)
    {
#if defined(__AVX2__)
        // The calling DeckLink callback must not create worker threads per
        // frame. A static pool keeps the real-time path allocation-free after
        // first use while allowing conservative row-level parallelism.
        if (workers_.empty() || !should_parallelize(width, height, workerCount())) {
            process_rows_avx2(src, srcRowBytes, width, 0, height, dstY, dstU, dstV);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            src_ = src;
            srcRowBytes_ = srcRowBytes;
            width_ = width;
            dstY_ = dstY;
            dstU_ = dstU;
            dstV_ = dstV;
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
        doneCv_.wait(lk, [this] {
            return remaining_.load(std::memory_order_acquire) == 0;
        });

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
#if defined(__AVX2__)
        process_rows_avx2(src_, srcRowBytes_, width_, startRow, endRow, dstY_, dstU_, dstV_);
#else
        (void)startRow; (void)endRow;
#endif
    }

    void processChunks()
    {
        for (;;) {
            const int start = nextRow_.fetch_add(chunkRows_, std::memory_order_acq_rel);
            if (start >= endRow_) break;
            const int end = std::min(start + chunkRows_, endRow_);
            processOneRange(start, end);
        }
    }

    void workerLoop()
    {
        uint64_t seenGeneration = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this, &seenGeneration] {
                    return shutdown_ || (active_ && generation_ != seenGeneration);
                });
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
    std::mutex mtx_;
    std::condition_variable cv_;
    std::mutex doneMtx_;
    std::condition_variable doneCv_;

    bool shutdown_;
    bool active_;
    uint64_t generation_;

    std::atomic<int> nextRow_;
    int endRow_;
    int chunkRows_;
    std::atomic<int> remaining_;

    const uint8_t* src_;
    int srcRowBytes_;
    int width_;
    uint16_t* dstY_;
    uint16_t* dstU_;
    uint16_t* dstV_;
};

static RowThreadPool& row_pool()
{
    static RowThreadPool pool;
    return pool;
}

} // namespace

void v210_to_yuv422p10le_avx2(
    const uint8_t* src, int srcRowBytes, int w, int h,
    uint16_t* dstY, uint16_t* dstU, uint16_t* dstV)
{
#if defined(__AVX2__)
    row_pool().run(src, srcRowBytes, w, h, dstY, dstU, dstV);
#else
    v210_to_yuv422p10le_scalar(src, srcRowBytes, w, h, dstY, dstU, dstV);
#endif
}