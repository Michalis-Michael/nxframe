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
 * Encoder manager implementation. This file parses codec-related preset sections, creates the selected video/audio encoders, and forwards frame encode/flush operations to the active codec legs.
 */

#include "encoder_manager.h"

#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace {

static std::string getCodecTypeFromPreset(const json& presetJson)
{
    if (presetJson.contains("video") &&
        presetJson["video"].is_object() &&
        presetJson["video"].contains("codec") &&
        presetJson["video"]["codec"].is_string()) {
        return presetJson["video"]["codec"].get<std::string>();
    }

    if (presetJson.contains("codec") && presetJson["codec"].is_string()) {
        return presetJson["codec"].get<std::string>();
    }

    return "x264";
}

static std::string normalizeLower(std::string v)
{
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] >= 'A' && v[i] <= 'Z') {
            v[i] = static_cast<char>(v[i] - 'A' + 'a');
        }
    }
    return v;
}

static bool isPcmAudioCodecName(const std::string& codec)
{
    const std::string c = normalizeLower(codec);
    return c == "pcm" || c == "pcm_s16le" || c == "lpcm" ||
           c == "s302m" || c == "smpte302m" || c == "smpte_302m";
}

static bool isAacAudioCodecName(const std::string& codec)
{
    const std::string c = normalizeLower(codec);
    return c == "aac" || c == "aac_lc" || c == "aac-lc" ||
           c == "mpeg2_aac_lc" || c == "mpeg-2_aac_lc" ||
           c == "aac_lc_mpeg2" || c == "aac-lc-mpeg2" ||
           c == "mpeg4_aac_lc" || c == "mpeg-4_aac_lc" ||
           c == "aac_lc_mpeg4" || c == "aac-lc-mpeg4";
}

static std::string getAudioModeFromAudioBlock(const json& a)
{
    if (!a.is_object()) {
        return std::string();
    }

    std::string mode = normalizeLower(a.value("mode", std::string()));
    const std::string codec = a.value("codec", std::string());
    const std::string type = a.value("type", std::string());

    if (mode.empty()) {
        if (isPcmAudioCodecName(codec) || isPcmAudioCodecName(type) ||
            normalizeLower(type) == "dolby_e" ||
            normalizeLower(type) == "ac3" ||
            normalizeLower(type) == "eac3") {
            mode = "pcm";
        } else if (isAacAudioCodecName(codec) || isAacAudioCodecName(type)) {
            mode = "encode";
        } else if (!codec.empty()) {
            mode = "encode";
        }
    }

    if (mode == "s302m" || mode == "lpcm") {
        mode = "pcm";
    }
    if (isAacAudioCodecName(mode)) {
        mode = "encode";
    }

    return mode;
}

static std::string getAudioModeFromPreset(const json& presetJson)
{
    if (!presetJson.contains("audio") || !presetJson["audio"].is_object()) {
        return std::string();
    }
    return getAudioModeFromAudioBlock(presetJson["audio"]);
}

static void applyPairAudioOverrides(json& leg, const json& pair)
{
    if (!pair.is_object()) {
        return;
    }

    for (const char* key : {"mode", "codec", "profile", "standard", "transport",
                            "bitrate", "sample_rate", "bits_per_raw_sample", "bit_depth"}) {
        if (pair.contains(key)) {
            leg[key] = pair[key];
        }
    }

    const std::string type = pair.value("type", std::string());
    const std::string type_l = normalizeLower(type);
    if (!type_l.empty()) {
        leg["type"] = type_l;

        if (type_l == "pcm" || type_l == "s302m" || type_l == "smpte302m" || type_l == "lpcm") {
            leg["mode"] = "pcm";
            leg["codec"] = "s302m";
        } else if (type_l == "dolby_e" || type_l == "ac3" || type_l == "eac3") {
            leg["mode"] = "pcm";
            leg["codec"] = "s302m";
        } else if (isAacAudioCodecName(type_l)) {
            leg["mode"] = "encode";
            leg["codec"] = type_l;
        }
    }

    if (leg.contains("codec") && leg["codec"].is_string()) {
        const std::string c = normalizeLower(leg["codec"].get<std::string>());
        leg["codec"] = c;
        if (isPcmAudioCodecName(c)) {
            leg["mode"] = "pcm";
        } else if (isAacAudioCodecName(c)) {
            leg["mode"] = "encode";
        }
    }
}

