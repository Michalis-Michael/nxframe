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
extern "C" {
#include <libavutil/cpu.h>
}
#include "core/caption_a53.h"
#include "stage_timing.h"
#include "core/sender_dashboard.h"
#include <ctime>
#include <limits>
#include <stdexcept>
#include <set>
#include <map>
#include <cmath>
#include <new>
#include <regex>

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
                const auto text = v.get<std::string>();
                size_t consumed = 0;
                const int value = std::stoi(text, &consumed);
                if (consumed == text.size()) return value;
            } catch (...) {
            }
        }
    }
    return def;
}

// Quality/search overrides supported by the linked x265 4.2 API. Keeping one
// schema for validation and forwarding avoids silently accepted unused options.
static const std::map<std::string, std::pair<int,int>>& qualityOptionRanges()
{
    static const std::map<std::string, std::pair<int,int>> ranges{
        {"rdoq-level", {0,2}}, {"rskip", {0,2}}, {"rskip-edge-threshold", {0,100}},
        {"limit-refs", {0,3}}, {"limit-tu", {0,4}}, {"max-merge", {1,5}},
        {"tu-intra-depth", {1,4}}, {"tu-inter-depth", {1,4}},
        {"max-tu-size", {4,32}}, {"selective-sao", {0,4}},
        {"nr-intra", {0,2000}}, {"nr-inter", {0,2000}},
        {"cbqpoffs", {-12,12}}, {"crqpoffs", {-12,12}}};
    return ranges;
}

static const std::set<std::string>& qualityBooleanOptions()
{
    static const std::set<std::string> names{
        "early-skip", "fast-intra", "rd-refine", "limit-modes", "signhide",
        "weightp", "weightb", "cutree", "aq-motion", "tskip", "tskip-fast",
        "sao-non-deblock"};
    return names;
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
        if (v.is_number()) return v.dump();
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
        profile == "main422-10") return "main422-10";
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
    try {
        preset_valid_ = parsePreset(presetJson);
    } catch (const std::exception& e) {
        std::cerr << "[EncoderX265] ERROR: invalid preset: " << e.what() << "\n";
    }
    if (preset_valid_) allocateBlackFrame();
}

