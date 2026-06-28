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
 * H.264 encoder implementation. This module maps NxFrame video frames into FFmpeg AVFrames, applies preset-driven libx264 options, preserves metadata, drains packets, and keeps buffer ownership safe for the zero-copy-oriented path.
 */

#include "encoder_x264.h"

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <chrono>
#include <unordered_map>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/buffer.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}

#include "../stage_timing.h"

using json = nlohmann::json;

namespace {

// Owner object attached to AVBufferRef. It keeps the pipeline shared_ptr alive
// until FFmpeg/x264 is finished with the submitted input frame.
struct SharedBufHolder {
    std::shared_ptr<uint8_t> ptr;
};

static void avbuffer_release_sharedptr(void* opaque, uint8_t* /*data*/)
{
    delete reinterpret_cast<SharedBufHolder*>(opaque);
}

} // namespace

struct X264RuntimeState {
    AVPixelFormat internalFmt = AV_PIX_FMT_YUV422P10LE;
    AVPixelFormat targetFmt   = AV_PIX_FMT_YUV422P10LE;
    bool outputInterlaced     = false;
    bool outputTff            = true;
    SwsContext* sws           = nullptr;
    AVFrame* convertedFrame   = nullptr;

    ~X264RuntimeState()
    {
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        if (convertedFrame) {
            av_frame_free(&convertedFrame);
        }
    }
};

namespace {

static void reportX264EncodeTiming(uint64_t elapsedNs, bool producedPacket)
{
    struct Stats {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> packets{0};
        std::atomic<uint64_t> totalNs{0};
        std::atomic<uint64_t> maxNs{0};
        std::atomic<int64_t> lastReportNs{0};
    };

    static Stats st;

    st.calls.fetch_add(1, std::memory_order_relaxed);
    if (producedPacket) {
        st.packets.fetch_add(1, std::memory_order_relaxed);
    }
    st.totalNs.fetch_add(elapsedNs, std::memory_order_relaxed);

    uint64_t oldMax = st.maxNs.load(std::memory_order_relaxed);
    while (elapsedNs > oldMax &&
           !st.maxNs.compare_exchange_weak(oldMax, elapsedNs, std::memory_order_relaxed)) {
    }

    const auto now = std::chrono::steady_clock::now();
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    int64_t last = st.lastReportNs.load(std::memory_order_relaxed);
    if (last != 0 && nowNs - last < 5000000000LL) {
        return;
    }
    if (!st.lastReportNs.compare_exchange_strong(last, nowNs, std::memory_order_relaxed)) {
        return;
    }

    const uint64_t calls = st.calls.exchange(0, std::memory_order_relaxed);
    const uint64_t packets = st.packets.exchange(0, std::memory_order_relaxed);
    const uint64_t total = st.totalNs.exchange(0, std::memory_order_relaxed);
    const uint64_t maxv = st.maxNs.exchange(0, std::memory_order_relaxed);
    if (calls == 0) {
        return;
    }

    const double avgUs = static_cast<double>(total) / static_cast<double>(calls) / 1000.0;
    const double maxUs = static_cast<double>(maxv) / 1000.0;
    std::cout << "[EncoderX264] x264_encode avg_us=" << avgUs
              << " max_us=" << maxUs
              << " frames=" << calls
              << " packets=" << packets
              << "\n";
}

static int setPrivateOptionChecked(void* obj,
                                   const std::string& key,
                                   const json& value)
{
    if (!obj || key.empty()) return AVERROR(EINVAL);

    if (value.is_number_integer()) {
        return av_opt_set_int(obj, key.c_str(), value.get<int>(), 0);
    }
    if (value.is_number_float()) {
        return av_opt_set_double(obj, key.c_str(), value.get<double>(), 0);
    }
    if (value.is_string()) {
        return av_opt_set(obj, key.c_str(), value.get<std::string>().c_str(), 0);
    }
    return AVERROR(EINVAL);
}

static const json* getVideoSection(const json& preset)
{
    if (preset.contains("video") && preset["video"].is_object())
        return &preset["video"];
    return nullptr;
}

static int getIntFlexible(const json& root,
                          const json* video,
                          const std::string& key,
                          int def)
{
    if (video && video->contains(key) && (*video)[key].is_number_integer())
        return (*video)[key].get<int>();
    if (root.contains(key) && root[key].is_number_integer())
        return root[key].get<int>();
    return def;
}

static std::string getStringFlexible(const json& root,
                                     const json* video,
                                     const std::string& key,
                                     const std::string& def)
{
    if (video && video->contains(key) && (*video)[key].is_string())
        return (*video)[key].get<std::string>();
    if (root.contains(key) && root[key].is_string())
        return root[key].get<std::string>();
    return def;
}

static bool getBoolFlexible(const json& root,
                            const json* video,
                            const std::string& key,
                            bool def)
{
    if (video && video->contains(key) && (*video)[key].is_boolean())
        return (*video)[key].get<bool>();
    if (root.contains(key) && root[key].is_boolean())
        return root[key].get<bool>();
    return def;
}

static const json* getObjectFlexible(const json& root,
                                     const json* video,
                                     const std::string& key)
{
    if (video && video->contains(key) && (*video)[key].is_object())
        return &(*video)[key];
    if (root.contains(key) && root[key].is_object())
        return &root[key];
    return nullptr;
}

static int getIntFromObject(const json* obj, const std::string& key, int def)
{
    if (obj && obj->contains(key) && (*obj)[key].is_number_integer())
        return (*obj)[key].get<int>();
    return def;
}

static bool getBoolFromObject(const json* obj, const std::string& key, bool def)
{
    if (obj && obj->contains(key) && (*obj)[key].is_boolean())
        return (*obj)[key].get<bool>();
    return def;
}

static std::string getStringFromObject(const json* obj,
                                       const std::string& key,
                                       const std::string& def)
{
    if (obj && obj->contains(key) && (*obj)[key].is_string())
        return (*obj)[key].get<std::string>();
    return def;
}

static std::string toLowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static void appendX264Param(std::string& params, const std::string& key, const std::string& value)
{
    if (key.empty() || value.empty()) return;
    params += ":" + key + "=" + value;
}

static void appendX264Param(std::string& params, const std::string& key, int value)
{
    appendX264Param(params, key, std::to_string(value));
}


static std::string jsonScalarToX264String(const json& value)
{
    if (value.is_boolean()) {
        return value.get<bool>() ? "1" : "0";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned>());
    }
    if (value.is_number_float()) {
        std::ostringstream oss;
        oss << value.get<double>();
        return oss.str();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::string();
}

static std::string normalizeX264OptionKey(std::string key)
{
    std::replace(key.begin(), key.end(), '_', '-');
    return key;
}

static std::string makeX264ParamValueSafe(const std::string& key,
                                          std::string value)
{
    // FFmpeg's libx264 x264-params option is ':' separated. Some native
    // x264 values also historically use ':' internally. x264 accepts ',' as
    // the internal separator for these options, which keeps the outer
    // x264-params list parseable.
    if (key == "deblock" || key == "psy-rd") {
        std::replace(value.begin(), value.end(), ':', ',');
    }
    return value;
}

static bool appendX264NativeOption(std::string& x264_params,
                                   const std::string& jsonKey,
                                   const json& value)
{
    static const std::unordered_map<std::string, std::string> nativeNames = {
        {"level", "level"},
        {"vbv-init", "vbv-init"},
        {"rc-lookahead", "rc-lookahead"},
        {"sync-lookahead", "sync-lookahead"},
        {"lookahead-threads", "lookahead-threads"},
        {"sliced-threads", "sliced-threads"},
        {"slices", "slices"},
        {"ref", "ref"},
        {"subme", "subme"},
        {"trellis", "trellis"},
        {"me", "me"},
        {"merange", "merange"},
        {"direct", "direct"},
        {"bframes", "bframes"},
        {"b-adapt", "b-adapt"},
        {"b-pyramid", "b-pyramid"},
        {"weightp", "weightp"},
        {"fast-pskip", "fast-pskip"},
        {"dct-decimate", "dct-decimate"},
        {"deblock", "deblock"},
        {"aq-mode", "aq-mode"},
        {"aq-strength", "aq-strength"},
        {"psy", "psy"},
        {"psy-rd", "psy-rd"},
        {"aud", "aud"},
        {"pic-struct", "pic-struct"},
        {"no-mbtree", "no-mbtree"}
    };

    const std::string normalized = normalizeX264OptionKey(jsonKey);
    const auto it = nativeNames.find(normalized);
    if (it == nativeNames.end()) {
        return false;
    }

    std::string encoded = jsonScalarToX264String(value);
    if (encoded.empty()) {
        std::cerr << "[EncoderX264] WARN: x264-native option '" << jsonKey
                  << "' has unsupported JSON value type; ignoring.\n";
        return true;
    }

    encoded = makeX264ParamValueSafe(it->second, encoded);
    appendX264Param(x264_params, it->second, encoded);
    return true;
}

static bool isEncoderManagedOption(const std::string& key)
{
    const std::string k = normalizeX264OptionKey(key);
    return k == "rate-control" || k == "filler" || k == "nal-hrd" ||
           k == "interlaced" || k == "field-order" ||
           k == "min-keyint" || k == "scenecut" || k == "gop-closed" ||
           k == "color-primaries" || k == "color-transfer" ||
           k == "colorspace" || k == "color-range" || k == "chroma-location" ||
           k == "threads";
}

static AVColorPrimaries parseColorPrimaries(const std::string& value)
{
    const std::string v = toLowerCopy(value);
    if (v.empty() || v == "auto") return AVCOL_PRI_UNSPECIFIED;
    if (v == "bt709" || v == "709" || v == "rec709") return AVCOL_PRI_BT709;
    if (v == "bt470bg" || v == "470bg" || v == "rec601-pal" || v == "601-pal") return AVCOL_PRI_BT470BG;
    if (v == "smpte170m" || v == "170m" || v == "rec601-ntsc" || v == "601-ntsc") return AVCOL_PRI_SMPTE170M;
    if (v == "bt2020" || v == "2020" || v == "rec2020") return AVCOL_PRI_BT2020;
    if (v == "smpte240m" || v == "240m") return AVCOL_PRI_SMPTE240M;
    return AVCOL_PRI_UNSPECIFIED;
}

static AVColorTransferCharacteristic parseColorTransfer(const std::string& value)
{
    const std::string v = toLowerCopy(value);
    if (v.empty() || v == "auto") return AVCOL_TRC_UNSPECIFIED;
    if (v == "bt709" || v == "709" || v == "rec709") return AVCOL_TRC_BT709;
    if (v == "bt470bg" || v == "470bg") return AVCOL_TRC_GAMMA28;
    if (v == "smpte170m" || v == "170m") return AVCOL_TRC_SMPTE170M;
    if (v == "smpte240m" || v == "240m") return AVCOL_TRC_SMPTE240M;
    if (v == "linear") return AVCOL_TRC_LINEAR;
    if (v == "log100") return AVCOL_TRC_LOG;
    if (v == "log316") return AVCOL_TRC_LOG_SQRT;
    if (v == "iec61966-2-4" || v == "xvycc") return AVCOL_TRC_IEC61966_2_4;
    if (v == "bt1361" || v == "bt1361e") return AVCOL_TRC_BT1361_ECG;
    if (v == "iec61966-2-1" || v == "srgb") return AVCOL_TRC_IEC61966_2_1;
    if (v == "bt2020-10" || v == "2020-10") return AVCOL_TRC_BT2020_10;
    if (v == "bt2020-12" || v == "2020-12") return AVCOL_TRC_BT2020_12;
    if (v == "pq" || v == "smpte2084" || v == "st2084") return AVCOL_TRC_SMPTE2084;
    if (v == "hlg" || v == "arib-std-b67" || v == "arib_b67") return AVCOL_TRC_ARIB_STD_B67;
    return AVCOL_TRC_UNSPECIFIED;
}

static AVColorSpace parseColorSpace(const std::string& value)
{
    const std::string v = toLowerCopy(value);
    if (v.empty() || v == "auto") return AVCOL_SPC_UNSPECIFIED;
    if (v == "bt709" || v == "709" || v == "rec709") return AVCOL_SPC_BT709;
    if (v == "fcc") return AVCOL_SPC_FCC;
    if (v == "bt470bg" || v == "470bg" || v == "rec601-pal" || v == "601-pal") return AVCOL_SPC_BT470BG;
    if (v == "smpte170m" || v == "170m" || v == "rec601-ntsc" || v == "601-ntsc") return AVCOL_SPC_SMPTE170M;
    if (v == "smpte240m" || v == "240m") return AVCOL_SPC_SMPTE240M;
    if (v == "ycgco" || v == "ycocg") return AVCOL_SPC_YCGCO;
    if (v == "bt2020nc" || v == "2020nc" || v == "bt2020-ncl" || v == "2020ncl" || v == "rec2020nc") return AVCOL_SPC_BT2020_NCL;
    if (v == "bt2020c" || v == "2020c" || v == "bt2020-cl" || v == "2020cl" || v == "rec2020c") return AVCOL_SPC_BT2020_CL;
    return AVCOL_SPC_UNSPECIFIED;
}

static AVColorRange parseColorRange(const std::string& value)
{
    const std::string v = toLowerCopy(value);
    if (v.empty() || v == "auto") return AVCOL_RANGE_UNSPECIFIED;
    if (v == "tv" || v == "limited" || v == "mpeg") return AVCOL_RANGE_MPEG;
    if (v == "pc" || v == "full" || v == "jpeg") return AVCOL_RANGE_JPEG;
    return AVCOL_RANGE_UNSPECIFIED;
}

static AVChromaLocation parseChromaLocation(const std::string& value)
{
    const std::string v = toLowerCopy(value);
    if (v.empty() || v == "auto") return AVCHROMA_LOC_UNSPECIFIED;
    if (v == "left") return AVCHROMA_LOC_LEFT;
    if (v == "center") return AVCHROMA_LOC_CENTER;
    if (v == "topleft" || v == "top-left") return AVCHROMA_LOC_TOPLEFT;
    if (v == "top") return AVCHROMA_LOC_TOP;
    if (v == "bottomleft" || v == "bottom-left") return AVCHROMA_LOC_BOTTOMLEFT;
    if (v == "bottom") return AVCHROMA_LOC_BOTTOM;
    return AVCHROMA_LOC_UNSPECIFIED;
}


static bool isHdrTransfer(AVColorTransferCharacteristic trc)
{
    return trc == AVCOL_TRC_ARIB_STD_B67 || trc == AVCOL_TRC_SMPTE2084;
}

static int pixFmtBitDepth(AVPixelFormat fmt)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || desc->nb_components <= 0) return 0;
    return desc->comp[0].depth;
}