static std::vector<json> buildSplitPairAudioLegs(const json& presetJson)
{
    std::vector<json> legs;
    if (!presetJson.contains("audio") || !presetJson["audio"].is_object()) {
        return legs;
    }

    const json& audio = presetJson["audio"];
    const std::string layout = audio.value("layout", std::string("multichannel"));
    if (layout != "split_pairs") {
        legs.push_back(audio);
        return legs;
    }

    if (audio.contains("pairs") && audio["pairs"].is_array() && !audio["pairs"].empty()) {
        for (const auto& pair : audio["pairs"]) {
            if (!pair.is_object() || !pair.contains("channels") || !pair["channels"].is_array() || pair["channels"].size() != 2) {
                continue;
            }
            json leg = audio;
            leg["channels"] = 2;
            leg["channel_map"] = pair["channels"];
            applyPairAudioOverrides(leg, pair);
            if (pair.contains("name")) leg["name"] = pair["name"];
            if (pair.contains("type") && pair["type"].is_string()) {
                const std::string pairType = normalizeLower(pair["type"].get<std::string>());
                json passthrough = json::array();
                if (pairType == "dolby_e" || pairType == "ac3" || pairType == "eac3") {
                    json p = {{"type", pairType}, {"channels", pair["channels"]}};
                    if (pair.contains("bits_per_raw_sample")) {
                        p["bits_per_raw_sample"] = pair["bits_per_raw_sample"];
                        leg["bits_per_raw_sample"] = pair["bits_per_raw_sample"];
                    } else if (pair.contains("bit_depth")) {
                        p["bits_per_raw_sample"] = pair["bit_depth"];
                        leg["bits_per_raw_sample"] = pair["bit_depth"];
                    } else {
                        p["bits_per_raw_sample"] = 20;
                        leg["bits_per_raw_sample"] = 20;
                    }
                    passthrough.push_back(p);
                }
                if (!passthrough.empty()) {
                    leg["passthrough_pairs"] = passthrough;
                } else {
                    leg.erase("passthrough_pairs");
                }
            } else {
                leg.erase("passthrough_pairs");
            }
            legs.push_back(leg);
        }
        if (!legs.empty()) {
            return legs;
        }
    }

    std::vector<int> map;
    if (audio.contains("channel_map") && audio["channel_map"].is_array() && !audio["channel_map"].empty()) {
        for (const auto& entry : audio["channel_map"]) {
            if (entry.is_number_integer()) {
                map.push_back(entry.get<int>());
            }
        }
    } else {
        int ch = audio.value("channels", 0);
        for (int i = 1; i <= ch; ++i) {
            map.push_back(i);
        }
    }

    for (size_t i = 0; i + 1 < map.size(); i += 2) {
        json leg = audio;
        leg["channels"] = 2;
        leg["channel_map"] = json::array({map[i], map[i + 1]});

        json passthrough = json::array();
        if (audio.contains("passthrough_pairs") && audio["passthrough_pairs"].is_array()) {
            for (const auto& p : audio["passthrough_pairs"]) {
                if (!p.is_object() || !p.contains("channels") || !p["channels"].is_array() || p["channels"].size() != 2) {
                    continue;
                }
                if (!p["channels"][0].is_number_integer() ||
                    !p["channels"][1].is_number_integer()) {
                    continue;
                }
                const int a = p["channels"][0].get<int>();
                const int b = p["channels"][1].get<int>();
                if (a == map[i] && b == map[i + 1]) {
                    passthrough.push_back(p);
                }
            }
        }
        if (!passthrough.empty()) leg["passthrough_pairs"] = passthrough;
        else leg.erase("passthrough_pairs");
        legs.push_back(leg);
    }

    return legs;
}

} // namespace