EncoderX265::~EncoderX265()
{
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
    if (receive_packet_) av_packet_free(&receive_packet_);
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
    if (!presetJson.is_object()) return false;
    const json* video = getVideoSection(presetJson);
    if (presetJson.contains("video") && !video)
        throw std::invalid_argument("video must be an object");
    // Integral FPS is the current sender clock contract; never silently round.
    for (const auto* section : {&presetJson, video}) {
        if (!section) continue;
        for (const char* key : {"width", "height", "framerate", "bitrate",
                                "vbv-maxrate", "vbv_maxrate", "max_bitrate",
                                "vbv-bufsize", "vbv_bufsize", "bufsize",
                                "max_b_frames", "bframes", "gop_size", "min_keyint", "refs"}) {
            if (section->contains(key)) {
                const auto& value = (*section)[key];
                if (!value.is_number_integer() || value < std::numeric_limits<int>::min() ||
                    value > std::numeric_limits<int>::max())
                    throw std::invalid_argument(std::string(key) + " must fit an integer");
            }
        }
        for (const char* key : {"output", "gop", "color", "hdr10", "x265_params", "additional_options"}) {
            if (section->contains(key) && !(*section)[key].is_object())
                throw std::invalid_argument(std::string(key) + " must be an object");
        }
        for (const char* key : {"interlaced", "closed_gop", "single_frame_encoding"}) {
            if (section->contains(key) && !(*section)[key].is_boolean())
                throw std::invalid_argument(std::string(key) + " must be a boolean");
        }
        for (const char* key : {"preset", "tune", "profile", "field_order", "pix_fmt",
                                "pixel_format", "format", "rate_control", "rate-control"}) {
            if (section->contains(key) && !(*section)[key].is_string())
                throw std::invalid_argument(std::string(key) + " must be a string");
        }
    }

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
    const json* crfSection = video && video->contains("crf") ? video : &presetJson;
    if (crfSection->contains("crf")) {
        const auto& value = (*crfSection)["crf"];
        if (!value.is_number()) throw std::invalid_argument("crf must be numeric");
        crf_ = value.get<double>();
    }

    if (width_ < 16 || height_ < 16 || (width_ & 1) ||
        framerate_ <= 0 || framerate_ > 240 || bitrate_ < 0 ||
        vbv_maxrate_ < 0 || vbv_bufsize_ < 0 || max_b_frames_ < 0 ||
        max_b_frames_ > 16 || !std::isfinite(crf_) || (crf_ < 0 && crf_ != -1) || crf_ > 51 ||
        (vbv_maxrate_ > 0 && vbv_maxrate_ < 1000) ||
        (vbv_bufsize_ > 0 && vbv_bufsize_ < 1000) ||
        ((vbv_maxrate_ == 0) != (vbv_bufsize_ == 0)) ||
        av_image_check_size(width_, height_, 0, nullptr) < 0 ||
        av_image_get_buffer_size(input_fmt_, width_, height_, 1) < 0)
        throw std::invalid_argument("invalid dimensions, cadence or rate-control bounds");

    input_bytes_ = static_cast<size_t>(av_image_get_buffer_size(input_fmt_, width_, height_, 1));

    const json* gop = getObjectFlexible(presetJson, video, "gop");
    if (gop) {
        for (const char* key : {"size", "min_keyint", "scenecut"})
            if (gop->contains(key) && (!(*gop)[key].is_number_integer() ||
                (*gop)[key] < std::numeric_limits<int>::min() ||
                (*gop)[key] > std::numeric_limits<int>::max()))
                throw std::invalid_argument(std::string("gop.") + key + " must be an integer");
    }
    if (gop && gop->contains("closed") && !(*gop)["closed"].is_boolean())
        throw std::invalid_argument("gop.closed must be a boolean");
    if (gop) {
        gop_size_   = getIntFromObject(gop, "size", framerate_ * 2);
        keyint_min_ = getIntFromObject(gop, "min_keyint", gop_size_);
        closed_gop_ = getBoolFromObject(gop, "closed", true);
    } else {
        gop_size_   = getIntFlexible(presetJson, video, "gop_size", framerate_ * 2);
        keyint_min_ = getIntFlexible(presetJson, video, "min_keyint", gop_size_);
        closed_gop_ = getBoolFlexible(presetJson, video, "closed_gop", true);
    }

    if (gop_size_ <= 0 || keyint_min_ <= 0 || keyint_min_ > gop_size_)
        throw std::invalid_argument("invalid GOP/keyint bounds");

    preset_  = getStringFlexible(presetJson, video, "preset", "medium");
    tune_    = getStringFlexible(presetJson, video, "tune", "");
    profile_ = getStringFlexible(presetJson, video, "profile", "");

    interlaced_ = getBoolFlexible(presetJson, video, "interlaced", false);
    legacy_single_frame_requested_ = getBoolFlexible(presetJson, video, "single_frame_encoding", false);

    if (interlaced_)
        throw std::invalid_argument("interlaced HEVC requires field splitting; woven-frame encoding is unsupported");

    const json* output = getObjectFlexible(presetJson, video, "output");
    if (output && output->contains("bit_depth") && (!(*output)["bit_depth"].is_number_integer() ||
        ((*output)["bit_depth"] != 8 && (*output)["bit_depth"] != 10)))
        throw std::invalid_argument("output.bit_depth must be an integer");
    output_bit_depth_ = getIntFromObject(output, "bit_depth", 10);
    output_chroma_ = toLowerCopy(getStringFromObject(output, "chroma", "422"));
    output_fmt_ = AV_PIX_FMT_NONE;

    const std::string explicitPixFmt = getStringFlexibleAny(presetJson, video,
                                                            {"pix_fmt", "pixel_format", "format"},
                                                            getStringFromObject(output, "pix_fmt", ""));
    if (!explicitPixFmt.empty()) {
        output_fmt_ = parsePixelFormatName(explicitPixFmt);
        if (output_fmt_ == AV_PIX_FMT_NONE) {
            throw std::invalid_argument("unknown output pixel format: " + explicitPixFmt);
        } else {
            output_bit_depth_ = bitDepthFromPixFmt(output_fmt_, output_bit_depth_);
            output_chroma_ = chromaFromPixFmt(output_fmt_, output_chroma_);
        }
    }

    if (output_fmt_ == AV_PIX_FMT_NONE) {
        output_fmt_ = mapOutputFormat(output_bit_depth_, output_chroma_);
    }

    if (output_fmt_ == AV_PIX_FMT_NONE) {
        throw std::invalid_argument("unsupported output depth/chroma");
    }
    if (mapOutputFormat(output_bit_depth_, output_chroma_) != output_fmt_ ||
        (output_chroma_ == "420" && (height_ & 1)))
        throw std::invalid_argument("unsupported planar output format or chroma dimensions");
    if (output && ((!explicitPixFmt.empty() && output->contains("bit_depth") &&
                   (*output)["bit_depth"].get<int>() != output_bit_depth_) ||
                  (!explicitPixFmt.empty() && output->contains("chroma") &&
                   toLowerCopy((*output)["chroma"].get<std::string>()) != output_chroma_)))
        throw std::invalid_argument("pix_fmt conflicts with output depth/chroma");

    const json* color = getObjectFlexible(presetJson, video, "color");
    if (color) {
        for (const char* key : {"primaries", "transfer", "matrix", "range", "chroma_location"})
            if (color->contains(key) && !(*color)[key].is_string())
                throw std::invalid_argument(std::string("color.") + key + " must be a string");
    }
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
    auto unknownColor = [](const std::string& value, bool unspecified) {
        return !value.empty() && toLowerCopy(value) != "auto" && unspecified;
    };
    if (unknownColor(colorPrimariesText, color_primaries_ == AVCOL_PRI_UNSPECIFIED) ||
        unknownColor(colorTransferText, color_trc_ == AVCOL_TRC_UNSPECIFIED) ||
        unknownColor(colorMatrixText, colorspace_ == AVCOL_SPC_UNSPECIFIED) ||
        unknownColor(colorRangeText, color_range_ == AVCOL_RANGE_UNSPECIFIED) ||
        unknownColor(chromaLocationText, chroma_location_ == AVCHROMA_LOC_UNSPECIFIED))
        throw std::invalid_argument("unrecognized colorimetry");

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
    if (!x265_master_display_.empty()) {
        const std::regex syntax("G\\([0-9]+,[0-9]+\\)B\\([0-9]+,[0-9]+\\)R\\([0-9]+,[0-9]+\\)WP\\([0-9]+,[0-9]+\\)L\\([0-9]+,[0-9]+\\)");
        if (!std::regex_match(x265_master_display_, syntax))
            throw std::invalid_argument("malformed HDR10 master_display");
        const std::regex digits("[0-9]+");
        unsigned long long luminanceMax = 0;
        size_t index = 0;
        for (auto it = std::sregex_iterator(x265_master_display_.begin(), x265_master_display_.end(), digits);
             it != std::sregex_iterator(); ++it, ++index) {
            const auto value = std::stoull(it->str());
            if ((index < 8 && value > 50000) || value > std::numeric_limits<uint32_t>::max() ||
                (index == 9 && value > luminanceMax))
                throw std::invalid_argument("HDR10 mastering coordinates/luminance outside SEI bounds");
            if (index == 8) luminanceMax = value;
        }
    }
    if (!x265_max_cll_.empty()) {
        if (!std::regex_match(x265_max_cll_, std::regex("[0-9]+,[0-9]+")))
            throw std::invalid_argument("malformed HDR10 max_cll");
        int maxCll = 0;
        int maxFall = 0;
        const size_t comma = x265_max_cll_.find(',');
        maxCll = std::stoi(x265_max_cll_.substr(0, comma));
        maxFall = std::stoi(x265_max_cll_.substr(comma + 1));
        if (maxCll > 65535 || maxFall > 65535)
            throw std::invalid_argument("HDR10 max_cll exceeds 16-bit SEI range");
        content_light_.MaxCLL = static_cast<unsigned>(maxCll);
        content_light_.MaxFALL = static_cast<unsigned>(maxFall);
        has_content_light_ = true;
    }

    additional_options_ = json::object();
    mergeObjectInto(additional_options_, getObjectFlexible(presetJson, video, "x265_params"));
    mergeObjectInto(additional_options_, getObjectFlexible(presetJson, video, "additional_options"));
    if (gop && gop->contains("scenecut") && !additional_options_.contains("scenecut"))
        additional_options_["scenecut"] = (*gop)["scenecut"];

    // This is a supported option schema, not an unchecked libx265 CLI passthrough.
    // FFmpeg can merely warn on malformed x265-params, so catch mistakes here.
    const std::set<std::string> textOptions{
        "rate-control", "level", "level-idc", "me", "pools", "deblock", "nal-hrd", "asm"};
    const std::set<std::string> realOptions{"vbv-init", "aq-strength", "psy-rd", "psy-rdoq"};
    const std::set<std::string> intOptions{
        "threads", "rc-lookahead", "slices", "ref", "refs", "scenecut", "b-adapt",
        "lookahead-slices", "high-tier", "aq-mode", "subme", "merange", "rd", "rect",
        "amp", "strong-intra-smoothing", "no-strong-intra-smoothing", "sao", "no-sao",
        "limit-sao", "frame-threads", "repeat-headers", "aud", "strict-cbr", "wpp", "ctu"};
    std::set<std::string> seenOptions;
    for (auto it = additional_options_.begin(); it != additional_options_.end(); ++it) {
        std::string key = it.key();
        std::replace(key.begin(), key.end(), '_', '-');
        const bool negatedQualityBoolean = key.compare(0,3,"no-")==0 &&
            qualityBooleanOptions().count(key.substr(3));
        const std::string qualityKey = negatedQualityBoolean ? key.substr(3) : key;
        const std::string slot = negatedQualityBoolean ? qualityKey : key == "refs" ? "ref" : key == "level" ? "level-idc" :
                                 key == "no-sao" ? "sao" : key == "no-strong-intra-smoothing" ?
                                 "strong-intra-smoothing" : key == "threads" ? "frame-threads" : key;
        if (!seenOptions.insert(slot).second)
            throw std::invalid_argument("duplicate x265 option alias: " + key);
        std::string value = getStringFromObjectAny(&additional_options_, {it.key()}, "");
        if (value.empty() || value.find(':') != std::string::npos ||
            value.find('=') != std::string::npos)
            throw std::invalid_argument("invalid x265 option value: " + key);
        const auto range = qualityOptionRanges().find(qualityKey);
        const bool qualityBoolean = qualityBooleanOptions().count(qualityKey)!=0;
        if (range!=qualityOptionRanges().end() || qualityBoolean) {
            if (qualityBoolean && (value=="true" || value=="false"))
                value=value=="true" ? "1" : "0";
            size_t used=0;
            const double number=std::stod(value,&used);
            const int minimum=qualityBoolean ? 0 : range->second.first;
            const int maximum=qualityBoolean ? 1 : range->second.second;
            if (used!=value.size() || !std::isfinite(number) || number!=std::floor(number) ||
                number<minimum || number>maximum ||
                (qualityKey=="max-tu-size" && number!=4 && number!=8 && number!=16 && number!=32))
                throw std::invalid_argument("x265 option outside supported range: " + key);
            it.value()=static_cast<int>(number);
            continue;
        }
        if (textOptions.count(key)) {
            if (key == "asm") {
                if (value != "auto" && value != "avx2" && value != "avx512")
                    throw std::invalid_argument("asm supports auto, avx2 or avx512");
                const int required = value == "avx2" ? AV_CPU_FLAG_AVX2 :
                                     value == "avx512" ? AV_CPU_FLAG_AVX512 : 0;
                if (required && !(av_get_cpu_flags() & required))
                    throw std::invalid_argument("requested asm is unavailable on this CPU/OS/build");
            }
            if (key == "deblock" && value != "0" && value != "1" && value != "true" && value != "false")
                throw std::invalid_argument("deblock supports boolean values in this wrapper");
            if (key == "pools" && value != "none" &&
                value.find_first_not_of("0123456789,+- ") != std::string::npos)
                throw std::invalid_argument("invalid thread pool specification");
            if (key == "me" && value != "dia" && value != "hex" && value != "umh" &&
                value != "star" && value != "sea" && value != "full")
                throw std::invalid_argument("invalid motion estimation method");
            if (key == "nal-hrd" && value != "none" && value != "vbr" && value != "cbr")
                throw std::invalid_argument("invalid nal-hrd mode");
            if (key == "level" || key == "level-idc") {
                const std::set<std::string> levels{"1", "1.0", "10", "2", "2.0", "20", "2.1", "21",
                    "3", "3.0", "30", "3.1", "31", "4", "4.0", "40", "4.1", "41",
                    "5", "5.0", "50", "5.1", "51", "5.2", "52", "6", "6.0", "60",
                    "6.1", "61", "6.2", "62"};
                if (!levels.count(value)) throw std::invalid_argument("invalid HEVC level");
            }
            continue;
        }
        if (!intOptions.count(key) && !realOptions.count(key))
            throw std::invalid_argument("unsupported x265 option: " + key);
        size_t used = 0;
        const double number = std::stod(value, &used);
        if (used != value.size() || !std::isfinite(number) ||
            (intOptions.count(key) && (number != std::floor(number) ||
             number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())))
            throw std::invalid_argument("invalid numeric x265 option: " + key);
        if (key == "vbv-init" && number <= 0)
            throw std::invalid_argument("vbv-init must be positive");
        if ((key=="aq-strength" && number>3) || (key=="psy-rd" && number>5) ||
            (key=="psy-rdoq" && number>50))
            throw std::invalid_argument("x265 option outside supported range: " + key);
        if (realOptions.count(key) && number < 0)
            throw std::invalid_argument("negative x265 option: " + key);
        const std::set<std::string> boolOptions{"high-tier", "rect", "amp", "strong-intra-smoothing",
            "no-strong-intra-smoothing", "sao", "no-sao", "limit-sao", "repeat-headers", "aud", "strict-cbr", "wpp"};
        if (intOptions.count(key) && (number < 0 ||
            (boolOptions.count(key) && number > 1) ||
            (key == "b-adapt" && number > 2) || (key == "aq-mode" && number > 4) ||
            (key == "rd" && (number < 1 || number > 6)) || (key == "subme" && number > 7) ||
            (key == "rc-lookahead" && number > 250) ||
            (key == "lookahead-slices" && number > 16) ||
            ((key == "ref" || key == "refs") && (number < 1 || number > 16)) ||
            ((key == "frame-threads" || key == "threads") && number >= 16) ||
            (key == "slices" && (number < 1 || number > 16)) ||
            (key == "ctu" && number != 16 && number != 32 && number != 64)))
            throw std::invalid_argument("x265 option outside supported range: " + key);
        if (intOptions.count(key)) it.value() = static_cast<int>(number);
    }

    rate_control_ = toLowerCopy(getStringFlexibleAny(presetJson, video,
                                                     {"rate_control", "rate-control"},
                                                     getStringFromObjectAny(&additional_options_,
                                                                            {"rate_control", "rate-control"},
                                                                            "cbr")));

    thread_count_ = getIntFromObjectAny(&additional_options_, {"threads", "frame-threads", "frame_threads"}, 0);
    rc_lookahead_ = getIntFromObjectAny(&additional_options_, {"rc_lookahead", "rc-lookahead"}, -1);
    slices_       = getIntFromObjectAny(&additional_options_, {"slices"}, -1);
    refs_         = getIntFromObjectAny(&additional_options_, {"ref", "refs"},
                    getIntFlexible(presetJson, video, "refs", -1));

    // The old single_frame_encoding mode forced frame-threads=1, rc-lookahead=0
    // and same-call output. That made latency small by disabling the native x265
    // pipeline, but it also removed the frame/WPP parallelism needed for stable
    // high-quality 1080p50 contribution. Keep accepting the legacy flag so old
    // presets remain loadable, but do not let it modify codec policy.
    if (legacy_single_frame_requested_) {
        std::cerr << "[EncoderX265] WARN: single_frame_encoding is deprecated and ignored; "
                  << "x265 now uses its native buffered pipeline. Configure frame-threads, "
                  << "rc-lookahead, WPP and pools explicitly when required.\n";
    }

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

    if (profile_ != "main" && profile_ != "main10" && profile_ != "main422-10")
        throw std::invalid_argument("supported profiles: main, main10, main422-10");
    if (rate_control_ != "cbr" && rate_control_ != "abr" && rate_control_ != "vbr" &&
        rate_control_ != "crf")
        throw std::invalid_argument("supported rate_control: cbr, abr, vbr, crf");
    if (rate_control_ != "crf" && bitrate_ < 1000)
        throw std::invalid_argument("bitrate must be at least 1000 bps");
    if (rate_control_ == "cbr" && (vbv_maxrate_ < 1000 || vbv_bufsize_ < 1000))
        throw std::invalid_argument("CBR requires a positive VBV maxrate and buffer");
    if (thread_count_ < 0 || thread_count_ >= 16 || refs_ == 0 || refs_ < -1 || refs_ > 16 || rc_lookahead_ < -1 ||
        rc_lookahead_ > 250 || slices_ < -1)
        throw std::invalid_argument("invalid threading, reference or lookahead bounds");

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
    // AVCodecContext::thread_count maps to libx265 frame threads in FFmpeg's
    // wrapper. Zero deliberately means x265/FFmpeg automatic selection. Do not
    // force FF_THREAD_SLICE here: libx265's native frame threading, WPP and
    // worker pools must remain free to cooperate for stable high-throughput HEVC.
    codec_ctx_->thread_count = std::max(0, thread_count_);
    // For MPEG-TS contribution we want Annex-B style in-band headers.
    // x265 repeat-headers=1 below handles VPS/SPS/PPS before keyframes.

    codec_ctx_->field_order = AV_FIELD_PROGRESSIVE;

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

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)
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