static std::string x264ColorPrimariesName(AVColorPrimaries v)
{
    switch (v) {
        case AVCOL_PRI_BT709: return "bt709";
        case AVCOL_PRI_BT470BG: return "bt470bg";
        case AVCOL_PRI_SMPTE170M: return "smpte170m";
        case AVCOL_PRI_SMPTE240M: return "smpte240m";
        case AVCOL_PRI_BT2020: return "bt2020";
        default: return "";
    }
}

static std::string x264TransferName(AVColorTransferCharacteristic v)
{
    switch (v) {
        case AVCOL_TRC_BT709: return "bt709";
        case AVCOL_TRC_SMPTE170M: return "smpte170m";
        case AVCOL_TRC_SMPTE240M: return "smpte240m";
        case AVCOL_TRC_LINEAR: return "linear";
        case AVCOL_TRC_LOG: return "log100";
        case AVCOL_TRC_LOG_SQRT: return "log316";
        case AVCOL_TRC_IEC61966_2_4: return "iec61966-2-4";
        case AVCOL_TRC_BT1361_ECG: return "bt1361e";
        case AVCOL_TRC_IEC61966_2_1: return "iec61966-2-1";
        case AVCOL_TRC_BT2020_10: return "bt2020-10";
        case AVCOL_TRC_BT2020_12: return "bt2020-12";
        case AVCOL_TRC_SMPTE2084: return "smpte2084";
        case AVCOL_TRC_ARIB_STD_B67: return "arib-std-b67";
        default: return "";
    }
}

static std::string x264ColorMatrixName(AVColorSpace v)
{
    switch (v) {
        case AVCOL_SPC_BT709: return "bt709";
        case AVCOL_SPC_FCC: return "fcc";
        case AVCOL_SPC_BT470BG: return "bt470bg";
        case AVCOL_SPC_SMPTE170M: return "smpte170m";
        case AVCOL_SPC_SMPTE240M: return "smpte240m";
        case AVCOL_SPC_YCGCO: return "YCgCo";
        case AVCOL_SPC_BT2020_NCL: return "bt2020nc";
        case AVCOL_SPC_BT2020_CL: return "bt2020c";
        default: return "";
    }
}

