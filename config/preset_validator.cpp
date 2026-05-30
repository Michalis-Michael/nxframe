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
 * Preset validation implementation. This module checks sender and receiver JSON presets before the pipeline opens hardware or network resources.
 */

#include "config/preset_validator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace {

// Validator helpers report human-readable JSON paths so preset errors are actionable at startup.
std::string lowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string pathJoin(const std::string& parent, const std::string& key)
{
    return parent.empty() ? key : parent + "." + key;
}

const json* sectionObject(const json& root, const char* key)
{
    if (root.contains(key) && root[key].is_object()) {
        return &root[key];
    }
    return nullptr;
}

const json* videoSection(const json& root)
{
    if (const json* v = sectionObject(root, "video")) {
        return v;
    }
    return &root;
}

const json* nestedObject(const json& root, const std::vector<std::string>& keys)
{
    const json* cur = &root;
    for (const auto& key : keys) {
        if (!cur->is_object() || !cur->contains(key) || !(*cur)[key].is_object()) {
            return nullptr;
        }
        cur = &(*cur)[key];
    }
    return cur;
}

bool hasAny(const json& obj, const std::vector<std::string>& keys)
{
    if (!obj.is_object()) {
        return false;
    }
    for (const auto& k : keys) {
        if (obj.contains(k)) {
            return true;
        }
    }
    return false;
}

std::string typeName(const json& v)
{
    if (v.is_null()) return "null";
    if (v.is_boolean()) return "boolean";
    if (v.is_number_integer()) return "integer";
    if (v.is_number_unsigned()) return "unsigned integer";
    if (v.is_number_float()) return "number";
    if (v.is_string()) return "string";
    if (v.is_array()) return "array";
    if (v.is_object()) return "object";
    return "unknown";
}

void requireObject(const json& obj,
                   const std::string& key,
                   const std::string& path,
                   PresetValidator::Result& r)
{
    if (!obj.contains(key)) {
        r.errors.push_back(pathJoin(path, key) + " is required");
        return;
    }
    if (!obj[key].is_object()) {
        r.errors.push_back(pathJoin(path, key) + " must be an object, got " + typeName(obj[key]));
    }
}

bool requireInt(const json& obj,
                const std::string& key,
                const std::string& path,
                int minValue,
                int maxValue,
                PresetValidator::Result& r,
                bool required = true)
{
    const std::string p = pathJoin(path, key);
    if (!obj.contains(key)) {
        if (required) r.errors.push_back(p + " is required");
        return false;
    }
    if (!obj[key].is_number_integer()) {
        r.errors.push_back(p + " must be an integer, got " + typeName(obj[key]));
        return false;
    }
    const int v = obj[key].get<int>();
    if (v < minValue || v > maxValue) {
        r.errors.push_back(p + " value " + std::to_string(v) + " outside allowed range " +
                           std::to_string(minValue) + ".." + std::to_string(maxValue));
        return false;
    }
    return true;
}

bool requireIntAny(const json& obj,
                   const std::vector<std::string>& keys,
                   const std::string& path,
                   int minValue,
                   int maxValue,
                   PresetValidator::Result& r,
                   bool required = true)
{
    for (const auto& key : keys) {
        if (obj.contains(key)) {
            return requireInt(obj, key, path, minValue, maxValue, r, true);
        }
    }
    if (required && !keys.empty()) {
        std::ostringstream oss;
        oss << path << " requires one of: ";
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) oss << ", ";
            oss << keys[i];
        }
        r.errors.push_back(oss.str());
    }
    return false;
}

bool optionalInt(const json& obj,
                 const std::string& key,
                 const std::string& path,
                 int minValue,
                 int maxValue,
                 PresetValidator::Result& r)
{
    return requireInt(obj, key, path, minValue, maxValue, r, false);
}

