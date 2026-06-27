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
 * HEVC encoder implementation. This module parses x265 presets, manages encoder-owned working frames, applies metadata, handles optional conversion, and drains packets for the muxer.
 */

#include "encoder_x265.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

using json = nlohmann::json;

namespace {

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

static int getIntFromObjectAny(const json* obj,
                               const std::vector<std::string>& keys,
                               int def)
{
    if (!obj) return def;
    for (const auto& key : keys) {
        if (!obj->contains(key)) continue;
        const json& v = (*obj)[key];
        if (v.is_number_integer()) return v.get<int>();
        if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
        if (v.is_string()) {
            try {
                return std::stoi(v.get<std::string>());
            } catch (...) {
            }
        }
    }
    return def;
}

static std::string getStringFromObjectAny(const json* obj,
                                          const std::vector<std::string>& keys,
                                          const std::string& def)
{
    if (!obj) return def;
    for (const auto& key : keys) {
        if (!obj->contains(key)) continue;
        const json& v = (*obj)[key];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_integer()) return std::to_string(v.get<int>());
        if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    }
    return def;
}

static bool objectHasAny(const json* obj, const std::vector<std::string>& keys)
{
    if (!obj) return false;
    for (const auto& key : keys) {
        if (obj->contains(key)) return true;
    }
    return false;
}

static std::string getStringFlexibleAny(const json& root,
                                        const json* video,
                                        const std::vector<std::string>& keys,
                                        const std::string& def)
{
    for (const auto& key : keys) {
        if (video && video->contains(key) && (*video)[key].is_string())
            return (*video)[key].get<std::string>();
        if (root.contains(key) && root[key].is_string())
            return root[key].get<std::string>();
    }
    return def;
}

static std::string toLowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static void appendX265Param(std::string& params, const std::string& key, const std::string& value)
{
    if (key.empty() || value.empty()) return;
    if (!params.empty()) params += ":";
    params += key + "=" + value;
}

static void appendX265Param(std::string& params, const std::string& key, int value)
{
    appendX265Param(params, key, std::to_string(value));
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

static std::string x265ColorPrimariesName(AVColorPrimaries v)
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

static std::string x265TransferName(AVColorTransferCharacteristic v)
{
    switch (v) {
        case AVCOL_TRC_BT709: return "bt709";
        case AVCOL_TRC_SMPTE170M: return "smpte170m";
        case AVCOL_TRC_SMPTE240M: return "smpte240m";
        case AVCOL_TRC_LINEAR: return "linear";
        case AVCOL_TRC_IEC61966_2_1: return "iec61966-2-1";
        case AVCOL_TRC_BT2020_10: return "bt2020-10";
        case AVCOL_TRC_BT2020_12: return "bt2020-12";
        case AVCOL_TRC_SMPTE2084: return "smpte2084";
        case AVCOL_TRC_ARIB_STD_B67: return "arib-std-b67";
        default: return "";
    }
}

static std::string x265ColorMatrixName(AVColorSpace v)
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

static bool isHdrTransfer(AVColorTransferCharacteristic trc)
{
    return trc == AVCOL_TRC_ARIB_STD_B67 || trc == AVCOL_TRC_SMPTE2084;
}

static const char* colorPrimariesLabel(AVColorPrimaries v)
{
    switch (v) {
        case AVCOL_PRI_BT709: return "BT.709";
        case AVCOL_PRI_BT2020: return "BT.2020";
        case AVCOL_PRI_UNSPECIFIED: return "unspecified";
        default: return "other";
    }
}

static const char* transferLabel(AVColorTransferCharacteristic v)
{
    switch (v) {
        case AVCOL_TRC_BT709: return "BT.709 SDR";
        case AVCOL_TRC_ARIB_STD_B67: return "HLG";
        case AVCOL_TRC_SMPTE2084: return "PQ/ST2084";
        case AVCOL_TRC_UNSPECIFIED: return "unspecified";
        default: return "other";
    }
}

static const char* colorMatrixLabel(AVColorSpace v)
{
    switch (v) {
        case AVCOL_SPC_BT709: return "BT.709";
        case AVCOL_SPC_BT2020_NCL: return "BT.2020 non-constant luminance";
        case AVCOL_SPC_BT2020_CL: return "BT.2020 constant luminance";
        case AVCOL_SPC_UNSPECIFIED: return "unspecified";
        default: return "other";
    }
}

static std::string normalizeProfileName(std::string profile)
{
    profile = toLowerCopy(profile);
    std::replace(profile.begin(), profile.end(), '_', '-');
    if (profile == "main-10" || profile == "main 10") return "main10";
    if (profile == "main-422-10" || profile == "main42210" || profile == "main-4:2:2-10" ||
        profile == "main422-10-intra" || profile == "main422-10") return "main422-10";
    return profile;
}

static AVPixelFormat mapOutputFormat(int bitDepth, const std::string& chroma)
{
    if (bitDepth == 8 && chroma == "420")  return AV_PIX_FMT_YUV420P;
    if (bitDepth == 8 && chroma == "422")  return AV_PIX_FMT_YUV422P;
    if (bitDepth == 8 && chroma == "444")  return AV_PIX_FMT_YUV444P;
    if (bitDepth == 10 && chroma == "420") return AV_PIX_FMT_YUV420P10LE;
    if (bitDepth == 10 && chroma == "422") return AV_PIX_FMT_YUV422P10LE;
    if (bitDepth == 10 && chroma == "444") return AV_PIX_FMT_YUV444P10LE;
    return AV_PIX_FMT_NONE;
}

static AVPixelFormat parsePixelFormatName(const std::string& value)
{
    if (value.empty()) return AV_PIX_FMT_NONE;
    return av_get_pix_fmt(toLowerCopy(value).c_str());
}

static int bitDepthFromPixFmt(AVPixelFormat fmt, int fallback)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || desc->nb_components <= 0) return fallback;
    return desc->comp[0].depth;
}

static std::string chromaFromPixFmt(AVPixelFormat fmt, const std::string& fallback)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc) return fallback;
    if (desc->flags & AV_PIX_FMT_FLAG_RGB) return fallback;
    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) return "420";
    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) return "422";
    if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) return "444";
    return fallback;
}

static void mergeObjectInto(json& dst, const json* src)
{
    if (!src || !src->is_object()) return;
    for (auto it = src->begin(); it != src->end(); ++it) {
        dst[it.key()] = it.value();
    }
}