// Load the preset once, then create the manager from the parsed JSON. The
// constructor performs object selection; initialize() performs codec startup.
std::unique_ptr<EncoderManager> EncoderManager::createEncoder(const std::string& presetPath)
{
    std::ifstream file(presetPath);
    if (!file) {
        std::cerr << "[EncoderManager] ERROR: Failed to open preset file: " << presetPath << "\n";
        return nullptr;
    }

    json presetJson;
    try {
        file >> presetJson;
    } catch (const json::parse_error& e) {
        std::cerr << "[EncoderManager] ERROR: JSON parsing failed: " << e.what() << "\n";
        return nullptr;
    } catch (const std::exception& e) {
        std::cerr << "[EncoderManager] ERROR: Failed to read preset JSON: " << e.what() << "\n";
        return nullptr;
    }

    try {
        return std::make_unique<EncoderManager>(presetJson);
    } catch (const std::exception& e) {
        std::cerr << "[EncoderManager] ERROR: Invalid encoder preset: " << e.what() << "\n";
        return nullptr;
    }
}

EncoderManager::EncoderManager(const json& presetJson)
    : codecType(getCodecTypeFromPreset(presetJson))
{
    if (codecType == "x264") {
        std::cout << "[EncoderManager] Creating EncoderX264\n";
        encoderX264 = std::make_unique<EncoderX264>(presetJson);
    } else if (codecType == "x265") {
        std::cout << "[EncoderManager] Creating EncoderX265\n";
        encoderX265 = std::make_unique<EncoderX265>(presetJson);
    } else {
        std::cerr << "[EncoderManager] ERROR: Unsupported video codec: " << codecType << "\n";
    }

    if (presetJson.contains("audio") && presetJson["audio"].is_object()) {
        const auto legs = buildSplitPairAudioLegs(presetJson);
        if (legs.empty()) {
            std::cout << "[EncoderManager] Audio block present but no valid audio legs were built. Audio disabled.\n";
        } else {
            for (const auto& legAudio : legs) {
                const std::string audioMode = getAudioModeFromAudioBlock(legAudio);
                if (audioMode.empty()) {
                    std::cerr << "[EncoderManager] WARNING: skipping audio leg with no valid mode.\n";
                    continue;
                }

                json legPreset = presetJson;
                legPreset["audio"] = legAudio;
                AudioLeg leg;
                if (audioMode == "pcm") {
                    std::cout << "[EncoderManager] Creating EncoderPCM";
                    if (legs.size() > 1 && legAudio.contains("channel_map")) {
                        std::cout << " for pair [" << legAudio["channel_map"][0].get<int>() << ","
                                  << legAudio["channel_map"][1].get<int>() << "]";
                    }
                    std::cout << "\n";
                    leg.pcm = std::make_unique<EncoderPCM>(legPreset);
                } else if (audioMode == "encode") {
                    std::cout << "[EncoderManager] Creating EncoderAAC";
                    if (legs.size() > 1 && legAudio.contains("channel_map")) {
                        std::cout << " for pair [" << legAudio["channel_map"][0].get<int>() << ","
                                  << legAudio["channel_map"][1].get<int>() << "]";
                    }
                    std::cout << "\n";
                    leg.aac = std::make_unique<EncoderAAC>(legPreset);
                } else {
                    std::cerr << "[EncoderManager] WARNING: unsupported audio mode '"
                              << audioMode << "'. Leg skipped.\n";
                    continue;
                }
                audioLegs.push_back(std::move(leg));
            }
        }
    }

    if (presetJson.contains("video") && presetJson["video"].is_object()) {
        const auto& v = presetJson["video"];
        width     = v.value("width", 0);
        height    = v.value("height", 0);
        framerate = v.value("framerate", 0);
    } else {
        if (presetJson.contains("resolution") && presetJson["resolution"].is_object()) {
            width  = presetJson["resolution"].value("width", 0);
            height = presetJson["resolution"].value("height", 0);
        }
        framerate = presetJson.value("framerate", 0);
    }
}


void EncoderManager::requestVideoKeyFrame()
{
    if (encoderX264) {
        encoderX264->requestKeyFrame();
        return;
    }
    if (encoderX265) {
        encoderX265->requestKeyFrame();
    }
}

uint8_t* EncoderManager::getBlackFrame() const
{
    if (encoderX264) {
        return encoderX264->getBlackFrame();
    }
    if (encoderX265) {
        return encoderX265->getBlackFrame();
    }
    return nullptr;
}