static AVPixelFormat mapOutputFormat(int bitDepth, const std::string& chroma)
{
    if (bitDepth == 8 && chroma == "420")  return AV_PIX_FMT_YUV420P;
    if (bitDepth == 8 && chroma == "422")  return AV_PIX_FMT_YUV422P;
    if (bitDepth == 10 && chroma == "420") return AV_PIX_FMT_YUV420P10LE;
    if (bitDepth == 10 && chroma == "422") return AV_PIX_FMT_YUV422P10LE;
    return AV_PIX_FMT_NONE;
}

static std::string defaultProfileFor(AVPixelFormat fmt)
{
    switch (fmt) {
        case AV_PIX_FMT_YUV420P:     return "high";
        case AV_PIX_FMT_YUV420P10LE: return "high10";
        case AV_PIX_FMT_YUV422P:     return "high422";
        case AV_PIX_FMT_YUV422P10LE: return "high422";
        default:                     return "high";
    }
}

static const char* pixFmtNameSafe(AVPixelFormat fmt)
{
    const char* n = av_get_pix_fmt_name(fmt);
    return n ? n : "unknown";
}

static std::string rationalToString(AVRational r)
{
    return std::to_string(r.num) + "/" + std::to_string(r.den);
}

static const char* fieldOrderName(AVFieldOrder order)
{
    switch (order) {
        case AV_FIELD_PROGRESSIVE: return "progressive";
        case AV_FIELD_TT:          return "tff";
        case AV_FIELD_BB:          return "bff";
        case AV_FIELD_TB:          return "tff-mixed";
        case AV_FIELD_BT:          return "bff-mixed";
        case AV_FIELD_UNKNOWN:
        default:                   return "unknown";
    }
}

static const char* profileNameSafe(const AVCodecContext* ctx)
{
    if (!ctx) return "unknown";

    const char* name = avcodec_profile_name(ctx->codec_id, ctx->profile);
    return name ? name : "unknown";
}

static bool fillFramePointersForContiguousInternalBus(AVFrame* f,
                                                      uint8_t* base,
                                                      int width,
                                                      int height)
{
    if (!f || !base || width <= 0 || height <= 0) return false;

    const size_t yBytes  = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
    const size_t uvBytes = static_cast<size_t>(width / 2) * static_cast<size_t>(height) * 2;

    f->format = AV_PIX_FMT_YUV422P10LE;
    f->width  = width;
    f->height = height;

    f->data[0] = base;
    f->data[1] = base + yBytes;
    f->data[2] = base + yBytes + uvBytes;

    f->linesize[0] = width * 2;
    f->linesize[1] = (width / 2) * 2;
    f->linesize[2] = (width / 2) * 2;

    return true;
}

static void applyFrameCodingMetadata(AVFrame* f,
                                     const X264RuntimeState& st,
                                     bool forceKeyframe)
{
    if (!f) return;

    f->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    if (forceKeyframe) {
        f->flags |= AV_FRAME_FLAG_KEY;
    } else {
        f->flags &= ~AV_FRAME_FLAG_KEY;
    }

#ifdef AV_FRAME_FLAG_INTERLACED
    if (st.outputInterlaced) {
        f->flags |= AV_FRAME_FLAG_INTERLACED;
        if (st.outputTff) {
            f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
        } else {
            f->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
        }
    } else {
        f->flags &= ~AV_FRAME_FLAG_INTERLACED;
        f->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
    }
#endif
}

static bool ensureConvertedFrame(X264RuntimeState& st,
                                 int width,
                                 int height,
                                 AVPixelFormat targetFmt)
{
    if (st.convertedFrame &&
        st.convertedFrame->width  == width &&
        st.convertedFrame->height == height &&
        st.convertedFrame->format == targetFmt) {
        return true;
    }

    if (st.convertedFrame) {
        av_frame_free(&st.convertedFrame);
    }

    st.convertedFrame = av_frame_alloc();
    if (!st.convertedFrame) return false;

    st.convertedFrame->format = targetFmt;
    st.convertedFrame->width  = width;
    st.convertedFrame->height = height;

    if (av_frame_get_buffer(st.convertedFrame, 32) < 0) {
        av_frame_free(&st.convertedFrame);
        return false;
    }

    return true;
}

static SwsContext* ensureSws(X264RuntimeState& st,
                             int width,
                             int height,
                             AVPixelFormat srcFmt,
                             AVPixelFormat dstFmt)
{
    st.sws = sws_getCachedContext(
        st.sws,
        width, height, srcFmt,
        width, height, dstFmt,
        SWS_BICUBIC,
        nullptr, nullptr, nullptr
    );
    return st.sws;
}


static inline uint8_t clipU8FromInt(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

// FFmpeg planar 10-bit formats are stored in 16-bit words with the active
// 10-bit value in the low bits. This converts 0..1023 -> 0..255 by rounding.
// For limited-range SDI video this preserves legal-range mapping:
//   Y  64..940  -> 16..235
//   C  64..960  -> 16..240
static inline uint8_t p10ToP8(uint16_t v)
{
    return clipU8FromInt((static_cast<int>(v) + 2) >> 2);
}

// Field-aware 4:2:2 10-bit -> 4:2:0 8-bit converter for interlaced output.
//
// swscale's normal vertical chroma downsample can behave like a progressive
// conversion and blend adjacent raster lines. In 1080i50, adjacent raster lines
// belong to different temporal fields, so progressive downsample can soften or
// dirty chroma edges during motion. This converter keeps the fields separate:
//
//   output chroma line 0 = average source chroma lines 0 and 2  (top field)
//   output chroma line 1 = average source chroma lines 1 and 3  (bottom field)
//   output chroma line 2 = average source chroma lines 4 and 6  (top field)
//   output chroma line 3 = average source chroma lines 5 and 7  (bottom field)
//
// It is intentionally used only for NxFrame's internal yuv422p10le bus to
// yuv420p interlaced x264 path. All other conversions still use swscale.
static bool convertYuv422p10leToYuv420p8Interlaced(const AVFrame* src, AVFrame* dst)
{
    if (!src || !dst) return false;
    if (src->format != AV_PIX_FMT_YUV422P10LE) return false;
    if (dst->format != AV_PIX_FMT_YUV420P) return false;

    const int w = src->width;
    const int h = src->height;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) {
        std::cerr << "[EncoderX264] ERROR: field-aware 422p10->420p8 requires positive even dimensions."
                  << " got " << w << "x" << h << "\n";
        return false;
    }

    const int cw = w / 2;
    const int ch = h / 2;

    // Luma: 10-bit -> 8-bit, no spatial resampling.
    for (int y = 0; y < h; ++y) {
        const auto* srcY = reinterpret_cast<const uint16_t*>(
            src->data[0] + static_cast<size_t>(y) * src->linesize[0]);
        auto* dstY = dst->data[0] + static_cast<size_t>(y) * dst->linesize[0];

        for (int x = 0; x < w; ++x) {
            dstY[x] = p10ToP8(srcY[x]);
        }
    }

    // Chroma: source is 4:2:2, so chroma is already half horizontal resolution
    // and full vertical resolution. Target 4:2:0 halves vertical chroma, but
    // for interlaced video we must average within the same field only.
    for (int cy = 0; cy < ch; ++cy) {
        const int field = cy & 1;            // 0 = top/even raster lines, 1 = bottom/odd raster lines
        const int fieldPair = cy >> 1;
        const int srcY0 = fieldPair * 4 + field;
        int srcY1 = srcY0 + 2;
        if (srcY1 >= h) srcY1 = srcY0;

        const auto* srcU0 = reinterpret_cast<const uint16_t*>(
            src->data[1] + static_cast<size_t>(srcY0) * src->linesize[1]);
        const auto* srcU1 = reinterpret_cast<const uint16_t*>(
            src->data[1] + static_cast<size_t>(srcY1) * src->linesize[1]);
        const auto* srcV0 = reinterpret_cast<const uint16_t*>(
            src->data[2] + static_cast<size_t>(srcY0) * src->linesize[2]);
        const auto* srcV1 = reinterpret_cast<const uint16_t*>(
            src->data[2] + static_cast<size_t>(srcY1) * src->linesize[2]);

        auto* dstU = dst->data[1] + static_cast<size_t>(cy) * dst->linesize[1];
        auto* dstV = dst->data[2] + static_cast<size_t>(cy) * dst->linesize[2];

        for (int x = 0; x < cw; ++x) {
            const uint16_t uAvg = static_cast<uint16_t>(
                (static_cast<int>(srcU0[x]) + static_cast<int>(srcU1[x]) + 1) >> 1);
            const uint16_t vAvg = static_cast<uint16_t>(
                (static_cast<int>(srcV0[x]) + static_cast<int>(srcV1[x]) + 1) >> 1);

            dstU[x] = p10ToP8(uAvg);
            dstV[x] = p10ToP8(vAvg);
        }
    }

    dst->pts = src->pts;
    dst->duration = src->duration;
    dst->sample_aspect_ratio = src->sample_aspect_ratio;
    dst->color_range = src->color_range;
    dst->color_primaries = src->color_primaries;
    dst->color_trc = src->color_trc;
    dst->colorspace = src->colorspace;
    dst->chroma_location = AVCHROMA_LOC_LEFT;

#ifdef AV_FRAME_FLAG_INTERLACED
    dst->flags = src->flags;
    dst->flags |= AV_FRAME_FLAG_INTERLACED;
    if (src->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) {
        dst->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
    }
#endif

    return true;
}

} // namespace