static std::string defaultProfileFor(AVPixelFormat fmt)
{
    switch (fmt) {
        case AV_PIX_FMT_YUV420P:     return "main";
        case AV_PIX_FMT_YUV420P10LE: return "main10";
        case AV_PIX_FMT_YUV422P10LE: return "main422-10";
        default:                     return "";
    }
}

static const char* pixFmtNameSafe(AVPixelFormat fmt)
{
    const char* n = av_get_pix_fmt_name(fmt);
    return n ? n : "unknown";
}
static bool pixFmtIsAtLeast10Bit(AVPixelFormat fmt)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    return desc && desc->nb_components > 0 && desc->comp[0].depth >= 10;
}

static bool pixFmtIs422(AVPixelFormat fmt)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    return desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB) &&
           desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0;
}

static bool profileMatchesPixFmt(const std::string& profile, AVPixelFormat fmt, std::string& reason)
{
    const std::string p = normalizeProfileName(profile);
    if (p.empty()) return true;

    if (p == "main") {
        if (fmt == AV_PIX_FMT_YUV420P) return true;
        reason = "profile main requires 8-bit 4:2:0 output (yuv420p)";
        return false;
    }

    if (p == "main10" || p == "main-10") {
        if (fmt == AV_PIX_FMT_YUV420P10LE) return true;
        reason = "profile main10 requires 10-bit 4:2:0 output (yuv420p10le)";
        return false;
    }

    if (p == "main422-10" || p == "main42210" || p == "main-422-10") {
        if (fmt == AV_PIX_FMT_YUV422P10LE) return true;
        reason = "profile main422-10 requires 10-bit 4:2:2 output (yuv422p10le)";
        return false;
    }

    // Unknown/custom profiles are passed through to FFmpeg/libx265.
    return true;
}

static std::string canonicalizeX265Params(const std::string& params)
{
    std::vector<std::pair<std::string, std::string>> ordered;
    size_t start = 0;
    while (start <= params.size()) {
        const size_t end = params.find(':', start);
        const std::string token = params.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!token.empty()) {
            const size_t eq = token.find('=');
            if (eq != std::string::npos && eq > 0) {
                const std::string key = token.substr(0, eq);
                const std::string value = token.substr(eq + 1);
                bool updated = false;
                for (auto& kv : ordered) {
                    if (kv.first == key) {
                        kv.second = value;  // Last value wins, position remains stable.
                        updated = true;
                        break;
                    }
                }
                if (!updated) ordered.emplace_back(key, value);
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }

    std::string out;
    for (const auto& kv : ordered) {
        if (kv.first.empty() || kv.second.empty()) continue;
        if (!out.empty()) out += ':';
        out += kv.first;
        out += '=';
        out += kv.second;
    }
    return out;
}


} // namespace

EncoderX265::EncoderX265(const json& presetJson)
{
    parsePreset(presetJson);
    allocateBlackFrame();
    fps_start_time_ = std::chrono::high_resolution_clock::now();
}

EncoderX265::~EncoderX265()
{
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (copy_input_frame_) av_frame_free(&copy_input_frame_);
    if (zc_input_frame_) av_frame_free(&zc_input_frame_);
    if (converted_frame_) av_frame_free(&converted_frame_);
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (blackFrameYUV_) {
        free(blackFrameYUV_);
        blackFrameYUV_ = nullptr;
    }
}