bool requireNumber(const json& obj,
                   const std::string& key,
                   const std::string& path,
                   double minValue,
                   double maxValue,
                   PresetValidator::Result& r,
                   bool required = true)
{
    const std::string p = pathJoin(path, key);
    if (!obj.contains(key)) {
        if (required) r.errors.push_back(p + " is required");
        return false;
    }
    if (!obj[key].is_number()) {
        r.errors.push_back(p + " must be numeric, got " + typeName(obj[key]));
        return false;
    }
    const double v = obj[key].get<double>();
    if (v < minValue || v > maxValue) {
        r.errors.push_back(p + " value outside allowed range");
        return false;
    }
    return true;
}

bool optionalStringEnum(const json& obj,
                        const std::string& key,
                        const std::string& path,
                        const std::set<std::string>& allowed,
                        PresetValidator::Result& r)
{
    if (!obj.contains(key)) {
        return true;
    }
    const std::string p = pathJoin(path, key);
    if (!obj[key].is_string()) {
        r.errors.push_back(p + " must be a string, got " + typeName(obj[key]));
        return false;
    }
    const std::string v = lowerCopy(obj[key].get<std::string>());
    if (!allowed.empty() && !allowed.count(v)) {
        std::ostringstream oss;
        oss << p << " unsupported value '" << obj[key].get<std::string>() << "'. Allowed: ";
        size_t i = 0;
        for (const auto& a : allowed) {
            if (i++) oss << ", ";
            oss << a;
        }
        r.errors.push_back(oss.str());
        return false;
    }
    return true;
}

bool optionalBool(const json& obj,
                  const std::string& key,
                  const std::string& path,
                  PresetValidator::Result& r)
{
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_boolean()) {
        r.errors.push_back(pathJoin(path, key) + " must be a boolean, got " + typeName(obj[key]));
        return false;
    }
    return true;
}

bool optionalPbKeyLen(const json& obj,
                      const std::string& key,
                      const std::string& path,
                      PresetValidator::Result& r)
{
    if (!obj.contains(key)) {
        return true;
    }
    const std::string p = pathJoin(path, key);
    if (!obj[key].is_number_integer()) {
        r.errors.push_back(p + " must be an integer, got " + typeName(obj[key]));
        return false;
    }
    const int v = obj[key].get<int>();
    if (!(v == 0 || v == 16 || v == 24 || v == 32)) {
        r.errors.push_back(p + " must be one of 0, 16, 24, or 32");
        return false;
    }
    return true;
}

void validateChannelPairArray(const json& arr,
                              const std::string& path,
                              PresetValidator::Result& r)
{
    if (!arr.is_array()) {
        r.errors.push_back(path + " must be an array");
        return;
    }
    if (arr.size() != 2) {
        r.errors.push_back(path + " must contain exactly two channel numbers");
        return;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].is_number_integer()) {
            r.errors.push_back(path + "[" + std::to_string(i) + "] must be an integer");
            continue;
        }
        const int ch = arr[i].get<int>();
        if (ch < 1 || ch > 64) {
            r.errors.push_back(path + "[" + std::to_string(i) + "] value " +
                               std::to_string(ch) + " outside allowed range 1..64");
        }
    }
}

void validateChannelMapArray(const json& arr,
                             const std::string& path,
                             PresetValidator::Result& r)
{
    if (!arr.is_array()) {
        r.errors.push_back(path + " must be an array");
        return;
    }
    if (arr.empty()) {
        r.errors.push_back(path + " must not be empty");
        return;
    }
    if (arr.size() % 2 != 0) {
        r.errors.push_back(path + " must contain an even number of channels for stereo pairs");
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].is_number_integer()) {
            r.errors.push_back(path + "[" + std::to_string(i) + "] must be an integer");
            continue;
        }
        const int ch = arr[i].get<int>();
        if (ch < 1 || ch > 64) {
            r.errors.push_back(path + "[" + std::to_string(i) + "] value " +
                               std::to_string(ch) + " outside allowed range 1..64");
        }
    }
}