// Returns the submitted frame unchanged when the preset target equals the
// internal YUV422P10LE bus. Any other target format uses the explicit conversion
// fallback and is therefore not zero-copy.
bool EncoderX264::prepareConvertedFrame(AVFrame* srcFrame, AVFrame** outFrame)
{
    stage_timing::ScopedTimer timer(stage_timing::get("x264_prepare_frame"));
    if (!srcFrame || !outFrame || !runtime_)
        return false;

    X264RuntimeState& st = *runtime_;

    if (st.targetFmt == AV_PIX_FMT_YUV422P10LE) {
        *outFrame = srcFrame;
        return true;
    }

    if (!ensureConvertedFrame(st, srcFrame->width, srcFrame->height, st.targetFmt)) {
        std::cerr << "[EncoderX264] ERROR: Failed to allocate converted frame.\n";
        return false;
    }

    if (av_frame_make_writable(st.convertedFrame) < 0) {
        std::cerr << "[EncoderX264] ERROR: converted frame not writable.\n";
        return false;
    }

    if (st.outputInterlaced &&
        st.internalFmt == AV_PIX_FMT_YUV422P10LE &&
        st.targetFmt == AV_PIX_FMT_YUV420P) {
        stage_timing::ScopedTimer convTimer(stage_timing::get("x264_convert_422p10_to_420p8_interlaced"));

        if (!convertYuv422p10leToYuv420p8Interlaced(srcFrame, st.convertedFrame)) {
            std::cerr << "[EncoderX264] ERROR: field-aware 422p10->420p8 conversion failed.\n";
            return false;
        }

        *outFrame = st.convertedFrame;
        return true;
    }

    SwsContext* sws = nullptr;
    {
        stage_timing::ScopedTimer t(stage_timing::get("x264_prepare_sws"));
        sws = ensureSws(st,
                        srcFrame->width,
                        srcFrame->height,
                        AV_PIX_FMT_YUV422P10LE,
                        st.targetFmt);
    }
    if (!sws) {
        std::cerr << "[EncoderX264] ERROR: sws_getCachedContext failed.\n";
        return false;
    }

    {
        stage_timing::ScopedTimer swsTimer(stage_timing::get("x264_sws_scale"));
        const int ret = sws_scale(
            sws,
            srcFrame->data,
            srcFrame->linesize,
            0,
            srcFrame->height,
            st.convertedFrame->data,
            st.convertedFrame->linesize
        );

        if (ret <= 0) {
            std::cerr << "[EncoderX264] ERROR: sws_scale failed.\n";
            return false;
        }
    }

    st.convertedFrame->pts = srcFrame->pts;
    *outFrame = st.convertedFrame;
    return true;
}

// Submit one frame and immediately drain all currently available packets. This
// keeps latency low and avoids hiding packet backlog inside the encoder wrapper.
bool EncoderX264::sendFrameAndReceivePackets(AVFrame* inFrame, std::vector<AVPacketPtr>& out)
{
    stage_timing::ScopedTimer totalTimer(stage_timing::get("x264_send_receive_total"));

    if (!codec_ctx) {
        return false;
    }

    auto sendOnce = [&]() -> int {
        stage_timing::ScopedTimer t(stage_timing::get("x264_send_frame"));
        return avcodec_send_frame(codec_ctx, inFrame);
    };

    int ret = sendOnce();
    if (ret == AVERROR(EAGAIN)) {
        // FFmpeg requires callers to drain pending output before submitting more input.
        // This retry path makes the encoder robust for modes that buffer/delay packets.
        if (!receiveAvailablePackets(out)) {
            return false;
        }
        ret = sendOnce();
    }

    if (ret < 0) {
        std::cerr << "[EncoderX264] ERROR: avcodec_send_frame failed: " << ret << "\n";
        return false;
    }

    return receiveAvailablePackets(out);
}

bool EncoderX264::receiveAvailablePackets(std::vector<AVPacketPtr>& out)
{
    if (!codec_ctx) {
        return false;
    }

    while (true) {
        AVPacketPtr pkt = acquirePacket();
        if (!pkt) {
            std::cerr << "[EncoderX264] ERROR: Failed to acquire packet from pool.\n";
            return false;
        }

        int ret = 0;
        {
            stage_timing::ScopedTimer t(stage_timing::get("x264_receive_packet"));
            ret = avcodec_receive_packet(codec_ctx, pkt.get());
        }

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }

        if (ret < 0) {
            std::cerr << "[EncoderX264] ERROR: avcodec_receive_packet failed: " << ret << "\n";
            return false;
        }

        if (pkt->duration <= 0) {
            pkt->duration = 1;
        }
        out.push_back(std::move(pkt));
    }
}