bool EncoderX265::parsePreset(const json& presetJson)
{
    const json* video = getVideoSection(presetJson);

    width_       = getIntFlexible(presetJson, video, "width", 0);
    height_      = getIntFlexible(presetJson, video, "height", 0);
    framerate_   = getIntFlexible(presetJson, video, "framerate", 25);
    bitrate_     = getIntFlexible(presetJson, video, "bitrate", 12000000);
    vbv_maxrate_ = getIntFlexible(presetJson, video, "vbv-maxrate",
                    getIntFlexible(presetJson, video, "vbv_maxrate",
                    getIntFlexible(presetJson, video, "max_bitrate", bitrate_)));
    vbv_bufsize_ = getIntFlexible(presetJson, video, "vbv-bufsize",
                    getIntFlexible(presetJson, video, "vbv_bufsize",
                    getIntFlexible(presetJson, video, "bufsize", vbv_maxrate_)));
    max_b_frames_= getIntFlexible(presetJson, video, "max_b_frames",
                    getIntFlexible(presetJson, video, "bframes", 0));
    crf_         = getIntFlexible(presetJson, video, "crf", -1);

    const json* gop = getObjectFlexible(presetJson, video, "gop");
    if (gop) {
        gop_size_   = getIntFromObject(gop, "size", framerate_ * 2);
        keyint_min_ = getIntFromObject(gop, "min_keyint", gop_size_);
        closed_gop_ = getBoolFromObject(gop, "closed", true);
    } else {
        gop_size_   = getIntFlexible(presetJson, video, "gop_size", framerate_ * 2);
        keyint_min_ = getIntFlexible(presetJson, video, "min_keyint", gop_size_);
        closed_gop_ = getBoolFlexible(presetJson, video, "closed_gop", true);
    }

    preset_  = getStringFlexible(presetJson, video, "preset", "medium");
    tune_    = getStringFlexible(presetJson, video, "tune", "");
    profile_ = getStringFlexible(presetJson, video, "profile", "");

    interlaced_ = getBoolFlexible(presetJson, video, "interlaced", false);
    top_field_first_ = (toLowerCopy(getStringFlexible(presetJson, video, "field_order", "tff")) != "bff");

    const json* output = getObjectFlexible(presetJson, video, "output");
    output_bit_depth_ = getIntFromObject(output, "bit_depth", 10);
    output_chroma_ = toLowerCopy(getStringFromObject(output, "chroma", "422"));
    output_fmt_ = AV_PIX_FMT_NONE;

    const std::string explicitPixFmt = getStringFlexibleAny(presetJson, video,
                                                            {"pix_fmt", "pixel_format", "format"},
                                                            getStringFromObject(output, "pix_fmt", ""));
    if (!explicitPixFmt.empty()) {
        output_fmt_ = parsePixelFormatName(explicitPixFmt);
        if (output_fmt_ == AV_PIX_FMT_NONE) {
            std::cerr << "[EncoderX265] WARN: Unknown pix_fmt '" << explicitPixFmt
                      << "'. Falling back to output bit_depth/chroma.\n";
        } else {
            output_bit_depth_ = bitDepthFromPixFmt(output_fmt_, output_bit_depth_);
            output_chroma_ = chromaFromPixFmt(output_fmt_, output_chroma_);
        }
    }

    if (output_fmt_ == AV_PIX_FMT_NONE) {
        output_fmt_ = mapOutputFormat(output_bit_depth_, output_chroma_);
    }

    if (output_fmt_ == AV_PIX_FMT_NONE) {
        std::cerr << "[EncoderX265] WARN: Requested output format "
                  << output_chroma_ << "/" << output_bit_depth_
                  << "-bit is not supported by this encoder wrapper. Falling back to 10-bit 4:2:2.\n";
        output_bit_depth_ = 10;
        output_chroma_ = "422";
        output_fmt_ = AV_PIX_FMT_YUV422P10LE;
    }

    const json* color = getObjectFlexible(presetJson, video, "color");
    const std::string colorPrimariesText = getStringFromObject(color, "primaries",
        getStringFlexibleAny(presetJson, video, {"color_primaries", "color-primaries"}, ""));
    const std::string colorTransferText = getStringFromObject(color, "transfer",
        getStringFlexibleAny(presetJson, video, {"color_transfer", "color-transfer"}, ""));
    const std::string colorMatrixText = getStringFromObject(color, "matrix",
        getStringFlexibleAny(presetJson, video, {"colorspace", "color_matrix", "color-matrix"}, ""));
    const std::string colorRangeText = getStringFromObject(color, "range",
        getStringFlexibleAny(presetJson, video, {"color_range", "color-range"}, ""));
    const std::string chromaLocationText = getStringFromObject(color, "chroma_location",
        getStringFlexibleAny(presetJson, video, {"chroma_location", "chroma-location"}, ""));

    color_primaries_  = parseColorPrimaries(colorPrimariesText);
    color_trc_        = parseColorTransfer(colorTransferText);
    colorspace_       = parseColorSpace(colorMatrixText);
    color_range_      = parseColorRange(colorRangeText);
    chroma_location_  = parseChromaLocation(chromaLocationText);

    if (isHdrTransfer(color_trc_) && color_primaries_ == AVCOL_PRI_UNSPECIFIED) {
        color_primaries_ = AVCOL_PRI_BT2020;
        std::cerr << "[EncoderX265] WARN: HDR transfer requested without color primaries; defaulting to BT.2020.\n";
    }
    if (isHdrTransfer(color_trc_) && colorspace_ == AVCOL_SPC_UNSPECIFIED) {
        colorspace_ = AVCOL_SPC_BT2020_NCL;
        std::cerr << "[EncoderX265] WARN: HDR transfer requested without color matrix; defaulting to BT.2020 non-constant luminance.\n";
    }
    if (color_range_ == AVCOL_RANGE_UNSPECIFIED) {
        color_range_ = AVCOL_RANGE_MPEG;
    }
    if (chroma_location_ == AVCHROMA_LOC_UNSPECIFIED) {
        chroma_location_ = AVCHROMA_LOC_LEFT;
    }

    const json* hdr10 = getObjectFlexible(presetJson, video, "hdr10");
    x265_master_display_ = getStringFromObjectAny(hdr10,
                                                   {"master_display", "master-display"},
                                                   std::string());
    x265_max_cll_ = getStringFromObjectAny(hdr10,
                                            {"max_cll", "max-cll", "max_content_light"},
                                            std::string());
    if (!x265_max_cll_.empty()) {
        int maxCll = 0;
        int maxFall = 0;
        if (std::sscanf(x265_max_cll_.c_str(), "%d,%d", &maxCll, &maxFall) == 2 &&
            maxCll > 0 && maxFall > 0) {
            content_light_.MaxCLL = static_cast<unsigned>(maxCll);
            content_light_.MaxFALL = static_cast<unsigned>(maxFall);
            has_content_light_ = true;
        }
    }

    additional_options_ = json::object();
    mergeObjectInto(additional_options_, getObjectFlexible(presetJson, video, "x265_params"));
    mergeObjectInto(additional_options_, getObjectFlexible(presetJson, video, "additional_options"));

    rate_control_ = toLowerCopy(getStringFlexibleAny(presetJson, video,
                                                     {"rate_control", "rate-control"},
                                                     getStringFromObjectAny(&additional_options_,
                                                                            {"rate_control", "rate-control"},
                                                                            "cbr")));

    thread_count_ = getIntFromObjectAny(&additional_options_, {"threads"}, 0);
    rc_lookahead_ = getIntFromObjectAny(&additional_options_, {"rc_lookahead", "rc-lookahead"}, -1);
    slices_       = getIntFromObjectAny(&additional_options_, {"slices"}, -1);
    refs_         = getIntFromObjectAny(&additional_options_, {"ref", "refs"},
                    getIntFlexible(presetJson, video, "refs", -1));

    const std::string level = getStringFromObjectAny(&additional_options_,
                                                     {"level", "level-idc", "level_idc"},
                                                     std::string());
    if (!level.empty()) {
        std::string digits;
        for (char c : level) {
            if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
        }
        if (!digits.empty()) {
            level_idc_ = std::atoi(digits.c_str());
        }
    }

    profile_ = normalizeProfileName(profile_);
    if (profile_.empty()) {
        profile_ = defaultProfileFor(output_fmt_);
    }

    if (output_fmt_ == AV_PIX_FMT_YUV422P10LE && profile_ != "main422-10") {
        std::cerr << "[EncoderX265] WARN: forcing profile main422-10 for 10-bit 4:2:2 output.\n";
        profile_ = "main422-10";
    } else if (output_fmt_ == AV_PIX_FMT_YUV420P10LE && profile_ != "main10") {
        std::cerr << "[EncoderX265] WARN: forcing profile main10 for 10-bit 4:2:0 output.\n";
        profile_ = "main10";
    } else if (output_fmt_ == AV_PIX_FMT_YUV420P && profile_ != "main") {
        std::cerr << "[EncoderX265] WARN: forcing profile main for 8-bit 4:2:0 output.\n";
        profile_ = "main";
    }

    std::string profileReason;
    if (!profileMatchesPixFmt(profile_, output_fmt_, profileReason)) {
        std::cerr << "[EncoderX265] ERROR: invalid x265 profile/output combination: "
                  << profileReason << ". Requested profile=" << profile_
                  << " output=" << pixFmtNameSafe(output_fmt_) << "\n";
        return false;
    }

    if (isHdrTransfer(color_trc_) && output_bit_depth_ < 10) {
        std::cerr << "[EncoderX265] ERROR: HDR HLG/PQ output requires 10-bit output. "
                  << "Requested " << output_bit_depth_ << "-bit " << output_chroma_ << ".\n";
        return false;
    }
    if (isHdrTransfer(color_trc_) &&
        color_primaries_ != AVCOL_PRI_BT2020 && color_primaries_ != AVCOL_PRI_BT709) {
        std::cerr << "[EncoderX265] ERROR: HDR transfer requires BT.2020 WCG or BT.709 primaries.\n";
        return false;
    }
    if (color_trc_ == AVCOL_TRC_SMPTE2084 &&
        (x265_master_display_.empty() || x265_max_cll_.empty())) {
        std::cerr << "[EncoderX265] WARN: PQ/ST2084 preset has no complete hdr10.master_display/max_cll metadata. "
                  << "Stream will still signal PQ, but HDR10 mastering metadata will be incomplete.\n";
    }

    std::cerr << "[EncoderX265] Internal bus: " << pixFmtNameSafe(input_fmt_)
              << " | Target: " << pixFmtNameSafe(output_fmt_)
              << " | Profile: " << profile_
              << " | Primaries: " << colorPrimariesLabel(color_primaries_)
              << " | Transfer: " << transferLabel(color_trc_)
              << " | Matrix: " << colorMatrixLabel(colorspace_) << "\n";

    return true;
}