#else
    pix_fmts = codec->pix_fmts;
    if (pix_fmts) while (pix_fmts[num_pix_fmts] != AV_PIX_FMT_NONE) ++num_pix_fmts;
#endif

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

    // Shared native input needs no copy pixels. Allocate them lazily only if
    // the borrowed-pointer compatibility path is actually used.
    receive_packet_ = av_packet_alloc();
    if (!receive_packet_) {
        std::cerr << "[EncoderX265] ERROR: failed to allocate receive scratch packet.\n";
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
    if (initialized_) return !flushed_ && !failed_;
    // Initialization is one-shot: a partial codec must never be reused.
    if (!preset_valid_ || !blackFrameYUV_ || codec_ctx_) return false;
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

    av_dict_set(&opts, "forced-idr", "1", 0);
    av_dict_set(&opts, "a53cc", "1", 0);

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
        appendX265Param(x265_params, "crf", json(crf_ >= 0 ? crf_ : 20.0).dump());
        if (vbv_maxrate_ > 0) appendX265Param(x265_params, "vbv-maxrate", vbv_maxrate_ / 1000);
        if (vbv_bufsize_ > 0) appendX265Param(x265_params, "vbv-bufsize", vbv_bufsize_ / 1000);
    } else {
        appendX265Param(x265_params, "bitrate", std::max(0, bitrate_ / 1000));
        if (vbv_maxrate_ > 0) appendX265Param(x265_params, "vbv-maxrate", vbv_maxrate_ / 1000);
        if (vbv_bufsize_ > 0) appendX265Param(x265_params, "vbv-bufsize", vbv_bufsize_ / 1000);
    }
    const auto hrd = getStringFromObjectAny(&additional_options_, {"nal-hrd", "nal_hrd"}, "none");
    if (hrd == "cbr" || hrd == "vbr") appendX265Param(x265_params, "hrd", 1);

    const std::string vbvInit = getStringFromObjectAny(&additional_options_,
                                                       {"vbv-init", "vbv_init"},
                                                       std::string());
    if (!vbvInit.empty()) {
        appendX265Param(x265_params, "vbv-init", vbvInit);
    }

    for (const char* key : {"strict-cbr", "wpp", "ctu", "aq-strength", "psy-rd", "psy-rdoq"}) {
        std::string alias = key;
        std::replace(alias.begin(), alias.end(), '-', '_');
        const std::string value = getStringFromObjectAny(&additional_options_, {key, alias}, "");
        if (!value.empty()) appendX265Param(x265_params, key, value);
    }
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

    // Preset/tune defaults are established by FFmpeg first; these explicit
    // x265-params are parsed afterward and therefore override those defaults.
    for (auto it=additional_options_.begin(); it!=additional_options_.end(); ++it) {
        std::string key=it.key();
        std::replace(key.begin(),key.end(),'_','-');
        const bool negated=key.compare(0,3,"no-")==0 && qualityBooleanOptions().count(key.substr(3));
        if (negated) key=key.substr(3);
        if (qualityOptionRanges().count(key) || qualityBooleanOptions().count(key)) {
            int value=it.value().get<int>();
            if (negated) value=1-value;
            appendX265Param(x265_params,key,value);
        }
    }

    const std::string assembly = getStringFromObjectAny(&additional_options_, {"asm"}, "auto");
    if (assembly != "auto") appendX265Param(x265_params, "asm", assembly);
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
    if (opts) {
        std::cerr << "[EncoderX265] ERROR: unconsumed FFmpeg options.\n";
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    if (!allocateWorkingFrames()) return false;
    initialized_ = true;
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

// Retain reference-counted native input; copy borrowed native input; otherwise
// convert directly into an encoder-owned output frame.
bool EncoderX265::prepareInputFrame(AVFrame* src, int64_t pts, bool forceKeyframe, AVFrame** out)
{
    if (!src || !out || !copy_input_frame_) return false;

    src->pts = pts;
    src->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (forceKeyframe) src->flags |= AV_FRAME_FLAG_KEY;
    else src->flags &= ~AV_FRAME_FLAG_KEY;
    if (output_fmt_ == input_fmt_ && src->buf[0]) {
        *out = src;
        return true;
    }

    if (output_fmt_ == input_fmt_ && !copy_input_frame_->buf[0] &&
        av_frame_get_buffer(copy_input_frame_, 32) < 0) {
        std::cerr << "[EncoderX265] ERROR: failed to allocate copy input frame buffer.\n";
        return false;
    }
    if (output_fmt_ == input_fmt_ && av_frame_make_writable(copy_input_frame_) < 0) {
        std::cerr << "[EncoderX265] ERROR: input frame not writable.\n";
        return false;
    }

    if (output_fmt_ == input_fmt_) {
        av_image_copy(copy_input_frame_->data, copy_input_frame_->linesize,
                      const_cast<const uint8_t**>(src->data), src->linesize,
                      input_fmt_, width_, height_);
        copy_input_frame_->pts = pts;
        copy_input_frame_->pict_type = src->pict_type;
        copy_input_frame_->flags = src->flags;
        copyColorMetadata(copy_input_frame_, src);
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

    static auto& conversionStat=stage_timing::get("x265_convert");
    int sws_ret;
    {
        stage_timing::ScopedTimer timer(conversionStat);
        sws_ret = sws_scale(sws_ctx_,
                                  src->data,
                                  src->linesize,
                                  0,
                                  height_,
                                  converted_frame_->data,
                                  converted_frame_->linesize);
    }
    if (sws_ret != height_) {
        std::cerr << "[EncoderX265] ERROR: sws_scale failed.\n";
        return false;
    }

    converted_frame_->pts = pts;
    converted_frame_->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (forceKeyframe)
        converted_frame_->flags |= AV_FRAME_FLAG_KEY;
    else
        converted_frame_->flags &= ~AV_FRAME_FLAG_KEY;
    copyColorMetadata(converted_frame_, src);

    *out = converted_frame_;
    return true;
}

bool EncoderX265::submitFrame(AVFrame* in)
{
    if (!codec_ctx_) return false;

    static auto& sendStat=stage_timing::get("x265_send_frame");
    const auto send=[&]() {
        diagnosticSubmission_=false;
        if(!stage_timing::enabled()) return avcodec_send_frame(codec_ctx_,in);
        timespec cpuStart{},cpuEnd{};
        const bool cpuStartValid=::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpuStart)==0;
        const auto started=std::chrono::steady_clock::now();
        int result;
        { stage_timing::ScopedTimer timer(sendStat); result=avcodec_send_frame(codec_ctx_,in); }
        diagnosticSubmitWallNs_=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count());
        const bool cpuEndValid=::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpuEnd)==0;
        diagnosticSubmitCpuNs_=cpuStartValid && cpuEndValid ?
            int64_t(cpuEnd.tv_sec-cpuStart.tv_sec)*1000000000LL+cpuEnd.tv_nsec-cpuStart.tv_nsec : -1;
        diagnosticSubmittedPts_=in?in->pts:AV_NOPTS_VALUE;
        diagnosticSubmission_=in && result==0;
        return result;
    };
    int ret = send();
    if (ret == AVERROR(EAGAIN)) {
        if (!drainPackets()) return false;
        ret = send();
    }
    if (!in && ret == AVERROR_EOF) return true;
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_make_error_string(errbuf, sizeof(errbuf), ret);
        failed_ = true;
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
    if (!codec_ctx_ || !receive_packet_) return false;
    static auto& receiveStat=stage_timing::get("x265_receive_packet");
    for (;;) {
        int ret;
        { stage_timing::ScopedTimer timer(receiveStat); ret=avcodec_receive_packet(codec_ctx_,receive_packet_); }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, sizeof(errbuf), ret);
            failed_ = true;
            std::cerr << "[EncoderX265] ERROR: avcodec_receive_packet failed: " << errbuf << "\n";
            return false;
        }
        if(diagnosticSubmission_) {
            diagnosticSubmission_=false;
            const bool key=(receive_packet_->flags & AV_PKT_FLAG_KEY)!=0;
            static auto& keyStat=stage_timing::get("x265_key_output_submit");
            static auto& interStat=stage_timing::get("x265_inter_output_submit");
            static auto& cpuStat=stage_timing::get("x265_submit_caller_cpu");
            (key?keyStat:interStat).add(diagnosticSubmitWallNs_);
            if(diagnosticSubmitCpuNs_>=0) cpuStat.add(static_cast<uint64_t>(diagnosticSubmitCpuNs_));
            const double budget=codec_ctx_->framerate.num>0 ?
                1000.0*codec_ctx_->framerate.den/codec_ctx_->framerate.num : 0;
            const double wallMs=diagnosticSubmitWallNs_/1000000.0;
            if((budget>0 && wallMs>budget) &&
               (stage_timing::verbose_enabled() || nxframe::senderDashboard().diagnosticsEnabled())) {
                std::cerr<<"[EncoderX265][DIAG] submit_wall_ms="<<wallMs
                         <<" caller_cpu_ms="<<(diagnosticSubmitCpuNs_>=0 ? diagnosticSubmitCpuNs_/1000000.0 : -1)
                         <<" submitted_pts="<<diagnosticSubmittedPts_
                         <<" output_pts="<<receive_packet_->pts<<" output_key="<<key
                         <<" packet_bytes="<<receive_packet_->size<<" budget_ms="<<budget<<"\n";
            }
        }
        // Acquire an output-owned packet only after receiving a real packet.
        // Empty/EAGAIN probes avoid the shared packet-pool mutex entirely.
        AVPacketPtr pkt = acquirePacket();
        if (!pkt) {
            av_packet_unref(receive_packet_);
            failed_ = true;
            std::cerr << "[EncoderX265] ERROR: Failed to acquire packet.\n";
            return false;
        }
        av_packet_move_ref(pkt.get(),receive_packet_);
        if (pkt->duration <= 0) pkt->duration = 1;
        appendPendingPacket(std::move(pkt));
    }
}