EncoderX264::EncoderX264(const json& presetJson)
    : runtime_(new X264RuntimeState())
{
    const json* video = getVideoSection(presetJson);

    width        = getIntFlexible(presetJson, video, "width", 1920);
    height       = getIntFlexible(presetJson, video, "height", 1080);
    bitrate      = getIntFlexible(presetJson, video, "bitrate", 10000000);
    framerate    = getIntFlexible(presetJson, video, "framerate", 50);
    gop_size     = getIntFlexible(presetJson, video, "gop_size", framerate);
    max_b_frames = getIntFlexible(presetJson, video, "max_b_frames", 0);
    crf          = getIntFlexible(presetJson, video, "crf", 18);
    vbv_maxrate  = getIntFlexible(presetJson, video, "vbv-maxrate", bitrate);
    vbv_bufsize  = getIntFlexible(
        presetJson,
        video,
        "vbv_bufsize",
        getIntFlexible(presetJson, video, "vbv-bufsize", bitrate * 2)
    );

    preset  = getStringFlexible(presetJson, video, "preset", "fast");
    tune    = getStringFlexible(presetJson, video, "tune", "");
    profile = getStringFlexible(presetJson, video, "profile", "");

    if (video && video->contains("additional_options") && (*video)["additional_options"].is_object()) {
        additional_options = (*video)["additional_options"];
    } else if (presetJson.contains("additional_options") && presetJson["additional_options"].is_object()) {
        additional_options = presetJson["additional_options"];
    } else {
        additional_options = json::object();
    }

    const std::string rc = getStringFlexible(presetJson, video, "rate_control", "");
    if (!rc.empty()) {
        additional_options["rate_control"] = rc;
    }

    const json* gop = getObjectFlexible(presetJson, video, "gop");
    if (gop) {
        gop_size = getIntFromObject(gop, "size", gop_size);
    }

    const int strictMinKeyint = getIntFromObject(gop, "min_keyint",
        getIntFlexible(presetJson, video, "min_keyint", gop_size));
    const int strictScenecut = getIntFromObject(gop, "scenecut",
        getIntFlexible(presetJson, video, "scenecut", 0));
    const bool closedGop = getBoolFromObject(gop, "closed",
        getBoolFlexible(presetJson, video, "gop_closed", true));

    additional_options["min_keyint"] = strictMinKeyint;
    additional_options["scenecut"] = strictScenecut;
    additional_options["gop_closed"] = closedGop ? 1 : 0;

    const json* color = getObjectFlexible(presetJson, video, "color");
    const std::string colorPrimaries = getStringFromObject(color, "primaries",
        getStringFlexible(presetJson, video, "color_primaries", ""));
    const std::string colorTransfer = getStringFromObject(color, "transfer",
        getStringFlexible(presetJson, video, "color_transfer", ""));
    const std::string colorMatrix = getStringFromObject(color, "matrix",
        getStringFlexible(presetJson, video, "colorspace", ""));
    const std::string colorRange = getStringFromObject(color, "range",
        getStringFlexible(presetJson, video, "color_range", ""));
    const std::string chromaLocation = getStringFromObject(color, "chroma_location",
        getStringFlexible(presetJson, video, "chroma_location", ""));

    if (!colorPrimaries.empty())  additional_options["color_primaries"] = colorPrimaries;
    if (!colorTransfer.empty())   additional_options["color_transfer"] = colorTransfer;
    if (!colorMatrix.empty())     additional_options["colorspace"] = colorMatrix;
    if (!colorRange.empty())      additional_options["color_range"] = colorRange;
    if (!chromaLocation.empty())  additional_options["chroma_location"] = chromaLocation;

    int outputBitDepth = 10;
    std::string outputChroma = "422";

    if (video && video->contains("output") && (*video)["output"].is_object()) {
        const json& out = (*video)["output"];
        outputBitDepth = out.value("bit_depth", 10);
        outputChroma   = out.value("chroma", std::string("422"));
    } else if (presetJson.contains("output") && presetJson["output"].is_object()) {
        const json& out = presetJson["output"];
        outputBitDepth = out.value("bit_depth", 10);
        outputChroma   = out.value("chroma", std::string("422"));
    } else {
        outputBitDepth = getIntFlexible(presetJson, video, "output_bit_depth", 10);
        outputChroma   = getStringFlexible(presetJson, video, "output_chroma", "422");
    }

    X264RuntimeState& st = *runtime_;
    st.internalFmt = AV_PIX_FMT_YUV422P10LE;
    st.targetFmt   = mapOutputFormat(outputBitDepth, outputChroma);
    st.outputInterlaced = getBoolFlexible(presetJson, video, "interlaced", false);
    st.outputTff        = (getStringFlexible(presetJson, video, "field_order", "tff") != "bff");

    if (st.targetFmt == AV_PIX_FMT_NONE) {
        std::cerr << "[EncoderX264] WARN: Unsupported requested output format "
                  << outputBitDepth << "-bit 4:" << outputChroma
                  << ", defaulting to yuv422p10le.\n";
        st.targetFmt = AV_PIX_FMT_YUV422P10LE;
    }

    const std::string expectedProfile = defaultProfileFor(st.targetFmt);

    if (profile.empty()) {
        profile = expectedProfile;
    } else {
        if ((st.targetFmt == AV_PIX_FMT_YUV422P || st.targetFmt == AV_PIX_FMT_YUV422P10LE) &&
            profile != "high422") {
            std::cerr << "[EncoderX264] WARN: Requested profile '" << profile
                      << "' is incompatible with 4:2:2 output. Forcing high422.\n";
            profile = "high422";
        }

        if (st.targetFmt == AV_PIX_FMT_YUV420P10LE && profile != "high10") {
            std::cerr << "[EncoderX264] WARN: Requested profile '" << profile
                      << "' is incompatible with 10-bit 4:2:0 output. Forcing high10.\n";
            profile = "high10";
        }

        if (st.targetFmt == AV_PIX_FMT_YUV420P && profile == "high422") {
            std::cerr << "[EncoderX264] WARN: Requested profile '" << profile
                      << "' is incompatible with 4:2:0 output. Forcing high.\n";
            profile = "high";
        }
    }

    allocateBlackFrame();
    startFpsTracking();

    std::cerr << "[EncoderX264] Internal bus: " << pixFmtNameSafe(st.internalFmt)
              << " | Target: " << pixFmtNameSafe(st.targetFmt)
              << " | Profile: " << profile << "\n";

    if (st.outputInterlaced && st.internalFmt == AV_PIX_FMT_YUV422P10LE && st.targetFmt == AV_PIX_FMT_YUV420P) {
        std::cerr << "[EncoderX264] Conversion path: field-aware yuv422p10le -> yuv420p interlaced\n";
    }
}

EncoderX264::~EncoderX264()
{
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (frame)     av_frame_free(&frame);
    if (zc_frame)  av_frame_free(&zc_frame);
    if (blackFrameYUV) {
        free(blackFrameYUV);
        blackFrameYUV = nullptr;
    }

}

void EncoderX264::allocateBlackFrame()
{
    const size_t y_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t uv_size = y_size / 2;
    const size_t total_bytes = (y_size + uv_size * 2) * sizeof(uint16_t);

    void* ptr = nullptr;
    if (posix_memalign(&ptr, 32, total_bytes) == 0 && ptr) {
        blackFrameYUV = reinterpret_cast<uint8_t*>(ptr);

        uint16_t* y = reinterpret_cast<uint16_t*>(blackFrameYUV);
        uint16_t* u = y + y_size;
        uint16_t* v = u + uv_size;

        std::fill(y, y + y_size,  64);
        std::fill(u, u + uv_size, 512);
        std::fill(v, v + uv_size, 512);
    } else {
        std::cerr << "[EncoderX264] ERROR: Failed to allocate black frame.\n";
        blackFrameYUV = nullptr;
    }
}

uint8_t* EncoderX264::getBlackFrame() const
{
    return blackFrameYUV;
}

void EncoderX264::startFpsTracking()
{
    fps_start_time = std::chrono::high_resolution_clock::now();
}



bool EncoderX264::validateRequestedPixelFormat(const AVCodec* codec) const
{
    if (!codec_ctx) {
        std::cerr << "[EncoderX264] codec_ctx is null during pixel format validation" << std::endl;
        return false;
    }

    const AVPixelFormat requested = codec_ctx->pix_fmt;
    if (requested == AV_PIX_FMT_NONE) {
        std::cerr << "[EncoderX264] Requested pixel format is AV_PIX_FMT_NONE" << std::endl;
        return false;
    }

    const AVPixelFormat* pix_fmts = nullptr;
    int num_pix_fmts = 0;

    const int ret = avcodec_get_supported_config(
        codec_ctx,
        codec,
        AV_CODEC_CONFIG_PIX_FORMAT,
        0,
        reinterpret_cast<const void**>(&pix_fmts),
        &num_pix_fmts
    );

    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        std::cerr << "[EncoderX264] avcodec_get_supported_config(PIX_FORMAT) failed: "
                  << errbuf << std::endl;
        return false;
    }

    if (!pix_fmts || num_pix_fmts <= 0) {
        return true;
    }

    for (int i = 0; i < num_pix_fmts; ++i) {
        if (pix_fmts[i] == AV_PIX_FMT_NONE) {
            break;
        }
        if (pix_fmts[i] == requested) {
            return true;
        }
    }

    std::cerr << "[EncoderX264] Requested pixel format "
              << (av_get_pix_fmt_name(requested) ? av_get_pix_fmt_name(requested) : "unknown")
              << " is not supported by encoder "
              << ((codec && codec->name) ? codec->name : "unknown")
              << ". Supported pixel formats:" << std::endl;

    for (int i = 0; i < num_pix_fmts; ++i) {
        if (pix_fmts[i] == AV_PIX_FMT_NONE) {
            break;
        }
        const char* name = av_get_pix_fmt_name(pix_fmts[i]);
        std::cerr << "  - " << (name ? name : "unknown") << std::endl;
    }

    return false;
}