void EncoderX265::allocateBlackFrame()
{
    const size_t y_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    const size_t uv_size = y_size / 2;
    const size_t total_bytes = (y_size + uv_size * 2) * sizeof(uint16_t);

    void* ptr = nullptr;
    if (posix_memalign(&ptr, 32, total_bytes) == 0 && ptr) {
        blackFrameYUV_ = reinterpret_cast<uint8_t*>(ptr);

        uint16_t* y = reinterpret_cast<uint16_t*>(blackFrameYUV_);
        uint16_t* u = y + y_size;
        uint16_t* v = u + uv_size;

        std::fill(y, y + y_size, 64);
        std::fill(u, u + uv_size, 512);
        std::fill(v, v + uv_size, 512);
    } else {
        std::cerr << "[EncoderX265] ERROR: Failed to allocate black frame.\n";
        blackFrameYUV_ = nullptr;
    }
}

uint8_t* EncoderX265::getBlackFrame() const
{
    return blackFrameYUV_;
}

AVPacketPtr EncoderX265::acquirePacket()
{
    return packetPool_.acquire();
}

AVCodecContext* EncoderX265::getCodecContext() const
{
    return codec_ctx_;
}

void EncoderX265::requestKeyFrame()
{
    force_next_keyframe_.store(true, std::memory_order_release);
}

bool EncoderX265::configureCodecContext(const AVCodec* codec)
{
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::cerr << "[EncoderX265] ERROR: avcodec_alloc_context3 failed.\n";
        return false;
    }

    codec_ctx_->width = width_;
    codec_ctx_->height = height_;
    codec_ctx_->pix_fmt = output_fmt_;
    codec_ctx_->time_base = AVRational{1, std::max(1, framerate_)};
    codec_ctx_->framerate = AVRational{std::max(1, framerate_), 1};
    codec_ctx_->bit_rate = std::max(0, bitrate_);
    codec_ctx_->rc_max_rate = std::max(0, vbv_maxrate_);
    codec_ctx_->rc_buffer_size = std::max(0, vbv_bufsize_);
    codec_ctx_->gop_size = std::max(0, gop_size_);
    codec_ctx_->max_b_frames = std::max(0, max_b_frames_);
    codec_ctx_->thread_count = std::max(0, thread_count_);
    codec_ctx_->thread_type = FF_THREAD_FRAME;
    // For MPEG-TS contribution we want Annex-B style in-band headers.
    // x265 repeat-headers=1 below handles VPS/SPS/PPS before keyframes.

    if (interlaced_) {
        codec_ctx_->field_order = top_field_first_ ? AV_FIELD_TT : AV_FIELD_BB;
    } else {
        codec_ctx_->field_order = AV_FIELD_PROGRESSIVE;
    }

    codec_ctx_->color_primaries = color_primaries_;
    codec_ctx_->color_trc = color_trc_;
    codec_ctx_->colorspace = colorspace_;
    codec_ctx_->color_range = color_range_;
    codec_ctx_->chroma_sample_location = chroma_location_;

    return true;
}

bool EncoderX265::validateRequestedPixelFormat(const AVCodec* codec) const
{
    if (!codec_ctx_) {
        std::cerr << "[EncoderX265] codec_ctx_ is null during pixel format validation\n";
        return false;
    }

    const AVPixelFormat requested = codec_ctx_->pix_fmt;
    if (requested == AV_PIX_FMT_NONE) {
        std::cerr << "[EncoderX265] Requested pixel format is AV_PIX_FMT_NONE\n";
        return false;
    }

    const AVPixelFormat* pix_fmts = nullptr;
    int num_pix_fmts = 0;

    const int ret = avcodec_get_supported_config(
        codec_ctx_,
        codec,
        AV_CODEC_CONFIG_PIX_FORMAT,
        0,
        reinterpret_cast<const void**>(&pix_fmts),
        &num_pix_fmts
    );

    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        std::cerr << "[EncoderX265] avcodec_get_supported_config(PIX_FORMAT) failed: "
                  << errbuf << "\n";
        return false;
    }

    if (!pix_fmts || num_pix_fmts <= 0) {
        return true;
    }

    for (int i = 0; i < num_pix_fmts; ++i) {
        if (pix_fmts[i] == AV_PIX_FMT_NONE) break;
        if (pix_fmts[i] == requested) return true;
    }

    std::cerr << "[EncoderX265] Requested pixel format "
              << (av_get_pix_fmt_name(requested) ? av_get_pix_fmt_name(requested) : "unknown")
              << " is not supported by encoder "
              << ((codec && codec->name) ? codec->name : "unknown")
              << ". Supported pixel formats:\n";

    bool supports10Bit = false;
    bool supports422 = false;
    for (int i = 0; i < num_pix_fmts; ++i) {
        if (pix_fmts[i] == AV_PIX_FMT_NONE) break;
        const char* name = av_get_pix_fmt_name(pix_fmts[i]);
        std::cerr << "  - " << (name ? name : "unknown") << "\n";
        supports10Bit = supports10Bit || pixFmtIsAtLeast10Bit(pix_fmts[i]);
        supports422 = supports422 || pixFmtIs422(pix_fmts[i]);
    }

    if (pixFmtIsAtLeast10Bit(requested) && !supports10Bit) {
        std::cerr << "[EncoderX265] Hint: this FFmpeg/libx265 build appears to expose only 8-bit x265 formats. "
                  << "Rebuild x265 as high-bit-depth/multilib and rebuild FFmpeg, or use an 8-bit preset such as yuv420p/main.\n";
    }
    if (pixFmtIs422(requested) && !supports422) {
        std::cerr << "[EncoderX265] Hint: requested 4:2:2 output is not exposed by this libx265 build. "
                  << "Use a 4:2:0 preset or rebuild the x265/FFmpeg stack with the required profile support.\n";
    }

    return false;
}

