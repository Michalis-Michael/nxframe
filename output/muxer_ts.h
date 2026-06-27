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
 * MPEG-TS muxer declarations. MuxerTS stores codec parameters, owns the FFmpeg format context, normalizes live timestamps, and exposes pooled MPEG-TS output chunks to the transport layer.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// MPEG-TS muxer with memory-backed FFmpeg IO. Encoded AVPacket input is
// converted into ready-to-send MPEG-TS byte chunks for the transport layer.
class MuxerTS
{
public:
    // Ready MPEG-TS bytes produced by the custom AVIO writer. The shared_ptr
    // keeps the pooled backing chunk alive until the transport has sent it.
    struct OutputChunk
    {
        std::shared_ptr<uint8_t> data;
        size_t size = 0;
    };

    MuxerTS();
    ~MuxerTS();

    void setVideoCodecContext(AVCodecContext* ctx);
    void setAudioCodecContext(AVCodecContext* ctx);
    void setAudioCodecContexts(const std::vector<AVCodecContext*>& ctxs);

    bool initialize();

    bool writeVideoPacket(AVPacket* pkt);
    bool writeAudioPacket(AVPacket* pkt);

    void flushOutput();
    bool popOutputChunk(OutputChunk& out);
    void clearOutputQueue();
    size_t getReadyChunkCount() const;
    bool resetLiveSession();

    int64_t getLastPCR90k() const noexcept { return last_pcr_90k_; }
    uint64_t getSessionId() const noexcept { return session_id_; }
    bool isVideoSessionAnchored() const noexcept { return video_session_anchor_set_; }
    uint64_t getVideoDtsRepairCount() const noexcept { return video_dts_repair_count_.load(std::memory_order_relaxed); }
    uint64_t getVideoPtsRepairCount() const noexcept { return video_pts_repair_count_.load(std::memory_order_relaxed); }

    void enableTimestampDebug(bool enabled, int maxPacketsPerStream = 10);
    void enableTsFileCapture(const std::string& path);
    void disableTsFileCapture();
    void resetTimestampState(const char* reason = "manual");
    void setServiceMetadata(const std::string& provider, const std::string& name);
    void setMuxrateBps(int64_t muxrateBps);
    int64_t getMuxrateBps() const noexcept { return muxrate_bps_; }

    std::string getLastError() const;

private:
    class ChunkPool
    {
    public:
        // Lifetime invariant: OutputChunk shared_ptr instances acquired from
        // this pool must be released before MuxerTS/ChunkPool destruction.
        // The deleter returns chunk indices to this pool for low-allocation
        // MPEG-TS output under sustained live streaming.
        ChunkPool(size_t chunkSize, size_t preallocate);
        ~ChunkPool() = default;

        std::shared_ptr<uint8_t> acquire();
        size_t chunkSize() const noexcept { return chunk_size_; }

    private:
        size_t chunk_size_ = 0;
        mutable std::mutex mtx_;
        std::vector<std::unique_ptr<uint8_t[]> > storage_;
        std::queue<size_t> free_indices_;
    };

    struct StreamDebugState
    {
        int printed = 0;
        int max_to_print = 10;
        int64_t last_pts = AV_NOPTS_VALUE;
        int64_t last_dts = AV_NOPTS_VALUE;
        int64_t last_duration = 0;
    };

    // Cloned codec description used to recreate muxer sessions without
    // depending on the original encoder context lifetime.
    struct StoredStreamConfig
    {
        AVCodecParameters* codecpar = nullptr;
        AVRational encoder_time_base{0, 1};
    };

    static int writePacketCallback(void* opaque, const uint8_t* buf, int buf_size);

    bool createFormatContext();
    void destroyFormatContext(bool writeTrailer);
    bool configureStreamsFromStored();
    bool recreateSession(bool writeTrailer);

    bool appendOutputBytes(const uint8_t* buf, size_t len);
    bool ensureCurrentChunk();
    void sealCurrentChunk();

    static int64_t packetTimestampReference(const AVPacket* pkt);
    static void ensurePacketPtsDtsPair(AVPacket* pkt);
    void normalizePacketTsToLiveSessionBase(AVPacket* pkt, const char* tag);

    bool validatePacketPreRescale(const char* tag, AVPacket* pkt, StreamDebugState& st);
    bool validatePacketPostRescale(const char* tag,
                                   AVPacket* pkt,
                                   StreamDebugState& st,
                                   AVRational tb,
                                   bool normalized,
                                   int64_t basePts) const;

    void debugPrintPacket(const char* phase,
                          const char* tag,
                          const AVPacket* pkt,
                          AVRational tb,
                          bool normalized,
                          int64_t basePts,
                          const StreamDebugState& st) const;

    void logTimestampBase(const char* tag,
                          int streamIndex,
                          int64_t rawBasePts,
                          AVRational rawTimeBase,
                          int64_t base90k);

    bool writePacketInternal(AVPacket* pkt,
                             AVStream* stream,
                             AVRational encTb,
                             StreamDebugState& dbg,
                             int64_t& basePts,
                             bool& baseSet,
                             const char* tag,
                             bool updatePcr);

    static AVCodecParameters* cloneCodecParametersFromContext(AVCodecContext* ctx);

private:
    static const size_t kIoBufferSize = 32768;
    static const size_t kOutputChunkSize = 65536;
    static const size_t kOutputChunkCount = 32;

    AVFormatContext* format_ctx_ = nullptr;
    AVIOContext* io_ctx_ = nullptr;
    uint8_t* io_buffer_ = nullptr;

    AVStream* video_stream_ = nullptr;
    std::vector<AVStream*> audio_streams_;

    StoredStreamConfig video_cfg_;
    std::vector<StoredStreamConfig> audio_cfgs_;

    bool header_written_ = false;

    ChunkPool chunk_pool_;
    std::shared_ptr<uint8_t> current_chunk_;
    size_t current_chunk_bytes_ = 0;

    mutable std::mutex ready_chunks_mutex_;
    std::deque<OutputChunk> ready_chunks_;

    bool video_base_set_ = false;
    std::vector<uint8_t> audio_base_set_;
    int64_t video_base_pts_ = AV_NOPTS_VALUE;
    std::vector<int64_t> audio_base_pts_;
    int64_t last_pcr_90k_ = -1;
    bool session_base_90k_set_ = false;
    bool video_session_anchor_set_ = false;
    int64_t session_base_90k_ = AV_NOPTS_VALUE;
    int64_t first_video_base_90k_ = AV_NOPTS_VALUE;
    std::vector<int64_t> first_audio_base_90k_;
    uint64_t session_id_ = 0;

    bool timestamp_debug_enabled_ = false;
    std::atomic<uint64_t> video_dts_repair_count_{0};
    std::atomic<uint64_t> video_pts_repair_count_{0};
    StreamDebugState video_debug_;
    std::vector<StreamDebugState> audio_debugs_;

    std::ofstream capture_file_;
    std::string capture_path_;

    std::string service_provider_ = "NxFrame";
    std::string service_name_ = "NxFrame Contribution Feed";
    int64_t muxrate_bps_ = 0;

    mutable std::mutex err_mutex_;
    std::string last_error_;
};