bool EncoderX264::validateEncoderConfiguration(const std::string& rateControl,
                                                int keyint,
                                                int minKeyint) const
{
    bool ok = true;

    auto fail = [&](const std::string& msg) {
        std::cerr << "[EncoderX264] ERROR: " << msg << "\n";
        ok = false;
    };
    auto warn = [&](const std::string& msg) {
        std::cerr << "[EncoderX264] WARN: " << msg << "\n";
    };

    if (width <= 0 || height <= 0) {
        fail("invalid frame size " + std::to_string(width) + "x" + std::to_string(height));
    }
    if ((width % 2) != 0) {
        fail("width must be even for 4:2:0/4:2:2 encoding");
    }
    if (runtime_ && runtime_->outputInterlaced && (height % 2) != 0) {
        fail("interlaced output requires an even frame height");
    }
    if (framerate <= 0) {
        fail("framerate must be greater than zero");
    }
    if (gop_size <= 0) {
        fail("gop_size must be greater than zero");
    }
    if (keyint <= 0) {
        fail("computed keyint must be greater than zero");
    }
    if (minKeyint <= 0) {
        fail("computed min-keyint must be greater than zero");
    }
    if (minKeyint > keyint) {
        fail("min-keyint cannot be greater than keyint");
    }
    if (max_b_frames < 0) {
        fail("max_b_frames cannot be negative");
    }

    if (rateControl != "cbr" && rateControl != "vbr" && rateControl != "crf") {
        fail("unsupported rate_control '" + rateControl + "' (expected cbr, vbr, or crf)");
    }

    if (rateControl == "cbr") {
        if (bitrate <= 0) {
            fail("CBR requires bitrate > 0");
        }
        if (vbv_maxrate <= 0) {
            fail("CBR requires vbv_maxrate > 0");
        }
        if (vbv_bufsize <= 0) {
            fail("CBR requires vbv_bufsize > 0");
        }
    } else {
        if (crf < 0 || crf > 51) {
            fail("CRF value must be in the range 0..51 for VBR/CRF mode");
        }
    }

    if (bitrate > 0 && vbv_maxrate > 0 && rateControl == "cbr" && vbv_maxrate != bitrate) {
        warn("CBR vbv_maxrate differs from bitrate; this is allowed but may not be strict CBR");
    }
    if (runtime_ && runtime_->outputInterlaced && max_b_frames > 0) {
        warn("interlaced low-latency contribution output normally uses max_b_frames=0");
    }

    return ok;
}

void EncoderX264::logEffectiveEncoderConfiguration(const std::string& rateControl,
                                                   const std::string& x264Params) const
{
    if (!codec_ctx) return;

    const X264RuntimeState* st = runtime_.get();
    std::ostringstream log;
    log << "[EncoderX264] Effective config: "
        << "internal_fmt=" << (st ? pixFmtNameSafe(st->internalFmt) : "unknown")
        << " target_fmt=" << (st ? pixFmtNameSafe(st->targetFmt) : "unknown")
        << " codec_pix_fmt=" << pixFmtNameSafe(codec_ctx->pix_fmt)
        << " profile=" << profileNameSafe(codec_ctx)
        << " requested_profile=" << (profile.empty() ? "auto" : profile)
        << " time_base=" << rationalToString(codec_ctx->time_base)
        << " framerate=" << rationalToString(codec_ctx->framerate)
        << " gop=" << codec_ctx->gop_size
        << " max_b_frames=" << codec_ctx->max_b_frames
        << " bitrate=" << codec_ctx->bit_rate
        << " rc_max_rate=" << codec_ctx->rc_max_rate
        << " rc_buffer_size=" << codec_ctx->rc_buffer_size
        << " field_order=" << fieldOrderName(codec_ctx->field_order)
        << " thread_count=" << codec_ctx->thread_count
        << " rate_control=" << rateControl
        << " x264_params=" << x264Params;

    std::cerr << log.str() << std::endl;
}

bool EncoderX264::initialize()
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        std::cerr << "[EncoderX264] ERROR: libx264 encoder not found.\n";
        return false;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "[EncoderX264] ERROR: avcodec_alloc_context3 failed.\n";
        return false;
    }

    X264RuntimeState& st = *runtime_;

    codec_ctx->width   = width;
    codec_ctx->height  = height;
    codec_ctx->pix_fmt = st.targetFmt;
    codec_ctx->time_base       = AVRational{1, framerate > 0 ? framerate : 50};
    codec_ctx->framerate       = AVRational{framerate > 0 ? framerate : 50, 1};
    codec_ctx->gop_size        = gop_size;
    codec_ctx->max_b_frames    = max_b_frames;
    codec_ctx->field_order     = st.outputInterlaced
        ? (st.outputTff ? AV_FIELD_TT : AV_FIELD_BB)
        : AV_FIELD_PROGRESSIVE;

    if (!validateRequestedPixelFormat(codec)) {
        return false;
    }

    if (additional_options.contains("threads") && additional_options["threads"].is_number_integer()) {
        codec_ctx->thread_count = additional_options["threads"].get<int>();
    } else {
        codec_ctx->thread_count = 0;
    }

    av_opt_set(codec_ctx->priv_data, "preset", preset.c_str(), 0);
    if (!tune.empty())    av_opt_set(codec_ctx->priv_data, "tune", tune.c_str(), 0);
    if (!profile.empty()) av_opt_set(codec_ctx->priv_data, "profile", profile.c_str(), 0);
    av_opt_set(codec_ctx->priv_data, "force-cfr", "1", 0);
    av_opt_set(codec_ctx->priv_data, "forced-idr", "1", 0);

    std::string x264_params = "repeat-headers=1";
    std::string rate_control = "cbr";
    if (additional_options.contains("rate_control") && additional_options["rate_control"].is_string()) {
        rate_control = toLowerCopy(additional_options["rate_control"].get<std::string>());
    }

    const int bitrate_kbps     = (bitrate     > 0) ? (bitrate     / 1000) : 0;
    const int vbv_maxrate_kbps = (vbv_maxrate > 0) ? (vbv_maxrate / 1000) : 0;
    const int vbv_bufsize_kbps = (vbv_bufsize > 0) ? (vbv_bufsize / 1000) : 0;

    const int keyint = (gop_size > 0) ? gop_size : (framerate > 0 ? framerate : 25);
    const int minKeyint = (additional_options.contains("min_keyint") && additional_options["min_keyint"].is_number_integer())
        ? std::max(1, additional_options["min_keyint"].get<int>())
        : keyint;
    const int scenecut = (additional_options.contains("scenecut") && additional_options["scenecut"].is_number_integer())
        ? std::max(0, additional_options["scenecut"].get<int>())
        : 0;
    const bool closedGop = !(additional_options.contains("gop_closed") &&
                             ((additional_options["gop_closed"].is_number_integer() && additional_options["gop_closed"].get<int>() == 0) ||
                              (additional_options["gop_closed"].is_boolean() && !additional_options["gop_closed"].get<bool>())));

    if (!validateEncoderConfiguration(rate_control, keyint, std::min(minKeyint, keyint))) {
        return false;
    }

    appendX264Param(x264_params, "keyint", keyint);
    appendX264Param(x264_params, "min-keyint", std::min(minKeyint, keyint));
    appendX264Param(x264_params, "scenecut", scenecut);
    appendX264Param(x264_params, "open-gop", closedGop ? 0 : 1);

    if (st.outputInterlaced) {
        x264_params += ":interlaced=1";
        x264_params += st.outputTff ? ":tff=1" : ":bff=1";
    }

    AVColorPrimaries colorPrimaries = parseColorPrimaries(
        additional_options.contains("color_primaries") && additional_options["color_primaries"].is_string()
            ? additional_options["color_primaries"].get<std::string>()
            : std::string());
    const AVColorTransferCharacteristic colorTransfer = parseColorTransfer(
        additional_options.contains("color_transfer") && additional_options["color_transfer"].is_string()
            ? additional_options["color_transfer"].get<std::string>()
            : std::string());
    AVColorSpace colorSpace = parseColorSpace(
        additional_options.contains("colorspace") && additional_options["colorspace"].is_string()
            ? additional_options["colorspace"].get<std::string>()
            : std::string());
    AVColorRange colorRange = parseColorRange(
        additional_options.contains("color_range") && additional_options["color_range"].is_string()
            ? additional_options["color_range"].get<std::string>()
            : std::string());
    AVChromaLocation chromaLoc = parseChromaLocation(
        additional_options.contains("chroma_location") && additional_options["chroma_location"].is_string()
            ? additional_options["chroma_location"].get<std::string>()
            : std::string());

    if (isHdrTransfer(colorTransfer) && colorPrimaries == AVCOL_PRI_UNSPECIFIED) {
        colorPrimaries = AVCOL_PRI_BT2020;
        std::cerr << "[EncoderX264] WARN: HDR transfer requested without color primaries; defaulting to BT.2020.\n";
    }
    if (isHdrTransfer(colorTransfer) && colorSpace == AVCOL_SPC_UNSPECIFIED) {
        colorSpace = AVCOL_SPC_BT2020_NCL;
        std::cerr << "[EncoderX264] WARN: HDR transfer requested without color matrix; defaulting to BT.2020 non-constant luminance.\n";
    }
    if (colorRange == AVCOL_RANGE_UNSPECIFIED) {
        colorRange = AVCOL_RANGE_MPEG;
    }
    if (chromaLoc == AVCHROMA_LOC_UNSPECIFIED) {
        chromaLoc = AVCHROMA_LOC_LEFT;
    }
    if (isHdrTransfer(colorTransfer) && pixFmtBitDepth(st.targetFmt) < 10) {
        std::cerr << "[EncoderX264] ERROR: HDR HLG/PQ output requires 10-bit output.\n";
        return false;
    }

    codec_ctx->color_primaries = colorPrimaries;
    codec_ctx->color_trc = colorTransfer;
    codec_ctx->colorspace = colorSpace;
    codec_ctx->color_range = colorRange;
    codec_ctx->chroma_sample_location = chromaLoc;

    appendX264Param(x264_params, "colorprim", x264ColorPrimariesName(colorPrimaries));
    appendX264Param(x264_params, "transfer", x264TransferName(colorTransfer));
    appendX264Param(x264_params, "colormatrix", x264ColorMatrixName(colorSpace));

    if (rate_control == "cbr") {
        codec_ctx->bit_rate       = bitrate;
        codec_ctx->rc_max_rate    = vbv_maxrate;
        codec_ctx->rc_buffer_size = vbv_bufsize;

        if (bitrate_kbps > 0)     x264_params += ":bitrate=" + std::to_string(bitrate_kbps);
        if (vbv_maxrate_kbps > 0) x264_params += ":vbv-maxrate=" + std::to_string(vbv_maxrate_kbps);
        if (vbv_bufsize_kbps > 0) x264_params += ":vbv-bufsize=" + std::to_string(vbv_bufsize_kbps);

        x264_params += ":nal-hrd=cbr";

        int filler = 0;
        if (additional_options.contains("filler") && additional_options["filler"].is_number_integer())
            filler = additional_options["filler"].get<int>();
        if (filler == 1)
            x264_params += ":filler=1";
    } else if (rate_control == "vbr") {
        av_opt_set_int(codec_ctx->priv_data, "crf", crf, 0);
        if (vbv_maxrate_kbps > 0) x264_params += ":vbv-maxrate=" + std::to_string(vbv_maxrate_kbps);
        if (vbv_bufsize_kbps > 0) x264_params += ":vbv-bufsize=" + std::to_string(vbv_bufsize_kbps);
        if (additional_options.contains("nal-hrd") && additional_options["nal-hrd"].is_string()) {
            x264_params += ":nal-hrd=" + additional_options["nal-hrd"].get<std::string>();
        }
    } else {
        av_opt_set_int(codec_ctx->priv_data, "crf", crf, 0);
    }

    for (auto it = additional_options.begin(); it != additional_options.end(); ++it) {
        const std::string& key = it.key();
        const json& value = it.value();

        if (isEncoderManagedOption(key)) {
            continue;
        }

        if (appendX264NativeOption(x264_params, key, value)) {
            continue;
        }

        const int optRet = setPrivateOptionChecked(codec_ctx->priv_data, key, value);
        if (optRet < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, sizeof(errbuf), optRet);
            std::cerr << "[EncoderX264] WARN: libx264 option '" << key
                      << "' was not accepted by FFmpeg private options (" << errbuf
                      << "). If this is an x264-native parameter, add it to appendX264NativeOption().\n";
        }
    }

    av_opt_set(codec_ctx->priv_data, "x264-params", x264_params.c_str(), 0);

    {
        std::ostringstream open_log;
        open_log << "[EncoderX264] Opening libx264 with pix_fmt="
                 << pixFmtNameSafe(codec_ctx->pix_fmt)
                 << ", profile=" << profile
                 << ", x264-params=" << x264_params;
        std::cerr << open_log.str() << std::endl;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "[EncoderX264] ERROR: Failed to open x264 encoder.\n";
        std::cerr << "[EncoderX264] Requested output format was " << pixFmtNameSafe(st.targetFmt)
                  << ". This usually means your FFmpeg/libx264 build does not support that combination.\n";
        return false;
    }

    logEffectiveEncoderConfiguration(rate_control, x264_params);

    frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "[EncoderX264] ERROR: av_frame_alloc failed.\n";
        return false;
    }

    frame->format = codec_ctx->pix_fmt;
    frame->width  = width;
    frame->height = height;

    if (av_frame_get_buffer(frame, 32) < 0) {
        std::cerr << "[EncoderX264] ERROR: Failed to allocate copy-path frame.\n";
        return false;
    }

    zc_frame = av_frame_alloc();
    if (!zc_frame) {
        std::cerr << "[EncoderX264] ERROR: Failed to allocate zero-copy wrapper frame.\n";
        return false;
    }

    return true;
}