void validateSenderVideo(const json& root, PresetValidator::Result& r)
{
    const json* video = videoSection(root);
    const std::string path = (video == &root) ? "preset" : "video";

    optionalStringEnum(*video, "codec", path, {"x264", "x265", "h264", "hevc"}, r);
    if (!video->contains("codec") && !(root.contains("codec") && root["codec"].is_string())) {
        r.errors.push_back(pathJoin(path, "codec") + " is required");
    }

    // Existing presets use either root/video width+height or resolution object.
    if (const json* res = sectionObject(root, "resolution")) {
        requireInt(*res, "width", "resolution", 16, 8192, r);
        requireInt(*res, "height", "resolution", 16, 8192, r);
    } else {
        requireInt(*video, "width", path, 16, 8192, r);
        requireInt(*video, "height", path, 16, 8192, r);
    }

    if (video->contains("framerate")) {
        requireNumber(*video, "framerate", path, 1.0, 240.0, r);
    } else {
        r.errors.push_back(pathJoin(path, "framerate") + " is required");
    }

    requireInt(*video, "bitrate", path, 100000, 300000000, r);
    optionalInt(*video, "max_b_frames", path, 0, 16, r);
    optionalBool(*video, "interlaced", path, r);
    optionalStringEnum(*video, "profile", path,
                       {"baseline", "main", "high", "high422", "high444", "main10", "main422-10"}, r);
    optionalStringEnum(*video, "preset", path,
                       {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"}, r);
    optionalStringEnum(*video, "tune", path,
                       {"zerolatency", "film", "animation", "grain", "stillimage", "fastdecode"}, r);

    if (const json* out = sectionObject(*video, "output")) {
        optionalInt(*out, "bit_depth", pathJoin(path, "output"), 8, 12, r);
        optionalStringEnum(*out, "chroma", pathJoin(path, "output"), {"420", "422", "444"}, r);
    }
    if (const json* color = sectionObject(*video, "color")) {
        const std::string p = pathJoin(path, "color");
        optionalStringEnum(*color, "primaries", p, {"bt709", "bt2020", "smpte170m", "bt470bg"}, r);
        optionalStringEnum(*color, "transfer", p, {"bt709", "bt2020-10", "bt2020-12", "hlg", "pq", "smpte2084"}, r);
        optionalStringEnum(*color, "matrix", p, {"bt709", "bt2020nc", "bt2020c", "smpte170m", "bt470bg"}, r);
        optionalStringEnum(*color, "range", p, {"limited", "full", "tv", "pc"}, r);
    }

    if (const json* gop = sectionObject(*video, "gop")) {
        const std::string p = pathJoin(path, "gop");
        optionalInt(*gop, "size", p, 1, 1000, r);
        optionalInt(*gop, "min_keyint", p, 1, 1000, r);
        optionalInt(*gop, "scenecut", p, 0, 1000, r);
        optionalBool(*gop, "closed", p, r);
    }
}

void validateSenderAudio(const json& root, PresetValidator::Result& r)
{
    const json* audio = sectionObject(root, "audio");
    if (!audio) {
        r.warnings.push_back("audio section missing; sender will run video-only if encoder permits it");
        return;
    }

    const std::string path = "audio";
    optionalStringEnum(*audio, "mode", path, {"aac", "aac_low", "aac_lc", "aac_lc_mpeg2", "aac_lc_mpeg4", "pcm", "encode", "passthrough"}, r);
    optionalStringEnum(*audio, "codec", path, {"aac", "aac_low", "aac_lc", "aac_lc_mpeg2", "aac_lc_mpeg4", "pcm", "s302m"}, r);
    optionalStringEnum(*audio, "layout", path, {"split_pairs", "stereo", "mono"}, r);
    optionalInt(*audio, "input_channels", path, 1, 64, r);
    optionalInt(*audio, "sample_rate", path, 8000, 192000, r);
    optionalInt(*audio, "bitrate", path, 8000, 50000000, r);

    if (audio->contains("channel_map")) {
        validateChannelMapArray((*audio)["channel_map"], pathJoin(path, "channel_map"), r);
    }

    if (audio->contains("pairs")) {
        const json& pairs = (*audio)["pairs"];
        if (!pairs.is_array()) {
            r.errors.push_back("audio.pairs must be an array");
        } else if (pairs.empty()) {
            r.errors.push_back("audio.pairs must not be empty when present");
        } else {
            for (size_t i = 0; i < pairs.size(); ++i) {
                const std::string p = "audio.pairs[" + std::to_string(i) + "]";
                if (!pairs[i].is_object()) {
                    r.errors.push_back(p + " must be an object");
                    continue;
                }
                if (!pairs[i].contains("channels")) {
                    r.errors.push_back(p + ".channels is required");
                } else {
                    validateChannelPairArray(pairs[i]["channels"], p + ".channels", r);
                }
                optionalStringEnum(pairs[i], "type", p, {"aac", "aac_low", "aac_lc", "aac_lc_mpeg2", "aac_lc_mpeg4", "pcm", "s302m", "dolby_e"}, r);
                optionalInt(pairs[i], "bits_per_raw_sample", p, 16, 24, r);
                optionalInt(pairs[i], "bit_depth", p, 16, 24, r);
            }
        }
    }

    if (audio->contains("passthrough_pairs")) {
        const json& pairs = (*audio)["passthrough_pairs"];
        if (!pairs.is_array()) {
            r.errors.push_back("audio.passthrough_pairs must be an array");
        } else {
            for (size_t i = 0; i < pairs.size(); ++i) {
                const std::string p = "audio.passthrough_pairs[" + std::to_string(i) + "]";
                if (!pairs[i].is_object() || !pairs[i].contains("channels")) {
                    r.errors.push_back(p + ".channels is required");
                    continue;
                }
                validateChannelPairArray(pairs[i]["channels"], p + ".channels", r);
            }
        }
    }
}

void validateTransport(const json& root, PresetValidator::Result& r)
{
    const json* srt = sectionObject(root, "srt");
    if (!srt) {
        srt = nestedObject(root, {"output", "srt"});
    }
    if (!srt && root.contains("transport") && root["transport"].is_object() &&
        !root["transport"].contains("udp") && !root["transport"].contains("srt")) {
        srt = &root["transport"];
    }
    if (srt) {
        const std::string p = "srt";
        optionalStringEnum(*srt, "mode", p, {"listener", "caller", "rendezvous"}, r);
        optionalInt(*srt, "latency", p, 20, 30000, r);
        optionalInt(*srt, "payload_size", p, 188, 1456, r);
        if (srt->contains("payload_size") && (*srt)["payload_size"].is_number_integer()) {
            const int payload = (*srt)["payload_size"].get<int>();
            if (payload % 188 != 0) {
                r.warnings.push_back("srt.payload_size is not a multiple of 188; MPEG-TS packet grouping may be inefficient");
            }
        }
        optionalInt(*srt, "reconnect_attempts", p, 0, 1000000, r);
        optionalInt(*srt, "reconnect_delay_ms", p, 0, 600000, r);
        optionalInt(*srt, "connect_timeout_ms", p, 100, 600000, r);
        optionalInt(*srt, "send_timeout_ms", p, 1, 600000, r);
        optionalInt(*srt, "recv_timeout_ms", p, 1, 600000, r);
        optionalPbKeyLen(*srt, "pbkeylen", p, r);
        if (srt->contains("passphrase")) {
            if (!(*srt)["passphrase"].is_string()) {
                r.errors.push_back("srt.passphrase must be a string");
            } else {
                const size_t passLen = (*srt)["passphrase"].get<std::string>().size();
                if (passLen > 0 && (passLen < 10 || passLen > 79)) {
                    r.errors.push_back("srt.passphrase length must be 10..79 characters when encryption is enabled");
                }
            }
        }
        if (srt->contains("pbkeylen") && (*srt)["pbkeylen"].is_number_integer() &&
            (*srt)["pbkeylen"].get<int>() > 0 &&
            (!srt->contains("passphrase") || !(*srt)["passphrase"].is_string() || (*srt)["passphrase"].get<std::string>().empty())) {
            r.errors.push_back("srt.pbkeylen requires a non-empty passphrase");
        }
        optionalInt(*srt, "oheadbw", p, 0, 1000, r);
        optionalBool(*srt, "tlpktdrop", p, r);
        optionalBool(*srt, "messageapi", p, r);
    }

    const json* udp = sectionObject(root, "udp");
    if (!udp) udp = nestedObject(root, {"transport", "udp"});
    if (!udp) udp = nestedObject(root, {"output", "udp"});
    if (udp) {
        const std::string p = "udp";
        optionalInt(*udp, "payload_size", p, 188, 1316, r);
        if (udp->contains("payload_size") && (*udp)["payload_size"].is_number_integer()) {
            const int payload = (*udp)["payload_size"].get<int>();
            if (payload % 188 != 0) {
                r.warnings.push_back("udp.payload_size is not a multiple of 188; RTP/UDP MPEG-TS should normally use 1316 bytes");
            }
        }
        optionalInt(*udp, "ttl", p, 0, 255, r);
        optionalInt(*udp, "multicast_ttl", p, 0, 255, r);
        optionalBool(*udp, "multicast_loop", p, r);
        optionalBool(*udp, "pacing_enabled", p, r);
    }
}

void validateMpegTs(const json& root, PresetValidator::Result& r)
{
    const json* mpegts = sectionObject(root, "mpegts");
    if (!mpegts) mpegts = nestedObject(root, {"output", "mpegts"});
    if (!mpegts) return;

    const std::string p = "mpegts";
    if (mpegts->contains("service_provider") && !(*mpegts)["service_provider"].is_string()) {
        r.errors.push_back("mpegts.service_provider must be a string");
    }
    if (mpegts->contains("service_name") && !(*mpegts)["service_name"].is_string()) {
        r.errors.push_back("mpegts.service_name must be a string");
    }
    (void)p;
}

void validateReceiverAudio(const json& root, PresetValidator::Result& r)
{
    const json* section = sectionObject(root, "receiver_audio");
    const std::string path = section ? "receiver_audio" : "preset";
    if (!section) {
        section = &root;
    }

    requireInt(*section, "packed_audio_channels", path, 2, 16, r, false);
    requireInt(*section, "max_audio_pairs", path, 1, 8, r, false);

    int packedChannels = 16;
    int maxPairs = 8;
    if (section->contains("packed_audio_channels") && (*section)["packed_audio_channels"].is_number_integer()) {
        packedChannels = (*section)["packed_audio_channels"].get<int>();
        if (!(packedChannels == 2 || packedChannels == 8 || packedChannels == 16)) {
            r.errors.push_back(path + ".packed_audio_channels supported values are exactly 2, 8, or 16");
        }
    }
    if (section->contains("max_audio_pairs") && (*section)["max_audio_pairs"].is_number_integer()) {
        maxPairs = (*section)["max_audio_pairs"].get<int>();
    }
    if (packedChannels < maxPairs * 2) {
        r.errors.push_back(path + ".packed_audio_channels must be at least max_audio_pairs * 2");
    }

    if (section->contains("audio_pair_route")) {
        const json& route = (*section)["audio_pair_route"];
        if (!route.is_array()) {
            r.errors.push_back(path + ".audio_pair_route must be an array");
        } else {
            if (!route.empty() && static_cast<int>(route.size()) > maxPairs) {
                r.warnings.push_back(path + ".audio_pair_route has more entries than max_audio_pairs; extra entries may not be used");
            }
            for (size_t i = 0; i < route.size(); ++i) {
                if (!route[i].is_number_integer()) {
                    r.errors.push_back(path + ".audio_pair_route[" + std::to_string(i) + "] must be an integer");
                    continue;
                }
                const int value = route[i].get<int>();
                if (value < 0 || value > maxPairs) {
                    r.errors.push_back(path + ".audio_pair_route[" + std::to_string(i) + "] outside allowed range 0..max_audio_pairs");
                }
            }
        }
    }
}

void validateDeckLinkPlayout(const json& root, PresetValidator::Result& r)
{
    const json* section = sectionObject(root, "decklink_playout");
    if (!section) section = nestedObject(root, {"receiver", "decklink_playout"});
    if (!section) {
        r.warnings.push_back("decklink_playout section missing; receiver will use built-in playout defaults");
        return;
    }

    const std::string p = "decklink_playout";
    optionalInt(*section, "video_preroll_frames", p, 1, 120, r);
    optionalInt(*section, "max_video_queue_frames", p, 1, 240, r);
    optionalInt(*section, "max_audio_queue_samples", p, 480, 960000, r);
    optionalInt(*section, "startup_anchor_timeout_ms", p, 1, 30000, r);
    optionalInt(*section, "source_loss_ms", p, 1, 30000, r);
    optionalInt(*section, "black_fallback_ms", p, 1, 30000, r);
    optionalInt(*section, "log_status_interval_ms", p, 100, 60000, r);

    if (section->contains("video_preroll_frames") && section->contains("max_video_queue_frames") &&
        (*section)["video_preroll_frames"].is_number_integer() && (*section)["max_video_queue_frames"].is_number_integer()) {
        const int preroll = (*section)["video_preroll_frames"].get<int>();
        const int maxq = (*section)["max_video_queue_frames"].get<int>();
        if (maxq < preroll + 2) {
            r.warnings.push_back("decklink_playout.max_video_queue_frames should be at least video_preroll_frames + 2");
        }
    }
}

} // namespace

// File-level entry point: read JSON, validate by kind, and return all errors/warnings together.
PresetValidator::Result PresetValidator::validateFile(const std::string& path, Kind kind)
{
    Result r;
    std::ifstream file(path);
    if (!file) {
        r.errors.push_back("failed to open preset file: " + path);
        return r;
    }

    json root;
    try {
        file >> root;
    } catch (const json::parse_error& e) {
        r.errors.push_back(std::string("JSON parse error: ") + e.what());
        return r;
    } catch (const std::exception& e) {
        r.errors.push_back(std::string("JSON read error: ") + e.what());
        return r;
    }

    return validateJson(root, kind);
}

// Validate the in-memory preset object before any sender/receiver resources are created.
PresetValidator::Result PresetValidator::validateJson(const json& root, Kind kind)
{
    Result r;
    if (!root.is_object()) {
        r.errors.push_back("preset root must be a JSON object");
        return r;
    }

    if (kind == Kind::Sender) {
        validateSenderVideo(root, r);
        validateSenderAudio(root, r);
        validateTransport(root, r);
        validateMpegTs(root, r);
    } else {
        validateReceiverAudio(root, r);
        validateDeckLinkPlayout(root, r);
    }

    return r;
}

// Keep validator output stable because it is used during release testing and field diagnostics.
void PresetValidator::printResult(const Result& result, const std::string& path, Kind kind)
{
    const char* label = (kind == Kind::Sender) ? "sender" : "receiver";
    if (result.ok()) {
        std::cout << "[PresetValidator] " << label << " preset validated: " << path << "\n";
    } else {
        std::cerr << "[PresetValidator] ERROR: " << label << " preset validation failed: " << path << "\n";
    }

    for (const auto& e : result.errors) {
        std::cerr << "[PresetValidator]   error: " << e << "\n";
    }
    for (const auto& w : result.warnings) {
        std::cerr << "[PresetValidator]   warning: " << w << "\n";
    }
}