// Start all selected codec instances. Video and audio initialization remain
// separate internally, but this is the normal sender startup entry point.
bool EncoderManager::initialize()
{
    bool ok = true;

    if (codecType == "x264" && encoderX264) {
        std::cout << "[EncoderManager] Initializing x264 encoder...\n";
        ok &= encoderX264->initialize();
    } else if (codecType == "x265" && encoderX265) {
        std::cout << "[EncoderManager] Initializing x265 encoder...\n";
        ok &= encoderX265->initialize();
    } else {
        std::cerr << "[EncoderManager] ERROR: Unsupported or missing video encoder.\n";
        ok = false;
    }

    for (auto& leg : audioLegs) {
        if (leg.aac) {
            std::cout << "[EncoderManager] Initializing AAC encoder...\n";
            ok &= leg.aac->initialize();
        }
        if (leg.pcm) {
            std::cout << "[EncoderManager] Initializing PCM audio packetizer...\n";
            ok &= leg.pcm->initialize();
        }
    }

    return ok;
}

bool EncoderManager::initializeAudio()
{
    bool ok = !audioLegs.empty();
    for (auto& leg : audioLegs) {
        if (leg.aac) ok &= leg.aac->initialize();
        if (leg.pcm) ok &= leg.pcm->initialize();
    }
    if (!ok) {
        std::cerr << "[EncoderManager] initializeAudio(): no audio encoder present or init failed.\n";
    }
    return ok;
}

std::vector<AVPacketPtr> EncoderManager::encodeFrameZeroCopyPackets(const std::shared_ptr<uint8_t>& inputBuf,
                                                                    size_t inputBytes,
                                                                    int64_t pts)
{
    if (encoderX264) {
        return encoderX264->encodeFrameZeroCopyPackets(inputBuf, inputBytes, pts);
    }
    if (encoderX265) {
        return encoderX265->encodeFrameZeroCopyPackets(inputBuf, inputBytes, pts);
    }
    return {};
}


// Preferred video handoff. VideoFrame carries the shared image owner and
// metadata required for a safe zero-copy-oriented x264 submit path.
std::vector<AVPacketPtr> EncoderManager::encodeVideoFramePackets(const VideoFrame& vf)
{
    if (encoderX264) {
        return encoderX264->encodeVideoFramePackets(vf);
    }
    if (encoderX265) {
        return encoderX265->encodeVideoFramePackets(vf);
    }
    return {};
}

AVPacketPtr EncoderManager::encodeFrameZeroCopy(const std::shared_ptr<uint8_t>& inputBuf,
                                                size_t inputBytes,
                                                int64_t pts)
{
    std::vector<AVPacketPtr> packets = encodeFrameZeroCopyPackets(inputBuf, inputBytes, pts);
    if (packets.empty()) {
        return {};
    }
    return std::move(packets.front());
}

std::vector<AVPacketPtr> EncoderManager::encodeFramePackets(uint8_t* inputFrame, int64_t pts)
{
    if (encoderX264) {
        return encoderX264->encodeFramePackets(inputFrame, pts);
    }

    if (encoderX265) {
        return encoderX265->encodeFramePackets(inputFrame, pts);
    }

    return {};
}

AVPacketPtr EncoderManager::encodeFrame(uint8_t* inputFrame, int64_t pts)
{
    std::vector<AVPacketPtr> packets = encodeFramePackets(inputFrame, pts);
    if (packets.empty()) {
        return {};
    }
    return std::move(packets.front());
}

// Audio packets are queued because one input AudioFrame may produce zero, one,
// or many codec packets depending on codec frame size and internal buffering.
void EncoderManager::enqueueAudioPacket(AVPacket* pkt, size_t legIndex)
{
    if (!pkt) {
        return;
    }
    pkt->stream_index = static_cast<int>(legIndex);
    pendingAudioPackets.push_back(pkt);
}

AVPacket* EncoderManager::popPendingAudioPacket()
{
    if (pendingAudioPackets.empty()) {
        return nullptr;
    }
    AVPacket* pkt = pendingAudioPackets.front();
    pendingAudioPackets.pop_front();
    return pkt;
}