std::vector<AVPacketPtr> EncoderX265::collectAllPendingPackets()
{
    std::vector<AVPacketPtr> out;
    collectAllPendingPackets(out);
    return out;
}

void EncoderX265::collectAllPendingPackets(std::vector<AVPacketPtr>& out)
{
    out.reserve(out.size()+pending_packets_.size());
    while (!pending_packets_.empty()) {
        out.emplace_back(std::move(pending_packets_.front()));
        pending_packets_.pop_front();
    }
}

void EncoderX265::reportPipelineDiagnostics(int64_t submittedPts,
                                              const std::vector<AVPacketPtr>& out)
{
    if (!stage_timing::enabled()) return;

    ++pipeline_diag_submissions_;
    pipeline_diag_packets_ += out.size();

    int64_t outputPts = AV_NOPTS_VALUE;
    for (const auto& pkt : out) {
        if (pkt && pkt->pts != AV_NOPTS_VALUE) {
            outputPts = pkt->pts;
            break;
        }
    }

    if (submittedPts == AV_NOPTS_VALUE || outputPts == AV_NOPTS_VALUE) {
        ++pipeline_diag_no_output_;
    } else {
        ++pipeline_diag_with_output_;
        const int64_t lag = submittedPts - outputPts;
        pipeline_diag_lag_sum_ += lag;
        pipeline_diag_lag_min_ = std::min(pipeline_diag_lag_min_, lag);
        pipeline_diag_lag_max_ = std::max(pipeline_diag_lag_max_, lag);
        if (pipeline_diag_have_last_lag_ && lag != pipeline_diag_last_lag_)
            ++pipeline_diag_lag_changes_;
        pipeline_diag_last_lag_ = lag;
        pipeline_diag_have_last_lag_ = true;
        pipeline_diag_latest_submitted_pts_ = submittedPts;
        pipeline_diag_latest_output_pts_ = outputPts;
    }

    const auto now = std::chrono::steady_clock::now();
    if (pipeline_diag_last_report_.time_since_epoch().count() == 0) {
        pipeline_diag_last_report_ = now;
        return;
    }
    if (now - pipeline_diag_last_report_ < std::chrono::seconds(5)) return;

    const double lagAvg = pipeline_diag_with_output_ > 0
        ? static_cast<double>(pipeline_diag_lag_sum_) /
          static_cast<double>(pipeline_diag_with_output_)
        : 0.0;
    const int64_t lagMin = pipeline_diag_with_output_ > 0 ? pipeline_diag_lag_min_ : 0;
    const int64_t lagMax = pipeline_diag_with_output_ > 0 ? pipeline_diag_lag_max_ : 0;

    std::cerr << "[EncoderX265][PIPELINE] submissions=" << pipeline_diag_submissions_
              << " with_output=" << pipeline_diag_with_output_
              << " no_output=" << pipeline_diag_no_output_
              << " packets=" << pipeline_diag_packets_
              << " lag_frames_avg=" << lagAvg
              << " lag_frames_min=" << lagMin
              << " lag_frames_max=" << lagMax
              << " lag_changes=" << pipeline_diag_lag_changes_
              << " latest_submitted_pts=" << pipeline_diag_latest_submitted_pts_
              << " latest_output_pts=" << pipeline_diag_latest_output_pts_
              << "\n";

    pipeline_diag_submissions_ = 0;
    pipeline_diag_with_output_ = 0;
    pipeline_diag_no_output_ = 0;
    pipeline_diag_packets_ = 0;
    pipeline_diag_lag_sum_ = 0;
    pipeline_diag_lag_min_ = std::numeric_limits<int64_t>::max();
    pipeline_diag_lag_max_ = std::numeric_limits<int64_t>::min();
    pipeline_diag_lag_changes_ = 0;
    pipeline_diag_last_report_ = now;
}