bool EncoderX265::allocateWorkingFrames()
{
    copy_input_frame_ = av_frame_alloc();
    zc_input_frame_ = av_frame_alloc();
    if (!copy_input_frame_ || !zc_input_frame_) {
        std::cerr << "[EncoderX265] ERROR: failed to allocate input frames.\n";
        return false;
    }

    copy_input_frame_->format = input_fmt_;
    copy_input_frame_->width = width_;
    copy_input_frame_->height = height_;
    copy_input_frame_->color_range = color_range_;
    copy_input_frame_->color_primaries = color_primaries_;
    copy_input_frame_->color_trc = color_trc_;
    copy_input_frame_->colorspace = colorspace_;
    copy_input_frame_->chroma_location = chroma_location_;

    if (av_frame_get_buffer(copy_input_frame_, 32) < 0) {
        std::cerr << "[EncoderX265] ERROR: failed to allocate copy input frame buffer.\n";
        return false;
    }

    return true;
}


static void appendIntOptionIfPresent(const json& opts,
                                     std::string& params,
                                     const std::string& x265Key,
                                     const std::vector<std::string>& jsonKeys)
{
    const int sentinel = -999999;
    const int v = getIntFromObjectAny(&opts, jsonKeys, sentinel);
    if (v != sentinel) {
        appendX265Param(params, x265Key, v);
    }
}

bool EncoderX265::initialize()
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
    if (!codec) {
        std::cerr << "[EncoderX265] ERROR: libx265 encoder not found.\n";
        return false;
    }

    if (!configureCodecContext(codec)) {
        return false;
    }

    std::string profileReason;
    if (!profileMatchesPixFmt(profile_, codec_ctx_->pix_fmt, profileReason)) {
        std::cerr << "[EncoderX265] ERROR: refusing invalid profile/output combination before opening libx265: "
                  << profileReason << ". profile=" << profile_
                  << " pix_fmt=" << pixFmtNameSafe(codec_ctx_->pix_fmt) << "\n";
        return false;
    }

    if (!validateRequestedPixelFormat(codec)) {
        return false;
    }

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", preset_.c_str(), 0);
    if (!tune_.empty()) {
        av_dict_set(&opts, "tune", tune_.c_str(), 0);
    }
    if (!profile_.empty()) {
        av_dict_set(&opts, "profile", profile_.c_str(), 0);
    }

    std::string x265_params;
    // Default to TS-friendly in-band headers/AUD, but do not append them
    // twice when the preset already provides repeat-headers/aud.
    if (!objectHasAny(&additional_options_, {"repeat_headers", "repeat-headers"})) {
        appendX265Param(x265_params, "repeat-headers", 1);
    }
    if (!objectHasAny(&additional_options_, {"aud"})) {
        appendX265Param(x265_params, "aud", 1);
    }
    appendX265Param(x265_params, "keyint", gop_size_);
    appendX265Param(x265_params, "min-keyint", keyint_min_ > 0 ? keyint_min_ : gop_size_);
    appendX265Param(x265_params, "scenecut", getIntFromObjectAny(&additional_options_, {"scenecut"}, 0));
    appendX265Param(x265_params, "open-gop", closed_gop_ ? 0 : 1);
    appendX265Param(x265_params, "bframes", max_b_frames_);
    if (refs_ >= 0) appendX265Param(x265_params, "ref", refs_);
    if (rc_lookahead_ >= 0) appendX265Param(x265_params, "rc-lookahead", rc_lookahead_);
    if (slices_ > 0) appendX265Param(x265_params, "slices", slices_);
    if (level_idc_ > 0) appendX265Param(x265_params, "level-idc", level_idc_);

    appendIntOptionIfPresent(additional_options_, x265_params, "b-adapt", {"b-adapt", "b_adapt"});
    appendIntOptionIfPresent(additional_options_, x265_params, "lookahead-slices", {"lookahead-slices", "lookahead_slices"});
    appendIntOptionIfPresent(additional_options_, x265_params, "high-tier", {"high-tier", "high_tier"});

    if (rate_control_ == "crf") {
        const int crf_value = (crf_ >= 0) ? crf_ : 20;
        appendX265Param(x265_params, "crf", crf_value);
        if (vbv_maxrate_ > 0) appendX265Param(x265_params, "vbv-maxrate", vbv_maxrate_ / 1000);
        if (vbv_bufsize_ > 0) appendX265Param(x265_params, "vbv-bufsize", vbv_bufsize_ / 1000);
    } else {
        appendX265Param(x265_params, "bitrate", std::max(0, bitrate_ / 1000));
        if (vbv_maxrate_ > 0) appendX265Param(x265_params, "vbv-maxrate", vbv_maxrate_ / 1000);
        if (vbv_bufsize_ > 0) appendX265Param(x265_params, "vbv-bufsize", vbv_bufsize_ / 1000);
        if (getStringFromObjectAny(&additional_options_, {"nal-hrd", "nal_hrd"}, std::string()) == "cbr") {
            appendX265Param(x265_params, "hrd", 1);
        }
    }

    const std::string vbvInit = getStringFromObjectAny(&additional_options_,
                                                       {"vbv-init", "vbv_init"},
                                                       std::string());
    if (!vbvInit.empty()) {
        appendX265Param(x265_params, "vbv-init", vbvInit);
    }

    appendIntOptionIfPresent(additional_options_, x265_params, "qp", {"qp"});
    appendIntOptionIfPresent(additional_options_, x265_params, "aq-mode", {"aq-mode", "aq_mode"});
    appendIntOptionIfPresent(additional_options_, x265_params, "subme", {"subme"});

    const std::string me = getStringFromObjectAny(&additional_options_, {"me"}, std::string());
    if (!me.empty()) appendX265Param(x265_params, "me", me);

    appendIntOptionIfPresent(additional_options_, x265_params, "merange", {"merange"});
    appendIntOptionIfPresent(additional_options_, x265_params, "rd", {"rd"});
    appendIntOptionIfPresent(additional_options_, x265_params, "rect", {"rect"});
    appendIntOptionIfPresent(additional_options_, x265_params, "amp", {"amp"});

    if (objectHasAny(&additional_options_, {"strong-intra-smoothing", "strong_intra_smoothing"})) {
        appendX265Param(x265_params, "strong-intra-smoothing",
                        getIntFromObjectAny(&additional_options_, {"strong-intra-smoothing", "strong_intra_smoothing"}, 1));
    } else if (objectHasAny(&additional_options_, {"no-strong-intra-smoothing", "no_strong_intra_smoothing"})) {
        appendX265Param(x265_params, "strong-intra-smoothing",
                        getIntFromObjectAny(&additional_options_, {"no-strong-intra-smoothing", "no_strong_intra_smoothing"}, 0) ? 0 : 1);
    }

    const std::string deblock = getStringFromObjectAny(&additional_options_, {"deblock"}, std::string());
    if (!deblock.empty()) appendX265Param(x265_params, "deblock", deblock);

    if (objectHasAny(&additional_options_, {"sao"})) {
        appendX265Param(x265_params, "sao",
                        getIntFromObjectAny(&additional_options_, {"sao"}, 1));
    } else if (objectHasAny(&additional_options_, {"no-sao", "no_sao"})) {
        appendX265Param(x265_params, "sao",
                        getIntFromObjectAny(&additional_options_, {"no-sao", "no_sao"}, 0) ? 0 : 1);
    }
    appendIntOptionIfPresent(additional_options_, x265_params, "limit-sao", {"limit-sao", "limit_sao"});

    const std::string pools = getStringFromObjectAny(&additional_options_, {"pools"}, std::string());
    if (!pools.empty()) appendX265Param(x265_params, "pools", pools);
    appendIntOptionIfPresent(additional_options_, x265_params, "frame-threads", {"frame-threads", "frame_threads"});
    // Defaults above already enable repeat-headers and AUD for TS friendliness.
    // These aliases allow presets to override them explicitly without requiring one spelling.
    if (objectHasAny(&additional_options_, {"repeat_headers", "repeat-headers"})) {
        appendX265Param(x265_params, "repeat-headers",
                        getIntFromObjectAny(&additional_options_, {"repeat_headers", "repeat-headers"}, 1));
    }
    if (objectHasAny(&additional_options_, {"aud"})) {
        appendX265Param(x265_params, "aud",
                        getIntFromObjectAny(&additional_options_, {"aud"}, 1));
    }

    if (interlaced_) {
        appendX265Param(x265_params, "interlace", top_field_first_ ? "tff" : "bff");
    }

    const std::string cp = x265ColorPrimariesName(color_primaries_);
    const std::string tr = x265TransferName(color_trc_);
    const std::string cm = x265ColorMatrixName(colorspace_);
    if (!cp.empty()) appendX265Param(x265_params, "colorprim", cp);
    if (!tr.empty()) appendX265Param(x265_params, "transfer", tr);
    if (!cm.empty()) appendX265Param(x265_params, "colormatrix", cm);
    if (color_range_ == AVCOL_RANGE_JPEG) appendX265Param(x265_params, "range", "full");
    else if (color_range_ == AVCOL_RANGE_MPEG) appendX265Param(x265_params, "range", "limited");
    if (!x265_master_display_.empty()) appendX265Param(x265_params, "master-display", x265_master_display_);
    if (!x265_max_cll_.empty()) appendX265Param(x265_params, "max-cll", x265_max_cll_);

    x265_params = canonicalizeX265Params(x265_params);

    std::cerr << "[EncoderX265] Opening libx265 with pix_fmt=" << pixFmtNameSafe(output_fmt_)
              << " preset=" << preset_
              << " tune=" << (tune_.empty() ? "none" : tune_)
              << " rate_control=" << rate_control_
              << " params=" << x265_params << "\n";

    av_dict_set(&opts, "x265-params", x265_params.c_str(), 0);

    const int open_ret = avcodec_open2(codec_ctx_, codec, &opts);
    if (open_ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_make_error_string(errbuf, sizeof(errbuf), open_ret);
        std::cerr << "[EncoderX265] ERROR: Failed to open libx265 encoder: "
                  << errbuf << "\n";
        if (opts) {
            AVDictionaryEntry* t = nullptr;
            while ((t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX))) {
                std::cerr << "[EncoderX265]   leftover option: " << t->key
                          << "=" << t->value << "\n";
            }
        }
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    if (!allocateWorkingFrames()) {
        return false;
    }

    return true;
}