void EncoderX264::applyVideoFrameMetadata(AVFrame* dst, const VideoFrame& src) const
{
    if (!dst) return;

    if (codec_ctx) {
        dst->color_range = codec_ctx->color_range;
        dst->color_primaries = codec_ctx->color_primaries;
        dst->color_trc = codec_ctx->color_trc;
        dst->colorspace = codec_ctx->colorspace;
        dst->chroma_location = codec_ctx->chroma_sample_location;
    }

    if (src.color_range != AVCOL_RANGE_UNSPECIFIED)
        dst->color_range = src.color_range;
    if (src.color_primaries != AVCOL_PRI_UNSPECIFIED)
        dst->color_primaries = src.color_primaries;
    if (src.color_trc != AVCOL_TRC_UNSPECIFIED)
        dst->color_trc = src.color_trc;
    if (src.colorspace != AVCOL_SPC_UNSPECIFIED)
        dst->colorspace = src.colorspace;
    if (src.chroma_location != AVCHROMA_LOC_UNSPECIFIED)
        dst->chroma_location = src.chroma_location;

    attachHdrSideData(dst, src);
}

bool EncoderX264::attachHdrSideData(AVFrame* dst, const VideoFrame& src) const
{
    if (!dst) return false;

    av_frame_remove_side_data(dst, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    av_frame_remove_side_data(dst, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);

    if (src.has_mastering_display) {
        AVFrameSideData* sd = av_frame_new_side_data(dst,
                                                     AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                                     sizeof(AVMasteringDisplayMetadata));
        if (!sd) {
            std::cerr << "[EncoderX264] WARN: failed to attach mastering display metadata.\n";
            return false;
        }
        std::memcpy(sd->data, &src.mastering_display, sizeof(AVMasteringDisplayMetadata));
    }

    if (src.has_content_light) {
        AVFrameSideData* sd = av_frame_new_side_data(dst,
                                                     AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
                                                     sizeof(AVContentLightMetadata));
        if (!sd) {
            std::cerr << "[EncoderX264] WARN: failed to attach content light metadata.\n";
            return false;
        }
        std::memcpy(sd->data, &src.content_light, sizeof(AVContentLightMetadata));
    }

    return true;
}

// Legacy shared-buffer submit path. It assumes the buffer is contiguous
// YUV422P10LE and attaches the shared owner to the AVFrame before encoding.
std::vector<AVPacketPtr> EncoderX264::encodeFrameZeroCopyPackets(const std::shared_ptr<uint8_t>& inputBuf,
                                                                     size_t inputBytes,
                                                                     int64_t pts)
{
    stage_timing::ScopedTimer timer(stage_timing::get("x264_encode_zero_copy_total"));
    const auto encodeStart = std::chrono::steady_clock::now();
    fps_frame_count++;

    if (!inputBuf || inputBytes == 0 || !codec_ctx || !zc_frame)
        return {};

    const size_t y_bytes  = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
    const size_t uv_bytes = static_cast<size_t>(width / 2) * static_cast<size_t>(height) * 2;
    const size_t need     = y_bytes + uv_bytes + uv_bytes;

    if (inputBytes < need) {
        std::cerr << "[EncoderX264] ERROR: input buffer too small: "
                  << inputBytes << " < " << need << "\n";
        return {};
    }

    av_frame_unref(zc_frame);

    const bool forceKeyframe = (frame_counter == 0) ||
                               force_next_keyframe_.exchange(false, std::memory_order_acq_rel);

    zc_frame->pts = pts;
    applyFrameCodingMetadata(zc_frame, *runtime_, forceKeyframe);
    {
        stage_timing::ScopedTimer t(stage_timing::get("x264_map_input_zc"));
        if (!fillFramePointersForContiguousInternalBus(zc_frame, inputBuf.get(), width, height)) {
            std::cerr << "[EncoderX264] ERROR: Failed to map zero-copy input frame.\n";
            return {};
        }
    }

    AVBufferRef* bref = nullptr;
    {
        auto* holder = new SharedBufHolder{inputBuf};
        stage_timing::ScopedTimer t(stage_timing::get("x264_attach_input_buffer"));
        bref = av_buffer_create(
            inputBuf.get(),
            static_cast<int>(need),
            avbuffer_release_sharedptr,
            holder,
            0
        );
        if (!bref) {
            delete holder;
        }
    }

    if (!bref) {
        std::cerr << "[EncoderX264] ERROR: av_buffer_create failed.\n";
        return {};
    }

    zc_frame->buf[0] = bref;

    AVFrame* encFrame = nullptr;
    if (!prepareConvertedFrame(zc_frame, &encFrame))
        return {};

    applyFrameCodingMetadata(encFrame, *runtime_, forceKeyframe);

    std::vector<AVPacketPtr> out;
    const bool ok = sendFrameAndReceivePackets(encFrame, out);
    const auto encodeEnd = std::chrono::steady_clock::now();
    reportX264EncodeTiming(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(encodeEnd - encodeStart).count()),
        !out.empty());
    if (!ok)
        return {};

    frame_counter++;
    return out;
}