AVPacket* EncoderManager::drainAudioPacket()
{
    if (audioLegs.empty()) {
        return nullptr;
    }

    if (AVPacket* pkt = popPendingAudioPacket()) {
        return pkt;
    }

    for (size_t i = 0; i < audioLegs.size(); ++i) {
        AVPacket* extra = audioLegs[i].aac ? audioLegs[i].aac->receivePacket()
                                           : audioLegs[i].pcm ? audioLegs[i].pcm->receivePacket()
                                                             : nullptr;
        if (extra) {
            enqueueAudioPacket(extra, i);
        }
    }

    return popPendingAudioPacket();
}

AVPacket* EncoderManager::encodeAudioFrame(const AudioFrame& frame)
{
    if (audioLegs.empty()) {
        return nullptr;
    }

    if (!frame.buffer || frame.buffer_size == 0) {
        if (AVPacket* pkt = popPendingAudioPacket()) {
            return pkt;
        }
        for (size_t i = 0; i < audioLegs.size(); ++i) {
            AVPacket* extra = audioLegs[i].aac ? audioLegs[i].aac->receivePacket()
                                               : audioLegs[i].pcm ? audioLegs[i].pcm->receivePacket()
                                                                 : nullptr;
            if (extra) {
                enqueueAudioPacket(extra, i);
            }
        }
        return popPendingAudioPacket();
    }

    for (size_t i = 0; i < audioLegs.size(); ++i) {
        AVPacket* pkt = audioLegs[i].aac ? audioLegs[i].aac->encodeAudioFrame(frame)
                                         : audioLegs[i].pcm ? audioLegs[i].pcm->encodeAudioFrame(frame)
                                                           : nullptr;
        if (pkt) {
            enqueueAudioPacket(pkt, i);
        }

        while (true) {
            AVPacket* extra = audioLegs[i].aac ? audioLegs[i].aac->receivePacket()
                                               : audioLegs[i].pcm ? audioLegs[i].pcm->receivePacket()
                                                                 : nullptr;
            if (!extra) {
                break;
            }
            enqueueAudioPacket(extra, i);
        }
    }

    return popPendingAudioPacket();
}

bool EncoderManager::audioOutputMayBuffer() const
{
    for (const auto& leg : audioLegs) {
        if (leg.aac) {
            return true;
        }
    }
    return false;
}

std::vector<AVPacket*> EncoderManager::flushAudio()
{
    std::vector<AVPacket*> out;
    while (AVPacket* pkt = popPendingAudioPacket()) {
        out.push_back(pkt);
    }

    for (size_t i = 0; i < audioLegs.size(); ++i) {
        std::vector<AVPacket*> flushed = audioLegs[i].aac ? audioLegs[i].aac->flush()
                                                          : audioLegs[i].pcm ? audioLegs[i].pcm->flush()
                                                                            : std::vector<AVPacket*>();
        for (AVPacket* pkt : flushed) {
            enqueueAudioPacket(pkt, i);
        }
    }

    while (AVPacket* pkt = popPendingAudioPacket()) {
        out.push_back(pkt);
    }
    return out;
}

std::vector<AVPacketPtr> EncoderManager::flushVideo()
{
    if (encoderX264) {
        return encoderX264->flush();
    }
    if (encoderX265) {
        return encoderX265->flush();
    }
    return {};
}

AVCodecContext* EncoderManager::getVideoCodecContext() const
{
    if (encoderX264) {
        return encoderX264->getCodecContext();
    }
    if (encoderX265) {
        return encoderX265->getCodecContext();
    }
    return nullptr;
}

std::vector<AVCodecContext*> EncoderManager::getAudioCodecContexts() const
{
    std::vector<AVCodecContext*> ctxs;
    for (const auto& leg : audioLegs) {
        if (leg.aac && leg.aac->getCodecContext()) ctxs.push_back(leg.aac->getCodecContext());
        else if (leg.pcm && leg.pcm->getCodecContext()) ctxs.push_back(leg.pcm->getCodecContext());
    }
    return ctxs;
}

AVCodecContext* EncoderManager::getAudioCodecContext() const
{
    auto ctxs = getAudioCodecContexts();
    return ctxs.empty() ? nullptr : ctxs.front();
}

AVCodecContext* EncoderManager::getCodecContext() const
{
    return getVideoCodecContext();
}