bool EncoderX265::fillFramePointersForContiguousInternalBus(AVFrame* f, uint8_t* base) const
{
    if (!f || !base || width_ <= 0 || height_ <= 0) return false;

    const size_t yBytes  = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
    const size_t uvBytes = static_cast<size_t>(width_ / 2) * static_cast<size_t>(height_) * 2;

    f->format = input_fmt_;
    f->width  = width_;
    f->height = height_;

    f->data[0] = base;
    f->data[1] = base + yBytes;
    f->data[2] = base + yBytes + uvBytes;

    f->linesize[0] = width_ * 2;
    f->linesize[1] = (width_ / 2) * 2;
    f->linesize[2] = (width_ / 2) * 2;

    f->color_range = color_range_;
    f->color_primaries = color_primaries_;
    f->color_trc = color_trc_;
    f->colorspace = colorspace_;
    f->chroma_location = chroma_location_;

    return true;
}

bool EncoderX265::copyColorMetadata(AVFrame* dst, const AVFrame* src) const
{
    if (!dst || !src) return false;
    dst->color_range = src->color_range;
    dst->color_primaries = src->color_primaries;
    dst->color_trc = src->color_trc;
    dst->colorspace = src->colorspace;
    dst->chroma_location = src->chroma_location;
    return true;
}

bool EncoderX265::ensureConvertedFrame()
{
    if (output_fmt_ == input_fmt_) {
        return true;
    }

    if (converted_frame_ &&
        converted_frame_->width == width_ &&
        converted_frame_->height == height_ &&
        converted_frame_->format == output_fmt_) {
        return true;
    }

    if (converted_frame_) {
        av_frame_free(&converted_frame_);
    }

    converted_frame_ = av_frame_alloc();
    if (!converted_frame_) {
        std::cerr << "[EncoderX265] ERROR: failed to allocate converted frame.\n";
        return false;
    }

    converted_frame_->format = output_fmt_;
    converted_frame_->width = width_;
    converted_frame_->height = height_;
    converted_frame_->color_range = color_range_;
    converted_frame_->color_primaries = color_primaries_;
    converted_frame_->color_trc = color_trc_;
    converted_frame_->colorspace = colorspace_;
    converted_frame_->chroma_location = chroma_location_;

    if (av_frame_get_buffer(converted_frame_, 32) < 0) {
        std::cerr << "[EncoderX265] ERROR: failed to allocate converted frame buffer.\n";
        av_frame_free(&converted_frame_);
        return false;
    }

    return true;
}

