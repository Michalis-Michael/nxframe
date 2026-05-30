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
 * Core video/audio frame structures. These types carry owned media buffers, timing, colorimetry, HDR data, and broadcast metadata between pipeline stages.
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>

#include "core/metadata.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/mastering_display_metadata.h>
}


struct VideoFrame
{
    // Shared ownership buffer for the underlying media payload. Plane pointers below must remain inside this allocation.
    std::shared_ptr<uint8_t> buffer;
    size_t buffer_size = 0;

    int width  = 0;
    int height = 0;

    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;

    // FFmpeg-style plane pointers used to avoid copying when wrapping frames for FFmpeg encoders/decoders.
    uint8_t* data[3] = { nullptr, nullptr, nullptr };
    int linesize[3]  = { 0, 0, 0 };

    // Timing
    int64_t pts = 0;
    // Legacy/general timing field used by older parts of the pipeline.
    AVRational time_base{1, 50};
    // Original timestamp clock from demux/decode.
    AVRational pts_time_base{1, 50};
    // Nominal output cadence hint derived from stream metadata or measured cadence.
    AVRational nominal_frame_rate{0, 1};

    bool interlaced = false;
    bool tff = true;

    // Colorimetry / HDR metadata propagated through the video pipeline.
    // Encoder presets remain the fallback when these values are unspecified.
    AVColorPrimaries color_primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic color_trc = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace colorspace = AVCOL_SPC_UNSPECIFIED;
    AVColorRange color_range = AVCOL_RANGE_UNSPECIFIED;
    AVChromaLocation chroma_location = AVCHROMA_LOC_UNSPECIFIED;

    // Optional HDR10 metadata for PQ/ST2084 workflows. Values use FFmpeg's
    // AVMasteringDisplayMetadata / AVContentLightMetadata representation.
    bool has_mastering_display = false;
    AVMasteringDisplayMetadata mastering_display{};
    bool has_content_light = false;
    AVContentLightMetadata content_light{};

    // Broadcast metadata carried alongside this video frame. Alpha 2.1 uses
    // this for SMPTE ST 12/RP188 timecode and provides the raw ANC packet
    // foundation for later ST 334 captions, ST 352 VPID and SCTE-104.
    FrameMetadata metadata;
};


struct AudioFrame
{
    std::shared_ptr<uint8_t> buffer;
    size_t buffer_size = 0;

    int sample_rate = 48000;
    int channels    = 2;

    int bytes_per_sample = 2; // container bytes per sample: 2=S16, 4=S32/24-in-32
    int valid_bits_per_sample = 0; // 0 = same as bytes_per_sample * 8
    int num_samples      = 0; // samples per channel

    int64_t pts = 0;
    AVRational time_base{1, 48000};
};

// Allocate a shared byte buffer with array-delete semantics for frame payload ownership.

inline std::shared_ptr<uint8_t> make_shared_u8(size_t bytes)
{
    return std::shared_ptr<uint8_t>(
        new uint8_t[bytes],
        std::default_delete<uint8_t[]>()
    );
}