bool EncoderX265::applyVideoFrameMetadata(AVFrame* dst, const VideoFrame& src) const
{
    if (!dst) return false;

    if (codec_ctx_) {
        dst->color_range = codec_ctx_->color_range;
        dst->color_primaries = codec_ctx_->color_primaries;
        dst->color_trc = codec_ctx_->color_trc;
        dst->colorspace = codec_ctx_->colorspace;
        dst->chroma_location = codec_ctx_->chroma_sample_location;
    }

    av_frame_remove_side_data(dst, AV_FRAME_DATA_A53_CC);
    const auto a53 = nxframe::buildA53CcData(src.metadata.caption);
    if (!a53.empty()) {
        AVFrameSideData* sd = av_frame_new_side_data(dst, AV_FRAME_DATA_A53_CC, a53.size());
        if (sd) std::memcpy(sd->data, a53.data(), a53.size());
        else {
            std::cerr << "[EncoderX265] ERROR: failed to attach captions.\n";
            return false;
        }
    }

    return attachHdrSideData(dst, src);
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

// The AVBufferRef owns a shared_ptr, so FFmpeg can retain an input safely.
// Caller pixels are immutable until the final reference has been released.
static void releaseSharedInput(void* opaque, uint8_t*)
{
    delete static_cast<std::shared_ptr<uint8_t>*>(opaque);
}

std::vector<AVPacketPtr> EncoderX265::encodeFrameZeroCopyPackets(
    const std::shared_ptr<uint8_t>& inputBuf, size_t inputBytes, int64_t pts)
{
    VideoFrame vf;
    vf.buffer = inputBuf;
    vf.buffer_size = inputBytes;
    vf.width = width_;
    vf.height = height_;
    vf.pix_fmt = input_fmt_;
    vf.time_base = AVRational{1, framerate_};
    vf.pts = pts;
    return encodeVideoFramePackets(vf);
}

std::vector<AVPacketPtr> EncoderX265::encodeVideoFramePackets(const VideoFrame& vf)
{
    std::vector<AVPacketPtr> out;
    encodeVideoFramePackets(vf, out);
    return out;
}

void EncoderX265::encodeVideoFramePackets(const VideoFrame& vf, std::vector<AVPacketPtr>& out)
{
    out.clear();
    if (!initialized_ || flushed_ || failed_ || !vf.buffer || !zc_input_frame_)
        return collectAllPendingPackets(out);
    if (vf.pts == AV_NOPTS_VALUE || vf.width != width_ || vf.height != height_ || vf.pix_fmt != input_fmt_ ||
        vf.interlaced || vf.time_base.num <= 0 || vf.time_base.den <= 0) {
        std::cerr << "[EncoderX265] ERROR: incompatible input dimensions, format, fields or clock.\n";
        return collectAllPendingPackets(out);
    }
    av_frame_unref(zc_input_frame_);
    struct UnrefOnExit {
        AVFrame* frame;
        ~UnrefOnExit() { av_frame_unref(frame); }
    } releaseInput{zc_input_frame_};
    const uintptr_t base = reinterpret_cast<uintptr_t>(vf.buffer.get());
    if (base & 1) {
        std::cerr << "[EncoderX265] ERROR: 10-bit input allocation must be sample-aligned.\n";
        return collectAllPendingPackets(out);
    }
    // Explicit planes must all be present. Empty planes mean tightly packed bus.
    const bool explicitPlanes = vf.data[0] || vf.data[1] || vf.data[2];
    if (!explicitPlanes) {
        if (vf.buffer_size < input_bytes_) {
            std::cerr << "[EncoderX265] ERROR: input buffer too small.\n";
            return collectAllPendingPackets(out);
        }
        fillFramePointersForContiguousInternalBus(zc_input_frame_, vf.buffer.get());
    } else {
        for (int plane = 0; plane < 3; ++plane) {
            const uintptr_t address = reinterpret_cast<uintptr_t>(vf.data[plane]);
            const size_t rowBytes = plane ? width_ : width_ * 2;
            if (!vf.data[plane] || vf.linesize[plane] < static_cast<int>(rowBytes) || address < base ||
                (address & 1) || (vf.linesize[plane] & 1))
                return collectAllPendingPackets(out);
            const size_t offset = address - base;
            const size_t stride = static_cast<size_t>(vf.linesize[plane]);
            if (offset > vf.buffer_size || rowBytes > vf.buffer_size - offset ||
                static_cast<size_t>(height_ - 1) > (vf.buffer_size - offset - rowBytes) / stride) {
                std::cerr << "[EncoderX265] ERROR: plane exceeds input allocation.\n";
                return collectAllPendingPackets(out);
            }
            zc_input_frame_->data[plane] = vf.data[plane];
            zc_input_frame_->linesize[plane] = vf.linesize[plane];
        }
        zc_input_frame_->width = width_;
        zc_input_frame_->height = height_;
        zc_input_frame_->format = input_fmt_;
    }
    auto* holder = new (std::nothrow) std::shared_ptr<uint8_t>(vf.buffer);
    if (!holder) return collectAllPendingPackets(out);
    zc_input_frame_->buf[0] = av_buffer_create(vf.buffer.get(), vf.buffer_size,
                                             releaseSharedInput, holder, AV_BUFFER_FLAG_READONLY);
    if (!zc_input_frame_->buf[0]) {
        delete holder;
        return collectAllPendingPackets(out);
    }
    copyColorMetadata(zc_input_frame_, copy_input_frame_);
    const int64_t pts = av_rescale_q(vf.pts, vf.time_base, codec_ctx_->time_base);
    const bool force = frame_counter_ == 0 || force_next_keyframe_.load(std::memory_order_acquire);
    AVFrame* encoded = nullptr;
    if (!prepareInputFrame(zc_input_frame_, pts, force, &encoded))
        return collectAllPendingPackets(out);
    if (!applyVideoFrameMetadata(encoded, vf)) return collectAllPendingPackets(out);
    // Consume only an accepted frame's request; keep it on prepare/send failure.
    const bool requested = force_next_keyframe_.exchange(false, std::memory_order_acq_rel);
    if (requested) {
        encoded->pict_type = AV_PICTURE_TYPE_I;
        encoded->flags |= AV_FRAME_FLAG_KEY;
    }
    if (!submitFrame(encoded)) {
        if (requested) force_next_keyframe_.store(true, std::memory_order_release);
        return collectAllPendingPackets(out);
    }
    ++frame_counter_;
    if (!drainPackets()) {
        out.clear();
        return;
    }
    collectAllPendingPackets(out);
    reportPipelineDiagnostics(pts, out);
}

AVPacketPtr EncoderX265::encodeFrameZeroCopy(const std::shared_ptr<uint8_t>& inputBuf,
                                             size_t inputBytes,
                                             int64_t pts)
{
    std::vector<AVPacketPtr> packets = encodeFrameZeroCopyPackets(inputBuf, inputBytes, pts);
    if (packets.empty()) {
        return {};
    }
    for (size_t i = 1; i < packets.size(); ++i) appendPendingPacket(std::move(packets[i]));
    return std::move(packets.front());
}

std::vector<AVPacketPtr> EncoderX265::encodeFramePackets(uint8_t* inputFrame, int64_t pts)
{
    if (!inputFrame || pts == AV_NOPTS_VALUE || (reinterpret_cast<uintptr_t>(inputFrame) & 1) ||
        !initialized_ || flushed_ || failed_ || !zc_input_frame_) {
        return {};
    }

    av_frame_unref(zc_input_frame_);
    if (!fillFramePointersForContiguousInternalBus(zc_input_frame_, inputFrame)) {
        std::cerr << "[EncoderX265] ERROR: Failed to map input frame.\n";
        return {};
    }

    const bool requested = force_next_keyframe_.exchange(false, std::memory_order_acq_rel);
    const bool forceKeyframe = (frame_counter_ == 0) || requested;

    AVFrame* encFrame = nullptr;
    if (!prepareInputFrame(zc_input_frame_, pts, forceKeyframe, &encFrame)) {
        if (requested) force_next_keyframe_.store(true, std::memory_order_release);
        return {};
    }

    av_frame_remove_side_data(encFrame, AV_FRAME_DATA_A53_CC);
    av_frame_remove_side_data(encFrame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    av_frame_remove_side_data(encFrame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (!submitFrame(encFrame)) {
        if (requested) force_next_keyframe_.store(true, std::memory_order_release);
        return collectAllPendingPackets();
    }
    frame_counter_++;
    if (!drainPackets()) return {};
    std::vector<AVPacketPtr> out = collectAllPendingPackets();
    reportPipelineDiagnostics(pts, out);
    return out;
}

AVPacketPtr EncoderX265::encodeFrame(uint8_t* inputFrame, int64_t pts)
{
    std::vector<AVPacketPtr> packets = encodeFramePackets(inputFrame, pts);
    if (packets.empty()) {
        return {};
    }
    for (size_t i = 1; i < packets.size(); ++i) appendPendingPacket(std::move(packets[i]));
    return std::move(packets.front());
}

std::vector<AVPacketPtr> EncoderX265::flush()
{
    if (initialized_ && !flushed_ && !failed_) {
        if (submitFrame(nullptr)) {
            flushed_ = true;
            drainPackets();
        }
    }
    return collectAllPendingPackets();
}