// x265 may retain input for lookahead/reference decisions. Copying into an
// encoder-owned frame avoids lifetime hazards with caller-owned pipeline buffers.
bool EncoderX265::copyIntoInputFrame(AVFrame* src, int64_t pts, bool forceKeyframe, AVFrame** out)
{
    if (!src || !out || !copy_input_frame_) return false;

    if (av_frame_make_writable(copy_input_frame_) < 0) {
        std::cerr << "[EncoderX265] ERROR: input frame not writable.\n";
        return false;
    }

    av_image_copy(copy_input_frame_->data,
                  copy_input_frame_->linesize,
                  const_cast<const uint8_t**>(src->data),
                  src->linesize,
                  input_fmt_,
                  width_,
                  height_);

    copy_input_frame_->pts = pts;
    copy_input_frame_->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (forceKeyframe)
        copy_input_frame_->flags |= AV_FRAME_FLAG_KEY;
    else
        copy_input_frame_->flags &= ~AV_FRAME_FLAG_KEY;
    copyColorMetadata(copy_input_frame_, src);

    if (output_fmt_ == input_fmt_) {
        *out = copy_input_frame_;
        return true;
    }

    if (!ensureConvertedFrame()) {
        return false;
    }

    if (av_frame_make_writable(converted_frame_) < 0) {
        std::cerr << "[EncoderX265] ERROR: converted frame not writable.\n";
        return false;
    }

    sws_ctx_ = sws_getCachedContext(sws_ctx_,
                                    width_, height_, input_fmt_,
                                    width_, height_, output_fmt_,
                                    SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        std::cerr << "[EncoderX265] ERROR: failed to create sws context for "
                  << pixFmtNameSafe(input_fmt_) << " -> " << pixFmtNameSafe(output_fmt_) << ".\n";
        return false;
    }

    const int sws_ret = sws_scale(sws_ctx_,
                                  copy_input_frame_->data,
                                  copy_input_frame_->linesize,
                                  0,
                                  height_,
                                  converted_frame_->data,
                                  converted_frame_->linesize);
    if (sws_ret <= 0) {
        std::cerr << "[EncoderX265] ERROR: sws_scale failed.\n";
        return false;
    }

    converted_frame_->pts = pts;
    converted_frame_->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (forceKeyframe)
        converted_frame_->flags |= AV_FRAME_FLAG_KEY;
    else
        converted_frame_->flags &= ~AV_FRAME_FLAG_KEY;
    copyColorMetadata(converted_frame_, copy_input_frame_);

    *out = converted_frame_;
    return true;
}

bool EncoderX265::submitFrame(AVFrame* in)
{
    if (!codec_ctx_) return false;

    const int ret = avcodec_send_frame(codec_ctx_, in);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        std::cerr << "[EncoderX265] ERROR: avcodec_send_frame failed: " << errbuf << "\n";
        return false;
    }

    return true;
}

void EncoderX265::appendPendingPacket(AVPacketPtr pkt)
{
    if (pkt) {
        pending_packets_.push_back(std::move(pkt));
    }
}

AVPacketPtr EncoderX265::popPendingPacket()
{
    if (pending_packets_.empty()) {
        return {};
    }

    AVPacketPtr pkt = std::move(pending_packets_.front());
    pending_packets_.pop_front();
    return pkt;
}

bool EncoderX265::drainPackets()
{
    if (!codec_ctx_) return false;

    for (;;) {
        AVPacketPtr pkt = acquirePacket();
        if (!pkt) {
            std::cerr << "[EncoderX265] ERROR: Failed to acquire packet.\n";
            return false;
        }

        const int ret = avcodec_receive_packet(codec_ctx_, pkt.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }

        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, sizeof(errbuf), ret);
            std::cerr << "[EncoderX265] ERROR: avcodec_receive_packet failed: "
                      << errbuf << "\n";
            return false;
        }

        if (pkt->duration <= 0) {
            pkt->duration = 1;
        }

        appendPendingPacket(std::move(pkt));
    }
}

std::vector<AVPacketPtr> EncoderX265::collectAllPendingPackets()
{
    std::vector<AVPacketPtr> out;
    out.reserve(pending_packets_.size());
    while (!pending_packets_.empty()) {
        out.emplace_back(std::move(pending_packets_.front()));
        pending_packets_.pop_front();
    }
    return out;
}