// Main sender video submit path. It maps the VideoFrame planes directly and
// retains vf.buffer through AVBufferRef so the input memory remains valid.
std::vector<AVPacketPtr> EncoderX264::encodeVideoFramePackets(const VideoFrame& vf)
{
    stage_timing::ScopedTimer timer(stage_timing::get("x264_encode_zero_copy_total"));
    const auto encodeStart = std::chrono::steady_clock::now();
    fps_frame_count++;

    if (!vf.buffer || vf.buffer_size == 0 || !codec_ctx || !zc_frame)
        return {};

    const size_t y_bytes  = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
    const size_t uv_bytes = static_cast<size_t>(width / 2) * static_cast<size_t>(height) * 2;
    const size_t need     = y_bytes + uv_bytes + uv_bytes;

    if (vf.buffer_size < need) {
        std::cerr << "[EncoderX264] ERROR: input buffer too small: "
                  << vf.buffer_size << " < " << need << "\n";
        return {};
    }

    av_frame_unref(zc_frame);

    const bool forceKeyframe = (frame_counter == 0) ||
                               force_next_keyframe_.exchange(false, std::memory_order_acq_rel);

    zc_frame->pts = vf.pts;
    applyFrameCodingMetadata(zc_frame, *runtime_, forceKeyframe);
    applyVideoFrameMetadata(zc_frame, vf);
    {
        stage_timing::ScopedTimer t(stage_timing::get("x264_map_input_zc"));
        if (!fillFramePointersForContiguousInternalBus(zc_frame, vf.buffer.get(), width, height)) {
            std::cerr << "[EncoderX264] ERROR: Failed to map zero-copy input frame.\n";
            return {};
        }
    }

    AVBufferRef* bref = nullptr;
    {
        auto* holder = new SharedBufHolder{vf.buffer};
        stage_timing::ScopedTimer t(stage_timing::get("x264_attach_input_buffer"));
        bref = av_buffer_create(
            vf.buffer.get(),
            static_cast<int>(need),
            avbuffer_release_sharedptr,
            holder,
            0
        );
        if (!bref) {
            delete holder;
        }
    }

    if (!bref) {
        std::cerr << "[EncoderX264] ERROR: av_buffer_create failed.\n";
        return {};
    }

    zc_frame->buf[0] = bref;

    AVFrame* encFrame = nullptr;
    if (!prepareConvertedFrame(zc_frame, &encFrame))
        return {};

    applyFrameCodingMetadata(encFrame, *runtime_, forceKeyframe);
    applyVideoFrameMetadata(encFrame, vf);

    std::vector<AVPacketPtr> out;
    const bool ok = sendFrameAndReceivePackets(encFrame, out);
    const auto encodeEnd = std::chrono::steady_clock::now();
    reportX264EncodeTiming(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(encodeEnd - encodeStart).count()),
        !out.empty());
    if (!ok)
        return {};

    frame_counter++;
    return out;
}

AVPacketPtr EncoderX264::encodeFrameZeroCopy(const std::shared_ptr<uint8_t>& inputBuf,
                                             size_t inputBytes,
                                             int64_t pts)
{
    std::vector<AVPacketPtr> packets = encodeFrameZeroCopyPackets(inputBuf, inputBytes, pts);
    if (packets.empty()) {
        return {};
    }
    if (packets.size() > 1) {
        std::cerr << "[EncoderX264] WARN: encodeFrameZeroCopy compatibility API produced "
                  << packets.size() << " packets; returning first packet only. Use encodeFrameZeroCopyPackets().\n";
    }
    return std::move(packets.front());
}

std::vector<AVPacketPtr> EncoderX264::encodeFramePackets(uint8_t* inputFrame, int64_t pts)
{
    stage_timing::ScopedTimer timer(stage_timing::get("x264_encode_copy_total"));
    fps_frame_count++;

    if (!inputFrame || !codec_ctx || !frame || !zc_frame)
        return {};

    av_frame_unref(zc_frame);
    const bool forceKeyframe = (frame_counter == 0) ||
                               force_next_keyframe_.exchange(false, std::memory_order_acq_rel);
    zc_frame->pts = pts;
    applyFrameCodingMetadata(zc_frame, *runtime_, forceKeyframe);

    if (!fillFramePointersForContiguousInternalBus(zc_frame, inputFrame, width, height)) {
        std::cerr << "[EncoderX264] ERROR: Failed to map copy-path input frame.\n";
        return {};
    }

    AVFrame* encFrame = nullptr;
    if (!prepareConvertedFrame(zc_frame, &encFrame))
        return {};

    if (encFrame == zc_frame) {
        if (av_frame_make_writable(frame) < 0) {
            std::cerr << "[EncoderX264] ERROR: frame not writable.\n";
            return {};
        }

        {
            stage_timing::ScopedTimer memcpyTimer(stage_timing::get("x264_copy_path_memcpy"));
            for (int r = 0; r < height; ++r) {
                std::memcpy(frame->data[0] + static_cast<size_t>(r) * frame->linesize[0],
                            zc_frame->data[0] + static_cast<size_t>(r) * zc_frame->linesize[0],
                            static_cast<size_t>(width) * 2);
            }

            for (int r = 0; r < height; ++r) {
                std::memcpy(frame->data[1] + static_cast<size_t>(r) * frame->linesize[1],
                            zc_frame->data[1] + static_cast<size_t>(r) * zc_frame->linesize[1],
                            static_cast<size_t>(width / 2) * 2);
                std::memcpy(frame->data[2] + static_cast<size_t>(r) * frame->linesize[2],
                            zc_frame->data[2] + static_cast<size_t>(r) * zc_frame->linesize[2],
                            static_cast<size_t>(width / 2) * 2);
            }
        }

        frame->pts = pts;
        applyFrameCodingMetadata(frame, *runtime_, forceKeyframe);
        encFrame = frame;
    } else {
        encFrame->pts = pts;
        applyFrameCodingMetadata(encFrame, *runtime_, forceKeyframe);
    }

    std::vector<AVPacketPtr> out;
    const bool ok = sendFrameAndReceivePackets(encFrame, out);
    if (!ok)
        return {};

    frame_counter++;
    return out;
}

AVPacketPtr EncoderX264::encodeFrame(uint8_t* inputFrame, int64_t pts)
{
    std::vector<AVPacketPtr> packets = encodeFramePackets(inputFrame, pts);
    if (packets.empty()) {
        return {};
    }
    if (packets.size() > 1) {
        std::cerr << "[EncoderX264] WARN: encodeFrame compatibility API produced "
                  << packets.size() << " packets; returning first packet only. Use encodeFramePackets().\n";
    }
    return std::move(packets.front());
}

std::vector<AVPacketPtr> EncoderX264::flush()
{
    std::vector<AVPacketPtr> out;

    if (!codec_ctx) {
        return out;
    }

    // Flush encoder.
    int ret = avcodec_send_frame(codec_ctx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        std::cerr << "[EncoderX264] WARN: flush avcodec_send_frame(nullptr) failed: "
                  << ret << "\n";
    }

    if (!receiveAvailablePackets(out)) {
        std::cerr << "[EncoderX264] ERROR: flush receive/drain failed.\n";
    }

    return out;
}

void EncoderX264::requestKeyFrame()
{
    force_next_keyframe_.store(true, std::memory_order_release);
}

AVPacketPtr EncoderX264::acquirePacket()
{
    return packetPool.acquire();
}

AVCodecContext* EncoderX264::getCodecContext() const
{
    return codec_ctx;
}