void EncoderX265::applyVideoFrameMetadata(AVFrame* dst, const VideoFrame& src) const
{
    if (!dst) return;

    if (codec_ctx_) {
        dst->color_range = codec_ctx_->color_range;
        dst->color_primaries = codec_ctx_->color_primaries;
        dst->color_trc = codec_ctx_->color_trc;
        dst->colorspace = codec_ctx_->colorspace;
        dst->chroma_location = codec_ctx_->chroma_sample_location;
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

bool EncoderX265::attachHdrSideData(AVFrame* dst, const VideoFrame& src) const
{
    if (!dst) return false;

    av_frame_remove_side_data(dst, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    av_frame_remove_side_data(dst, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);

    if (src.has_mastering_display) {
        AVFrameSideData* sd = av_frame_new_side_data(dst,
                                                     AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                                     sizeof(AVMasteringDisplayMetadata));
        if (!sd) {
            std::cerr << "[EncoderX265] WARN: failed to attach mastering display metadata.\n";
            return false;
        }
        std::memcpy(sd->data, &src.mastering_display, sizeof(AVMasteringDisplayMetadata));
    } else if (has_mastering_display_) {
        AVFrameSideData* sd = av_frame_new_side_data(dst,
                                                     AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                                     sizeof(AVMasteringDisplayMetadata));
        if (!sd) {
            std::cerr << "[EncoderX265] WARN: failed to attach preset mastering display metadata.\n";
            return false;
        }
        std::memcpy(sd->data, &mastering_display_, sizeof(AVMasteringDisplayMetadata));
    }

    if (src.has_content_light) {
        AVFrameSideData* sd = av_frame_new_side_data(dst,
                                                     AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
                                                     sizeof(AVContentLightMetadata));
        if (!sd) {
            std::cerr << "[EncoderX265] WARN: failed to attach content light metadata.\n";
            return false;
        }
        std::memcpy(sd->data, &src.content_light, sizeof(AVContentLightMetadata));
    } else if (has_content_light_) {
        AVFrameSideData* sd = av_frame_new_side_data(dst,
                                                     AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
                                                     sizeof(AVContentLightMetadata));
        if (!sd) {
            std::cerr << "[EncoderX265] WARN: failed to attach preset content light metadata.\n";
            return false;
        }
        std::memcpy(sd->data, &content_light_, sizeof(AVContentLightMetadata));
    }

    return true;
}

std::vector<AVPacketPtr> EncoderX265::encodeFrameZeroCopyPackets(const std::shared_ptr<uint8_t>& inputBuf,
                                                                      size_t inputBytes,
                                                                      int64_t pts)
{
    fps_frame_count_++;

    if (!inputBuf || inputBytes == 0 || !codec_ctx_ || !zc_input_frame_)
        return {};

    const size_t y_bytes  = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
    const size_t uv_bytes = static_cast<size_t>(width_ / 2) * static_cast<size_t>(height_) * 2;
    const size_t need = y_bytes + uv_bytes + uv_bytes;

    if (inputBytes < need) {
        std::cerr << "[EncoderX265] ERROR: input buffer too small: "
                  << inputBytes << " < " << need << "\n";
        return {};
    }

    av_frame_unref(zc_input_frame_);
    if (!fillFramePointersForContiguousInternalBus(zc_input_frame_, inputBuf.get())) {
        std::cerr << "[EncoderX265] ERROR: Failed to map zero-copy input frame.\n";
        return {};
    }

    const bool forceKeyframe = (frame_counter_ == 0) ||
                               force_next_keyframe_.exchange(false, std::memory_order_acq_rel);

    AVFrame* encFrame = nullptr;
    if (!copyIntoInputFrame(zc_input_frame_, pts, forceKeyframe, &encFrame)) {
        return {};
    }

    if (!submitFrame(encFrame)) {
        return {};
    }

    if (!drainPackets()) {
        return {};
    }

    frame_counter_++;
    return collectAllPendingPackets();
}


// Manager-facing video submit path. The public API matches x264, but this
// implementation copies before submit for safe HEVC frame ownership.
std::vector<AVPacketPtr> EncoderX265::encodeVideoFramePackets(const VideoFrame& vf)
{
    fps_frame_count_++;

    if (!vf.buffer || vf.buffer_size == 0 || !codec_ctx_ || !zc_input_frame_)
        return {};

    const size_t y_bytes  = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
    const size_t uv_bytes = static_cast<size_t>(width_ / 2) * static_cast<size_t>(height_) * 2;
    const size_t need = y_bytes + uv_bytes + uv_bytes;

    if (vf.buffer_size < need) {
        std::cerr << "[EncoderX265] ERROR: input buffer too small: "
                  << vf.buffer_size << " < " << need << "\n";
        return {};
    }

    av_frame_unref(zc_input_frame_);
    if (!fillFramePointersForContiguousInternalBus(zc_input_frame_, vf.buffer.get())) {
        std::cerr << "[EncoderX265] ERROR: Failed to map zero-copy input frame.\n";
        return {};
    }
    applyVideoFrameMetadata(zc_input_frame_, vf);

    const bool forceKeyframe = (frame_counter_ == 0) ||
                               force_next_keyframe_.exchange(false, std::memory_order_acq_rel);

    AVFrame* encFrame = nullptr;
    if (!copyIntoInputFrame(zc_input_frame_, vf.pts, forceKeyframe, &encFrame)) {
        return {};
    }
    applyVideoFrameMetadata(encFrame, vf);

    if (!submitFrame(encFrame)) {
        return {};
    }

    if (!drainPackets()) {
        return {};
    }

    frame_counter_++;
    return collectAllPendingPackets();
}

AVPacketPtr EncoderX265::encodeFrameZeroCopy(const std::shared_ptr<uint8_t>& inputBuf,
                                             size_t inputBytes,
                                             int64_t pts)
{
    std::vector<AVPacketPtr> packets = encodeFrameZeroCopyPackets(inputBuf, inputBytes, pts);
    if (packets.empty()) {
        return {};
    }
    if (packets.size() > 1) {
        std::cerr << "[EncoderX265] WARN: encodeFrameZeroCopy compatibility API produced "
                  << packets.size() << " packets; returning first packet only. Use encodeFrameZeroCopyPackets().\n";
    }
    return std::move(packets.front());
}

std::vector<AVPacketPtr> EncoderX265::encodeFramePackets(uint8_t* inputFrame, int64_t pts)
{
    fps_frame_count_++;

    if (!inputFrame || !codec_ctx_ || !zc_input_frame_) {
        return {};
    }

    av_frame_unref(zc_input_frame_);
    if (!fillFramePointersForContiguousInternalBus(zc_input_frame_, inputFrame)) {
        std::cerr << "[EncoderX265] ERROR: Failed to map input frame.\n";
        return {};
    }

    const bool forceKeyframe = (frame_counter_ == 0) ||
                               force_next_keyframe_.exchange(false, std::memory_order_acq_rel);

    AVFrame* encFrame = nullptr;
    if (!copyIntoInputFrame(zc_input_frame_, pts, forceKeyframe, &encFrame)) {
        return {};
    }

    if (!submitFrame(encFrame)) {
        return {};
    }

    if (!drainPackets()) {
        return {};
    }

    frame_counter_++;
    return collectAllPendingPackets();
}

AVPacketPtr EncoderX265::encodeFrame(uint8_t* inputFrame, int64_t pts)
{
    std::vector<AVPacketPtr> packets = encodeFramePackets(inputFrame, pts);
    if (packets.empty()) {
        return {};
    }
    return std::move(packets.front());
}

std::vector<AVPacketPtr> EncoderX265::flush()
{
    std::vector<AVPacketPtr> out = collectAllPendingPackets();

    if (!codec_ctx_) {
        return out;
    }

    const int ret = avcodec_send_frame(codec_ctx_, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        std::cerr << "[EncoderX265] ERROR: flush send failed: " << errbuf << "\n";
        return out;
    }

    if (!drainPackets()) {
        return out;
    }

    std::vector<AVPacketPtr> extra = collectAllPendingPackets();
    for (auto& pkt : extra) {
        out.emplace_back(std::move(pkt));
    }
    return out;
}